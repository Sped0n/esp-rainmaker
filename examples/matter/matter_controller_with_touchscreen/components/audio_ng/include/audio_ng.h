/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct audio_ng *audio_ng_handle_t;

typedef enum {
    AUDIO_NG_EVENT_WAKE_START,
    AUDIO_NG_EVENT_WAKE_END,
    AUDIO_NG_EVENT_VAD_START,
    AUDIO_NG_EVENT_VAD_END,
    AUDIO_NG_EVENT_LEVEL,
    AUDIO_NG_EVENT_PLAYBACK_COMPLETE,
    AUDIO_NG_EVENT_FAULT,
} audio_ng_event_id_t;

typedef struct {
    audio_ng_event_id_t id;
    union {
        uint8_t level;
        esp_err_t error;
    } data;
} audio_ng_event_t;

/* Called from an audio-owned task. The event is immutable and valid only for the call. */
typedef void (*audio_ng_event_cb_t)(audio_ng_handle_t audio, const audio_ng_event_t *event, void *user_data);

typedef struct {
    const char *input_device_name;
    const char *output_device_name;
    float input_gain_db;
    uint8_t initial_volume;
    audio_ng_event_cb_t event_cb;
    void *event_user_data;
} audio_ng_config_t;

/* Initialization copies the device names but does not acquire or open either codec. */
esp_err_t audio_ng_init(const audio_ng_config_t *config, audio_ng_handle_t *out_audio);
void audio_ng_deinit(audio_ng_handle_t audio);

/* Replaces the observation callback before wake monitoring starts. */
esp_err_t audio_ng_set_event_callback(audio_ng_handle_t audio, audio_ng_event_cb_t callback, void *user_data);

/* Starts continuous AFE/WakeNet processing and encoded-frame draining. */
esp_err_t audio_ng_start_wake_monitoring(audio_ng_handle_t audio);
esp_err_t audio_ng_stop_wake_monitoring(audio_ng_handle_t audio);

/* Lazily acquires the output codec and starts the shared Opus playback path. */
esp_err_t audio_ng_prepare_output(audio_ng_handle_t audio);

/* Plays the module-owned, framed 16 kHz/60 ms Opus session cue. */
esp_err_t audio_ng_play_cue(audio_ng_handle_t audio);
esp_err_t audio_ng_play_stop_cue(audio_ng_handle_t audio);

/*
 * Begins a fresh upload epoch after discarding all previously encoded frames.
 * Reads made with an older epoch fail with ESP_ERR_INVALID_STATE.
 */
esp_err_t audio_ng_capture_begin(audio_ng_handle_t audio, uint32_t *out_epoch);
esp_err_t audio_ng_capture_end(audio_ng_handle_t audio);
esp_err_t audio_ng_capture_read(audio_ng_handle_t audio, uint32_t epoch, uint8_t *data, size_t capacity,
                                size_t *out_len, uint32_t timeout_ms);

/* Response data is one raw Opus packet negotiated as mono, 16 kHz, 60 ms. */
esp_err_t audio_ng_response_write(audio_ng_handle_t audio, const uint8_t *data, size_t len, uint32_t timeout_ms);
esp_err_t audio_ng_playback_wait(audio_ng_handle_t audio, uint32_t timeout_ms);
esp_err_t audio_ng_playback_abort(audio_ng_handle_t audio);

esp_err_t audio_ng_get_volume(audio_ng_handle_t audio, uint8_t *out_volume);
esp_err_t audio_ng_set_volume(audio_ng_handle_t audio, uint8_t volume);

#ifdef __cplusplus
}
#endif
