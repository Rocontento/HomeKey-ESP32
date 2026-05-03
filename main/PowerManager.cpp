#include "PowerManager.hpp"
#include "esp_log.h"

static const char* TAG = "PowerManager";

void PowerManager::begin() {
    esp_pm_config_t cfg = {
        .max_freq_mhz = 160,
        .min_freq_mhz = 40,
        .light_sleep_enable = true,
    };
    esp_err_t ret = esp_pm_configure(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_pm_configure failed: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "DFS configured: 40-160 MHz, light sleep enabled");
    }
}
