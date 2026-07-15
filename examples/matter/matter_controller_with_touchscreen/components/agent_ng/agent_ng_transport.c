/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include <cJSON.h>
#include <esp_check.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <sdkconfig.h>

#include "agent_ng_internal.h"

#define AGENT_NG_ACCESS_TOKEN_LIFETIME_US (3600LL * 1000000LL)
#define AGENT_NG_ACCESS_TOKEN_MARGIN_US (10LL * 1000000LL)
#define AGENT_NG_AUTH_RESPONSE_MAX 8192
#define AGENT_NG_MEDIA_SEND_TIMEOUT_MS 5000

static const char *TAG = "agent_ng_transport";

static char *preferred_format(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    va_list measure_args;
    va_copy(measure_args, args);
    int length = vsnprintf(NULL, 0, format, measure_args);
    va_end(measure_args);
    if (length < 0) {
        va_end(args);
        return NULL;
    }

    char *value = agent_ng_preferred_alloc((size_t)length + 1);
    if (value != NULL) {
        vsnprintf(value, (size_t)length + 1, format, args);
    }
    va_end(args);
    return value;
}

static char *json_print_preferred(cJSON *json)
{
    char *internal_json = cJSON_PrintUnformatted(json);
    if (internal_json == NULL) {
        return NULL;
    }
    char *external_json = agent_ng_preferred_strdup(internal_json);
    cJSON_free(internal_json);
    return external_json;
}

static void post_simple_intent(agent_ng_handle_t agent, agent_ng_intent_id_t id,
                               bool overload_on_failure)
{
    uint32_t notification = 0;
    switch (id) {
    case AGENT_NG_INT_WS_CONNECTED:
        notification = AGENT_NG_NOTIFY_WS_CONNECTED;
        break;
    case AGENT_NG_INT_WS_DOWN:
        notification = AGENT_NG_NOTIFY_WS_DOWN;
        break;
    case AGENT_NG_INT_HANDSHAKE_ACK:
        notification = AGENT_NG_NOTIFY_HANDSHAKE_ACK;
        break;
    case AGENT_NG_INT_SPEECH_START:
        notification = AGENT_NG_NOTIFY_SPEECH_START;
        break;
    case AGENT_NG_INT_SPEECH_END:
        notification = AGENT_NG_NOTIFY_SPEECH_END;
        break;
    case AGENT_NG_INT_OVERLOAD:
        notification = AGENT_NG_NOTIFY_OVERLOAD;
        break;
    default:
        break;
    }
    if (notification != 0 && agent->control_task != NULL) {
        xTaskNotify(agent->control_task, notification, eSetBits);
        return;
    }

    agent_ng_intent_t intent = {.id = id};
    agent_ng_post(agent, &intent, overload_on_failure);
}

static void release_inbound_fragment(agent_ng_handle_t agent)
{
    if (agent->transport.inbound_fragment != NULL) {
        xQueueSend(agent->inbound_free, &agent->transport.inbound_fragment, 0);
        agent->transport.inbound_fragment = NULL;
    }
}

static void reset_receive_state(agent_ng_handle_t agent)
{
    agent->transport.text_length = 0;
    release_inbound_fragment(agent);
}

static void receive_text(agent_ng_handle_t agent, const esp_websocket_event_data_t *data)
{
    if (data->payload_len <= 0 || data->payload_len >= AGENT_NG_TEXT_BYTES ||
            data->payload_offset < 0 || data->data_len < 0 ||
            data->payload_offset + data->data_len > data->payload_len) {
        reset_receive_state(agent);
        post_simple_intent(agent, AGENT_NG_INT_OVERLOAD, false);
        return;
    }

    if (data->payload_offset == 0) {
        agent->transport.text_length = 0;
    }
    if ((size_t)data->payload_offset != agent->transport.text_length) {
        reset_receive_state(agent);
        post_simple_intent(agent, AGENT_NG_INT_OVERLOAD, false);
        return;
    }

    memcpy(agent->transport.text_buffer + data->payload_offset, data->data_ptr, data->data_len);
    agent->transport.text_length += data->data_len;
    if (!data->fin && agent->transport.text_length != (size_t)data->payload_len) {
        return;
    }
    if (agent->transport.text_length != (size_t)data->payload_len) {
        reset_receive_state(agent);
        post_simple_intent(agent, AGENT_NG_INT_OVERLOAD, false);
        return;
    }

    agent->transport.text_buffer[agent->transport.text_length] = '\0';
    agent_ng_intent_t intent = {0};
    esp_err_t err = agent_ng_protocol_parse(agent->transport.text_buffer, &intent);
    agent->transport.text_length = 0;
    if (err == ESP_OK) {
        if (intent.id == AGENT_NG_INT_HANDSHAKE_ACK ||
                intent.id == AGENT_NG_INT_SPEECH_START ||
                intent.id == AGENT_NG_INT_SPEECH_END) {
            post_simple_intent(agent, intent.id, true);
            agent_ng_intent_free(&intent);
        } else {
            agent_ng_post(agent, &intent, true);
        }
    } else if (err != ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGW(TAG, "Discarding malformed agent message");
    }
}

static void receive_binary(agent_ng_handle_t agent, const esp_websocket_event_data_t *data)
{
    if (data->payload_len <= 0 || data->payload_len > AGENT_NG_MEDIA_BYTES ||
            data->payload_offset < 0 || data->data_len < 0 ||
            data->payload_offset + data->data_len > data->payload_len) {
        release_inbound_fragment(agent);
        post_simple_intent(agent, AGENT_NG_INT_OVERLOAD, false);
        return;
    }

    if (data->payload_offset == 0) {
        release_inbound_fragment(agent);
        if (xQueueReceive(agent->inbound_free, &agent->transport.inbound_fragment,
                          pdMS_TO_TICKS(1000)) != pdTRUE) {
            post_simple_intent(agent, AGENT_NG_INT_OVERLOAD, false);
            return;
        }
        agent->transport.inbound_fragment->len = 0;
    }

    agent_ng_media_t *slot = agent->transport.inbound_fragment;
    if (slot == NULL || data->payload_offset != slot->len) {
        release_inbound_fragment(agent);
        post_simple_intent(agent, AGENT_NG_INT_OVERLOAD, false);
        return;
    }

    memcpy(slot->data + data->payload_offset, data->data_ptr, data->data_len);
    slot->len += data->data_len;
    if (!data->fin && slot->len != data->payload_len) {
        return;
    }
    if (slot->len != data->payload_len) {
        release_inbound_fragment(agent);
        post_simple_intent(agent, AGENT_NG_INT_OVERLOAD, false);
        return;
    }

    agent->transport.inbound_fragment = NULL;
    if (xQueueSend(agent->inbound_ready, &slot, 0) != pdTRUE) {
        xQueueSend(agent->inbound_free, &slot, 0);
        post_simple_intent(agent, AGENT_NG_INT_OVERLOAD, false);
    }
}

static void websocket_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)base;
    agent_ng_handle_t agent = arg;
    esp_websocket_event_data_t *event = data;

    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        post_simple_intent(agent, AGENT_NG_INT_WS_CONNECTED, true);
        break;
    case WEBSOCKET_EVENT_DATA:
        if (event == NULL) {
            break;
        }
        if (event->op_code == WS_TRANSPORT_OPCODES_TEXT) {
            receive_text(agent, event);
        } else if (event->op_code == WS_TRANSPORT_OPCODES_BINARY) {
            receive_binary(agent, event);
        }
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_ERROR:
    case WEBSOCKET_EVENT_CLOSED:
    case WEBSOCKET_EVENT_FINISH:
        reset_receive_state(agent);
        post_simple_intent(agent, AGENT_NG_INT_WS_DOWN, true);
        break;
    default:
        break;
    }
}

static esp_err_t fetch_access_token(agent_ng_handle_t agent)
{
    int64_t age = esp_timer_get_time() - agent->transport.access_token_time;
    if (agent->transport.access_token != NULL &&
            age < AGENT_NG_ACCESS_TOKEN_LIFETIME_US - AGENT_NG_ACCESS_TOKEN_MARGIN_US) {
        return ESP_OK;
    }
    ESP_RETURN_ON_FALSE(agent->transport.refresh_token != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "Refresh token is unavailable");

    esp_err_t ret = ESP_FAIL;
    char *url = NULL;
    char *request_body = NULL;
    char *response = NULL;
    cJSON *request = NULL;
    cJSON *reply = NULL;
    esp_http_client_handle_t client = NULL;

    url = preferred_format("https://%s/user/auth/tokens", CONFIG_AGENT_NG_API_ENDPOINT);
    ESP_GOTO_ON_FALSE(url != NULL, ESP_ERR_NO_MEM, cleanup, TAG, "Allocate auth URL failed");

    request = cJSON_CreateObject();
    ESP_GOTO_ON_FALSE(request != NULL &&
                      cJSON_AddStringToObject(request, "refresh_token", agent->transport.refresh_token) != NULL,
                      ESP_ERR_NO_MEM, cleanup, TAG, "Create auth request failed");
    request_body = json_print_preferred(request);
    ESP_GOTO_ON_FALSE(request_body != NULL, ESP_ERR_NO_MEM, cleanup, TAG,
                      "Serialize auth request failed");

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 3072,
    };
    client = esp_http_client_init(&config);
    ESP_GOTO_ON_FALSE(client != NULL, ESP_FAIL, cleanup, TAG, "Initialize auth client failed");
    ESP_GOTO_ON_ERROR(esp_http_client_set_header(client, "Content-Type", "application/json"),
                      cleanup, TAG, "Set auth content type failed");
    ESP_GOTO_ON_ERROR(esp_http_client_open(client, strlen(request_body)), cleanup, TAG,
                      "Open auth request failed");
    ESP_GOTO_ON_FALSE(esp_http_client_write(client, request_body, strlen(request_body)) ==
                      (int)strlen(request_body), ESP_FAIL, cleanup, TAG,
                      "Write auth request failed");

    int content_length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    ESP_GOTO_ON_FALSE(status == 200 && content_length > 0 &&
                      content_length <= AGENT_NG_AUTH_RESPONSE_MAX,
                      ESP_ERR_INVALID_RESPONSE, cleanup, TAG, "Agent authentication failed");

    response = agent_ng_preferred_alloc((size_t)content_length + 1);
    ESP_GOTO_ON_FALSE(response != NULL, ESP_ERR_NO_MEM, cleanup, TAG,
                      "Allocate auth response failed");
    ESP_GOTO_ON_FALSE(esp_http_client_read_response(client, response, content_length) == content_length,
                      ESP_ERR_INVALID_RESPONSE, cleanup, TAG, "Read auth response failed");
    response[content_length] = '\0';

    reply = cJSON_Parse(response);
    const char *token = reply == NULL ? NULL :
                        cJSON_GetStringValue(cJSON_GetObjectItem(reply, "access_token"));
    ESP_GOTO_ON_FALSE(token != NULL, ESP_ERR_INVALID_RESPONSE, cleanup, TAG,
                      "Auth response has no access token");

    char *new_token = agent_ng_preferred_strdup(token);
    ESP_GOTO_ON_FALSE(new_token != NULL, ESP_ERR_NO_MEM, cleanup, TAG,
                      "Copy access token failed");
    free(agent->transport.access_token);
    agent->transport.access_token = new_token;
    agent->transport.access_token_time = esp_timer_get_time();
    ret = ESP_OK;

cleanup:
    if (client != NULL) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
    }
    cJSON_Delete(reply);
    cJSON_Delete(request);
    free(response);
    free(request_body);
    free(url);
    return ret;
}

static void sender_task(void *arg)
{
    agent_ng_handle_t agent = arg;
    while (true) {
        agent_ng_media_t *media = NULL;
        xQueueReceive(agent->outbound_ready, &media, portMAX_DELAY);
        if (media == NULL) {
            break;
        }

        bool sent = false;
        if (xSemaphoreTake(agent->websocket_send_lock, pdMS_TO_TICKS(2000)) == pdTRUE) {
            if (agent->transport.media_epoch != 0 &&
                    media->epoch == agent->transport.media_epoch) {
                sent = esp_websocket_client_send_bin(agent->transport.websocket, (char *)media->data,
                                                      media->len,
                                                      pdMS_TO_TICKS(AGENT_NG_MEDIA_SEND_TIMEOUT_MS)) >= 0;
            } else {
                sent = true;
            }
            xSemaphoreGive(agent->websocket_send_lock);
        }
        uint32_t media_epoch = media->epoch;
        xQueueSend(agent->outbound_free, &media, 0);
        if (!sent) {
            ESP_LOGE(TAG, "Media send failed connected=%d state=%d epoch=%lu",
                     esp_websocket_client_is_connected(agent->transport.websocket),
                     agent->transport.state, (unsigned long)media_epoch);
            post_simple_intent(agent, AGENT_NG_INT_OVERLOAD, false);
        }
    }

    agent->sender_task = NULL;
    xSemaphoreGive(agent->sender_done);
    vTaskDelete(NULL);
}

esp_err_t agent_ng_transport_init(agent_ng_handle_t agent)
{
    esp_websocket_client_config_t config = {
        .buffer_size = 8192,
        .network_timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_reconnect = true,
    };
    agent->transport.websocket = esp_websocket_client_init(&config);
    ESP_RETURN_ON_FALSE(agent->transport.websocket != NULL, ESP_ERR_NO_MEM, TAG,
                        "Initialize websocket failed");
    esp_err_t err = esp_websocket_register_events(agent->transport.websocket, WEBSOCKET_EVENT_ANY,
                                                   websocket_event, agent);
    if (err != ESP_OK) {
        esp_websocket_client_destroy(agent->transport.websocket);
        agent->transport.websocket = NULL;
        return err;
    }
    if (xTaskCreate(sender_task, "agent_ng_sender", 4096, agent, 7,
                    &agent->sender_task) != pdPASS) {
        esp_websocket_client_destroy(agent->transport.websocket);
        agent->transport.websocket = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void agent_ng_transport_deinit(agent_ng_handle_t agent)
{
    agent->lifecycle = AGENT_NG_LIFECYCLE_STOPPING;
    if (agent->sender_task != NULL && agent->sender_done != NULL) {
        agent_ng_transport_flush_media(agent);
        agent_ng_media_t *stop = NULL;
        xQueueSend(agent->outbound_ready, &stop, portMAX_DELAY);
        xSemaphoreTake(agent->sender_done, portMAX_DELAY);
    }
    if (agent->transport.websocket != NULL) {
        esp_websocket_client_stop(agent->transport.websocket);
        reset_receive_state(agent);
        esp_websocket_client_destroy(agent->transport.websocket);
        agent->transport.websocket = NULL;
    } else {
        reset_receive_state(agent);
    }
}

esp_err_t agent_ng_transport_connect(agent_ng_handle_t agent)
{
    ESP_RETURN_ON_ERROR(fetch_access_token(agent), TAG, "Refresh agent access token failed");

    char *uri = preferred_format("wss://%s/user/agents/%s/ws?token=%s",
                                 CONFIG_AGENT_NG_API_ENDPOINT, agent->agent_id,
                                 agent->transport.access_token);
    ESP_RETURN_ON_FALSE(uri != NULL, ESP_ERR_NO_MEM, TAG, "Allocate websocket URI failed");
    esp_err_t err = esp_websocket_client_set_uri(agent->transport.websocket, uri);
    free(uri);
    ESP_RETURN_ON_ERROR(err, TAG, "Configure websocket URI failed");
    return esp_websocket_client_start(agent->transport.websocket);
}

void agent_ng_transport_disconnect(agent_ng_handle_t agent)
{
    if (agent->transport.websocket != NULL) {
        esp_websocket_client_stop(agent->transport.websocket);
    }
}

void agent_ng_transport_flush_media(agent_ng_handle_t agent)
{
    xSemaphoreTake(agent->websocket_send_lock, portMAX_DELAY);
    agent->transport.media_epoch = 0;
    agent_ng_media_t *media = NULL;
    while (xQueueReceive(agent->outbound_ready, &media, 0) == pdTRUE) {
        xQueueSend(agent->outbound_free, &media, 0);
    }
    while (xQueueReceive(agent->inbound_ready, &media, 0) == pdTRUE) {
        xQueueSend(agent->inbound_free, &media, 0);
    }
    xSemaphoreGive(agent->websocket_send_lock);
}

void agent_ng_transport_set_media_epoch(agent_ng_handle_t agent, uint32_t epoch)
{
    xSemaphoreTake(agent->websocket_send_lock, portMAX_DELAY);
    agent->transport.media_epoch = epoch;
    xSemaphoreGive(agent->websocket_send_lock);
}

esp_err_t agent_ng_transport_send_control(agent_ng_handle_t agent, const char *json)
{
    ESP_RETURN_ON_FALSE((agent->transport.state == AGENT_NG_TRANSPORT_HANDSHAKING ||
                         agent->transport.state == AGENT_NG_TRANSPORT_READY) && json != NULL,
                        ESP_ERR_INVALID_STATE, TAG,
                        "Agent transport is not connected");
    ESP_RETURN_ON_FALSE(xSemaphoreTake(agent->websocket_send_lock, pdMS_TO_TICKS(2000)) == pdTRUE,
                        ESP_ERR_TIMEOUT, TAG, "Agent transport is busy");
    int sent = esp_websocket_client_send_text(agent->transport.websocket, json, strlen(json),
                                               pdMS_TO_TICKS(2000));
    xSemaphoreGive(agent->websocket_send_lock);
    return sent < 0 ? ESP_FAIL : ESP_OK;
}
