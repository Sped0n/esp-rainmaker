/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include <dev_audio_codec.h>
#include <esp_afe_config.h>
#include <esp_afe_sr_models.h>
#include <esp_board_manager.h>
#include <esp_check.h>
#include <esp_gmf_afe.h>
#include <esp_gmf_afe_manager.h>
#include <esp_gmf_audio_enc.h>
#include <esp_gmf_new_databus.h>
#include <esp_gmf_pool.h>
#include <esp_log.h>
#include <esp_opus_enc.h>
#include <model_path.h>

#include "audio_ng_internal.h"

static const char *TAG = "audio_ng_capture";

static void afe_event(esp_gmf_obj_handle_t object, esp_gmf_afe_evt_t *event, void *context)
{
    (void)object;
    audio_ng_handle_t audio = context;
    if (event->type == ESP_GMF_AFE_EVT_WAKEUP_START) {
        audio_ng_emit(audio, AUDIO_NG_EVENT_WAKE_START, ESP_OK);
    } else if (event->type == ESP_GMF_AFE_EVT_WAKEUP_END) {
        audio_ng_emit(audio, AUDIO_NG_EVENT_WAKE_END, ESP_OK);
    } else if (event->type == ESP_GMF_AFE_EVT_VAD_START) {
        audio_ng_emit(audio, AUDIO_NG_EVENT_VAD_START, ESP_OK);
    } else if (event->type == ESP_GMF_AFE_EVT_VAD_END) {
        audio_ng_emit(audio, AUDIO_NG_EVENT_VAD_END, ESP_OK);
    }
}

static esp_err_t capture_set_awake(audio_ng_handle_t audio, bool awake)
{
    esp_gmf_element_handle_t afe = NULL;
    ESP_RETURN_ON_FALSE(audio->capture_pipeline != NULL &&
                        esp_gmf_pipeline_get_el_by_name(audio->capture_pipeline, "ai_afe", &afe) == ESP_GMF_ERR_OK,
                        ESP_ERR_INVALID_STATE, TAG, "AFE element unavailable");
    ESP_RETURN_ON_FALSE(esp_gmf_afe_keep_awake(afe, awake) == ESP_GMF_ERR_OK,
                        ESP_FAIL, TAG, "Set AFE wake state failed");
    if (!awake) {
        ESP_RETURN_ON_FALSE(esp_gmf_afe_trigger_sleep(afe) == ESP_GMF_ERR_OK,
                            ESP_FAIL, TAG, "Put AFE to sleep failed");
    }
    return ESP_OK;
}

static esp_gmf_err_io_t capture_read(void *context, esp_gmf_data_bus_block_t *block, int wanted, int ticks)
{
    (void)ticks;
    audio_ng_handle_t audio = context;
    if (esp_codec_dev_read(audio->input, block->buf, wanted) != ESP_CODEC_DEV_OK) {
        return ESP_FAIL;
    }
    block->valid_size = wanted;
    return ESP_GMF_IO_OK;
}

static esp_gmf_err_io_t capture_write(void *context, esp_gmf_data_bus_block_t *block, int ticks)
{
    audio_ng_handle_t audio = context;
    if (xSemaphoreTake(audio->capture_lock, 0) != pdTRUE) {
        return ESP_GMF_IO_OK;
    }
    if (audio->capture_state != AUDIO_NG_CAPTURE_STREAMING) {
        xSemaphoreGive(audio->capture_lock);
        return ESP_GMF_IO_OK;
    }
    size_t written = xMessageBufferSend(audio->capture_messages, block->buf, block->valid_size, 0);
    while (written != (size_t)block->valid_size) {
        uint8_t dropped[AUDIO_NG_MAX_OPUS_PACKET];
        size_t dropped_size = xMessageBufferReceive(audio->capture_messages, dropped, sizeof(dropped), 0);
        if (dropped_size == 0) {
            break;
        }
        if (audio->capture_dropped_frames++ == 0) {
            ESP_LOGW(TAG, "Capture backlog full; dropping oldest encoded frame");
        }
        written = xMessageBufferSend(audio->capture_messages, block->buf, block->valid_size, 0);
    }
    xSemaphoreGive(audio->capture_lock);
    if (written != (size_t)block->valid_size) {
        ESP_LOGE(TAG, "Capture backlog invariant failed");
        audio_ng_emit(audio, AUDIO_NG_EVENT_FAULT, ESP_ERR_NO_MEM);
    }
    return ESP_GMF_IO_OK;
}

static esp_err_t capture_register_afe(audio_ng_handle_t audio, esp_gmf_pool_handle_t pool)
{
    if (audio->afe_manager != NULL) {
        return ESP_OK;
    }

    esp_err_t ret = ESP_FAIL;
    esp_gmf_afe_manager_handle_t manager = NULL;
    esp_gmf_element_handle_t element = NULL;
    afe_config_t *afe = NULL;
    srmodel_list_t *models = esp_srmodel_init("model");
    ESP_GOTO_ON_FALSE(models != NULL, ESP_ERR_NOT_FOUND, fail, TAG, "Speech models unavailable");
    afe = afe_config_init("MM", models, AFE_TYPE_SR, AFE_MODE_HIGH_PERF);
    ESP_GOTO_ON_FALSE(afe != NULL, ESP_ERR_NO_MEM, fail, TAG, "Create AFE config failed");
    afe->wakenet_init = true;
    afe->vad_init = true;
    afe->agc_init = true;
    afe->se_init = false;
    afe->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
    esp_gmf_afe_manager_cfg_t manager_cfg = DEFAULT_GMF_AFE_MANAGER_CFG(afe, NULL, NULL, NULL, NULL);
    manager_cfg.feed_task_setting.core = 1;
    manager_cfg.fetch_task_setting.core = 0;
    ESP_GOTO_ON_FALSE(esp_gmf_afe_manager_create(&manager_cfg, &manager) == ESP_GMF_ERR_OK,
                      ESP_FAIL, fail, TAG, "Create AFE manager failed");
    esp_gmf_afe_cfg_t cfg = DEFAULT_GMF_AFE_CFG(manager, afe_event, audio, models);
    ESP_GOTO_ON_FALSE(esp_gmf_afe_init(&cfg, &element) == ESP_GMF_ERR_OK,
                      ESP_FAIL, fail, TAG, "Create AFE element failed");
    ESP_GOTO_ON_FALSE(esp_gmf_pool_register_element(pool, element, "ai_afe") == ESP_GMF_ERR_OK,
                      ESP_FAIL, fail, TAG, "Register AFE element failed");
    audio->afe_manager = manager;
    audio->afe_models = models;
    audio->afe_config = afe;
    return ESP_OK;

fail:
    if (element != NULL) {
        esp_gmf_obj_delete(element);
    }
    if (manager != NULL) {
        esp_gmf_afe_manager_destroy(manager);
    }
    if (afe != NULL) {
        afe_config_free(afe);
    }
    if (models != NULL) {
        esp_srmodel_deinit(models);
    }
    return ret;
}

static esp_err_t capture_pipeline_start(audio_ng_handle_t audio, esp_gmf_pool_handle_t pool)
{
    ESP_RETURN_ON_ERROR(capture_register_afe(audio, pool), TAG, "Set up AFE failed");
    const char *elements[] = {"ai_afe", "aud_enc"};
    ESP_RETURN_ON_FALSE(esp_gmf_pool_new_pipeline(pool, NULL, elements, 2, NULL, &audio->capture_pipeline) == ESP_GMF_ERR_OK,
                        ESP_FAIL, TAG, "Create capture pipeline failed");
    esp_gmf_port_handle_t input = NEW_ESP_GMF_PORT_IN_BYTE(capture_read, NULL, NULL, audio, 2048, portMAX_DELAY);
    esp_gmf_port_handle_t output = NEW_ESP_GMF_PORT_OUT_BYTE(NULL, capture_write, NULL, audio, 2048, portMAX_DELAY);
    ESP_RETURN_ON_FALSE(esp_gmf_pipeline_reg_el_port(audio->capture_pipeline, "ai_afe", ESP_GMF_IO_DIR_READER, input) == ESP_GMF_ERR_OK &&
                        esp_gmf_pipeline_reg_el_port(audio->capture_pipeline, "aud_enc", ESP_GMF_IO_DIR_WRITER, output) == ESP_GMF_ERR_OK,
                        ESP_FAIL, TAG, "Register capture ports failed");
    esp_gmf_element_handle_t encoder = NULL;
    ESP_RETURN_ON_FALSE(esp_gmf_pipeline_get_el_by_name(audio->capture_pipeline, "aud_enc", &encoder) == ESP_GMF_ERR_OK,
                        ESP_FAIL, TAG, "Find encoder failed");
    esp_opus_enc_config_t opus = ESP_OPUS_ENC_CONFIG_DEFAULT();
    opus.sample_rate = 8000;
    opus.channel = 1;
    opus.bits_per_sample = 16;
    opus.frame_duration = ESP_OPUS_ENC_FRAME_DURATION_60_MS;
    opus.application_mode = ESP_OPUS_ENC_APPLICATION_VOIP;
    opus.enable_dtx = true;
    opus.enable_vbr = true;
    esp_audio_enc_config_t encoder_cfg = {.type = ESP_AUDIO_TYPE_OPUS, .cfg = &opus, .cfg_sz = sizeof(opus)};
    ESP_RETURN_ON_FALSE(esp_gmf_audio_enc_reconfig(encoder, &encoder_cfg) == ESP_GMF_ERR_OK, ESP_FAIL, TAG, "Configure Opus encoder failed");
    esp_gmf_info_sound_t info = {.sample_rates = 8000, .bits = 16, .channels = 2};
    ESP_RETURN_ON_FALSE(esp_gmf_pipeline_report_info(audio->capture_pipeline, ESP_GMF_INFO_SOUND, &info, sizeof(info)) == ESP_GMF_ERR_OK,
                        ESP_FAIL, TAG, "Report capture format failed");
    esp_gmf_task_cfg_t task_cfg = DEFAULT_ESP_GMF_TASK_CONFIG();
    task_cfg.name = "audio_ng_capture";
    task_cfg.thread.stack_in_ext = true;
    task_cfg.thread.stack = 32 * 1024;
    task_cfg.thread.core = 1;
    ESP_RETURN_ON_FALSE(esp_gmf_task_init(&task_cfg, &audio->capture_task) == ESP_GMF_ERR_OK, ESP_ERR_NO_MEM, TAG, "Create capture task failed");
    ESP_RETURN_ON_FALSE(esp_gmf_pipeline_bind_task(audio->capture_pipeline, audio->capture_task) == ESP_GMF_ERR_OK &&
                        esp_gmf_pipeline_loading_jobs(audio->capture_pipeline) == ESP_GMF_ERR_OK &&
                        esp_gmf_pipeline_run(audio->capture_pipeline) == ESP_GMF_ERR_OK,
                        ESP_FAIL, TAG, "Start capture pipeline failed");
    return ESP_OK;
}

esp_err_t audio_ng_capture_start(audio_ng_handle_t audio)
{
    if (audio->capture_state != AUDIO_NG_CAPTURE_STOPPED) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(esp_board_manager_init_device_by_name(audio->input_device_name), TAG, "Initialize input device failed");
    dev_audio_codec_handles_t *handles = NULL;
    ESP_RETURN_ON_ERROR(esp_board_manager_get_device_handle(audio->input_device_name, (void **)&handles), TAG, "Get input device failed");
    ESP_RETURN_ON_FALSE(handles != NULL && handles->codec_dev != NULL, ESP_ERR_NOT_FOUND, TAG, "Input codec unavailable");
    audio->input = handles->codec_dev;
    esp_codec_dev_sample_info_t input_info = {.sample_rate = 16000, .channel = 2, .bits_per_sample = 16};
    ESP_RETURN_ON_FALSE(esp_codec_dev_open(audio->input, &input_info) == ESP_CODEC_DEV_OK, ESP_FAIL, TAG, "Open input codec failed");
    if (esp_codec_dev_set_in_gain(audio->input, audio->input_gain_db) != ESP_CODEC_DEV_OK) {
        esp_codec_dev_close(audio->input);
        audio->input = NULL;
        return ESP_FAIL;
    }
    esp_err_t err = capture_pipeline_start(audio, audio_ng_pool_get());
    if (err != ESP_OK) {
        audio_ng_capture_stop(audio);
        audio_ng_emit(audio, AUDIO_NG_EVENT_FAULT, err);
        return err;
    }
    audio->capture_state = AUDIO_NG_CAPTURE_MONITORING;
    return ESP_OK;
}

void audio_ng_capture_stop(audio_ng_handle_t audio)
{
    if (audio->capture_task != NULL) {
        esp_gmf_task_deinit(audio->capture_task);
    }
    if (audio->capture_pipeline != NULL) {
        esp_gmf_pipeline_destroy(audio->capture_pipeline);
    }
    if (audio->input != NULL) {
        esp_codec_dev_close(audio->input);
    }
    audio->capture_task = NULL;
    audio->capture_pipeline = NULL;
    audio->input = NULL;
    audio->capture_state = AUDIO_NG_CAPTURE_STOPPED;
}

void audio_ng_capture_release(audio_ng_handle_t audio)
{
    if (audio->afe_manager != NULL) {
        esp_gmf_afe_manager_destroy(audio->afe_manager);
    }
    if (audio->afe_models != NULL) {
        esp_srmodel_deinit(audio->afe_models);
    }
    if (audio->afe_config != NULL) {
        afe_config_free(audio->afe_config);
    }
    audio->afe_manager = NULL;
    audio->afe_models = NULL;
    audio->afe_config = NULL;
}

esp_err_t audio_ng_capture_begin(audio_ng_handle_t audio, uint32_t *out_epoch)
{
    ESP_RETURN_ON_FALSE(audio != NULL && out_epoch != NULL, ESP_ERR_INVALID_ARG, TAG, "Invalid capture arguments");
    ESP_RETURN_ON_FALSE(audio->capture_state == AUDIO_NG_CAPTURE_MONITORING,
                        ESP_ERR_INVALID_STATE, TAG, "Wake monitoring is not idle");
    ESP_RETURN_ON_ERROR(capture_set_awake(audio, true), TAG, "Keep AFE awake failed");
    xSemaphoreTake(audio->capture_lock, portMAX_DELAY);
    xMessageBufferReset(audio->capture_messages);
    audio->capture_dropped_frames = 0;
    audio->capture_state = AUDIO_NG_CAPTURE_STREAMING;
    *out_epoch = ++audio->capture_epoch;
    xSemaphoreGive(audio->capture_lock);
    return ESP_OK;
}

esp_err_t audio_ng_capture_end(audio_ng_handle_t audio)
{
    ESP_RETURN_ON_FALSE(audio != NULL, ESP_ERR_INVALID_ARG, TAG, "Invalid audio handle");
    xSemaphoreTake(audio->capture_lock, portMAX_DELAY);
    if (audio->capture_state == AUDIO_NG_CAPTURE_STREAMING) {
        audio->capture_state = AUDIO_NG_CAPTURE_MONITORING;
    }
    xMessageBufferReset(audio->capture_messages);
    ++audio->capture_epoch;
    xSemaphoreGive(audio->capture_lock);
    return capture_set_awake(audio, false);
}

esp_err_t audio_ng_capture_read(audio_ng_handle_t audio, uint32_t epoch, uint8_t *data, size_t capacity, size_t *out_len, uint32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(audio != NULL && data != NULL && capacity != 0 && out_len != NULL, ESP_ERR_INVALID_ARG, TAG, "Invalid read arguments");
    *out_len = 0;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    do {
        xSemaphoreTake(audio->capture_lock, portMAX_DELAY);
        bool valid = audio->capture_state == AUDIO_NG_CAPTURE_STREAMING &&
                     epoch == audio->capture_epoch;
        size_t read = valid ? xMessageBufferReceive(audio->capture_messages, data, capacity, 0) : 0;
        xSemaphoreGive(audio->capture_lock);
        if (!valid) {
            return ESP_ERR_INVALID_STATE;
        }
        if (read != 0) {
            *out_len = read;
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    } while ((int32_t)(deadline - xTaskGetTickCount()) > 0);
    return ESP_ERR_TIMEOUT;
}
