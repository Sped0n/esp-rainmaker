/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <agent_ng.h>
#include <esp_event.h>
#include <esp_websocket_client.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#define AGENT_NG_CONTROL_DEPTH 16
#define AGENT_NG_OUTBOUND_MEDIA_SLOTS 36
#define AGENT_NG_INBOUND_MEDIA_SLOTS 12
#define AGENT_NG_MEDIA_BYTES 2048
#define AGENT_NG_TEXT_BYTES 4096

#define AGENT_NG_NOTIFY_WS_CONNECTED BIT(0)
#define AGENT_NG_NOTIFY_WS_DOWN BIT(1)
#define AGENT_NG_NOTIFY_HANDSHAKE_ACK BIT(2)
#define AGENT_NG_NOTIFY_SPEECH_START BIT(3)
#define AGENT_NG_NOTIFY_SPEECH_END BIT(4)
#define AGENT_NG_NOTIFY_OVERLOAD BIT(5)

typedef enum {
    AGENT_NG_INT_START,
    AGENT_NG_INT_ENABLE,
    AGENT_NG_INT_DISABLE,
    AGENT_NG_INT_NETWORK_UP,
    AGENT_NG_INT_NETWORK_DOWN,
    AGENT_NG_INT_AUTH_READY,
    AGENT_NG_INT_WAKE_START,
    AGENT_NG_INT_WAKE_END,
    AGENT_NG_INT_VAD_START,
    AGENT_NG_INT_VAD_END,
    AGENT_NG_INT_STOP,
    AGENT_NG_INT_WS_CONNECTED,
    AGENT_NG_INT_WS_DOWN,
    AGENT_NG_INT_HANDSHAKE_ACK,
    AGENT_NG_INT_SPEECH_START,
    AGENT_NG_INT_SPEECH_END,
    AGENT_NG_INT_PLAYBACK_COMPLETE,
    AGENT_NG_INT_TEXT,
    AGENT_NG_INT_TOOL,
    AGENT_NG_INT_LEVEL,
    AGENT_NG_INT_AUDIO_FAULT,
    AGENT_NG_INT_OVERLOAD,
    AGENT_NG_INT_SHUTDOWN,
} agent_ng_intent_id_t;

typedef struct {
    uint16_t len;
    uint32_t epoch;
    uint8_t data[AGENT_NG_MEDIA_BYTES];
} agent_ng_media_t;

typedef struct {
    agent_ng_intent_id_t id;
    char *text;
    char *tool_name;
    char *request_id;
    cJSON *arguments;
    agent_ng_role_t role;
    bool final;
    uint8_t level;
    esp_err_t error;
} agent_ng_intent_t;

typedef enum {
    AGENT_NG_LIFECYCLE_CREATED,
    AGENT_NG_LIFECYCLE_DISABLED,
    AGENT_NG_LIFECYCLE_STARTING,
    AGENT_NG_LIFECYCLE_MONITORING,
    AGENT_NG_LIFECYCLE_STOPPING,
    AGENT_NG_LIFECYCLE_FAILED,
} agent_ng_lifecycle_state_t;

typedef enum {
    AGENT_NG_TRANSPORT_NO_NETWORK,
    AGENT_NG_TRANSPORT_WAITING_CREDENTIALS,
    AGENT_NG_TRANSPORT_DISCONNECTED,
    AGENT_NG_TRANSPORT_CONNECTING,
    AGENT_NG_TRANSPORT_HANDSHAKING,
    AGENT_NG_TRANSPORT_READY,
} agent_ng_transport_state_t;

typedef enum {
    AGENT_NG_SESSION_IDLE,
    AGENT_NG_SESSION_PENDING,
    AGENT_NG_SESSION_CUE,
    AGENT_NG_SESSION_LISTENING,
    AGENT_NG_SESSION_RECORDING,
    AGENT_NG_SESSION_WAITING_RESPONSE,
    AGENT_NG_SESSION_SPEAKING,
    AGENT_NG_SESSION_DRAINING,
    AGENT_NG_SESSION_CLOSING,
} agent_ng_session_state_t;

typedef struct {
    agent_ng_transport_state_t state;
    char *refresh_token;
    char *access_token;
    int64_t access_token_time;
    esp_websocket_client_handle_t websocket;
    size_t text_length;
    char *text_buffer;
    agent_ng_media_t *inbound_fragment;
    uint32_t media_epoch;
} agent_ng_transport_t;

typedef struct {
    agent_ng_session_state_t state;
    uint32_t capture_epoch;
    int64_t interaction_deadline;
    int64_t playback_deadline;
} agent_ng_session_t;

struct agent_ng {
    char *agent_id;

    audio_ng_handle_t audio;
    agent_ng_tool_t *tools;
    size_t tool_count;
    agent_ng_event_cb_t event_cb;
    void *event_user_data;

    agent_ng_lifecycle_state_t lifecycle;
    esp_err_t lifecycle_result;
    agent_ng_transport_t transport;
    agent_ng_session_t session;

    QueueHandle_t control;
    QueueHandle_t outbound_free;
    QueueHandle_t outbound_ready;
    QueueHandle_t inbound_free;
    QueueHandle_t inbound_ready;
    agent_ng_media_t *outbound_slots;
    agent_ng_media_t *inbound_slots;

    SemaphoreHandle_t websocket_send_lock;
    TaskHandle_t control_task;
    TaskHandle_t sender_task;
    SemaphoreHandle_t control_done;
    SemaphoreHandle_t sender_done;
    SemaphoreHandle_t lifecycle_done;
    SemaphoreHandle_t lifecycle_api_lock;

    esp_event_handler_instance_t ip_handler;
    esp_event_handler_instance_t wifi_handler;
    esp_event_handler_instance_t auth_handler;
};

void agent_ng_intent_free(agent_ng_intent_t *intent);
bool agent_ng_post(agent_ng_handle_t agent, agent_ng_intent_t *intent, bool overload_on_failure);
bool agent_ng_post_wait(agent_ng_handle_t agent, agent_ng_intent_t *intent);
void *agent_ng_preferred_alloc(size_t size);
void *agent_ng_preferred_calloc(size_t count, size_t size);
char *agent_ng_preferred_strdup(const char *value);

esp_err_t agent_ng_transport_init(agent_ng_handle_t agent);
void agent_ng_transport_deinit(agent_ng_handle_t agent);
esp_err_t agent_ng_transport_connect(agent_ng_handle_t agent);
void agent_ng_transport_disconnect(agent_ng_handle_t agent);
void agent_ng_transport_flush_media(agent_ng_handle_t agent);
void agent_ng_transport_set_media_epoch(agent_ng_handle_t agent, uint32_t epoch);
esp_err_t agent_ng_transport_send_control(agent_ng_handle_t agent, const char *json);

esp_err_t agent_ng_protocol_handshake(char **out_json);
esp_err_t agent_ng_protocol_audio_start(char **out_json);
esp_err_t agent_ng_protocol_audio_end(char **out_json);
esp_err_t agent_ng_protocol_tool_result(const char *request_id, esp_err_t result,
                                        cJSON *result_json, char **out_json);
esp_err_t agent_ng_protocol_parse(const char *json, agent_ng_intent_t *out_intent);

void agent_ng_tools_execute(agent_ng_handle_t agent, const char *request_id,
                            const char *name, const cJSON *arguments);
void agent_ng_state_task(void *arg);
