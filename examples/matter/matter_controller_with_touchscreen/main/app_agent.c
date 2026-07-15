/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <agent_ng.h>
#include <audio_ng.h>
#include <esp_board_manager_defs.h>
#include <esp_check.h>
#include <esp_log.h>
#include <sdkconfig.h>
#include <ui_main.h>

#include "app_agent.h"
#include "app_agent_tools.h"
#include "app_agent_volume.h"

static const char *TAG = "app_agent";
static audio_ng_handle_t s_audio;
static agent_ng_handle_t s_agent;

static void voice_action(bool stop, void *user_data)
{
    (void)user_data;
    esp_err_t err = stop ? agent_ng_stop_session(s_agent) : agent_ng_request_session(s_agent);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s voice session failed: %s", stop ? "Stop" : "Start", esp_err_to_name(err));
    }
}

static void agent_event(agent_ng_handle_t agent, const agent_ng_event_t *event, void *user_data)
{
    (void)agent;
    (void)user_data;

    switch (event->id) {
    case AGENT_NG_EVENT_STATE:
        ESP_LOGI(TAG, "Agent state: %d", event->data.state);
        ui_acquire();
        switch (event->data.state) {
        case AGENT_NG_STATE_READY:
            ui_main_status_bar_set_voice_state(UI_MAIN_VOICE_READY);
            break;
        case AGENT_NG_STATE_CONNECTING:
            ui_main_status_bar_set_voice_state(UI_MAIN_VOICE_WAITING);
            break;
        case AGENT_NG_STATE_PREPARING:
        case AGENT_NG_STATE_LISTENING:
        case AGENT_NG_STATE_SPEAKING:
            ui_main_status_bar_set_voice_state(UI_MAIN_VOICE_ACTIVE);
            break;
        default:
            ui_main_status_bar_set_voice_state(UI_MAIN_VOICE_HIDDEN);
            break;
        }
        ui_release();
        break;
    case AGENT_NG_EVENT_TRANSCRIPT:
        ESP_LOGI(TAG, "Agent transcript [%s%s]: %s",
                 event->data.transcript.role == AGENT_NG_ROLE_ASSISTANT ? "assistant" : "user",
                 event->data.transcript.final ? "/final" : "/partial",
                 event->data.transcript.text);
        break;
    case AGENT_NG_EVENT_TOOL:
        ESP_LOGI(TAG, "Agent tool %s: %s", event->data.tool.name,
                 event->data.tool.active ? "started" : "finished");
        break;
    case AGENT_NG_EVENT_ERROR:
        ESP_LOGW(TAG, "Agent error (%s): %s", esp_err_to_name(event->data.error.code),
                 event->data.error.message);
        break;
    case AGENT_NG_EVENT_LEVEL:
        break;
    }
}

esp_err_t app_agent_init(esp_rmaker_node_t *node)
{
    ESP_RETURN_ON_FALSE(node != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "RainMaker node is not initialized");
    ESP_RETURN_ON_FALSE(CONFIG_APP_AGENT_ID[0] != '\0', ESP_ERR_INVALID_STATE, TAG,
                        "Agent ID is not configured");

    audio_ng_config_t audio_config = {
        .input_device_name = ESP_BOARD_DEVICE_NAME_AUDIO_ADC,
        .output_device_name = ESP_BOARD_DEVICE_NAME_AUDIO_DAC,
        .input_gain_db = 30.0f,
        .initial_volume = 60,
    };
    ESP_RETURN_ON_ERROR(audio_ng_init(&audio_config, &s_audio), TAG,
                        "Initialize audio module failed");

    size_t tool_count = 0;
    const agent_ng_tool_t *tools = app_agent_tools_get(&tool_count);
    agent_ng_config_t agent_config = {
        .agent_id = CONFIG_APP_AGENT_ID,
        .audio = s_audio,
        .tools = tools,
        .tool_count = tool_count,
        .event_cb = agent_event,
    };
    esp_err_t err = agent_ng_init(&agent_config, &s_agent);
    if (err == ESP_OK) {
        err = app_agent_volume_register(node, s_audio);
    }
    if (err != ESP_OK) {
        agent_ng_deinit(s_agent);
        s_agent = NULL;
        audio_ng_deinit(s_audio);
        s_audio = NULL;
        ESP_LOGE(TAG, "Initialize agent module failed: %s", esp_err_to_name(err));
    } else {
        ui_acquire();
        ui_main_status_bar_set_voice_action_callback(voice_action, NULL);
        ui_release();
    }
    return err;
}

esp_err_t app_agent_enable(void)
{
    ESP_RETURN_ON_FALSE(s_agent != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "Voice agent is not initialized");
    esp_err_t err = agent_ng_enable(s_agent);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Enable agent module failed: %s", esp_err_to_name(err));
    }
    return err;
}
