/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include <dev_audio_codec.h>
#include <esp_board_manager.h>
#include <esp_check.h>
#include <esp_gmf_audio_dec.h>
#include <esp_gmf_bit_cvt.h>
#include <esp_gmf_ch_cvt.h>
#include <esp_gmf_new_databus.h>
#include <esp_gmf_pool.h>
#include <esp_gmf_rate_cvt.h>
#include <esp_log.h>
#include <esp_opus_dec.h>

#include "audio_ng_internal.h"

static const char *TAG = "audio_ng_playback";
#define AUDIO_NG_OPUS_FRAME_PCM_BYTES (16000 * 60 / 1000 * 2 * 4)
#define AUDIO_NG_DMA_TAIL_MS 80

extern const uint8_t wakeup_opus_start[] asm("_binary_wakeup_opus_start");
extern const uint8_t wakeup_opus_end[] asm("_binary_wakeup_opus_end");
extern const uint8_t stop_opus_start[] asm("_binary_stop_opus_start");
extern const uint8_t stop_opus_end[] asm("_binary_stop_opus_end");

static esp_gmf_err_io_t playback_read(void *context, esp_gmf_data_bus_block_t *block, int wanted, int ticks)
{
    (void)wanted;
    (void)ticks;
    audio_ng_handle_t audio = context;
    audio_ng_packet_t packet;
    portENTER_CRITICAL(&audio->playback_lock);
    bool ready = audio->playback_state == AUDIO_NG_PLAYBACK_READY;
    portEXIT_CRITICAL(&audio->playback_lock);
    if (!ready || uxQueueMessagesWaiting(audio->playback_queue) == 0) {
        vTaskDelay(pdMS_TO_TICKS(1));
        block->valid_size = 0;
        return ESP_GMF_IO_OK;
    }
    portENTER_CRITICAL(&audio->playback_lock);
    audio->playback_inflight++;
    portEXIT_CRITICAL(&audio->playback_lock);
    if (xQueueReceive(audio->playback_queue, &packet, 0) != pdTRUE) {
        portENTER_CRITICAL(&audio->playback_lock);
        audio->playback_inflight--;
        portEXIT_CRITICAL(&audio->playback_lock);
        block->valid_size = 0;
        return ESP_GMF_IO_OK;
    }
    memcpy(block->buf, packet.data, packet.len);
    block->valid_size = packet.len;
    portENTER_CRITICAL(&audio->playback_lock);
    audio->playback_output_bytes += AUDIO_NG_OPUS_FRAME_PCM_BYTES;
    portEXIT_CRITICAL(&audio->playback_lock);
    return ESP_GMF_IO_OK;
}

static esp_gmf_err_io_t playback_write(void *context, esp_gmf_data_bus_block_t *block, int ticks)
{
    (void)ticks;
    audio_ng_handle_t audio = context;
    portENTER_CRITICAL(&audio->playback_lock);
    bool aborting = audio->playback_state == AUDIO_NG_PLAYBACK_ABORTING;
    portEXIT_CRITICAL(&audio->playback_lock);
    if (!aborting && esp_codec_dev_write(audio->output, block->buf, block->valid_size) != ESP_CODEC_DEV_OK) {
        audio_ng_emit(audio, AUDIO_NG_EVENT_FAULT, ESP_FAIL);
        return ESP_FAIL;
    }
    bool complete = false;
    portENTER_CRITICAL(&audio->playback_lock);
    if (audio->playback_output_bytes <= (uint32_t)block->valid_size) {
        audio->playback_output_bytes = 0;
        if (audio->playback_inflight != 0) {
            audio->playback_inflight--;
        }
        complete = audio->playback_state == AUDIO_NG_PLAYBACK_READY &&
                   audio->playback_inflight == 0 &&
                   uxQueueMessagesWaiting(audio->playback_queue) == 0;
    } else {
        audio->playback_output_bytes -= block->valid_size;
    }
    portEXIT_CRITICAL(&audio->playback_lock);
    if (complete) {
        audio_ng_emit(audio, AUDIO_NG_EVENT_PLAYBACK_COMPLETE, ESP_OK);
    }
    return ESP_GMF_IO_OK;
}

static esp_err_t playback_pipeline_start(audio_ng_handle_t audio)
{
    const char *elements[] = {"aud_dec", "aud_rate_cvt", "aud_bit_cvt", "aud_ch_cvt"};
    esp_gmf_pool_handle_t pool = audio_ng_pool_get();
    ESP_RETURN_ON_FALSE(pool != NULL && esp_gmf_pool_new_pipeline(pool, NULL, elements, 4, NULL, &audio->playback_pipeline) == ESP_GMF_ERR_OK,
                        ESP_FAIL, TAG, "Create playback pipeline failed");
    esp_gmf_port_handle_t input = NEW_ESP_GMF_PORT_IN_BYTE(playback_read, NULL, NULL, audio, 2048, portMAX_DELAY);
    esp_gmf_port_handle_t output = NEW_ESP_GMF_PORT_OUT_BYTE(NULL, playback_write, NULL, audio, 2048, portMAX_DELAY);
    ESP_RETURN_ON_FALSE(esp_gmf_pipeline_reg_el_port(audio->playback_pipeline, "aud_dec", ESP_GMF_IO_DIR_READER, input) == ESP_GMF_ERR_OK &&
                        esp_gmf_pipeline_reg_el_port(audio->playback_pipeline, "aud_ch_cvt", ESP_GMF_IO_DIR_WRITER, output) == ESP_GMF_ERR_OK,
                        ESP_FAIL, TAG, "Register playback ports failed");
    esp_gmf_element_handle_t element = NULL;
    ESP_RETURN_ON_FALSE(esp_gmf_pipeline_get_el_by_name(audio->playback_pipeline, "aud_dec", &element) == ESP_GMF_ERR_OK, ESP_FAIL, TAG, "Find decoder failed");
    esp_opus_dec_cfg_t opus = {.channel = ESP_AUDIO_MONO, .frame_duration = ESP_OPUS_DEC_FRAME_DURATION_60_MS,
                               .self_delimited = false, .sample_rate = 16000};
    esp_audio_simple_dec_cfg_t decoder_cfg = {
        .dec_type = ESP_AUDIO_TYPE_OPUS,
        .dec_cfg = &opus,
        .cfg_size = sizeof(opus),
        .use_frame_dec = true,
    };
    ESP_RETURN_ON_FALSE(esp_gmf_audio_dec_reconfig(element, &decoder_cfg) == ESP_GMF_ERR_OK, ESP_FAIL, TAG, "Configure Opus decoder failed");
    ESP_RETURN_ON_FALSE(esp_gmf_pipeline_get_el_by_name(audio->playback_pipeline, "aud_rate_cvt", &element) == ESP_GMF_ERR_OK &&
                        esp_gmf_rate_cvt_set_dest_rate(element, 16000) == ESP_GMF_ERR_OK, ESP_FAIL, TAG, "Configure rate converter failed");
    ESP_RETURN_ON_FALSE(esp_gmf_pipeline_get_el_by_name(audio->playback_pipeline, "aud_bit_cvt", &element) == ESP_GMF_ERR_OK &&
                        esp_gmf_bit_cvt_set_dest_bits(element, 32) == ESP_GMF_ERR_OK, ESP_FAIL, TAG, "Configure bit converter failed");
    ESP_RETURN_ON_FALSE(esp_gmf_pipeline_get_el_by_name(audio->playback_pipeline, "aud_ch_cvt", &element) == ESP_GMF_ERR_OK &&
                        esp_gmf_ch_cvt_set_dest_channel(element, 2) == ESP_GMF_ERR_OK, ESP_FAIL, TAG, "Configure channel converter failed");
    esp_gmf_info_sound_t info = {.sample_rates = 16000, .bits = 16, .channels = 1, .format_id = ESP_AUDIO_TYPE_OPUS};
    ESP_RETURN_ON_FALSE(esp_gmf_pipeline_report_info(audio->playback_pipeline, ESP_GMF_INFO_SOUND, &info, sizeof(info)) == ESP_GMF_ERR_OK,
                        ESP_FAIL, TAG, "Report playback format failed");
    esp_gmf_task_cfg_t task_cfg = DEFAULT_ESP_GMF_TASK_CONFIG();
    task_cfg.name = "audio_ng_playback";
    task_cfg.thread.stack_in_ext = true;
    task_cfg.thread.stack = 40 * 1024;
    task_cfg.thread.prio = 6;
    ESP_RETURN_ON_FALSE(esp_gmf_task_init(&task_cfg, &audio->playback_task) == ESP_GMF_ERR_OK, ESP_ERR_NO_MEM, TAG, "Create playback task failed");
    ESP_RETURN_ON_FALSE(esp_gmf_pipeline_bind_task(audio->playback_pipeline, audio->playback_task) == ESP_GMF_ERR_OK &&
                        esp_gmf_pipeline_loading_jobs(audio->playback_pipeline) == ESP_GMF_ERR_OK &&
                        esp_gmf_pipeline_run(audio->playback_pipeline) == ESP_GMF_ERR_OK,
                        ESP_FAIL, TAG, "Start playback pipeline failed");
    return ESP_OK;
}

esp_err_t audio_ng_playback_prepare(audio_ng_handle_t audio)
{
    esp_err_t ret = ESP_OK;
    bool output_opened = false;
    xSemaphoreTake(audio->playback_api_lock, portMAX_DELAY);
    if (audio->playback_state == AUDIO_NG_PLAYBACK_READY) {
        xSemaphoreGive(audio->playback_api_lock);
        return ESP_OK;
    }
    if (audio->playback_state == AUDIO_NG_PLAYBACK_ABORTING) {
        xSemaphoreGive(audio->playback_api_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (audio->output == NULL) {
        ESP_GOTO_ON_ERROR(esp_board_manager_init_device_by_name(audio->output_device_name), fail,
                          TAG, "Initialize output device failed");
        dev_audio_codec_handles_t *handles = NULL;
        ESP_GOTO_ON_ERROR(esp_board_manager_get_device_handle(audio->output_device_name,
                                                              (void **)&handles),
                          fail, TAG, "Get output device failed");
        ESP_GOTO_ON_FALSE(handles != NULL && handles->codec_dev != NULL,
                          ESP_ERR_NOT_FOUND, fail, TAG, "Output codec unavailable");
        esp_codec_dev_sample_info_t output_info = {.sample_rate = 16000, .channel = 2, .bits_per_sample = 32};
        ESP_GOTO_ON_FALSE(esp_codec_dev_open(handles->codec_dev, &output_info) == ESP_CODEC_DEV_OK,
                          ESP_FAIL, fail, TAG, "Open output codec failed");
        audio->output = handles->codec_dev;
        output_opened = true;
    }
    if (esp_codec_dev_set_out_vol(audio->output, audio->volume) != ESP_CODEC_DEV_OK) {
        ret = ESP_FAIL;
        goto fail;
    }
    esp_err_t err = playback_pipeline_start(audio);
    if (err != ESP_OK) {
        ret = err;
        goto fail;
    }
    portENTER_CRITICAL(&audio->playback_lock);
    audio->playback_state = AUDIO_NG_PLAYBACK_READY;
    portEXIT_CRITICAL(&audio->playback_lock);
    xSemaphoreGive(audio->playback_api_lock);
    return ESP_OK;

fail:
    if (audio->playback_task != NULL) {
        esp_gmf_task_deinit(audio->playback_task);
    }
    if (audio->playback_pipeline != NULL) {
        esp_gmf_pipeline_destroy(audio->playback_pipeline);
    }
    if (output_opened) {
        esp_codec_dev_close(audio->output);
        audio->output = NULL;
    }
    audio->playback_task = NULL;
    audio->playback_pipeline = NULL;
    portENTER_CRITICAL(&audio->playback_lock);
    audio->playback_state = AUDIO_NG_PLAYBACK_CLOSED;
    portEXIT_CRITICAL(&audio->playback_lock);
    xSemaphoreGive(audio->playback_api_lock);
    audio_ng_emit(audio, AUDIO_NG_EVENT_FAULT, ret);
    return ret;
}

static void playback_stop(audio_ng_handle_t audio, bool close_output)
{
    if (audio->playback_api_lock != NULL) {
        xSemaphoreTake(audio->playback_api_lock, portMAX_DELAY);
    }
    portENTER_CRITICAL(&audio->playback_lock);
    audio->playback_state = AUDIO_NG_PLAYBACK_ABORTING;
    audio->playback_inflight = 0;
    audio->playback_output_bytes = 0;
    portEXIT_CRITICAL(&audio->playback_lock);
    if (audio->playback_task != NULL) {
        esp_gmf_task_deinit(audio->playback_task);
    }
    if (audio->playback_pipeline != NULL) {
        esp_gmf_pipeline_destroy(audio->playback_pipeline);
    }
    if (close_output && audio->output != NULL) {
        esp_codec_dev_close(audio->output);
        audio->output = NULL;
    }
    audio->playback_task = NULL;
    audio->playback_pipeline = NULL;
    portENTER_CRITICAL(&audio->playback_lock);
    audio->playback_state = AUDIO_NG_PLAYBACK_CLOSED;
    portEXIT_CRITICAL(&audio->playback_lock);
    if (audio->playback_queue != NULL) {
        xQueueReset(audio->playback_queue);
    }
    if (audio->playback_api_lock != NULL) {
        xSemaphoreGive(audio->playback_api_lock);
    }
}

void audio_ng_playback_stop(audio_ng_handle_t audio)
{
    playback_stop(audio, true);
}

esp_err_t audio_ng_playback_enqueue(audio_ng_handle_t audio, const uint8_t *data, size_t len, uint32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(data != NULL && len != 0 && len <= AUDIO_NG_MAX_OPUS_PACKET, ESP_ERR_INVALID_SIZE, TAG, "Invalid Opus packet");
    ESP_RETURN_ON_FALSE(xSemaphoreTake(audio->playback_api_lock,
                                       pdMS_TO_TICKS(timeout_ms)) == pdTRUE,
                        ESP_ERR_TIMEOUT, TAG, "Playback is busy");
    if (audio->playback_state != AUDIO_NG_PLAYBACK_READY) {
        xSemaphoreGive(audio->playback_api_lock);
        return ESP_ERR_INVALID_STATE;
    }
    audio_ng_packet_t packet = {.len = len};
    memcpy(packet.data, data, len);
    if (xQueueSend(audio->playback_queue, &packet, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        xSemaphoreGive(audio->playback_api_lock);
        return ESP_ERR_TIMEOUT;
    }
    xSemaphoreGive(audio->playback_api_lock);
    return ESP_OK;
}

esp_err_t audio_ng_response_write(audio_ng_handle_t audio, const uint8_t *data, size_t len, uint32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(audio != NULL, ESP_ERR_INVALID_ARG, TAG, "Invalid audio handle");
    ESP_RETURN_ON_ERROR(audio_ng_prepare_output(audio), TAG, "Prepare output failed");
    return audio_ng_playback_enqueue(audio, data, len, timeout_ms);
}

esp_err_t audio_ng_playback_wait(audio_ng_handle_t audio, uint32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(audio != NULL, ESP_ERR_INVALID_ARG, TAG, "Invalid audio handle");
    portENTER_CRITICAL(&audio->playback_lock);
    bool ready = audio->playback_state == AUDIO_NG_PLAYBACK_READY;
    portEXIT_CRITICAL(&audio->playback_lock);
    ESP_RETURN_ON_FALSE(ready, ESP_ERR_INVALID_STATE, TAG, "Output is not prepared");
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (true) {
        portENTER_CRITICAL(&audio->playback_lock);
        bool pending = audio->playback_inflight != 0;
        portEXIT_CRITICAL(&audio->playback_lock);
        if (uxQueueMessagesWaiting(audio->playback_queue) == 0 && !pending) {
            break;
        }
        if ((int32_t)(deadline - xTaskGetTickCount()) <= 0) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    TickType_t remaining = deadline - xTaskGetTickCount();
    if ((int32_t)remaining <= 0 || remaining < pdMS_TO_TICKS(AUDIO_NG_DMA_TAIL_MS)) {
        return ESP_ERR_TIMEOUT;
    }
    vTaskDelay(pdMS_TO_TICKS(AUDIO_NG_DMA_TAIL_MS));
    return ESP_OK;
}

esp_err_t audio_ng_playback_abort(audio_ng_handle_t audio)
{
    ESP_RETURN_ON_FALSE(audio != NULL, ESP_ERR_INVALID_ARG, TAG, "Invalid audio handle");
    /* GMF has no safe running-pipeline flush operation; tear down its task before dropping queued packets. */
    playback_stop(audio, false);
    return ESP_OK;
}

static esp_err_t play_cue(audio_ng_handle_t audio, const uint8_t *start, const uint8_t *end)
{
    ESP_RETURN_ON_FALSE(audio != NULL, ESP_ERR_INVALID_ARG, TAG, "Invalid audio handle");
    ESP_RETURN_ON_ERROR(audio_ng_prepare_output(audio), TAG, "Prepare output failed");
    const uint8_t *cursor = start;
    while (cursor < end) {
        ESP_RETURN_ON_FALSE((size_t)(end - cursor) >= 2, ESP_ERR_INVALID_SIZE, TAG, "Malformed embedded cue");
        size_t len = (size_t)cursor[0] | ((size_t)cursor[1] << 8);
        cursor += 2;
        ESP_RETURN_ON_FALSE(len != 0 && len <= AUDIO_NG_MAX_OPUS_PACKET && len <= (size_t)(end - cursor),
                            ESP_ERR_INVALID_SIZE, TAG, "Malformed embedded cue packet");
        ESP_RETURN_ON_ERROR(audio_ng_playback_enqueue(audio, cursor, len, 100), TAG, "Queue cue packet failed");
        cursor += len;
    }
    return ESP_OK;
}

esp_err_t audio_ng_play_cue(audio_ng_handle_t audio)
{
    return play_cue(audio, wakeup_opus_start, wakeup_opus_end);
}

esp_err_t audio_ng_play_stop_cue(audio_ng_handle_t audio)
{
    return play_cue(audio, stop_opus_start, stop_opus_end);
}
