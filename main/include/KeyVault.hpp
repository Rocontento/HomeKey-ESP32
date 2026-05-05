#pragma once
#include <vector>
#include <cstdint>
#include "esp_err.h"

// KeyVault: wraps/unwraps sensitive key material using a KEK derived from an
// eFuse-resident HMAC key (HMAC_UP purpose, KEY_BLOCK_0). AES-256-GCM is used
// for authenticated encryption. The eFuse key must be provisioned once via
// KeyVault::provision() before wrap/unwrap will succeed.
//
// Guarded by CONFIG_HK_ENABLE_KEYVAULT (default n). When disabled, ReaderData
// stores keys as plaintext (legacy behaviour, no regression).
namespace KeyVault {

// Returns true if eFuse KEY_BLOCK_0 is programmed with HMAC_UP purpose.
bool isProvisioned();

// Burns 32 random bytes as HMAC_UP key to eFuse KEY_BLOCK_0. IRREVERSIBLE.
// Returns ESP_OK if already provisioned or newly burned. Intended to be called
// once explicitly on a provisioning device; never called automatically.
esp_err_t provision();

// Encrypt `plain` (len bytes) into `out`. Layout: IV(12)|CT(len)|TAG(16).
// Returns false if eFuse key is not provisioned or crypto fails.
bool wrap(const uint8_t* plain, size_t len, std::vector<uint8_t>& out);

// Decrypt `wrapped` (IV+CT+TAG) into `out`. Returns false if auth fails or
// eFuse key is not provisioned (signals wrong chip or data corruption).
bool unwrap(const uint8_t* wrapped, size_t len, std::vector<uint8_t>& out);

} // namespace KeyVault
