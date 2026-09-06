#include "XiaomiCloud.hpp"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <algorithm>
#include <cctype>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "mbedtls/md5.h"
#include "mbedtls/sha1.h"
#include "mbedtls/sha256.h"
#include "mbedtls/base64.h"
#include "cJSON.h"

static const char* TAG = "XiaomiCloud";
static constexpr size_t MAX_BODY = 96 * 1024;  // a big home is ~20 kB; bail well before OOM

// ---------- small crypto / encoding helpers ----------

static std::string b64Encode(const uint8_t* in, size_t len) {
  size_t need = 0;
  mbedtls_base64_encode(nullptr, 0, &need, in, len);
  std::string out(need, '\0');
  size_t written = 0;
  if (mbedtls_base64_encode(reinterpret_cast<uint8_t*>(out.data()), need, &written, in, len) != 0) return {};
  out.resize(written);
  return out;
}

static std::vector<uint8_t> b64Decode(const std::string& in) {
  size_t need = 0;
  mbedtls_base64_decode(nullptr, 0, &need, reinterpret_cast<const uint8_t*>(in.data()), in.size());
  std::vector<uint8_t> out(need);
  size_t written = 0;
  if (mbedtls_base64_decode(out.data(), need, &written, reinterpret_cast<const uint8_t*>(in.data()), in.size()) != 0)
    return {};
  out.resize(written);
  return out;
}

static std::string md5Upper(const std::string& s) {
  uint8_t d[16];
  mbedtls_md5_context ctx;
  mbedtls_md5_init(&ctx);
  mbedtls_md5_starts(&ctx);
  mbedtls_md5_update(&ctx, reinterpret_cast<const uint8_t*>(s.data()), s.size());
  mbedtls_md5_finish(&ctx, d);
  mbedtls_md5_free(&ctx);
  char hex[33];
  for (int i = 0; i < 16; i++) sprintf(hex + i * 2, "%02X", d[i]);
  return std::string(hex, 32);
}

static std::string sha1B64(const std::string& s) {
  uint8_t d[20];
  mbedtls_sha1_context ctx;
  mbedtls_sha1_init(&ctx);
  mbedtls_sha1_starts(&ctx);
  mbedtls_sha1_update(&ctx, reinterpret_cast<const uint8_t*>(s.data()), s.size());
  mbedtls_sha1_finish(&ctx, d);
  mbedtls_sha1_free(&ctx);
  return b64Encode(d, sizeof(d));
}

// signedNonce = b64(sha256(b64dec(ssecurity) || b64dec(nonce)))
static std::string signedNonce(const std::string& ssecurity, const std::string& nonce) {
  const auto sec = b64Decode(ssecurity);
  const auto non = b64Decode(nonce);
  uint8_t d[32];
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);
  mbedtls_sha256_update(&ctx, sec.data(), sec.size());
  mbedtls_sha256_update(&ctx, non.data(), non.size());
  mbedtls_sha256_finish(&ctx, d);
  mbedtls_sha256_free(&ctx);
  return b64Encode(d, sizeof(d));
}

// RC4 with the 1024-byte warm-up Xiaomi uses.
namespace {
struct Rc4 {
  uint8_t S[256];
  int i = 0, j = 0;

  void init(const uint8_t* key, size_t klen) {
    for (int k = 0; k < 256; k++) S[k] = static_cast<uint8_t>(k);
    int jj = 0;
    for (int k = 0; k < 256; k++) {
      jj = (jj + S[k] + key[k % klen]) & 0xff;
      std::swap(S[k], S[jj]);
    }
    i = j = 0;
    uint8_t skip[128] = {};
    for (int n = 0; n < 8; n++) crypt(skip, sizeof(skip));  // discard 1024 bytes
  }

  void crypt(uint8_t* buf, size_t n) {
    for (size_t k = 0; k < n; k++) {
      i = (i + 1) & 0xff;
      j = (j + S[i]) & 0xff;
      std::swap(S[i], S[j]);
      buf[k] ^= S[(S[i] + S[j]) & 0xff];
    }
  }
};
}  // namespace

static std::string rc4EncryptB64(const std::string& keyB64, const std::string& plain) {
  const auto key = b64Decode(keyB64);
  if (key.empty()) return {};
  Rc4 rc4;
  rc4.init(key.data(), key.size());
  std::string buf = plain;
  rc4.crypt(reinterpret_cast<uint8_t*>(buf.data()), buf.size());
  return b64Encode(reinterpret_cast<const uint8_t*>(buf.data()), buf.size());
}

static std::string rc4DecryptB64(const std::string& keyB64, const std::string& cipherB64) {
  const auto key = b64Decode(keyB64);
  auto data = b64Decode(cipherB64);
  if (key.empty() || data.empty()) return {};
  Rc4 rc4;
  rc4.init(key.data(), key.size());
  rc4.crypt(data.data(), data.size());
  return std::string(reinterpret_cast<char*>(data.data()), data.size());
}

// Percent-encode everything that is not unreserved, '%' included: this mirrors what
// requests does in the reference implementations, and the signature is computed on the
// raw values anyway.
static std::string urlEncode(const std::string& s) {
  static const char* hex = "0123456789ABCDEF";
  std::string out;
  out.reserve(s.size() * 3);
  for (unsigned char c : s) {
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(hex[c >> 4]);
      out.push_back(hex[c & 0xf]);
    }
  }
  return out;
}

// Xiaomi prefixes its JSON with a XSSI guard.
static std::string stripGuard(const std::string& body) {
  const size_t p = body.find('{');
  return p == std::string::npos ? body : body.substr(p);
}

// ---------- HTTP with a tiny cookie jar ----------

namespace {
struct HttpCtx {
  std::string body;
  std::map<std::string, std::string>* jar;
  bool overflow = false;
};

esp_err_t httpEvent(esp_http_client_event_t* evt) {
  auto* ctx = static_cast<HttpCtx*>(evt->user_data);
  if (!ctx) return ESP_OK;
  if (evt->event_id == HTTP_EVENT_ON_HEADER) {
    if (evt->header_key && strcasecmp(evt->header_key, "Set-Cookie") == 0 && evt->header_value) {
      const std::string sc = evt->header_value;
      const size_t eq = sc.find('=');
      const size_t end = sc.find(';');
      if (eq != std::string::npos) {
        const std::string name = sc.substr(0, eq);
        const std::string value = sc.substr(eq + 1, (end == std::string::npos ? sc.size() : end) - eq - 1);
        if (!value.empty() && value != "EXPIRED" && value != "\"\"") (*ctx->jar)[name] = value;
      }
    }
  } else if (evt->event_id == HTTP_EVENT_ON_DATA) {
    if (ctx->body.size() + evt->data_len > MAX_BODY) { ctx->overflow = true; return ESP_FAIL; }
    ctx->body.append(static_cast<const char*>(evt->data), evt->data_len);
  }
  return ESP_OK;
}
}  // namespace

static std::string cookieHeader(const std::map<std::string, std::string>& jar,
                                const std::map<std::string, std::string>& extra) {
  std::string out;
  for (const auto& kv : jar) {
    if (extra.count(kv.first)) continue;
    if (!out.empty()) out += "; ";
    out += kv.first + "=" + kv.second;
  }
  for (const auto& kv : extra) {
    if (!out.empty()) out += "; ";
    out += kv.first + "=" + kv.second;
  }
  return out;
}

static bool httpDo(const std::string& url, esp_http_client_method_t method,
                   const std::string& agent, const std::string& cookies,
                   std::map<std::string, std::string>& jar,
                   std::string& body, std::string& err) {
  HttpCtx ctx{.jar = &jar};
  esp_http_client_config_t cfg = {};
  cfg.url = url.c_str();
  cfg.method = method;
  cfg.event_handler = httpEvent;
  cfg.user_data = &ctx;
  cfg.crt_bundle_attach = esp_crt_bundle_attach;
  cfg.timeout_ms = 15000;
  cfg.buffer_size = 2048;
  cfg.buffer_size_tx = 2048;

  esp_http_client_handle_t client = esp_http_client_init(&cfg);
  if (!client) { err = "http init failed"; return false; }
  esp_http_client_set_header(client, "User-Agent", agent.c_str());
  esp_http_client_set_header(client, "Content-Type", "application/x-www-form-urlencoded");
  if (!cookies.empty()) esp_http_client_set_header(client, "Cookie", cookies.c_str());
  if (method == HTTP_METHOD_POST) esp_http_client_set_post_field(client, "", 0);

  const esp_err_t rc = esp_http_client_perform(client);
  const int status = esp_http_client_get_status_code(client);
  esp_http_client_cleanup(client);

  if (rc != ESP_OK) {
    err = ctx.overflow ? "response too large" : std::string("http error: ") + esp_err_to_name(rc);
    return false;
  }
  if (status != 200) { err = "HTTP " + std::to_string(status); return false; }
  body = std::move(ctx.body);
  return true;
}

// ---------- login flow ----------

bool XiaomiCloud::step1Sign(std::string& err) {
  char agent[96], devId[7];
  for (int i = 0; i < 6; i++) devId[i] = static_cast<char>('a' + esp_random() % 26);
  devId[6] = 0;
  char rnd[14];
  for (int i = 0; i < 13; i++) rnd[i] = static_cast<char>('A' + esp_random() % 5);
  rnd[13] = 0;
  snprintf(agent, sizeof(agent),
           "Android-7.1.1-1.0.0-ONEPLUS A3010-136-%s APP/xiaomi.smarthome APPV/62830", rnd);
  m_agent = agent;
  m_deviceId = devId;

  std::string body;
  const std::string cookies = "sdkVersion=accountsdk-18.8.15; deviceId=" + m_deviceId;
  if (!httpDo("https://account.xiaomi.com/pass/serviceLogin?sid=xiaomiio&_json=true",
              HTTP_METHOD_GET, m_agent, cookies, m_jar, body, err)) {
    return false;
  }
  cJSON* root = cJSON_Parse(stripGuard(body).c_str());
  if (!root) { err = "step 1: bad JSON"; return false; }
  const cJSON* sign = cJSON_GetObjectItemCaseSensitive(root, "_sign");
  const bool ok = cJSON_IsString(sign);
  if (ok) m_sign = sign->valuestring;
  cJSON_Delete(root);
  if (!ok) err = "step 1: no _sign in response";
  return ok;
}

bool XiaomiCloud::step2Auth(const std::string& user, const std::string& pass, std::string& err) {
  std::string url = "https://account.xiaomi.com/pass/serviceLoginAuth2";
  url += "?sid=xiaomiio";
  url += "&hash=" + urlEncode(md5Upper(pass));
  url += "&callback=" + urlEncode("https://sts.api.io.mi.com/sts");
  url += "&qs=" + urlEncode("%3Fsid%3Dxiaomiio%26_json%3Dtrue");
  url += "&user=" + urlEncode(user);
  url += "&_sign=" + urlEncode(m_sign);
  url += "&_json=true";

  std::string body;
  const std::string cookies = cookieHeader(m_jar, {{"sdkVersion", "accountsdk-18.8.15"},
                                                   {"deviceId", m_deviceId}});
  if (!httpDo(url, HTTP_METHOD_POST, m_agent, cookies, m_jar, body, err)) return false;

  cJSON* root = cJSON_Parse(stripGuard(body).c_str());
  if (!root) { err = "step 2: bad JSON"; return false; }

  const cJSON* ssec = cJSON_GetObjectItemCaseSensitive(root, "ssecurity");
  const cJSON* loc = cJSON_GetObjectItemCaseSensitive(root, "location");
  const cJSON* uid = cJSON_GetObjectItemCaseSensitive(root, "userId");
  const cJSON* notif = cJSON_GetObjectItemCaseSensitive(root, "notificationUrl");

  bool ok = false;
  if (cJSON_IsString(ssec) && strlen(ssec->valuestring) > 4 && cJSON_IsString(loc)) {
    m_ssecurity = ssec->valuestring;
    m_location = loc->valuestring;
    if (cJSON_IsNumber(uid)) m_userId = std::to_string(static_cast<long long>(uid->valuedouble));
    else if (cJSON_IsString(uid)) m_userId = uid->valuestring;
    ok = true;
  } else if (cJSON_IsString(notif)) {
    m_2faUrl = notif->valuestring;
    err = "Two-factor verification required";
  } else {
    const cJSON* desc = cJSON_GetObjectItemCaseSensitive(root, "desc");
    err = cJSON_IsString(desc) ? std::string("step 2: ") + desc->valuestring
                               : "step 2: wrong username or password";
  }
  cJSON_Delete(root);
  return ok;
}

bool XiaomiCloud::step3ServiceToken(std::string& err) {
  std::string body;
  const std::string cookies = cookieHeader(m_jar, {});
  if (!httpDo(m_location, HTTP_METHOD_GET, m_agent, cookies, m_jar, body, err)) return false;
  const auto it = m_jar.find("serviceToken");
  if (it == m_jar.end()) { err = "step 3: no serviceToken"; return false; }
  m_serviceToken = it->second;
  return true;
}

bool XiaomiCloud::deviceList(const std::string& region, std::vector<XiaomiDevice>& out,
                             std::string& err) {
  const std::string host =
      "https://" + (region.empty() || region == "cn" ? "" : region + ".") + "api.io.mi.com/app";
  const std::string path = "/home/device_list";
  const std::string data = R"({"getVirtualModel":false,"getHuamiDevices":0})";

  // nonce = b64(8 random bytes || minutes-since-epoch as big-endian uint32)
  uint8_t nonceBytes[12];
  esp_fill_random(nonceBytes, 8);
  const uint32_t minutes = static_cast<uint32_t>(time(nullptr) / 60);
  nonceBytes[8] = minutes >> 24; nonceBytes[9] = minutes >> 16;
  nonceBytes[10] = minutes >> 8; nonceBytes[11] = minutes;
  const std::string nonce = b64Encode(nonceBytes, sizeof(nonceBytes));
  const std::string sn = signedNonce(m_ssecurity, nonce);

  // Signatures are taken over the values in order, before and after RC4.
  const std::string rc4Hash = sha1B64("POST&" + path + "&data=" + data + "&" + sn);
  const std::string encData = rc4EncryptB64(sn, data);
  const std::string encHash = rc4EncryptB64(sn, rc4Hash);
  const std::string signature =
      sha1B64("POST&" + path + "&data=" + encData + "&rc4_hash__=" + encHash + "&" + sn);

  std::string url = host + path;
  url += "?data=" + urlEncode(encData);
  url += "&rc4_hash__=" + urlEncode(encHash);
  url += "&signature=" + urlEncode(signature);
  url += "&ssecurity=" + urlEncode(m_ssecurity);
  url += "&_nonce=" + urlEncode(nonce);

  const std::string cookies = cookieHeader({}, {{"userId", m_userId},
                                                {"serviceToken", m_serviceToken},
                                                {"yetAnotherServiceToken", m_serviceToken},
                                                {"locale", "en_GB"},
                                                {"channel", "MI_APP_STORE"}});
  std::string body;
  if (!httpDo(url, HTTP_METHOD_POST, m_agent, cookies, m_jar, body, err)) return false;

  const std::string json = rc4DecryptB64(sn, body);
  if (json.empty()) { err = "device list: decrypt failed"; return false; }

  cJSON* root = cJSON_Parse(json.c_str());
  if (!root) { err = "device list: bad JSON"; return false; }
  const cJSON* list = cJSON_GetObjectItem(cJSON_GetObjectItem(root, "result"), "list");
  if (!cJSON_IsArray(list)) {
    const cJSON* msg = cJSON_GetObjectItemCaseSensitive(root, "message");
    err = cJSON_IsString(msg) ? std::string("device list: ") + msg->valuestring
                              : "device list: unexpected response";
    cJSON_Delete(root);
    return false;
  }

  const cJSON* dev = nullptr;
  cJSON_ArrayForEach(dev, list) {
    XiaomiDevice d;
    const cJSON* v;
    if ((v = cJSON_GetObjectItem(dev, "name")) && cJSON_IsString(v)) d.name = v->valuestring;
    if ((v = cJSON_GetObjectItem(dev, "model")) && cJSON_IsString(v)) d.model = v->valuestring;
    if ((v = cJSON_GetObjectItem(dev, "did")) && cJSON_IsString(v)) d.did = v->valuestring;
    if ((v = cJSON_GetObjectItem(dev, "token")) && cJSON_IsString(v)) d.token = v->valuestring;
    if ((v = cJSON_GetObjectItem(dev, "localip")) && cJSON_IsString(v)) d.ip = v->valuestring;
    if ((v = cJSON_GetObjectItem(dev, "isOnline")) && cJSON_IsBool(v)) d.online = cJSON_IsTrue(v);
    if (!d.did.empty()) out.push_back(std::move(d));
  }
  cJSON_Delete(root);
  ESP_LOGI(TAG, "Fetched %d devices", static_cast<int>(out.size()));
  return true;
}

bool XiaomiCloud::loginAndListDevices(const std::string& user, const std::string& pass,
                                      const std::string& region,
                                      std::vector<XiaomiDevice>& out, std::string& err) {
  m_2faUrl.clear();
  m_jar.clear();
  return step1Sign(err) && step2Auth(user, pass, err) && step3ServiceToken(err) &&
         deviceList(region, out, err);
}
