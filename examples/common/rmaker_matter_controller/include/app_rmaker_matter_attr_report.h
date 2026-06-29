/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <esp_err.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint64_t node_id;
    bool online;
    uint16_t endpoint_id;
    uint32_t cluster_id;
    uint32_t attribute_id;
    const char *value_json;
} app_rmaker_matter_attr_report_event_t;

typedef void (*app_rmaker_matter_attr_report_cb_t)(const app_rmaker_matter_attr_report_event_t *event,
                                                   void *priv_data);

/**
 * @brief Register an observer for Matter attribute reports and node online status changes.
 *
 * Attribute events are invoked from the attribute-report task after attribute filtering, TLV-to-JSON conversion, and
 * pending-slot coalescing. If multiple reports for the same attribute path arrive before the task drains the slot,
 * only the latest value is observed. Node online status uses the reserved path endpoint=0, cluster=0, attribute=0,
 * with the online field set. Callback pointers are valid only during the callback; copy data before posting work
 * elsewhere. Registering NULL unregisters the callback.
 */
esp_err_t app_rmaker_matter_controller_register_attr_report_callback(app_rmaker_matter_attr_report_cb_t cb,
                                                                     void *priv_data);

#ifdef __cplusplus
}
#endif
