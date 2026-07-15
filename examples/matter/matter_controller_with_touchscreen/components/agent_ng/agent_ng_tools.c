/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <string.h>

#include <esp_log.h>

#include "agent_ng_internal.h"

static const char *TAG = "agent_ng_tools";

static void emit_tool_event(agent_ng_handle_t agent, const char *name, bool active,
                            esp_err_t result)
{
    if (agent->event_cb == NULL) {
        return;
    }
    agent_ng_event_t event = {
        .id = AGENT_NG_EVENT_TOOL,
        .data.tool = {
            .name = name,
            .active = active,
            .result = result,
        },
    };
    agent->event_cb(agent, &event, agent->event_user_data);
}

void agent_ng_tools_execute(agent_ng_handle_t agent, const char *request_id,
                            const char *name, const cJSON *arguments)
{
    cJSON *result = NULL;
    esp_err_t status = ESP_ERR_NOT_FOUND;

    for (size_t i = 0; i < agent->tool_count; ++i) {
        if (strcmp(agent->tools[i].name, name) != 0) {
            continue;
        }
        emit_tool_event(agent, name, true, ESP_OK);
        status = agent->tools[i].callback(arguments, &result,
                                          agent->tools[i].user_data);
        emit_tool_event(agent, name, false, status);
        break;
    }

    char *wire = NULL;
    esp_err_t err = agent_ng_protocol_tool_result(request_id, status, result, &wire);
    if (err == ESP_OK) {
        err = agent_ng_transport_send_control(agent, wire);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to send result for tool %s: %s", name,
                 esp_err_to_name(err));
    }
    free(wire);
}
