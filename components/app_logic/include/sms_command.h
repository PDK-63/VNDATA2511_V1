#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

bool sms_command_text_equals_ci(const char *text, const char *expected);
void sms_command_format_eng_info(char *out, size_t out_len);
void sms_command_format_status(char *out, size_t out_len);
void sms_command_format_time(char *out, size_t out_len);

esp_err_t sms_command_process_set(const char *sender, const char *body);

#ifdef __cplusplus
}
#endif