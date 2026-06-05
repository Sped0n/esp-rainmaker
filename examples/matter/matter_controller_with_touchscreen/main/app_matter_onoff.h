#pragma once

#include <stdint.h>

#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

void matter_onoff_ensure_retry_task_started(void);
void matter_onoff_subscribe_all(void);
esp_err_t matter_onoff_toggle(uint64_t node_id, uint16_t endpoint_id);

#ifdef __cplusplus
}
#endif
