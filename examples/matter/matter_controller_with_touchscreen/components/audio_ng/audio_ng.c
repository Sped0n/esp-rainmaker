/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <string.h>

#include <esp_audio_dec_default.h>
#include <esp_audio_enc_default.h>
#include <esp_board_manager.h>
#include <esp_board_manager_defs.h>
#include <esp_check.h>
#include <esp_gmf_afe.h>
#include <esp_gmf_afe_manager.h>
#include <esp_gmf_audio_dec.h>
#include <esp_gmf_audio_enc.h>
#include <esp_gmf_bit_cvt.h>
#include <esp_gmf_ch_cvt.h>
#include <esp_gmf_pool.h>
#include <esp_gmf_rate_cvt.h>
#include <esp_heap_caps.h>
#include <esp_log.h>

#include "audio_ng_internal.h"

static const char *TAG = "audio_ng";
static esp_gmf_pool_handle_t s_pool;

static void *preferred_alloc(size_t size)
{
    return heap_caps_malloc_prefer(size, 2,
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

static char *preferred_strdup(const char *value)
{
    size_t size = strlen(value) + 1;
    char *copy = preferred_alloc(size);
    if (copy != NULL) {
        memcpy(copy, value, size);
    }
    return copy;
}

esp_gmf_pool_handle_t audio_ng_pool_get(void)
{
    return s_pool;
}

static esp_err_t pool_register_element(esp_gmf_element_handle_t element, const char *name)
{
    if (element == NULL || esp_gmf_pool_register_element(s_pool, element, name) != ESP_GMF_ERR_OK) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t audio_ng_pool_init(void)
{
    esp_err_t ret = ESP_FAIL;
    if (s_pool != NULL) {
        return ESP_OK;
    }
    ESP_RETURN_ON_FALSE(esp_gmf_pool_init(&s_pool) == ESP_GMF_ERR_OK, ESP_FAIL, TAG, "Create GMF pool failed");

    esp_gmf_element_handle_t element = NULL;
    ESP_GOTO_ON_FALSE(esp_gmf_rate_cvt_init(NULL, &element) == ESP_GMF_ERR_OK &&
                      pool_register_element(element, NULL) == ESP_OK, ESP_FAIL, fail, TAG, "Register rate converter failed");
    element = NULL;
    ESP_GOTO_ON_FALSE(esp_gmf_bit_cvt_init(NULL, &element) == ESP_GMF_ERR_OK &&
                      pool_register_element(element, NULL) == ESP_OK, ESP_FAIL, fail, TAG, "Register bit converter failed");
    element = NULL;
    ESP_GOTO_ON_FALSE(esp_gmf_ch_cvt_init(NULL, &element) == ESP_GMF_ERR_OK &&
                      pool_register_element(element, NULL) == ESP_OK, ESP_FAIL, fail, TAG, "Register channel converter failed");
    element = NULL;
    ESP_GOTO_ON_FALSE(esp_gmf_audio_enc_init(NULL, &element) == ESP_GMF_ERR_OK &&
                      pool_register_element(element, NULL) == ESP_OK, ESP_FAIL, fail, TAG, "Register encoder failed");
    element = NULL;
    ESP_GOTO_ON_FALSE(esp_gmf_audio_dec_init(NULL, &element) == ESP_GMF_ERR_OK &&
                      pool_register_element(element, NULL) == ESP_OK, ESP_FAIL, fail, TAG, "Register decoder failed");
    ESP_GOTO_ON_FALSE(esp_audio_enc_register_default() == ESP_OK, ESP_FAIL, fail, TAG, "Register encoders failed");
    ESP_GOTO_ON_FALSE(esp_audio_dec_register_default() == ESP_OK, ESP_FAIL, fail, TAG, "Register decoders failed");
    return ESP_OK;

fail:
    esp_gmf_pool_deinit(s_pool);
    s_pool = NULL;
    return ret;
}

void audio_ng_emit(audio_ng_handle_t audio, audio_ng_event_id_t id, esp_err_t error)
{
    if (audio->event_cb == NULL) {
        return;
    }
    audio_ng_event_t event = {.id = id};
    if (id == AUDIO_NG_EVENT_LEVEL) {
        event.data.level = 0;
    } else if (id == AUDIO_NG_EVENT_FAULT) {
        event.data.error = error;
    }
    audio->event_cb(audio, &event, audio->event_user_data);
}

esp_err_t audio_ng_init(const audio_ng_config_t *config, audio_ng_handle_t *out_audio)
{
    ESP_RETURN_ON_FALSE(config != NULL && out_audio != NULL, ESP_ERR_INVALID_ARG, TAG, "Invalid init arguments");
    ESP_RETURN_ON_FALSE(config->initial_volume <= 100, ESP_ERR_INVALID_ARG, TAG, "Invalid initial volume");
    audio_ng_handle_t audio = heap_caps_calloc(1, sizeof(*audio),
                                                MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(audio != NULL, ESP_ERR_NO_MEM, TAG, "Allocate audio state failed");

    const char *input_name = config->input_device_name ? config->input_device_name : ESP_BOARD_DEVICE_NAME_AUDIO_ADC;
    const char *output_name = config->output_device_name ? config->output_device_name : ESP_BOARD_DEVICE_NAME_AUDIO_DAC;
    audio->input_device_name = preferred_strdup(input_name);
    audio->output_device_name = preferred_strdup(output_name);
    if (audio->input_device_name == NULL || audio->output_device_name == NULL) {
        audio_ng_deinit(audio);
        return ESP_ERR_NO_MEM;
    }
    audio->input_gain_db = config->input_gain_db == 0.0f ? 30.0f : config->input_gain_db;
    audio->volume = config->initial_volume == 0 ? 55 : config->initial_volume;
    audio->event_cb = config->event_cb;
    audio->event_user_data = config->event_user_data;
    audio->capture_epoch = 1;
    audio->playback_lock = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;
    if (audio_ng_pool_init() != ESP_OK) {
        audio_ng_deinit(audio);
        return ESP_FAIL;
    }
    audio->capture_storage = preferred_alloc(AUDIO_NG_CAPTURE_BUFFER_SIZE);
    audio->playback_queue_storage_bytes = preferred_alloc(AUDIO_NG_PLAYBACK_QUEUE_DEPTH *
                                                           sizeof(audio_ng_packet_t));
    if (audio->capture_storage == NULL || audio->playback_queue_storage_bytes == NULL) {
        audio_ng_deinit(audio);
        return ESP_ERR_NO_MEM;
    }
    audio->capture_messages = xMessageBufferCreateStatic(AUDIO_NG_CAPTURE_BUFFER_SIZE,
                                                          audio->capture_storage,
                                                          &audio->capture_message_storage);
    audio->capture_lock = xSemaphoreCreateMutexStatic(&audio->capture_lock_storage);
    audio->playback_api_lock = xSemaphoreCreateMutexStatic(&audio->playback_api_lock_storage);
    audio->playback_queue = xQueueCreateStatic(AUDIO_NG_PLAYBACK_QUEUE_DEPTH,
                                                sizeof(audio_ng_packet_t),
                                                audio->playback_queue_storage_bytes,
                                                &audio->playback_queue_storage);
    if (audio->capture_messages == NULL || audio->capture_lock == NULL ||
            audio->playback_api_lock == NULL ||
            audio->playback_queue == NULL) {
        audio_ng_deinit(audio);
        return ESP_ERR_NO_MEM;
    }
    *out_audio = audio;
    return ESP_OK;
}

void audio_ng_deinit(audio_ng_handle_t audio)
{
    if (audio == NULL) {
        return;
    }
    audio_ng_capture_stop(audio);
    audio_ng_playback_stop(audio);
    if (s_pool != NULL) {
        esp_gmf_pool_deinit(s_pool);
        s_pool = NULL;
    }
    audio_ng_capture_release(audio);
    free(audio->playback_queue_storage_bytes);
    free(audio->capture_storage);
    free(audio->output_device_name);
    free(audio->input_device_name);
    free(audio);
}

esp_err_t audio_ng_set_event_callback(audio_ng_handle_t audio, audio_ng_event_cb_t callback, void *user_data)
{
    ESP_RETURN_ON_FALSE(audio != NULL, ESP_ERR_INVALID_ARG, TAG, "Invalid audio handle");
    ESP_RETURN_ON_FALSE(audio->capture_state == AUDIO_NG_CAPTURE_STOPPED,
                        ESP_ERR_INVALID_STATE, TAG,
                        "Audio callback cannot change after wake monitoring starts");
    audio->event_cb = callback;
    audio->event_user_data = user_data;
    return ESP_OK;
}

esp_err_t audio_ng_start_wake_monitoring(audio_ng_handle_t audio)
{
    ESP_RETURN_ON_FALSE(audio != NULL, ESP_ERR_INVALID_ARG, TAG, "Invalid audio handle");
    ESP_RETURN_ON_ERROR(audio_ng_playback_prepare(audio), TAG, "Prepare output before capture failed");
    esp_err_t err = audio_ng_capture_start(audio);
    if (err != ESP_OK) {
        audio_ng_playback_stop(audio);
    }
    return err;
}

esp_err_t audio_ng_stop_wake_monitoring(audio_ng_handle_t audio)
{
    ESP_RETURN_ON_FALSE(audio != NULL, ESP_ERR_INVALID_ARG, TAG, "Invalid audio handle");
    audio_ng_capture_stop(audio);
    audio_ng_playback_stop(audio);
    return ESP_OK;
}

esp_err_t audio_ng_prepare_output(audio_ng_handle_t audio)
{
    ESP_RETURN_ON_FALSE(audio != NULL, ESP_ERR_INVALID_ARG, TAG, "Invalid audio handle");
    return audio_ng_playback_prepare(audio);
}

esp_err_t audio_ng_get_volume(audio_ng_handle_t audio, uint8_t *out_volume)
{
    ESP_RETURN_ON_FALSE(audio != NULL && out_volume != NULL, ESP_ERR_INVALID_ARG, TAG, "Invalid volume arguments");
    xSemaphoreTake(audio->playback_api_lock, portMAX_DELAY);
    *out_volume = audio->volume;
    xSemaphoreGive(audio->playback_api_lock);
    return ESP_OK;
}

esp_err_t audio_ng_set_volume(audio_ng_handle_t audio, uint8_t volume)
{
    ESP_RETURN_ON_FALSE(audio != NULL && volume <= 100, ESP_ERR_INVALID_ARG, TAG, "Invalid volume");
    xSemaphoreTake(audio->playback_api_lock, portMAX_DELAY);
    if (audio->output != NULL && esp_codec_dev_set_out_vol(audio->output, volume) != ESP_CODEC_DEV_OK) {
        xSemaphoreGive(audio->playback_api_lock);
        return ESP_FAIL;
    }
    audio->volume = volume;
    xSemaphoreGive(audio->playback_api_lock);
    return ESP_OK;
}
