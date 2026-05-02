#include "time_sync.h"

#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_err.h"
#include <time.h>
#include <stdlib.h>

static const char *TAG = "time_sync";
static bool s_started = false;

bool time_sync_is_valid(void)
{
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    return (tm_now.tm_year + 1900) >= 2025;
}

void time_sync_start(void)
{
    if (s_started) {
        ESP_LOGW(TAG, "SNTP already started");
        return;
    }

    setenv("TZ", "ICT-7", 1);
    tzset();

    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    cfg.start = true;
    cfg.server_from_dhcp = false;

    esp_err_t err = esp_netif_sntp_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_sntp_init failed: %s", esp_err_to_name(err));
        return;
    }

    s_started = true;
    ESP_LOGW(TAG, "SNTP started");
}