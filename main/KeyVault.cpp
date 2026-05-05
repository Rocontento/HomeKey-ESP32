#include "KeyVault.hpp"
#include "esp_log.h"
#include "esp_efuse.h"
#include "esp_hmac.h"
#include "esp_random.h"
#include "mbedtls/gcm.h"
#include <array>
#include <cstring>

static const char* TAG = "KeyVault";

static constexpr esp_efuse_block_t KEYVAULT_EFUSE_BLOCK = EFUSE_BLK_KEY0;
static constexpr hmac_key_id_t     KEYVAULT_HMAC_KEY    = HMAC_KEY0;

// Domain-separation label for KEK derivation.
static constexpr uint8_t KEK_LABEL[] = "HomeKey-Reader-KEK-v1";

static bool deriveKek(std::array<uint8_t, 32>& kek) {
    esp_err_t ret = esp_hmac_calculate(KEYVAULT_HMAC_KEY,
                                       KEK_LABEL, sizeof(KEK_LABEL) - 1,
                                       kek.data());
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "HMAC key derivation failed: %s", esp_err_to_name(ret));
        return false;
    }
    return true;
}

namespace KeyVault {

bool isProvisioned() {
    esp_efuse_purpose_t purpose = esp_efuse_get_key_purpose(KEYVAULT_EFUSE_BLOCK);
    return purpose == ESP_EFUSE_KEY_PURPOSE_HMAC_UP;
}

esp_err_t provision() {
    if (isProvisioned()) {
        ESP_LOGI(TAG, "eFuse HMAC key already provisioned in block %d", KEYVAULT_EFUSE_BLOCK);
        return ESP_OK;
    }

    uint8_t key[32];
    esp_fill_random(key, sizeof(key));
    esp_err_t ret = esp_efuse_write_key(KEYVAULT_EFUSE_BLOCK,
                                        ESP_EFUSE_KEY_PURPOSE_HMAC_UP,
                                        key, sizeof(key));
    memset(key, 0, sizeof(key));

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "eFuse write failed: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "HMAC key burned to eFuse block %d", KEYVAULT_EFUSE_BLOCK);
    }
    return ret;
}

bool wrap(const uint8_t* plain, size_t len, std::vector<uint8_t>& out) {
    if (!isProvisioned()) {
        ESP_LOGE(TAG, "wrap: eFuse key not provisioned");
        return false;
    }

    std::array<uint8_t, 32> kek;
    if (!deriveKek(kek)) return false;

    constexpr size_t IV_LEN  = 12;
    constexpr size_t TAG_LEN = 16;
    out.resize(IV_LEN + len + TAG_LEN);
    esp_fill_random(out.data(), IV_LEN);

    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    int rc = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, kek.data(), 256);
    if (rc == 0) {
        rc = mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT,
                                        len,
                                        out.data(), IV_LEN,
                                        nullptr, 0,
                                        plain,
                                        out.data() + IV_LEN,
                                        TAG_LEN,
                                        out.data() + IV_LEN + len);
    }
    mbedtls_gcm_free(&gcm);
    memset(kek.data(), 0, kek.size());

    if (rc != 0) {
        ESP_LOGE(TAG, "AES-GCM encrypt failed: %d", rc);
        out.clear();
        return false;
    }
    return true;
}

bool unwrap(const uint8_t* wrapped, size_t len, std::vector<uint8_t>& out) {
    constexpr size_t IV_LEN  = 12;
    constexpr size_t TAG_LEN = 16;

    if (len < IV_LEN + TAG_LEN) {
        ESP_LOGE(TAG, "unwrap: data too short (%zu bytes)", len);
        return false;
    }
    if (!isProvisioned()) {
        ESP_LOGE(TAG, "unwrap: eFuse key not provisioned");
        return false;
    }

    std::array<uint8_t, 32> kek;
    if (!deriveKek(kek)) return false;

    size_t plainlen = len - IV_LEN - TAG_LEN;
    out.resize(plainlen);

    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    int rc = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, kek.data(), 256);
    if (rc == 0) {
        rc = mbedtls_gcm_auth_decrypt(&gcm, plainlen,
                                       wrapped, IV_LEN,
                                       nullptr, 0,
                                       wrapped + IV_LEN + plainlen, TAG_LEN,
                                       wrapped + IV_LEN,
                                       out.data());
    }
    mbedtls_gcm_free(&gcm);
    memset(kek.data(), 0, kek.size());

    if (rc != 0) {
        ESP_LOGE(TAG, "AES-GCM auth/decrypt failed: %d (wrong chip or corrupted data)", rc);
        out.clear();
        return false;
    }
    return true;
}

} // namespace KeyVault
