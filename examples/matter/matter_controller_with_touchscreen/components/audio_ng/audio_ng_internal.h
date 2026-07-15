/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/message_buffer.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

#include <esp_codec_dev.h>
#include <esp_gmf_pipeline.h>
#include <esp_gmf_task.h>
#include <esp_gmf_pool.h>

#include "audio_ng.h"

#define AUDIO_NG_MAX_OPUS_PACKET 1275
#define AUDIO_NG_CAPTURE_BUFFER_SIZE (16 * 1024)
#define AUDIO_NG_PLAYBACK_QUEUE_DEPTH 6

typedef struct {
    size_t len;
    uint8_t data[AUDIO_NG_MAX_OPUS_PACKET];
} audio_ng_packet_t;

typedef enum {
    AUDIO_NG_CAPTURE_STOPPED,
    AUDIO_NG_CAPTURE_MONITORING,
    AUDIO_NG_CAPTURE_STREAMING,
} audio_ng_capture_state_t;

typedef enum {
    AUDIO_NG_PLAYBACK_CLOSED,
    AUDIO_NG_PLAYBACK_READY,
    AUDIO_NG_PLAYBACK_ABORTING,
} audio_ng_playback_state_t;

struct audio_ng {
    char *input_device_name;
    char *output_device_name;
    float input_gain_db;
    uint8_t volume;
    audio_ng_event_cb_t event_cb;
    void *event_user_data;

    esp_codec_dev_handle_t input;
    esp_codec_dev_handle_t output;
    esp_gmf_pipeline_handle_t capture_pipeline;
    esp_gmf_task_handle_t capture_task;
    esp_gmf_pipeline_handle_t playback_pipeline;
    esp_gmf_task_handle_t playback_task;
    void *afe_manager;
    void *afe_models;
    void *afe_config;
    MessageBufferHandle_t capture_messages;
    StaticMessageBuffer_t capture_message_storage;
    uint8_t *capture_storage;
    SemaphoreHandle_t capture_lock;
    StaticSemaphore_t capture_lock_storage;
    QueueHandle_t playback_queue;
    StaticQueue_t playback_queue_storage;
    uint8_t *playback_queue_storage_bytes;
    SemaphoreHandle_t playback_api_lock;
    StaticSemaphore_t playback_api_lock_storage;

    uint32_t capture_epoch;
    uint32_t capture_dropped_frames;
    audio_ng_capture_state_t capture_state;
    audio_ng_playback_state_t playback_state;
    uint32_t playback_inflight;
    uint32_t playback_output_bytes;
    portMUX_TYPE playback_lock;
};

void audio_ng_emit(audio_ng_handle_t audio, audio_ng_event_id_t id, esp_err_t error);
esp_err_t audio_ng_capture_start(audio_ng_handle_t audio);
void audio_ng_capture_stop(audio_ng_handle_t audio);
void audio_ng_capture_release(audio_ng_handle_t audio);
esp_err_t audio_ng_playback_prepare(audio_ng_handle_t audio);
void audio_ng_playback_stop(audio_ng_handle_t audio);
esp_err_t audio_ng_playback_enqueue(audio_ng_handle_t audio, const uint8_t *data, size_t len, uint32_t timeout_ms);
esp_gmf_pool_handle_t audio_ng_pool_get(void);
