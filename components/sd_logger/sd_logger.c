#include "sd_logger.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "app_config.h"
#include "app_logic.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_vfs_fat.h"

#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "sd_logger";

#define SD_MOUNT_POINT      "/sdcard"
#define SD_LOG_DIR          "/sdcard/LOG"

static sdmmc_card_t *s_card = NULL;
static bool s_mounted = false;
static bool s_det_inited = false;
static TaskHandle_t s_log_task_handle = NULL;

static void make_display_time_string(char *buf, size_t len)
{
    time_t now = time(NULL);
    struct tm tm_now = {0};

    localtime_r(&now, &tm_now);

    if ((tm_now.tm_year + 1900) < 2025) {
        snprintf(buf, len, "UNKNOWN");
        return;
    }

    snprintf(buf,
             len,
             "%02dh%02d - %02d/%02d/%04d",
             tm_now.tm_hour,
             tm_now.tm_min,
             tm_now.tm_mday,
             tm_now.tm_mon + 1,
             tm_now.tm_year + 1900);
}

static esp_err_t sd_det_init(void)
{
    if (s_det_inited) {
        return ESP_OK;
    }

    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << APP_SD_DET_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t err = gpio_config(&io_conf);
    if (err == ESP_OK) {
        s_det_inited = true;
    }

    return err;
}

bool sd_logger_card_inserted(void)
{
    if (!s_det_inited) {
        sd_det_init();
    }

    return gpio_get_level(APP_SD_DET_GPIO) == APP_SD_DET_ACTIVE_LEVEL;
}

bool sd_logger_is_mounted(void)
{
    return s_mounted;
}

static bool file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

static void make_time_string(char *buf, size_t len)
{
    time_t now = time(NULL);
    struct tm tm_now;

    localtime_r(&now, &tm_now);

    if (tm_now.tm_year < (2020 - 1900)) {
        snprintf(buf, len, "1970-01-01 00:00:00");
        return;
    }

    strftime(buf, len, "%Y-%m-%d %H:%M:%S", &tm_now);
}

static void make_log_file_path(char *buf, size_t len)
{
    time_t now = time(NULL);
    struct tm tm_now;

    localtime_r(&now, &tm_now);

    if (tm_now.tm_year < (2020 - 1900)) {
        snprintf(buf, len, SD_LOG_DIR "/UNKNOWN.CSV");
        return;
    }

    /*
     * Tên file chuẩn 8.3:
     * 260425.CSV = ngày 25/04/2026
     */
    snprintf(buf,
             len,
             SD_LOG_DIR "/%02d%02d%02d.CSV",
             (tm_now.tm_year + 1900) % 100,
             tm_now.tm_mon + 1,
             tm_now.tm_mday);
}

esp_err_t sd_logger_init(void)
{
#if !APP_SDMMC_ENABLE
    ESP_LOGW(TAG, "SDMMC logger disabled");
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (s_mounted) {
        return ESP_OK;
    }

    esp_err_t err = sd_det_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SD DET init failed: %s", esp_err_to_name(err));
        return err;
    }

    if (!sd_logger_card_inserted()) {
        ESP_LOGW(TAG, "No SD card inserted");
        return ESP_ERR_NOT_FOUND;
    }

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 4,
        .allocation_unit_size = 4096,
    };
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();

    host.max_freq_khz = 400;

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();

    slot_config.clk = APP_SDMMC_CLK_GPIO;
    slot_config.cmd = APP_SDMMC_CMD_GPIO;
    slot_config.d0  = APP_SDMMC_D0_GPIO;

#if APP_SDMMC_BUS_WIDTH == 4
    slot_config.width = 4;
    slot_config.d1 = APP_SDMMC_D1_GPIO;
    slot_config.d2 = APP_SDMMC_D2_GPIO;
    slot_config.d3 = APP_SDMMC_D3_GPIO;
#else
    slot_config.width = 1;
#endif

    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    ESP_LOGI(TAG, "Mounting SD card...");

    err = esp_vfs_fat_sdmmc_mount(
        SD_MOUNT_POINT,
        &host,
        &slot_config,
        &mount_config,
        &s_card
    );

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Mount SD failed: %s", esp_err_to_name(err));
        s_card = NULL;
        s_mounted = false;
        return err;
    }

    s_mounted = true;

    sdmmc_card_print_info(stdout, s_card);

    mkdir(SD_LOG_DIR, 0775);

    ESP_LOGI(TAG, "SD card mounted OK");
    return ESP_OK;
#endif
}

esp_err_t sd_logger_deinit(void)
{
    if (!s_mounted) {
        return ESP_OK;
    }

    esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, s_card);

    s_card = NULL;
    s_mounted = false;

    ESP_LOGW(TAG, "SD card unmounted");
    return ESP_OK;
}

static esp_err_t sd_logger_append_snapshot(const app_logic_log_snapshot_t *snap)
{
    if (!snap) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_mounted) {
        return ESP_ERR_INVALID_STATE;
    }

    char path[96];
    char time_str[32];

    make_log_file_path(path, sizeof(path));
    make_display_time_string(time_str, sizeof(time_str));

    bool need_header = !file_exists(path);

    FILE *f = fopen(path, "a");
    if (!f) {
        ESP_LOGE(TAG, "Open log file failed: %s", path);
        return ESP_FAIL;
    }

    /*
     * Format nội dung:
     * ID Thiết bị; Nhiệt độ hiện tại (NTC); Độ ẩm hiện tại (SHT); Thời gian hiện tại
     *
     * Yêu cầu:
     * - HUM_OFF: lưu độ ẩm = 0.0%
     * - SHT lỗi nhưng HUM_ON: lưu giá trị độ ẩm cuối cùng đọc được
     * - NTC lỗi: lưu giá trị nhiệt độ cuối cùng đọc được
     */
    if (need_header) {
        fprintf(f,
                "ID Thiet bi;"
                "Nhiet do hien tai (NTC);"
                "Do am hien tai (SHT);"
                "Thoi gian hien tai\n");
    }

    /*
     * Nhiệt độ NTC:
     * Lấy snap->temp1 làm NTC chính.
     * Nếu cảm biến lỗi, snap->temp1 vẫn phải là giá trị cuối cùng đọc được
     * do app_logic_get_log_snapshot() lấy từ s_last_temp1_c.
     */
    float ntc_value = snap->temp1;

    /*
     * Độ ẩm SHT:
     * - Nếu HUM_OFF: ghi 0.0
     * - Nếu HUM_ON: ghi snap->humidity
     *
     * Khi cảm biến SHT lỗi, snap->humidity vẫn phải là giá trị cuối cùng đọc được
     * do app_logic_get_log_snapshot() lấy từ s_last_humidity.
     */
    float hum_value = snap->hum_enabled ? snap->humidity : 0.0f;

    fprintf(f,
            "%s;%.2f;%.1f%%;%s\n",
            snap->device_id,
            ntc_value,
            hum_value,
            time_str);

    fflush(f);
    fclose(f);

    ESP_LOGI(TAG,
             "Log saved: %s id=%s ntc=%.2f hum=%.1f hum_en=%d time=%s",
             path,
             snap->device_id,
             ntc_value,
             hum_value,
             snap->hum_enabled ? 1 : 0,
             time_str);

    return ESP_OK;
}

static void sd_logger_task(void *arg)
{

    vTaskDelay(pdMS_TO_TICKS(5000));

    while (1) {
        if (!sd_logger_card_inserted()) {
            ESP_LOGW(TAG, "SD card not inserted");

            if (sd_logger_is_mounted()) {
                sd_logger_deinit();
            }

            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        if (!sd_logger_is_mounted()) {
            esp_err_t err = sd_logger_init();
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "SD mount failed, retry later: %s", esp_err_to_name(err));
                vTaskDelay(pdMS_TO_TICKS(10000));
                continue;
            }
        }

        app_logic_log_snapshot_t snap = {0};

        esp_err_t err = app_logic_get_log_snapshot(&snap);
        if (err == ESP_OK) {
            err = sd_logger_append_snapshot(&snap);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Append SD log failed: %s", esp_err_to_name(err));
            }
        } else {
            ESP_LOGW(TAG, "Get app snapshot failed: %s", esp_err_to_name(err));
        }

        vTaskDelay(pdMS_TO_TICKS(APP_SD_LOG_INTERVAL_MS));
    }
}

esp_err_t sd_logger_start(void)
{
#if !APP_SDMMC_ENABLE
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (s_log_task_handle) {
        return ESP_OK;
    }

    esp_err_t err = sd_det_init();
    if (err != ESP_OK) {
        return err;
    }

    BaseType_t ok = xTaskCreate(
        sd_logger_task,
        "sd_logger",
        4096,
        NULL,
        4,
        &s_log_task_handle
    );

    if (ok != pdPASS) {
        s_log_task_handle = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "SD logger task started");
    return ESP_OK;
#endif
}