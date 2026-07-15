/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <audio_ng.h>
#include <cJSON.h>
#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct agent_ng *agent_ng_handle_t;

typedef enum {
    AGENT_NG_STATE_DISABLED,
    AGENT_NG_STATE_WAITING_FOR_CREDENTIALS,
    AGENT_NG_STATE_STARTING,
    AGENT_NG_STATE_CONNECTING,
    AGENT_NG_STATE_READY,
    AGENT_NG_STATE_PREPARING,
    AGENT_NG_STATE_LISTENING,
    AGENT_NG_STATE_SPEAKING,
    AGENT_NG_STATE_ERROR,
} agent_ng_state_t;

typedef enum {
    AGENT_NG_ROLE_USER,
    AGENT_NG_ROLE_ASSISTANT,
} agent_ng_role_t;

typedef enum {
    AGENT_NG_EVENT_STATE,
    AGENT_NG_EVENT_TRANSCRIPT,
    AGENT_NG_EVENT_TOOL,
    AGENT_NG_EVENT_LEVEL,
    AGENT_NG_EVENT_ERROR,
} agent_ng_event_id_t;

typedef struct {
    agent_ng_event_id_t id;
    union {
        agent_ng_state_t state;
        struct {
            agent_ng_role_t role;
            bool final;
            const char *text;
        } transcript;
        struct {
            const char *name;
            bool active;
            esp_err_t result;
        } tool;
        uint8_t level;
        struct {
            esp_err_t code;
            const char *message;
        } error;
    } data;
} agent_ng_event_t;

/*
 * Tool callbacks run on the agent control task, never websocket receive context.
 * arguments is borrowed for the call. On success, transfer one owned cJSON result
 * to agent_ng; agent_ng deletes it after sending.
 */
typedef esp_err_t (*agent_ng_tool_cb_t)(const cJSON *arguments, cJSON **result, void *user_data);

typedef struct {
    const char *name;
    agent_ng_tool_cb_t callback;
    void *user_data;
} agent_ng_tool_t;

/* Called from the agent control task. Strings are copied and valid only for the call. */
typedef void (*agent_ng_event_cb_t)(agent_ng_handle_t agent, const agent_ng_event_t *event, void *user_data);

/* Event callbacks must not call agent_ng_disable() or agent_ng_deinit(). */

typedef struct {
    const char *agent_id;
    audio_ng_handle_t audio;
    const agent_ng_tool_t *tools;
    size_t tool_count;
    agent_ng_event_cb_t event_cb;
    void *event_user_data;
} agent_ng_config_t;

/* Initialization copies configuration, borrows audio, and starts disabled. */
esp_err_t agent_ng_init(const agent_ng_config_t *config, agent_ng_handle_t *out_agent);
void agent_ng_deinit(agent_ng_handle_t agent);

/* Enable and disable are idempotent and complete before returning. */
esp_err_t agent_ng_enable(agent_ng_handle_t agent);
esp_err_t agent_ng_disable(agent_ng_handle_t agent);

/* Wake and touch entry use this same queued intent. */
esp_err_t agent_ng_request_session(agent_ng_handle_t agent);
esp_err_t agent_ng_stop_session(agent_ng_handle_t agent);

#ifdef __cplusplus
}
#endif
