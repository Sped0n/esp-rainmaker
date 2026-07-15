/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <string.h>

#include <esp_log.h>
#include <esp_rmaker_auth_service.h>
#include <esp_timer.h>

#include "agent_ng_internal.h"

#define AGENT_NG_PLAYBACK_TIMEOUT_MS 4000
#define AGENT_NG_FOLLOW_UP_TIMEOUT_MS 8000
#define AGENT_NG_RESPONSE_TIMEOUT_MS 10000

static const char *TAG = "agent_ng_state";

static void pump_inbound_media(agent_ng_handle_t agent);
static void handle_intent(agent_ng_handle_t agent, agent_ng_intent_t *intent);

static agent_ng_state_t project_state(agent_ng_handle_t agent)
{
    if (agent->lifecycle == AGENT_NG_LIFECYCLE_DISABLED ||
            agent->lifecycle == AGENT_NG_LIFECYCLE_CREATED) {
        return AGENT_NG_STATE_DISABLED;
    }
    if (agent->lifecycle == AGENT_NG_LIFECYCLE_FAILED) {
        return AGENT_NG_STATE_ERROR;
    }
    switch (agent->session.state) {
    case AGENT_NG_SESSION_CUE:
        return AGENT_NG_STATE_PREPARING;
    case AGENT_NG_SESSION_LISTENING:
    case AGENT_NG_SESSION_RECORDING:
    case AGENT_NG_SESSION_WAITING_RESPONSE:
        return AGENT_NG_STATE_LISTENING;
    case AGENT_NG_SESSION_SPEAKING:
    case AGENT_NG_SESSION_DRAINING:
    case AGENT_NG_SESSION_CLOSING:
        return AGENT_NG_STATE_SPEAKING;
    case AGENT_NG_SESSION_IDLE:
        if (agent->lifecycle == AGENT_NG_LIFECYCLE_MONITORING &&
                agent->transport.state == AGENT_NG_TRANSPORT_DISCONNECTED) {
            return AGENT_NG_STATE_READY;
        }
        break;
    case AGENT_NG_SESSION_PENDING:
        if (agent->transport.state == AGENT_NG_TRANSPORT_READY) {
            return AGENT_NG_STATE_PREPARING;
        }
        break;
    }

    switch (agent->transport.state) {
    case AGENT_NG_TRANSPORT_READY:
        return AGENT_NG_STATE_READY;
    case AGENT_NG_TRANSPORT_DISCONNECTED:
    case AGENT_NG_TRANSPORT_CONNECTING:
    case AGENT_NG_TRANSPORT_HANDSHAKING:
        return AGENT_NG_STATE_CONNECTING;
    case AGENT_NG_TRANSPORT_NO_NETWORK:
    case AGENT_NG_TRANSPORT_WAITING_CREDENTIALS:
        return agent->lifecycle == AGENT_NG_LIFECYCLE_STARTING ?
               AGENT_NG_STATE_STARTING : AGENT_NG_STATE_WAITING_FOR_CREDENTIALS;
    }
    return AGENT_NG_STATE_ERROR;
}

static void emit_event(agent_ng_handle_t agent, const agent_ng_event_t *event)
{
    if (agent->event_cb != NULL) {
        agent->event_cb(agent, event, agent->event_user_data);
    }
}

static void emit_projected_transition(agent_ng_handle_t agent, agent_ng_state_t before)
{
    agent_ng_state_t after = project_state(agent);
    if (before == after) {
        return;
    }
    agent_ng_event_t event = {
        .id = AGENT_NG_EVENT_STATE,
        .data.state = after,
    };
    emit_event(agent, &event);
}

static void set_lifecycle_state(agent_ng_handle_t agent, agent_ng_lifecycle_state_t state)
{
    if (agent->lifecycle == state) {
        return;
    }
    agent_ng_state_t before = project_state(agent);
    agent->lifecycle = state;
    emit_projected_transition(agent, before);
}

static void set_transport_state(agent_ng_handle_t agent, agent_ng_transport_state_t state)
{
    if (agent->transport.state == state) {
        return;
    }
    agent_ng_state_t before = project_state(agent);
    agent->transport.state = state;
    emit_projected_transition(agent, before);
}

static void set_session_state(agent_ng_handle_t agent, agent_ng_session_state_t state)
{
    if (agent->session.state == state) {
        return;
    }
    agent_ng_state_t before = project_state(agent);
    agent->session.state = state;
    emit_projected_transition(agent, before);
}

static void emit_error(agent_ng_handle_t agent, esp_err_t code, const char *message)
{
    agent_ng_event_t event = {
        .id = AGENT_NG_EVENT_ERROR,
        .data.error = {
            .code = code,
            .message = message,
        },
    };
    emit_event(agent, &event);
}

static esp_err_t send_audio_control(agent_ng_handle_t agent, bool start)
{
    char *json = NULL;
    esp_err_t err = start ? agent_ng_protocol_audio_start(&json) :
                            agent_ng_protocol_audio_end(&json);
    if (err == ESP_OK) {
        err = agent_ng_transport_send_control(agent, json);
    }
    free(json);
    return err;
}

static void notify_server_conversation_end(agent_ng_handle_t agent)
{
    bool remote_audio_active = agent->session.state == AGENT_NG_SESSION_LISTENING ||
                               agent->session.state == AGENT_NG_SESSION_RECORDING ||
                               agent->session.state == AGENT_NG_SESSION_WAITING_RESPONSE ||
                               agent->session.state == AGENT_NG_SESSION_SPEAKING;
    if (!remote_audio_active || agent->transport.state != AGENT_NG_TRANSPORT_READY) {
        return;
    }
    esp_err_t err = send_audio_control(agent, false);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to end remote conversation: %s", esp_err_to_name(err));
    }
}

static void stop_session_capture(agent_ng_handle_t agent)
{
    if (agent->session.state == AGENT_NG_SESSION_LISTENING ||
            agent->session.state == AGENT_NG_SESSION_RECORDING ||
            agent->session.state == AGENT_NG_SESSION_WAITING_RESPONSE) {
        audio_ng_capture_end(agent->audio);
    }
    agent_ng_transport_flush_media(agent);
    agent->session.interaction_deadline = 0;
    agent->session.playback_deadline = 0;
}

static void abort_session(agent_ng_handle_t agent)
{
    stop_session_capture(agent);
    audio_ng_playback_abort(agent->audio);
    set_session_state(agent, AGENT_NG_SESSION_IDLE);
}

static void close_conversation(agent_ng_handle_t agent)
{
    stop_session_capture(agent);
    notify_server_conversation_end(agent);
    audio_ng_playback_abort(agent->audio);
    set_session_state(agent, AGENT_NG_SESSION_IDLE);
    agent_ng_transport_disconnect(agent);
    set_transport_state(agent, AGENT_NG_TRANSPORT_DISCONNECTED);
}

static void start_stop_cue(agent_ng_handle_t agent)
{
    set_session_state(agent, AGENT_NG_SESSION_CLOSING);

    esp_err_t err = audio_ng_play_stop_cue(agent->audio);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to play conversation stop cue: %s", esp_err_to_name(err));
        abort_session(agent);
        return;
    }
    agent->session.playback_deadline = esp_timer_get_time() +
                                       AGENT_NG_PLAYBACK_TIMEOUT_MS * 1000LL;
}

static void finish_conversation(agent_ng_handle_t agent)
{
    if (agent->session.state == AGENT_NG_SESSION_IDLE ||
            agent->session.state == AGENT_NG_SESSION_CLOSING) {
        return;
    }
    stop_session_capture(agent);
    notify_server_conversation_end(agent);
    start_stop_cue(agent);
    agent_ng_transport_disconnect(agent);
    set_transport_state(agent, AGENT_NG_TRANSPORT_DISCONNECTED);
}

static void stop_conversation(agent_ng_handle_t agent)
{
    if (agent->session.state == AGENT_NG_SESSION_IDLE ||
            agent->session.state == AGENT_NG_SESSION_CLOSING) {
        return;
    }
    audio_ng_playback_abort(agent->audio);
    finish_conversation(agent);
}

static esp_err_t begin_listening(agent_ng_handle_t agent)
{
    esp_err_t err = audio_ng_capture_begin(agent->audio, &agent->session.capture_epoch);
    if (err == ESP_OK) {
        err = send_audio_control(agent, true);
    }
    if (err != ESP_OK) {
        audio_ng_capture_end(agent->audio);
        return err;
    }
    agent_ng_transport_set_media_epoch(agent, agent->session.capture_epoch);
    agent->session.interaction_deadline = esp_timer_get_time() +
                                           AGENT_NG_FOLLOW_UP_TIMEOUT_MS * 1000LL;
    set_session_state(agent, AGENT_NG_SESSION_LISTENING);
    return ESP_OK;
}

static void finish_session_start(agent_ng_handle_t agent)
{
    if (agent->session.state != AGENT_NG_SESSION_CUE ||
            agent->transport.state != AGENT_NG_TRANSPORT_READY) {
        return;
    }

    esp_err_t err = begin_listening(agent);
    if (err != ESP_OK) {
        emit_error(agent, err, "Failed to start conversation");
        close_conversation(agent);
    }
}

static void start_session(agent_ng_handle_t agent)
{
    if (agent->session.state != AGENT_NG_SESSION_PENDING ||
            agent->transport.state != AGENT_NG_TRANSPORT_READY) {
        return;
    }

    set_session_state(agent, AGENT_NG_SESSION_CUE);
    esp_err_t err = audio_ng_prepare_output(agent->audio);
    if (err != ESP_OK) {
        emit_error(agent, err, "Failed to prepare audio output");
        close_conversation(agent);
        return;
    }

    err = audio_ng_play_cue(agent->audio);
    if (err != ESP_OK) {
        audio_ng_playback_abort(agent->audio);
        emit_error(agent, err, "Session cue was skipped");
        finish_session_start(agent);
    } else {
        agent->session.playback_deadline = esp_timer_get_time() +
                                           AGENT_NG_PLAYBACK_TIMEOUT_MS * 1000LL;
    }
}

static void complete_response(agent_ng_handle_t agent)
{
    agent->session.playback_deadline = 0;
    esp_err_t err = begin_listening(agent);
    if (err != ESP_OK) {
        emit_error(agent, err, "Failed to start follow-up turn");
        close_conversation(agent);
        return;
    }
}

static void handle_playback_complete(agent_ng_handle_t agent)
{
    if (agent->session.state != AGENT_NG_SESSION_CUE &&
            agent->session.state != AGENT_NG_SESSION_DRAINING &&
            agent->session.state != AGENT_NG_SESSION_CLOSING) {
        return;
    }
    if (agent->session.state == AGENT_NG_SESSION_DRAINING) {
        pump_inbound_media(agent);
        if (uxQueueMessagesWaiting(agent->inbound_ready) != 0) {
            return;
        }
    }
    if (audio_ng_playback_wait(agent->audio, 200) != ESP_OK) {
        return;
    }
    if (agent->session.state == AGENT_NG_SESSION_CUE) {
        agent->session.playback_deadline = 0;
        finish_session_start(agent);
    } else if (agent->session.state == AGENT_NG_SESSION_CLOSING) {
        agent->session.playback_deadline = 0;
        set_session_state(agent, AGENT_NG_SESSION_IDLE);
    } else {
        complete_response(agent);
    }
}

static void store_refresh_token(agent_ng_handle_t agent)
{
    char *token = NULL;
    if (esp_rmaker_auth_service_get_user_token(&token) != ESP_OK || token == NULL) {
        return;
    }

    bool changed = agent->transport.refresh_token == NULL ||
                   strcmp(agent->transport.refresh_token, token) != 0;
    char *preferred_token = agent_ng_preferred_strdup(token);
    free(token);
    if (preferred_token == NULL) {
        emit_error(agent, ESP_ERR_NO_MEM, "Failed to store RainMaker token");
        return;
    }

    free(agent->transport.refresh_token);
    agent->transport.refresh_token = preferred_token;
    if (changed) {
        free(agent->transport.access_token);
        agent->transport.access_token = NULL;
        agent->transport.access_token_time = 0;
    }
}

static void connect_transport(agent_ng_handle_t agent)
{
    if (agent->lifecycle != AGENT_NG_LIFECYCLE_MONITORING ||
            agent->transport.state != AGENT_NG_TRANSPORT_DISCONNECTED) {
        return;
    }

    set_transport_state(agent, AGENT_NG_TRANSPORT_CONNECTING);
    esp_err_t err = agent_ng_transport_connect(agent);
    if (err != ESP_OK) {
        set_transport_state(agent, AGENT_NG_TRANSPORT_DISCONNECTED);
        emit_error(agent, err, "Failed to connect agent transport");
        abort_session(agent);
    }
}

static esp_err_t start_monitoring(agent_ng_handle_t agent)
{
    if (agent->lifecycle != AGENT_NG_LIFECYCLE_STARTING ||
            agent->transport.state != AGENT_NG_TRANSPORT_DISCONNECTED) {
        return ESP_OK;
    }

    esp_err_t err = audio_ng_start_wake_monitoring(agent->audio);
    if (err != ESP_OK) {
        emit_error(agent, err, "Failed to start wake monitoring");
        set_lifecycle_state(agent, AGENT_NG_LIFECYCLE_FAILED);
        return err;
    }
    set_lifecycle_state(agent, AGENT_NG_LIFECYCLE_MONITORING);
    return ESP_OK;
}

static esp_err_t handle_enable(agent_ng_handle_t agent)
{
    if (agent->lifecycle == AGENT_NG_LIFECYCLE_STARTING ||
            agent->lifecycle == AGENT_NG_LIFECYCLE_MONITORING) {
        return ESP_OK;
    }
    set_lifecycle_state(agent, AGENT_NG_LIFECYCLE_STARTING);
    return start_monitoring(agent);
}

static esp_err_t handle_disable(agent_ng_handle_t agent)
{
    if (agent->lifecycle == AGENT_NG_LIFECYCLE_DISABLED) {
        return ESP_OK;
    }

    set_lifecycle_state(agent, AGENT_NG_LIFECYCLE_DISABLED);
    stop_session_capture(agent);
    notify_server_conversation_end(agent);
    audio_ng_playback_abort(agent->audio);
    set_session_state(agent, AGENT_NG_SESSION_IDLE);
    agent_ng_transport_disconnect(agent);
    if (agent->transport.state != AGENT_NG_TRANSPORT_NO_NETWORK) {
        set_transport_state(agent, agent->transport.refresh_token == NULL ?
                            AGENT_NG_TRANSPORT_WAITING_CREDENTIALS :
                            AGENT_NG_TRANSPORT_DISCONNECTED);
    }
    esp_err_t err = audio_ng_stop_wake_monitoring(agent->audio);
    return err;
}

static void handle_speech_start(agent_ng_handle_t agent)
{
    if (agent->session.state != AGENT_NG_SESSION_LISTENING &&
            agent->session.state != AGENT_NG_SESSION_RECORDING &&
            agent->session.state != AGENT_NG_SESSION_WAITING_RESPONSE) {
        return;
    }
    agent->session.interaction_deadline = 0;
    audio_ng_capture_end(agent->audio);
    agent_ng_transport_set_media_epoch(agent, 0);
    set_session_state(agent, AGENT_NG_SESSION_SPEAKING);
}

static void handle_vad_start(agent_ng_handle_t agent)
{
    if (agent->session.state == AGENT_NG_SESSION_LISTENING ||
            agent->session.state == AGENT_NG_SESSION_WAITING_RESPONSE) {
        agent->session.interaction_deadline = 0;
        set_session_state(agent, AGENT_NG_SESSION_RECORDING);
    }
}

static void handle_vad_end(agent_ng_handle_t agent)
{
    if (agent->session.state == AGENT_NG_SESSION_RECORDING) {
        agent->session.interaction_deadline = esp_timer_get_time() +
                                              AGENT_NG_RESPONSE_TIMEOUT_MS * 1000LL;
        set_session_state(agent, AGENT_NG_SESSION_WAITING_RESPONSE);
    }
}

static void handle_speech_end(agent_ng_handle_t agent)
{
    if (agent->session.state != AGENT_NG_SESSION_SPEAKING) {
        return;
    }
    pump_inbound_media(agent);
    esp_err_t err = send_audio_control(agent, false);
    if (err != ESP_OK) {
        emit_error(agent, err, "Failed to finish remote conversation");
        close_conversation(agent);
        return;
    }
    set_session_state(agent, AGENT_NG_SESSION_DRAINING);
    agent->session.playback_deadline = esp_timer_get_time() +
                                       AGENT_NG_PLAYBACK_TIMEOUT_MS * 1000LL;
    handle_playback_complete(agent);
}

static void emit_transcript(agent_ng_handle_t agent, const agent_ng_intent_t *intent)
{
    agent_ng_event_t event = {
        .id = AGENT_NG_EVENT_TRANSCRIPT,
        .data.transcript = {
            .role = intent->role,
            .final = intent->final,
            .text = intent->text,
        },
    };
    emit_event(agent, &event);
}

static void resume_ready_agent(agent_ng_handle_t agent)
{
    set_transport_state(agent, AGENT_NG_TRANSPORT_DISCONNECTED);
    start_monitoring(agent);
    if (agent->session.state == AGENT_NG_SESSION_PENDING) {
        connect_transport(agent);
    }
}

static void handle_network_up(agent_ng_handle_t agent)
{
    if (agent->transport.refresh_token == NULL) {
        set_transport_state(agent, AGENT_NG_TRANSPORT_WAITING_CREDENTIALS);
        return;
    }
    resume_ready_agent(agent);
}

static void handle_network_down(agent_ng_handle_t agent)
{
    abort_session(agent);
    agent_ng_transport_disconnect(agent);
    set_transport_state(agent, AGENT_NG_TRANSPORT_NO_NETWORK);
}

static void handle_auth_ready(agent_ng_handle_t agent)
{
    store_refresh_token(agent);
    if (agent->transport.refresh_token == NULL ||
            agent->transport.state == AGENT_NG_TRANSPORT_NO_NETWORK) {
        return;
    }
    if (agent->transport.state != AGENT_NG_TRANSPORT_WAITING_CREDENTIALS &&
            agent->transport.state != AGENT_NG_TRANSPORT_DISCONNECTED) {
        return;
    }
    resume_ready_agent(agent);
}

static void handle_wake_start(agent_ng_handle_t agent)
{
    if (agent->lifecycle != AGENT_NG_LIFECYCLE_MONITORING) {
        return;
    }
    if (agent->session.state == AGENT_NG_SESSION_PENDING &&
            agent->transport.state == AGENT_NG_TRANSPORT_DISCONNECTED) {
        connect_transport(agent);
        return;
    }
    if (agent->session.state != AGENT_NG_SESSION_IDLE) {
        return;
    }
    set_session_state(agent, AGENT_NG_SESSION_PENDING);
    if (agent->transport.state == AGENT_NG_TRANSPORT_READY) {
        start_session(agent);
    } else if (agent->transport.state == AGENT_NG_TRANSPORT_DISCONNECTED) {
        connect_transport(agent);
    }
}

static void handle_ws_connected(agent_ng_handle_t agent)
{
    if (agent->lifecycle != AGENT_NG_LIFECYCLE_MONITORING) {
        agent_ng_transport_disconnect(agent);
        return;
    }
    if (agent->transport.state != AGENT_NG_TRANSPORT_CONNECTING) {
        return;
    }
    set_transport_state(agent, AGENT_NG_TRANSPORT_HANDSHAKING);
    char *handshake = NULL;
    esp_err_t err = agent_ng_protocol_handshake(&handshake);
    if (err == ESP_OK) {
        err = agent_ng_transport_send_control(agent, handshake);
    }
    free(handshake);
    if (err != ESP_OK) {
        emit_error(agent, err, "Failed to send agent handshake");
        agent_ng_transport_disconnect(agent);
        set_transport_state(agent, AGENT_NG_TRANSPORT_DISCONNECTED);
        abort_session(agent);
    }
}

static void handle_ws_down(agent_ng_handle_t agent)
{
    if (agent->transport.state != AGENT_NG_TRANSPORT_NO_NETWORK) {
        set_transport_state(agent, AGENT_NG_TRANSPORT_DISCONNECTED);
    }
    if (agent->lifecycle != AGENT_NG_LIFECYCLE_MONITORING) {
        abort_session(agent);
        return;
    }
    if (agent->session.state == AGENT_NG_SESSION_CLOSING) {
        return;
    }
    if (agent->session.state == AGENT_NG_SESSION_IDLE) {
        abort_session(agent);
        return;
    }
    stop_session_capture(agent);
    start_stop_cue(agent);
}

static void handle_notifications(agent_ng_handle_t agent, uint32_t notifications)
{
    const struct {
        uint32_t bit;
        agent_ng_intent_id_t intent;
    } events[] = {
        {AGENT_NG_NOTIFY_WS_CONNECTED, AGENT_NG_INT_WS_CONNECTED},
        {AGENT_NG_NOTIFY_WS_DOWN, AGENT_NG_INT_WS_DOWN},
        {AGENT_NG_NOTIFY_HANDSHAKE_ACK, AGENT_NG_INT_HANDSHAKE_ACK},
        {AGENT_NG_NOTIFY_SPEECH_START, AGENT_NG_INT_SPEECH_START},
        {AGENT_NG_NOTIFY_SPEECH_END, AGENT_NG_INT_SPEECH_END},
        {AGENT_NG_NOTIFY_OVERLOAD, AGENT_NG_INT_OVERLOAD},
    };
    for (size_t i = 0; i < sizeof(events) / sizeof(events[0]); ++i) {
        if ((notifications & events[i].bit) != 0) {
            agent_ng_intent_t intent = {.id = events[i].intent};
            handle_intent(agent, &intent);
        }
    }
}

static void handle_intent(agent_ng_handle_t agent, agent_ng_intent_t *intent)
{
    switch (intent->id) {
    case AGENT_NG_INT_START:
        break;
    case AGENT_NG_INT_ENABLE:
        agent->lifecycle_result = handle_enable(agent);
        xSemaphoreGive(agent->lifecycle_done);
        break;
    case AGENT_NG_INT_DISABLE:
        agent->lifecycle_result = handle_disable(agent);
        xSemaphoreGive(agent->lifecycle_done);
        break;
    case AGENT_NG_INT_NETWORK_UP:
        handle_network_up(agent);
        break;
    case AGENT_NG_INT_NETWORK_DOWN:
        handle_network_down(agent);
        break;
    case AGENT_NG_INT_AUTH_READY:
        handle_auth_ready(agent);
        break;
    case AGENT_NG_INT_WAKE_START:
        handle_wake_start(agent);
        break;
    case AGENT_NG_INT_WAKE_END:
        /* WakeNet's local wake window is independent of the cloud conversation. */
        break;
    case AGENT_NG_INT_VAD_START:
        if (agent->lifecycle == AGENT_NG_LIFECYCLE_MONITORING) {
            handle_vad_start(agent);
        }
        break;
    case AGENT_NG_INT_VAD_END:
        if (agent->lifecycle == AGENT_NG_LIFECYCLE_MONITORING) {
            handle_vad_end(agent);
        }
        break;
    case AGENT_NG_INT_STOP:
        stop_conversation(agent);
        break;
    case AGENT_NG_INT_WS_CONNECTED:
        handle_ws_connected(agent);
        break;
    case AGENT_NG_INT_WS_DOWN:
        handle_ws_down(agent);
        break;
    case AGENT_NG_INT_HANDSHAKE_ACK:
        if (agent->lifecycle == AGENT_NG_LIFECYCLE_MONITORING &&
                agent->transport.state == AGENT_NG_TRANSPORT_HANDSHAKING) {
            set_transport_state(agent, AGENT_NG_TRANSPORT_READY);
            start_session(agent);
        }
        break;
    case AGENT_NG_INT_SPEECH_START:
        if (agent->lifecycle == AGENT_NG_LIFECYCLE_MONITORING) {
            handle_speech_start(agent);
        }
        break;
    case AGENT_NG_INT_SPEECH_END:
        if (agent->lifecycle == AGENT_NG_LIFECYCLE_MONITORING) {
            handle_speech_end(agent);
        }
        break;
    case AGENT_NG_INT_PLAYBACK_COMPLETE:
        if (agent->lifecycle == AGENT_NG_LIFECYCLE_MONITORING) {
            handle_playback_complete(agent);
        }
        break;
    case AGENT_NG_INT_TEXT:
        emit_transcript(agent, intent);
        break;
    case AGENT_NG_INT_TOOL:
        if (agent->lifecycle == AGENT_NG_LIFECYCLE_MONITORING &&
                agent->transport.state == AGENT_NG_TRANSPORT_READY) {
            agent_ng_tools_execute(agent, intent->request_id, intent->tool_name,
                                   intent->arguments);
            if (agent->session.state == AGENT_NG_SESSION_LISTENING) {
                agent->session.interaction_deadline = esp_timer_get_time() +
                                                       AGENT_NG_FOLLOW_UP_TIMEOUT_MS * 1000LL;
            } else if (agent->session.state == AGENT_NG_SESSION_WAITING_RESPONSE) {
                agent->session.interaction_deadline = esp_timer_get_time() +
                                                       AGENT_NG_RESPONSE_TIMEOUT_MS * 1000LL;
            }
        }
        break;
    case AGENT_NG_INT_LEVEL: {
        agent_ng_event_t event = {
            .id = AGENT_NG_EVENT_LEVEL,
            .data.level = intent->level,
        };
        emit_event(agent, &event);
        break;
    }
    case AGENT_NG_INT_AUDIO_FAULT:
        if (agent->lifecycle == AGENT_NG_LIFECYCLE_MONITORING) {
            emit_error(agent, intent->error, "Audio subsystem fault");
            close_conversation(agent);
        }
        break;
    case AGENT_NG_INT_OVERLOAD:
        if (agent->session.state == AGENT_NG_SESSION_IDLE ||
                agent->session.state == AGENT_NG_SESSION_CLOSING) {
            break;
        }
        emit_error(agent, ESP_ERR_NO_MEM, "Agent media transport overloaded");
        close_conversation(agent);
        break;
    case AGENT_NG_INT_SHUTDOWN:
        close_conversation(agent);
        set_lifecycle_state(agent, AGENT_NG_LIFECYCLE_STOPPING);
        break;
    }
}

static void pump_inbound_media(agent_ng_handle_t agent)
{
    if (agent->lifecycle != AGENT_NG_LIFECYCLE_MONITORING) {
        return;
    }
    if (agent->session.state == AGENT_NG_SESSION_LISTENING ||
            agent->session.state == AGENT_NG_SESSION_RECORDING ||
            agent->session.state == AGENT_NG_SESSION_WAITING_RESPONSE) {
        /* The first response packet can arrive before its start notification is consumed. */
        return;
    }

    agent_ng_media_t *media = NULL;
    while (xQueueReceive(agent->inbound_ready, &media, 0) == pdTRUE) {
        esp_err_t err = ESP_ERR_INVALID_STATE;
        if (agent->session.state == AGENT_NG_SESSION_SPEAKING ||
                agent->session.state == AGENT_NG_SESSION_DRAINING) {
            err = audio_ng_response_write(agent->audio, media->data, media->len, 100);
        }
        if (err == ESP_ERR_TIMEOUT) {
            if (xQueueSendToFront(agent->inbound_ready, &media, 0) != pdTRUE) {
                xQueueSend(agent->inbound_free, &media, 0);
                agent_ng_intent_t overload = {.id = AGENT_NG_INT_OVERLOAD};
                agent_ng_post(agent, &overload, false);
            }
            return;
        }
        xQueueSend(agent->inbound_free, &media, 0);
        if (err != ESP_OK && agent->session.state != AGENT_NG_SESSION_IDLE) {
            agent_ng_intent_t overload = {.id = AGENT_NG_INT_OVERLOAD};
            agent_ng_post(agent, &overload, false);
            break;
        }
    }
}

static void pump_outbound_media(agent_ng_handle_t agent)
{
    if (agent->lifecycle != AGENT_NG_LIFECYCLE_MONITORING) {
        return;
    }
    if (agent->session.state != AGENT_NG_SESSION_LISTENING &&
            agent->session.state != AGENT_NG_SESSION_RECORDING &&
            agent->session.state != AGENT_NG_SESSION_WAITING_RESPONSE) {
        return;
    }

    agent_ng_media_t *media = NULL;
    while (xQueueReceive(agent->outbound_free, &media, 0) == pdTRUE) {
        size_t len = 0;
        esp_err_t err = audio_ng_capture_read(agent->audio, agent->session.capture_epoch,
                                              media->data, sizeof(media->data), &len, 0);
        if (err == ESP_OK && len > 0) {
            media->len = len;
            media->epoch = agent->session.capture_epoch;
            if (xQueueSend(agent->outbound_ready, &media, 0) == pdTRUE) {
                continue;
            }
            xQueueSend(agent->outbound_free, &media, 0);
            agent_ng_intent_t overload = {.id = AGENT_NG_INT_OVERLOAD};
            agent_ng_post(agent, &overload, false);
            return;
        }
        xQueueSend(agent->outbound_free, &media, 0);
        if (err != ESP_ERR_TIMEOUT) {
            agent_ng_intent_t fault = {
                .id = AGENT_NG_INT_AUDIO_FAULT,
                .error = err,
            };
            agent_ng_post(agent, &fault, false);
        }
        return;
    }
    /* Leave encoded audio in audio_ng until the network sender returns a slot. */
}

static void check_playback_timeout(agent_ng_handle_t agent)
{
    if (agent->session.playback_deadline == 0 ||
            esp_timer_get_time() < agent->session.playback_deadline) {
        return;
    }
    if (agent->session.state == AGENT_NG_SESSION_DRAINING) {
        emit_error(agent, ESP_ERR_TIMEOUT, "Agent playback did not drain");
        close_conversation(agent);
    } else if (agent->session.state == AGENT_NG_SESSION_CUE) {
        emit_error(agent, ESP_ERR_TIMEOUT, "Session cue did not drain");
        audio_ng_playback_abort(agent->audio);
        agent->session.playback_deadline = 0;
        finish_session_start(agent);
    } else if (agent->session.state == AGENT_NG_SESSION_CLOSING) {
        ESP_LOGW(TAG, "Conversation stop cue did not drain");
        abort_session(agent);
    }
}

static void check_interaction_timeout(agent_ng_handle_t agent)
{
    if (agent->session.interaction_deadline == 0 ||
            esp_timer_get_time() < agent->session.interaction_deadline) {
        return;
    }

    if (agent->session.state == AGENT_NG_SESSION_LISTENING) {
        ESP_LOGI(TAG, "No follow-up speech detected; closing conversation");
        finish_conversation(agent);
    } else if (agent->session.state == AGENT_NG_SESSION_WAITING_RESPONSE) {
        ESP_LOGI(TAG, "Agent response did not start; closing conversation");
        finish_conversation(agent);
    } else {
        agent->session.interaction_deadline = 0;
    }
}

void agent_ng_state_task(void *arg)
{
    agent_ng_handle_t agent = arg;

    while (agent->lifecycle != AGENT_NG_LIFECYCLE_STOPPING) {
        uint32_t notifications = 0;
        xTaskNotifyWait(0, UINT32_MAX, &notifications, 0);
        handle_notifications(agent, notifications);
        agent_ng_intent_t intent = {0};
        if (xQueueReceive(agent->control, &intent, pdMS_TO_TICKS(20)) == pdTRUE) {
            handle_intent(agent, &intent);
            agent_ng_intent_free(&intent);
        }
        pump_inbound_media(agent);
        pump_outbound_media(agent);
        check_interaction_timeout(agent);
        check_playback_timeout(agent);
    }

    agent_ng_intent_t pending = {0};
    while (xQueueReceive(agent->control, &pending, 0) == pdTRUE) {
        agent_ng_intent_free(&pending);
    }

    agent->control_task = NULL;
    xSemaphoreGive(agent->control_done);
    vTaskDelete(NULL);
}
