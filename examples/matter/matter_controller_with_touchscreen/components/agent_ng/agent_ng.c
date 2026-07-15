/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <string.h>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_rmaker_auth_service.h>
#include <esp_wifi.h>

#include "agent_ng_internal.h"

static const char *TAG = "agent_ng";

static esp_err_t start_infrastructure(agent_ng_handle_t agent);

void *agent_ng_preferred_alloc(size_t size)
{
    return heap_caps_malloc_prefer(size, 2,
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

void *agent_ng_preferred_calloc(size_t count, size_t size)
{
    return heap_caps_calloc_prefer(count, size, 2,
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

char *agent_ng_preferred_strdup(const char *value)
{
    if (value == NULL) {
        return NULL;
    }
    size_t size = strlen(value) + 1;
    char *copy = agent_ng_preferred_alloc(size);
    if (copy != NULL) {
        memcpy(copy, value, size);
    }
    return copy;
}

void agent_ng_intent_free(agent_ng_intent_t *intent)
{
    free(intent->text);
    free(intent->tool_name);
    free(intent->request_id);
    cJSON_Delete(intent->arguments);
    memset(intent, 0, sizeof(*intent));
}

bool agent_ng_post(agent_ng_handle_t agent, agent_ng_intent_t *intent, bool overload_on_failure)
{
    if (agent != NULL && agent->control != NULL &&
            xQueueSend(agent->control, intent, 0) == pdTRUE) {
        memset(intent, 0, sizeof(*intent));
        return true;
    }

    agent_ng_intent_free(intent);
    if (overload_on_failure && agent != NULL && agent->control != NULL) {
        agent_ng_intent_t overload = {.id = AGENT_NG_INT_OVERLOAD};
        xQueueSend(agent->control, &overload, 0);
    }
    return false;
}

bool agent_ng_post_wait(agent_ng_handle_t agent, agent_ng_intent_t *intent)
{
    if (agent == NULL || agent->control == NULL ||
            xQueueSend(agent->control, intent, portMAX_DELAY) != pdTRUE) {
        agent_ng_intent_free(intent);
        return false;
    }
    memset(intent, 0, sizeof(*intent));
    return true;
}

static void readiness_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)data;
    agent_ng_intent_t intent = {0};

    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        intent.id = AGENT_NG_INT_NETWORK_UP;
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        intent.id = AGENT_NG_INT_NETWORK_DOWN;
    } else if (base == RMAKER_AUTH_SERVICE_EVENT &&
               (id == RMAKER_AUTH_SERVICE_EVENT_ENABLED ||
                id == RMAKER_AUTH_SERVICE_EVENT_TOKEN_RECEIVED)) {
        intent.id = AGENT_NG_INT_AUTH_READY;
    } else {
        return;
    }

    agent_ng_post(arg, &intent, false);
}

static void audio_event(audio_ng_handle_t audio, const audio_ng_event_t *event, void *arg)
{
    (void)audio;
    agent_ng_intent_t intent = {0};

    switch (event->id) {
    case AUDIO_NG_EVENT_WAKE_START:
        intent.id = AGENT_NG_INT_WAKE_START;
        break;
    case AUDIO_NG_EVENT_WAKE_END:
        intent.id = AGENT_NG_INT_WAKE_END;
        break;
    case AUDIO_NG_EVENT_VAD_START:
        intent.id = AGENT_NG_INT_VAD_START;
        break;
    case AUDIO_NG_EVENT_VAD_END:
        intent.id = AGENT_NG_INT_VAD_END;
        break;
    case AUDIO_NG_EVENT_PLAYBACK_COMPLETE:
        intent.id = AGENT_NG_INT_PLAYBACK_COMPLETE;
        break;
    case AUDIO_NG_EVENT_LEVEL:
        intent.id = AGENT_NG_INT_LEVEL;
        intent.level = event->data.level;
        break;
    case AUDIO_NG_EVENT_FAULT:
        intent.id = AGENT_NG_INT_AUDIO_FAULT;
        intent.error = event->data.error;
        break;
    }

    agent_ng_post(arg, &intent, event->id == AUDIO_NG_EVENT_FAULT);
}

static void delete_queues(agent_ng_handle_t agent)
{
    if (agent->control != NULL) {
        vQueueDelete(agent->control);
    }
    if (agent->outbound_free != NULL) {
        vQueueDelete(agent->outbound_free);
    }
    if (agent->outbound_ready != NULL) {
        vQueueDelete(agent->outbound_ready);
    }
    if (agent->inbound_free != NULL) {
        vQueueDelete(agent->inbound_free);
    }
    if (agent->inbound_ready != NULL) {
        vQueueDelete(agent->inbound_ready);
    }
}

static esp_err_t copy_tools(agent_ng_handle_t agent, const agent_ng_config_t *config)
{
    if (config->tool_count == 0) {
        return ESP_OK;
    }
    if (config->tools == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    agent->tools = agent_ng_preferred_calloc(config->tool_count, sizeof(*agent->tools));
    if (agent->tools == NULL) {
        return ESP_ERR_NO_MEM;
    }

    for (size_t i = 0; i < config->tool_count; ++i) {
        if (config->tools[i].name == NULL || config->tools[i].callback == NULL) {
            return ESP_ERR_INVALID_ARG;
        }
        agent->tools[i] = config->tools[i];
        agent->tools[i].name = agent_ng_preferred_strdup(config->tools[i].name);
        if (agent->tools[i].name == NULL) {
            return ESP_ERR_NO_MEM;
        }
        agent->tool_count++;
    }
    return ESP_OK;
}

static esp_err_t create_queues(agent_ng_handle_t agent)
{
    agent->control = xQueueCreate(AGENT_NG_CONTROL_DEPTH, sizeof(agent_ng_intent_t));
    agent->outbound_free = xQueueCreate(AGENT_NG_OUTBOUND_MEDIA_SLOTS, sizeof(agent_ng_media_t *));
    agent->outbound_ready = xQueueCreate(AGENT_NG_OUTBOUND_MEDIA_SLOTS, sizeof(agent_ng_media_t *));
    agent->inbound_free = xQueueCreate(AGENT_NG_INBOUND_MEDIA_SLOTS, sizeof(agent_ng_media_t *));
    agent->inbound_ready = xQueueCreate(AGENT_NG_INBOUND_MEDIA_SLOTS, sizeof(agent_ng_media_t *));
    agent->control_done = xSemaphoreCreateBinary();
    agent->sender_done = xSemaphoreCreateBinary();
    agent->lifecycle_done = xSemaphoreCreateBinary();
    agent->lifecycle_api_lock = xSemaphoreCreateMutex();
    agent->websocket_send_lock = xSemaphoreCreateMutex();

    agent->outbound_slots = agent_ng_preferred_calloc(AGENT_NG_OUTBOUND_MEDIA_SLOTS,
                                                       sizeof(*agent->outbound_slots));
    agent->inbound_slots = agent_ng_preferred_calloc(AGENT_NG_INBOUND_MEDIA_SLOTS,
                                                      sizeof(*agent->inbound_slots));
    agent->transport.text_buffer = agent_ng_preferred_alloc(AGENT_NG_TEXT_BYTES);

    if (agent->control == NULL || agent->outbound_free == NULL ||
            agent->outbound_ready == NULL || agent->inbound_free == NULL ||
            agent->inbound_ready == NULL || agent->control_done == NULL ||
            agent->sender_done == NULL || agent->lifecycle_done == NULL ||
            agent->lifecycle_api_lock == NULL || agent->websocket_send_lock == NULL ||
            agent->outbound_slots == NULL || agent->inbound_slots == NULL ||
            agent->transport.text_buffer == NULL) {
        return ESP_ERR_NO_MEM;
    }

    for (size_t i = 0; i < AGENT_NG_OUTBOUND_MEDIA_SLOTS; ++i) {
        agent_ng_media_t *outbound = &agent->outbound_slots[i];
        xQueueSend(agent->outbound_free, &outbound, 0);
    }
    for (size_t i = 0; i < AGENT_NG_INBOUND_MEDIA_SLOTS; ++i) {
        agent_ng_media_t *inbound = &agent->inbound_slots[i];
        xQueueSend(agent->inbound_free, &inbound, 0);
    }
    return ESP_OK;
}

esp_err_t agent_ng_init(const agent_ng_config_t *config, agent_ng_handle_t *out_agent)
{
    if (config == NULL || out_agent == NULL || config->agent_id == NULL ||
            config->agent_id[0] == '\0' || config->audio == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    agent_ng_handle_t agent = heap_caps_calloc(1, sizeof(*agent),
                                               MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (agent == NULL) {
        return ESP_ERR_NO_MEM;
    }

    agent->agent_id = agent_ng_preferred_strdup(config->agent_id);
    agent->audio = config->audio;
    agent->event_cb = config->event_cb;
    agent->event_user_data = config->event_user_data;
    agent->lifecycle = AGENT_NG_LIFECYCLE_CREATED;
    agent->transport.state = AGENT_NG_TRANSPORT_NO_NETWORK;
    agent->session.state = AGENT_NG_SESSION_IDLE;

    esp_err_t err = copy_tools(agent, config);
    if (agent->agent_id == NULL || err != ESP_OK) {
        esp_err_t create_err = agent->agent_id == NULL ? ESP_ERR_NO_MEM : err;
        agent_ng_deinit(agent);
        return create_err;
    }

    err = create_queues(agent);
    if (err != ESP_OK) {
        agent_ng_deinit(agent);
        return err;
    }

    err = start_infrastructure(agent);
    if (err != ESP_OK) {
        agent_ng_deinit(agent);
        return err;
    }
    *out_agent = agent;
    return ESP_OK;
}

static void unregister_handlers(agent_ng_handle_t agent)
{
    if (agent->auth_handler != NULL) {
        esp_event_handler_instance_unregister(RMAKER_AUTH_SERVICE_EVENT, ESP_EVENT_ANY_ID,
                                              agent->auth_handler);
        agent->auth_handler = NULL;
    }
    if (agent->wifi_handler != NULL) {
        esp_event_handler_instance_unregister(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                                              agent->wifi_handler);
        agent->wifi_handler = NULL;
    }
    if (agent->ip_handler != NULL) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, agent->ip_handler);
        agent->ip_handler = NULL;
    }
}

static esp_err_t start_infrastructure(agent_ng_handle_t agent)
{
    if (agent == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (agent->lifecycle != AGENT_NG_LIFECYCLE_CREATED) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = audio_ng_set_event_callback(agent->audio, audio_event, agent);
    if (err != ESP_OK) {
        return err;
    }
    err = agent_ng_transport_init(agent);
    if (err != ESP_OK) {
        audio_ng_set_event_callback(agent->audio, NULL, NULL);
        return err;
    }
    agent->lifecycle = AGENT_NG_LIFECYCLE_DISABLED;
    if (xTaskCreate(agent_ng_state_task, "agent_ng_control", 6144, agent, 6,
                    &agent->control_task) != pdPASS) {
        agent_ng_transport_deinit(agent);
        audio_ng_set_event_callback(agent->audio, NULL, NULL);
        return ESP_ERR_NO_MEM;
    }

    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                              readiness_event, agent, &agent->ip_handler);
    if (err == ESP_OK) {
        err = esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                                                  readiness_event, agent, &agent->wifi_handler);
    }
    if (err == ESP_OK) {
        err = esp_event_handler_instance_register(RMAKER_AUTH_SERVICE_EVENT, ESP_EVENT_ANY_ID,
                                                  readiness_event, agent, &agent->auth_handler);
    }
    if (err != ESP_OK) {
        unregister_handlers(agent);
        agent_ng_intent_t shutdown = {.id = AGENT_NG_INT_SHUTDOWN};
        agent_ng_post_wait(agent, &shutdown);
        xSemaphoreTake(agent->control_done, portMAX_DELAY);
        agent_ng_transport_deinit(agent);
        audio_ng_set_event_callback(agent->audio, NULL, NULL);
        return err;
    }

    agent_ng_intent_t start = {.id = AGENT_NG_INT_START};
    if (!agent_ng_post(agent, &start, false)) {
        ESP_LOGE(TAG, "Failed to queue start intent");
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static esp_err_t set_enabled(agent_ng_handle_t agent, bool enabled)
{
    if (agent == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xTaskGetCurrentTaskHandle() == agent->control_task) {
        ESP_LOGE(TAG, "Agent lifecycle cannot change from an agent callback");
        return ESP_ERR_INVALID_STATE;
    }

    if (agent->control_task == NULL || agent->lifecycle == AGENT_NG_LIFECYCLE_STOPPING) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(agent->lifecycle_api_lock, portMAX_DELAY);
    xSemaphoreTake(agent->lifecycle_done, 0);
    agent_ng_intent_t intent = {
        .id = enabled ? AGENT_NG_INT_ENABLE : AGENT_NG_INT_DISABLE,
    };
    if (!agent_ng_post_wait(agent, &intent)) {
        xSemaphoreGive(agent->lifecycle_api_lock);
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(agent->lifecycle_done, portMAX_DELAY);
    esp_err_t result = agent->lifecycle_result;
    xSemaphoreGive(agent->lifecycle_api_lock);
    return result;
}

esp_err_t agent_ng_enable(agent_ng_handle_t agent)
{
    return set_enabled(agent, true);
}

esp_err_t agent_ng_disable(agent_ng_handle_t agent)
{
    return set_enabled(agent, false);
}

esp_err_t agent_ng_request_session(agent_ng_handle_t agent)
{
    if (agent == NULL ||
            (agent->lifecycle != AGENT_NG_LIFECYCLE_STARTING &&
                          agent->lifecycle != AGENT_NG_LIFECYCLE_MONITORING)) {
        return ESP_ERR_INVALID_STATE;
    }
    agent_ng_intent_t intent = {.id = AGENT_NG_INT_WAKE_START};
    return agent_ng_post(agent, &intent, false) ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t agent_ng_stop_session(agent_ng_handle_t agent)
{
    if (agent == NULL ||
            (agent->lifecycle != AGENT_NG_LIFECYCLE_STARTING &&
                          agent->lifecycle != AGENT_NG_LIFECYCLE_MONITORING)) {
        return ESP_ERR_INVALID_STATE;
    }
    agent_ng_intent_t intent = {.id = AGENT_NG_INT_STOP};
    return agent_ng_post(agent, &intent, false) ? ESP_OK : ESP_ERR_TIMEOUT;
}

void agent_ng_deinit(agent_ng_handle_t agent)
{
    if (agent == NULL) {
        return;
    }

    if (xTaskGetCurrentTaskHandle() == agent->control_task) {
        ESP_LOGE(TAG, "agent_ng_deinit cannot run from an agent callback");
        return;
    }
    unregister_handlers(agent);
    if (agent->control_task != NULL) {
        agent_ng_intent_t shutdown = {.id = AGENT_NG_INT_SHUTDOWN};
        agent_ng_post_wait(agent, &shutdown);
        if (agent->control_done != NULL) {
            xSemaphoreTake(agent->control_done, portMAX_DELAY);
        }
    }

    audio_ng_stop_wake_monitoring(agent->audio);
    audio_ng_set_event_callback(agent->audio, NULL, NULL);
    agent_ng_transport_deinit(agent);

    for (size_t i = 0; i < agent->tool_count; ++i) {
        free((void *)agent->tools[i].name);
    }
    free(agent->tools);
    free(agent->agent_id);
    free(agent->transport.refresh_token);
    free(agent->transport.access_token);
    free(agent->transport.text_buffer);
    free(agent->outbound_slots);
    free(agent->inbound_slots);
    delete_queues(agent);

    if (agent->websocket_send_lock != NULL) {
        vSemaphoreDelete(agent->websocket_send_lock);
    }
    if (agent->control_done != NULL) {
        vSemaphoreDelete(agent->control_done);
    }
    if (agent->sender_done != NULL) {
        vSemaphoreDelete(agent->sender_done);
    }
    if (agent->lifecycle_done != NULL) {
        vSemaphoreDelete(agent->lifecycle_done);
    }
    if (agent->lifecycle_api_lock != NULL) {
        vSemaphoreDelete(agent->lifecycle_api_lock);
    }
    free(agent);
}
