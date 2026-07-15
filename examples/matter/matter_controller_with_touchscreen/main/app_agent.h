/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <esp_err.h>
#include <esp_rmaker_core.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t app_agent_init(esp_rmaker_node_t *node);
esp_err_t app_agent_enable(void);

#ifdef __cplusplus
}
#endif
