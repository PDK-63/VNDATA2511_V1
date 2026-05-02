#pragma once

#include "app_events.h"

#ifdef __cplusplus
extern "C" {
#endif

void config_sync_handle_cloud_command(const app_cloud_cmd_t *cmd);
void config_sync_publish_current_config_for_web(const char *request_id);

#ifdef __cplusplus
}
#endif