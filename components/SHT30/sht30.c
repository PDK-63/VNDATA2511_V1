
#include "sht30.h"

static const char *TAG_SHT30 = "SHT30";

static TickType_t sht30_single_shot_delay_ticks(sht30_clock_stretching_t clock,
                                                sht30_repeatability_t repeatability)
{
    if (clock == ClockStretching_Enable) {
        return 0;
    }

    uint32_t delay_ms = 20;
    switch (repeatability) {
        case Repeatability_High:
            delay_ms = 20;
            break;
        case Repeatability_Medium:
            delay_ms = 8;
            break;
        case Repeatability_Low:
            delay_ms = 5;
            break;
        default:
            delay_ms = 20;
            break;
    }

    return pdMS_TO_TICKS(delay_ms);
}

static sht30_status_t sht30_check_crc(sht30_t *sht30, const uint8_t *buf)
{
    uint8_t crc_temp = sht30_calculate_crc(sht30, (uint8_t *)&buf[0]);
    if (crc_temp != buf[2]) {
        ESP_LOGE(TAG_SHT30, "Temperature CRC invalid.");
        return data_not_valid;
    }

    uint8_t crc_hum = sht30_calculate_crc(sht30, (uint8_t *)&buf[3]);
    if (crc_hum != buf[5]) {
        ESP_LOGE(TAG_SHT30, "Humidity CRC invalid.");
        return data_not_valid;
    }

    return ok;
}

static void sht30_store_measurement(sht30_t *sht30, const uint8_t *buf)
{
    sht30->temperature = ((uint16_t)buf[0] << 8) | buf[1];
    sht30->humidity    = ((uint16_t)buf[3] << 8) | buf[4];
}

sht30_status_t sht30_init(sht30_t *sht30,
                          i2c_port_t _i2c_port,
                          gpio_num_t _scl_io_num,
                          gpio_num_t _sda_io_num,
                          uint8_t _device_address,
                          uint32_t _scl_speed_hz,
                          uint32_t _scl_wait_us)
{
    if (sht30 == NULL) {
        return init_error;
    }

    memset(sht30, 0, sizeof(*sht30));

    sht30->port = _i2c_port;
    sht30->scl_io_num = _scl_io_num;
    sht30->sda_io_num = _sda_io_num;
    sht30->device_address = _device_address;
    sht30->scl_speed_hz = _scl_speed_hz;
    sht30->scl_wait_us = _scl_wait_us;

    i2c_config_t cfg = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = _sda_io_num,
        .scl_io_num = _scl_io_num,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = _scl_speed_hz,
        .clk_flags = 0,
    };

    esp_err_t err = i2c_param_config(_i2c_port, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_SHT30, "i2c_param_config failed: %s", esp_err_to_name(err));
        return init_error;
    }

    err = i2c_driver_install(_i2c_port, cfg.mode, 0, 0, 0);
    if (err == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG_SHT30, "I2C driver already installed on port %d", _i2c_port);
        sht30->driver_installed = false;
        return ok;
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG_SHT30, "i2c_driver_install failed: %s", esp_err_to_name(err));
        return init_error;
    }

    sht30->driver_installed = true;
    return ok;
}

sht30_status_t sht30_deinit(sht30_t *sht30)
{
    if (sht30 == NULL) {
        return error;
    }

    if (sht30->driver_installed) {
        esp_err_t err = i2c_driver_delete(sht30->port);
        if (err != ESP_OK) {
            ESP_LOGE(TAG_SHT30, "i2c_driver_delete failed: %s", esp_err_to_name(err));
            return error;
        }
        sht30->driver_installed = false;
    }

    return ok;
}

sht30_status_t sht30_write(sht30_t *sht30, uint8_t *command)
{
    if (sht30 == NULL || command == NULL) {
        return error;
    }

    esp_err_t err = i2c_master_write_to_device(
        sht30->port,
        sht30->device_address,
        command,
        CommandLength,
        pdMS_TO_TICKS(100)
    );

    if (err != ESP_OK) {
        ESP_LOGE(TAG_SHT30, "I2C transmit failed: %s", esp_err_to_name(err));
        return error;
    }

    return ok;
}

sht30_status_t sht30_read(sht30_t *sht30, uint8_t *dataRec, size_t len)
{
    if (sht30 == NULL || dataRec == NULL || len == 0) {
        return error;
    }

    esp_err_t err = i2c_master_read_from_device(
        sht30->port,
        sht30->device_address,
        dataRec,
        len,
        pdMS_TO_TICKS(100)
    );

    if (err != ESP_OK) {
        ESP_LOGE(TAG_SHT30, "I2C receive failed: %s", esp_err_to_name(err));
        return error;
    }

    return ok;
}

sht30_status_t sht30_single_shot(sht30_t *sht30,
                                 sht30_repeatability_t repeatability,
                                 sht30_clock_stretching_t clock)
{
    if (sht30 == NULL) {
        return error;
    }

    uint8_t command[2];
    uint8_t returnData[6];

    switch (clock) {
        case ClockStretching_Enable:
            command[0] = SingleShot_CS;
            switch (repeatability) {
                case Repeatability_High:   command[1] = SingleShot_RH_CS;  break;
                case Repeatability_Medium: command[1] = SingleShot_RM_CS;  break;
                case Repeatability_Low:    command[1] = SingleShot_RL_CS;  break;
                default: return error;
            }
            break;

        case ClockStretching_Disable:
            command[0] = SingleShot_DCS;
            switch (repeatability) {
                case Repeatability_High:   command[1] = SingleShot_RH_DCS; break;
                case Repeatability_Medium: command[1] = SingleShot_RM_DCS; break;
                case Repeatability_Low:    command[1] = SingleShot_RL_DCS; break;
                default: return error;
            }
            break;

        default:
            return error;
    }

    if (sht30_write(sht30, command) != ok) {
        ESP_LOGE(TAG_SHT30, "Failed to send single-shot command.");
        return error;
    }

    TickType_t wait_ticks = sht30_single_shot_delay_ticks(clock, repeatability);
    if (wait_ticks > 0) {
        vTaskDelay(wait_ticks);
    }

    if (sht30_read(sht30, returnData, sizeof(returnData)) != ok) {
        ESP_LOGE(TAG_SHT30, "Failed to read single-shot measurement.");
        return error;
    }

    sht30_status_t crc_status = sht30_check_crc(sht30, returnData);
    if (crc_status != ok) {
        return crc_status;
    }

    sht30_store_measurement(sht30, returnData);
    return ok;
}

sht30_status_t sht30_periodic(sht30_t *sht30,
                              sht30_repeatability_t repeatability,
                              sht30_measurements_per_seconds_t mps)
{
    if (sht30 == NULL) {
        return error;
    }

    uint8_t command[2];

    switch (mps) {
        case MPS_05:
            command[0] = Periodic_05;
            switch (repeatability) {
                case Repeatability_High:   command[1] = Periodic_RH_05; break;
                case Repeatability_Medium: command[1] = Periodic_RM_05; break;
                case Repeatability_Low:    command[1] = Periodic_RL_05; break;
                default: return error;
            }
            break;

        case MPS_1:
            command[0] = Periodic_1;
            switch (repeatability) {
                case Repeatability_High:   command[1] = Periodic_RH_1; break;
                case Repeatability_Medium: command[1] = Periodic_RM_1; break;
                case Repeatability_Low:    command[1] = Periodic_RL_1; break;
                default: return error;
            }
            break;

        case MPS_2:
            command[0] = Periodic_2;
            switch (repeatability) {
                case Repeatability_High:   command[1] = Periodic_RH_2; break;
                case Repeatability_Medium: command[1] = Periodic_RM_2; break;
                case Repeatability_Low:    command[1] = Periodic_RL_2; break;
                default: return error;
            }
            break;

        case MPS_4:
            command[0] = Periodic_4;
            switch (repeatability) {
                case Repeatability_High:   command[1] = Periodic_RH_4; break;
                case Repeatability_Medium: command[1] = Periodic_RM_4; break;
                case Repeatability_Low:    command[1] = Periodic_RL_4; break;
                default: return error;
            }
            break;

        case MPS_10:
            command[0] = Periodic_10;
            switch (repeatability) {
                case Repeatability_High:   command[1] = Periodic_RH_10; break;
                case Repeatability_Medium: command[1] = Periodic_RM_10; break;
                case Repeatability_Low:    command[1] = Periodic_RL_10; break;
                default: return error;
            }
            break;

        default:
            return error;
    }

    if (sht30_write(sht30, command) != ok) {
        ESP_LOGE(TAG_SHT30, "Failed to start periodic mode.");
        return error;
    }

    return ok;
}

sht30_status_t sht30_fetch_data(sht30_t *sht30)
{
    if (sht30 == NULL) {
        return error;
    }

    uint8_t command[2] = {FetchCommand >> 8, FetchCommand & 0xFF};
    uint8_t returnData[6];

    if (sht30_write(sht30, command) != ok) {
        ESP_LOGE(TAG_SHT30, "Failed to send fetch command.");
        return error;
    }

    if (sht30_read(sht30, returnData, sizeof(returnData)) != ok) {
        ESP_LOGE(TAG_SHT30, "Failed to fetch measurement data.");
        return error;
    }

    sht30_status_t crc_status = sht30_check_crc(sht30, returnData);
    if (crc_status != ok) {
        return crc_status;
    }

    sht30_store_measurement(sht30, returnData);
    return ok;
}

sht30_status_t sht30_art(sht30_t *sht30)
{
    if (sht30 == NULL) {
        return error;
    }

    uint8_t command[2] = {ARTCommand >> 8, ARTCommand & 0xFF};
    sht30_status_t status = sht30_write(sht30, command);

    if (status != ok) {
        ESP_LOGE(TAG_SHT30, "Failed to send ART command.");
        return status;
    }

    return ok;
}

sht30_status_t sht30_break(sht30_t *sht30)
{
    if (sht30 == NULL) {
        return error;
    }

    uint8_t command[2] = {BreakCommand >> 8, BreakCommand & 0xFF};

    if (sht30_write(sht30, command) != ok) {
        ESP_LOGE(TAG_SHT30, "Failed to send break command.");
        return error;
    }

    return ok;
}

sht30_status_t sht30_soft_reset(sht30_t *sht30)
{
    if (sht30 == NULL) {
        return error;
    }

    uint8_t command[2] = {SoftResetCommand >> 8, SoftResetCommand & 0xFF};

    if (sht30_write(sht30, command) != ok) {
        ESP_LOGE(TAG_SHT30, "Failed to send soft reset command.");
        return error;
    }

    return ok;
}

sht30_status_t sht30_heater_control(sht30_t *sht30, sht30_heater_t control)
{
    if (sht30 == NULL) {
        return error;
    }

    uint8_t command[2] = {Heater, 0};

    switch (control) {
        case Heater_Disable:
            command[1] = HeaterDisable;
            break;
        case Heater_Enable:
            command[1] = HeaterEnable;
            break;
        default:
            return error;
    }

    if (sht30_write(sht30, command) != ok) {
        ESP_LOGE(TAG_SHT30, "Failed to set heater control.");
        return error;
    }

    return ok;
}

sht30_status_t sht30_read_status_register(sht30_t *sht30)
{
    if (sht30 == NULL) {
        return error;
    }

    uint8_t command[2] = {StatusRegister >> 8, StatusRegister & 0xFF};
    uint8_t returnData[3];

    if (sht30_write(sht30, command) != ok) {
        ESP_LOGE(TAG_SHT30, "Failed to request status register.");
        return error;
    }

    if (sht30_read(sht30, returnData, sizeof(returnData)) != ok) {
        ESP_LOGE(TAG_SHT30, "Failed to read status register.");
        return error;
    }

    uint8_t crc = sht30_calculate_crc(sht30, returnData);
    if (crc != returnData[2]) {
        ESP_LOGE(TAG_SHT30, "Status register CRC invalid.");
        return data_not_valid;
    }

    ESP_LOGI(TAG_SHT30, "--------------READING STATUS REGISTER--------------");
    ESP_LOGI(TAG_SHT30, "%s", (returnData[0] & (1 << 7)) ? "At least one pending alert." : "No pending alert.");
    ESP_LOGI(TAG_SHT30, "%s", (returnData[0] & (1 << 5)) ? "Heater ON." : "Heater OFF.");
    ESP_LOGI(TAG_SHT30, "%s", (returnData[0] & (1 << 3)) ? "RH tracking alert." : "No RH tracking alert.");
    ESP_LOGI(TAG_SHT30, "%s", (returnData[0] & (1 << 2)) ? "T tracking alert." : "No T tracking alert.");
    ESP_LOGI(TAG_SHT30, "%s", (returnData[1] & (1 << 4)) ? "Reset detected." : "No reset detected since last reset.");
    ESP_LOGI(TAG_SHT30, "%s",
             (returnData[1] & (1 << 1)) ?
             "Last command not processed." :
             "Last command executed successfully.");
    ESP_LOGI(TAG_SHT30, "%s",
             (returnData[1] & (1 << 0)) ?
             "Checksum of last transfer failed." :
             "Checksum of last transfer was correct.");
    ESP_LOGI(TAG_SHT30, "---------------------------------------------------");

    return ok;
}

sht30_status_t sht30_clear_status_register(sht30_t *sht30)
{
    if (sht30 == NULL) {
        return error;
    }

    uint8_t command[2] = {ClrStatusRegister >> 8, ClrStatusRegister & 0xFF};

    if (sht30_write(sht30, command) != ok) {
        ESP_LOGE(TAG_SHT30, "Failed to clear status register.");
        return error;
    }

    return ok;
}

uint8_t sht30_calculate_crc(sht30_t *sht30, uint8_t *data)
{
    (void)sht30;

    uint8_t crc = 0xFF;
    const uint8_t polynomial = 0x31;

    for (int i = 0; i < 2; i++) {
        crc ^= data[i];

        for (int j = 0; j < 8; j++) {
            if (crc & 0x80) {
                crc = (crc << 1) ^ polynomial;
            } else {
                crc <<= 1;
            }
        }
    }

    return crc;
}

float sht30_read_temperature_celsius(sht30_t *sht30)
{
    return -45.0f + 175.0f * ((float)sht30->temperature / 65535.0f);
}

float sht30_read_temperature_fahrenheit(sht30_t *sht30)
{
    return -49.0f + 315.0f * ((float)sht30->temperature / 65535.0f);
}

float sht30_read_humidity(sht30_t *sht30)
{
    return 100.0f * ((float)sht30->humidity / 65535.0f);
}