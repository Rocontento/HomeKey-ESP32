#include "MiioLock.hpp"

#include <cstdio>
#include <cstring>
#include <span>
#include <array>

#include <unistd.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "mbedtls/md5.h"
#include "mbedtls/aes.h"

#include "app_events.hpp"
#include "eventStructs.hpp"
#include "LockManager.hpp"

static const char* TAG = "MiioLock";
static constexpr uint16_t MIIO_PORT = 54321;
static constexpr int64_t TAP_COOLDOWN_US = 3 * 1000 * 1000;  // reader can fire twice per tap

// ---------- crypto / packing (pure, testable) ----------

static void md5_3(const uint8_t* a, size_t al, const uint8_t* b, size_t bl,
                  const uint8_t* c, size_t cl, uint8_t out[16]) {
  mbedtls_md5_context ctx;
  mbedtls_md5_init(&ctx);
  mbedtls_md5_starts(&ctx);
  if (a) mbedtls_md5_update(&ctx, a, al);
  if (b) mbedtls_md5_update(&ctx, b, bl);
  if (c) mbedtls_md5_update(&ctx, c, cl);
  mbedtls_md5_finish(&ctx, out);
  mbedtls_md5_free(&ctx);
}

static void derive(const uint8_t token[16], uint8_t key[16], uint8_t iv[16]) {
  md5_3(token, 16, nullptr, 0, nullptr, 0, key);
  md5_3(key, 16, token, 16, nullptr, 0, iv);
}

static bool hex2bin(const char* hex, uint8_t* out, size_t n) {
  if (strlen(hex) != n * 2) return false;
  for (size_t i = 0; i < n; i++) {
    unsigned v;
    if (sscanf(hex + i * 2, "%2x", &v) != 1) return false;
    out[i] = static_cast<uint8_t>(v);
  }
  return true;
}

// AES-128-CBC with PKCS7. Returns plaintext/ciphertext length, 0 on error.
static size_t aes_cbc(const uint8_t key[16], const uint8_t iv_in[16], bool encrypt,
                      const uint8_t* in, size_t len, uint8_t* out, size_t out_sz) {
  uint8_t iv[16];
  memcpy(iv, iv_in, 16);
  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);
  size_t n = len;
  int rc;
  if (encrypt) {
    const size_t pad = 16 - (len % 16);  // PKCS7 always pads, even on a multiple
    n = len + pad;
    if (n > out_sz) { mbedtls_aes_free(&aes); return 0; }
    memcpy(out, in, len);
    memset(out + len, static_cast<int>(pad), pad);
    rc = mbedtls_aes_setkey_enc(&aes, key, 128);
    if (rc == 0) rc = mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_ENCRYPT, n, iv, out, out);
  } else {
    if (len == 0 || len % 16 || len > out_sz) { mbedtls_aes_free(&aes); return 0; }
    rc = mbedtls_aes_setkey_dec(&aes, key, 128);
    if (rc == 0) rc = mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_DECRYPT, len, iv, in, out);
    if (rc == 0) {
      const uint8_t pad = out[len - 1];
      if (pad == 0 || pad > 16 || pad > len) { mbedtls_aes_free(&aes); return 0; }
      n = len - pad;
    }
  }
  mbedtls_aes_free(&aes);
  return rc == 0 ? n : 0;
}

static void be32(uint8_t* p, uint32_t v) {
  p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}

// 0x2131 | len | 0000 0000 | device_id | stamp | md5(header+token+data) | data
static size_t pack(const uint8_t token[16], uint32_t dev_id, uint32_t stamp,
                   const char* payload, uint8_t* out, size_t out_sz) {
  uint8_t key[16], iv[16];
  derive(token, key, iv);
  const size_t plen = strlen(payload) + 1;  // the trailing NUL is part of the payload
  if (out_sz < 32 + plen + 16) return 0;
  const size_t dlen = aes_cbc(key, iv, true, reinterpret_cast<const uint8_t*>(payload),
                              plen, out + 32, out_sz - 32);
  if (!dlen) return 0;

  out[0] = 0x21; out[1] = 0x31;
  out[2] = static_cast<uint8_t>((32 + dlen) >> 8);
  out[3] = static_cast<uint8_t>(32 + dlen);
  memset(out + 4, 0, 4);
  be32(out + 8, dev_id);
  be32(out + 12, stamp);
  memcpy(out + 16, token, 16);  // the checksum field holds the token while hashing
  md5_3(out, 32 + dlen, nullptr, 0, nullptr, 0, out + 16);
  return 32 + dlen;
}

// ---------- transport ----------

void MiioLock::setCredentials(std::string host, std::string token, std::string did) {
  m_host = std::move(host);
  m_token = std::move(token);
  m_did = std::move(did);
  ESP_LOGI(TAG, "Credentials set for %s (did %s), configured: %d",
           m_host.c_str(), m_did.c_str(), isConfigured());
}

bool MiioLock::callAction(int siid, int aiid) {
  if (!isConfigured()) { ESP_LOGW(TAG, "Not configured, ignoring action"); return false; }

  uint8_t token[16];
  if (!hex2bin(m_token.c_str(), token, 16)) {
    ESP_LOGE(TAG, "Token must be 32 hex chars");
    return false;
  }

  int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (sock < 0) { ESP_LOGE(TAG, "socket() failed"); return false; }
  timeval tv{.tv_sec = 2, .tv_usec = 0};
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(MIIO_PORT);
  addr.sin_addr.s_addr = inet_addr(m_host.c_str());

  bool ok = false;
  uint8_t rx[1024];
  do {
    // Handshake on every call: unlatching is rare, and this refreshes the device
    // stamp so we never fight clock drift.
    static const uint8_t hello[32] = {
      0x21, 0x31, 0x00, 0x20, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
      0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
      0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    sendto(sock, hello, sizeof(hello), 0, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    int n = recv(sock, rx, sizeof(rx), 0);
    if (n < 32 || rx[0] != 0x21 || rx[1] != 0x31) { ESP_LOGE(TAG, "No hello reply from %s", m_host.c_str()); break; }
    const uint32_t dev_id = (rx[8] << 24) | (rx[9] << 16) | (rx[10] << 8) | rx[11];
    const uint32_t stamp  = (rx[12] << 24) | (rx[13] << 16) | (rx[14] << 8) | rx[15];

    char json[256];
    const int msg_id = static_cast<int>(esp_random() % 10000) + 1;
    snprintf(json, sizeof(json),
             "{\"id\":%d,\"method\":\"action\",\"params\":"
             "{\"did\":\"%s\",\"siid\":%d,\"aiid\":%d,\"in\":[]}}",
             msg_id, m_did.c_str(), siid, aiid);

    uint8_t tx[512];
    const size_t len = pack(token, dev_id, stamp + 1, json, tx, sizeof(tx));
    if (!len) { ESP_LOGE(TAG, "pack failed"); break; }

    sendto(sock, tx, len, 0, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    n = recv(sock, rx, sizeof(rx), 0);
    if (n <= 32) { ESP_LOGE(TAG, "No action reply"); break; }

    uint8_t key[16], iv[16], plain[512];
    derive(token, key, iv);
    const size_t plen = aes_cbc(key, iv, false, rx + 32, n - 32, plain, sizeof(plain));
    if (!plen) { ESP_LOGE(TAG, "Decrypt failed (wrong token?)"); break; }
    plain[plen < sizeof(plain) ? plen : sizeof(plain) - 1] = 0;
    ESP_LOGI(TAG, "reply: %s", reinterpret_cast<char*>(plain));
    ok = strstr(reinterpret_cast<char*>(plain), "\"code\":0") != nullptr;
  } while (false);

  close(sock);
  return ok;
}

// ---------- event wiring ----------

namespace {
struct Job { MiioLock* self; int siid; int aiid; };

void miioTask(void* arg) {
  auto* job = static_cast<Job*>(arg);
  const bool ok = job->self->callAction(job->siid, job->aiid);

  // Report what really happened so the HomeKit tile isn't lying.
  EventLockState s{
      .currentState = static_cast<uint8_t>(ok ? LockManager::lockStates::UNLOCKED
                                              : LockManager::lockStates::JAMMED),
      .targetState = static_cast<uint8_t>(LockManager::lockStates::UNLOCKED),
      .source = LockManager::INTERNAL,
  };
  std::array<uint8_t, sizeof(EventLockState)> d{};
  const size_t d_len = alpaca::serialize(s, d);
  AppEventLoop::publish(LOCK_EVENT, LOCK_UPDATE_STATE, d.data(), d_len);

  delete job;
  vTaskDelete(nullptr);
}
}  // namespace

void MiioLock::begin() {
  if (!selftest()) ESP_LOGE(TAG, "miIO selftest FAILED - packets would be rejected");

  m_nfc_event = AppEventLoop::subscribe(NFC_EVENT, NFC_TAP_EVENT, [this](const uint8_t* data, size_t size) {
    if (size == 0 || data == nullptr) return;
    std::span<const uint8_t> payload(data, size);
    std::error_code ec;
    NfcEvent ev = alpaca::deserialize<NfcEvent>(payload, ec);
    if (ec) { ESP_LOGE(TAG, "deserialize NFC event failed: %s", ec.message().c_str()); return; }
    if (ev.type != HOMEKEY_TAP) return;

    EventHKTap tap = alpaca::deserialize<EventHKTap>(ev.data, ec);
    if (ec) { ESP_LOGE(TAG, "deserialize HomeKey tap failed: %s", ec.message().c_str()); return; }
    if (!tap.status) return;  // authentication failed, no unlatch
    if (!isConfigured()) return;

    const int64_t now = esp_timer_get_time();
    if (now - m_lastTapUs < TAP_COOLDOWN_US) {
      ESP_LOGD(TAG, "Tap within cooldown, ignoring");
      return;
    }
    m_lastTapUs = now;

    ESP_LOGI(TAG, "HomeKey authenticated, unlatching (siid %d aiid %d)", siid, unlatch_aiid);
    // Off the event-loop task: the miIO round-trip blocks up to ~2 s.
    auto* job = new Job{this, siid, unlatch_aiid};
    if (xTaskCreate(miioTask, "miio", 4096, job, 5, nullptr) != pdPASS) {
      ESP_LOGE(TAG, "xTaskCreate failed");
      delete job;
    }
  });
}

// ---------- selftest ----------

bool MiioLock::selftest() {
  const uint8_t token[16] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
                             0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
  const char* json =
      "{\"id\":1,\"method\":\"action\",\"params\":"
      "{\"did\":\"1\",\"siid\":18,\"aiid\":4,\"in\":[]}}";
  const char* want =
      "21310070000000000102abcd0000100020caf89c9dd67591005aeacf60550dc5a5"
      "516ec6151955dc2bb2d43e7c84c183ee3917cbdb14386193df742cda2ef993827c"
      "360c48a7f25cad8413ee4dfe44e2447f9404bb47202fa9a9d24288f910aa6e2cae"
      "0fe95d019858d6dd0dd0bd2e7a";

  uint8_t pkt[256];
  const size_t n = pack(token, 0x0102ABCD, 0x00001000, json, pkt, sizeof(pkt));
  if (n != strlen(want) / 2) return false;

  char hex[2 * sizeof(pkt) + 1];
  for (size_t i = 0; i < n; i++) sprintf(hex + i * 2, "%02x", pkt[i]);
  return strcmp(hex, want) == 0;
}
