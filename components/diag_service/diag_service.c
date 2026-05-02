#include "diag_service.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs.h"
#include <stdio.h>
#include <string.h>
#include "app_config.h"
#define DIAG_NS "diag"
#define KEY_BOOT_COUNT "boot_cnt"
#define KEY_UNCLEAN "unclean"
#define KEY_CLEAN "clean"

static const char *TAG = "diag";

typedef struct {
    uint32_t sync_fail;
    uint32_t ppp_restart;
    uint32_t mqtt_restart;
    uint32_t modem_power_cycle;
    uint32_t boot_count;
    uint32_t unclean_reset_count;
    int64_t boot_us;
    esp_reset_reason_t reset_reason;
} diag_state_t;

static diag_state_t s_diag;

static void persist_counter(const char *key, uint32_t value)
{
    nvs_handle_t nvs;
    if (nvs_open(DIAG_NS, NVS_READWRITE, &nvs) == ESP_OK) {
        if (nvs_set_u32(nvs, key, value) == ESP_OK) {
            nvs_commit(nvs);
        }
        nvs_close(nvs);
    }
}

esp_err_t diag_service_init(void)
{
    nvs_handle_t nvs;
    s_diag.boot_us = esp_timer_get_time();
    s_diag.reset_reason = esp_reset_reason();

    if (nvs_open(DIAG_NS, NVS_READWRITE, &nvs) == ESP_OK) {
        uint32_t boot_count = 0;
        uint32_t clean = 1;
        uint32_t unclean = 0;
        nvs_get_u32(nvs, KEY_BOOT_COUNT, &boot_count);
        nvs_get_u32(nvs, KEY_CLEAN, &clean);
        nvs_get_u32(nvs, KEY_UNCLEAN, &unclean);
        boot_count++;
        if (clean == 0) {
            unclean++;
        }
        s_diag.boot_count = boot_count;
        s_diag.unclean_reset_count = unclean;
        nvs_set_u32(nvs, KEY_BOOT_COUNT, boot_count);
        nvs_set_u32(nvs, KEY_UNCLEAN, unclean);
        nvs_set_u32(nvs, KEY_CLEAN, 0);
        nvs_commit(nvs);
        nvs_close(nvs);
    }

    ESP_LOGI(TAG, "boot reset_reason=%d boot_count=%lu unclean=%lu",
             (int)s_diag.reset_reason,
             (unsigned long)s_diag.boot_count,
             (unsigned long)s_diag.unclean_reset_count);
    return ESP_OK;
}

void diag_mark_clean_shutdown(void)
{
    persist_counter(KEY_CLEAN, 1);
}

void diag_inc_sync_fail(void) { s_diag.sync_fail++; }
void diag_inc_ppp_restart(void) { s_diag.ppp_restart++; }
void diag_inc_mqtt_restart(void) { s_diag.mqtt_restart++; }
void diag_inc_modem_power_cycle(void) { s_diag.modem_power_cycle++; }

void diag_get_snapshot(diag_snapshot_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->boot_count = s_diag.boot_count;
    out->unclean_reset_count = s_diag.unclean_reset_count;
    out->reset_reason = s_diag.reset_reason;
    out->sync_fail = s_diag.sync_fail;
    out->ppp_restart = s_diag.ppp_restart;
    out->mqtt_restart = s_diag.mqtt_restart;
    out->modem_power_cycle = s_diag.modem_power_cycle;
}

int diag_build_attributes_json(char *buf, size_t len)
{
    if (!buf || len == 0) return -1;
    return snprintf(buf, len,
                    "{\"fw\":\"%s\",\"boot_count\":%lu,\"unclean_reset_count\":%lu,\"reset_reason\":%d,\"sync_fail\":%lu,\"ppp_restart\":%lu,\"mqtt_restart\":%lu,\"modem_power_cycle\":%lu}",
                    APP_FW_VERSION,
                    (unsigned long)s_diag.boot_count,
                    (unsigned long)s_diag.unclean_reset_count,
                    (int)s_diag.reset_reason,
                    (unsigned long)s_diag.sync_fail,
                    (unsigned long)s_diag.ppp_restart,
                    (unsigned long)s_diag.mqtt_restart,
                    (unsigned long)s_diag.modem_power_cycle);
}

void diag_log_health(const char *reason, bool ip_ready, bool mqtt_ready, int outbox_size)
{
    ESP_LOGI(TAG,
             "health reason=%s uptime_s=%lld heap_free=%u heap_min=%u ip=%d mqtt=%d outbox=%d sync_fail=%lu ppp_restart=%lu mqtt_restart=%lu power_cycle=%lu reset_reason=%d boot_count=%lu unclean=%lu",
             reason ? reason : "na",
             (long long)((esp_timer_get_time() - s_diag.boot_us) / 1000000LL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT),
             ip_ready,
             mqtt_ready,
             outbox_size,
             (unsigned long)s_diag.sync_fail,
             (unsigned long)s_diag.ppp_restart,
             (unsigned long)s_diag.mqtt_restart,
             (unsigned long)s_diag.modem_power_cycle,
             (int)s_diag.reset_reason,
             (unsigned long)s_diag.boot_count,
             (unsigned long)s_diag.unclean_reset_count);
}
