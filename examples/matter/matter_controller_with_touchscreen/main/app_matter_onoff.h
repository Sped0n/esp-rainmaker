/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#pragma once

#include <esp_err.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void matter_onoff_ensure_retry_task_started(void);
void matter_onoff_subscribe_all(void);
esp_err_t matter_onoff_toggle(uint64_t node_id, uint16_t endpoint_id);

#ifdef __cplusplus
}
#endif
