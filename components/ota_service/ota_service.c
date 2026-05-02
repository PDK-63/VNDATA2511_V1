#include "ota_service.h"
#include "app_config.h"

#include "esp_crt_bundle.h"
#include "esp_https_ota.h"
#include "esp_log.h"

static const char *TAG = "ota_service";

esp_err_t ota_service_start(const char *url)
{
    const char *ota_url = url;

    if (!ota_url || ota_url[0] == '\0') {
        ota_url = APP_OTA_URL_DEFAULT;
    }

    if (!ota_url || ota_url[0] == '\0') {
        ESP_LOGE(TAG, "OTA URL is empty");
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Starting OTA from: %s", ota_url);

    esp_http_client_config_t http_cfg = {
        .url = ota_url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
        .keep_alive_enable = true,
    };

    esp_https_ota_config_t ota_cfg = {
        .http_config = &http_cfg,
    };

    esp_err_t err = esp_https_ota(&ota_cfg);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "OTA success, rebooting...");
        return ESP_OK;
    } else {
        ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(err));
        return err;
    }
}