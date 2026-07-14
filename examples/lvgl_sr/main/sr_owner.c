#include "sr_owner.h"

#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>

#include "dev_audio_codec.h"
#include "esp_afe_config.h"
#include "esp_afe_sr_models.h"
#include "esp_board_manager.h"
#include "esp_board_manager_defs.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_gmf_afe.h"
#include "esp_gmf_afe_manager.h"
#include "esp_gmf_new_databus.h"
#include "esp_gmf_obj.h"
#include "esp_gmf_pipeline.h"
#include "esp_gmf_pool.h"
#include "esp_gmf_task.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define SR_SAMPLE_RATE 16000
#define SR_CHANNELS 2
#define SR_BITS_PER_SAMPLE 16
#define SR_INPUT_FORMAT "MM"
#define SR_QUEUE_LENGTH 8
#define SR_CLEANUP_RETRIES 3
#define SR_AFE_TASK_CORE 0
#define SR_AFE_TASK_PRIORITY 6
#define SR_AFE_TASK_STACK_SIZE (5 * 1024)

typedef enum {
    OWNER_MSG_INTENT_START,
    OWNER_MSG_INTENT_PAUSE,
    OWNER_MSG_INTENT_RESUME,
    OWNER_MSG_INTENT_STOP,
    OWNER_MSG_WAKE_DETECTED,
} owner_msg_t;

typedef struct {
    dev_audio_codec_handles_t *adc;
    bool adc_initialized;
    bool codec_open;
    srmodel_list_t *models;
    afe_config_t *afe_config;
    esp_gmf_pool_handle_t pool;
    esp_gmf_afe_manager_handle_t afe_manager;
    esp_gmf_pipeline_handle_t pipeline;
    esp_gmf_task_handle_t pipeline_task;
    esp_gmf_element_handle_t afe_element;
    bool pipeline_started;
    bool pipeline_active;
    bool afe_suspended;
} sr_resources_t;

static const char *TAG = "sr_owner";
static QueueHandle_t s_queue;
static SemaphoreHandle_t s_snapshot_lock;
static SemaphoreHandle_t s_wake_events;
static sr_snapshot_t s_snapshot = {
    .state = SR_STATE_STOPPED,
};
static sr_resources_t s_resources;
static bool s_cleanup_incomplete;

static void set_state(sr_state_t state)
{
    xSemaphoreTake(s_snapshot_lock, portMAX_DELAY);
    s_snapshot.state = state;
    xSemaphoreGive(s_snapshot_lock);
}

static esp_gmf_err_io_t output_acquire(void *handle, esp_gmf_data_bus_block_t *block,
                                       int wanted_size, int block_ticks)
{
    return ESP_GMF_IO_OK;
}

static esp_gmf_err_io_t output_release(void *handle, esp_gmf_data_bus_block_t *block, int block_ticks)
{
    return ESP_GMF_IO_OK;
}

static esp_gmf_err_io_t input_acquire(void *handle, esp_gmf_data_bus_block_t *block,
                                      int wanted_size, int block_ticks)
{
    sr_resources_t *resources = handle;
    int ret = esp_codec_dev_read(resources->adc->codec_dev, block->buf, wanted_size);
    if (ret != ESP_CODEC_DEV_OK) {
        block->valid_size = 0;
        ESP_LOGE(TAG, "Microphone read failed: %d", ret);
        return ESP_GMF_IO_FAIL;
    }
    block->valid_size = wanted_size;
    return ESP_GMF_IO_OK;
}

static esp_gmf_err_io_t input_release(void *handle, esp_gmf_data_bus_block_t *block, int block_ticks)
{
    return ESP_GMF_IO_OK;
}

static void afe_event_cb(esp_gmf_obj_handle_t object, esp_gmf_afe_evt_t *event, void *user_data)
{
    if (event->type != ESP_GMF_AFE_EVT_WAKEUP_START) {
        return;
    }

    xSemaphoreGive(s_wake_events);
    const owner_msg_t msg = OWNER_MSG_WAKE_DETECTED;
    xQueueSend(s_queue, &msg, 0);
}

static void log_gmf_error(const char *operation, esp_gmf_err_t err)
{
    if (err != ESP_GMF_ERR_OK) {
        ESP_LOGE(TAG, "%s failed: 0x%x", operation, err);
    }
}

static esp_err_t cleanup_resources(void)
{
    sr_resources_t *resources = &s_resources;
    esp_err_t result = ESP_OK;

    if (resources->afe_suspended && resources->afe_manager) {
        esp_gmf_err_t err = esp_gmf_afe_manager_suspend(resources->afe_manager, false);
        if (err != ESP_GMF_ERR_OK) {
            log_gmf_error("Resume AFE manager for teardown", err);
            return ESP_FAIL;
        }
        resources->afe_suspended = false;
    }
    if (resources->pipeline_started && resources->pipeline) {
        esp_gmf_err_t err = esp_gmf_pipeline_stop(resources->pipeline);
        if (err != ESP_GMF_ERR_OK) {
            log_gmf_error("Stop AFE pipeline", err);
            // Nothing below is safe to release while the pipeline may still be running.
            return ESP_FAIL;
        }
        resources->pipeline_started = false;
        resources->pipeline_active = false;
    }
    if (resources->afe_element) {
        esp_gmf_err_t err = esp_gmf_afe_set_event_cb(resources->afe_element, NULL, NULL);
        if (err != ESP_GMF_ERR_OK) {
            log_gmf_error("Unregister AFE callback", err);
            result = ESP_FAIL;
        }
    }
    if (resources->pipeline_task) {
        esp_gmf_err_t err = esp_gmf_task_deinit(resources->pipeline_task);
        if (err != ESP_GMF_ERR_OK) {
            log_gmf_error("Destroy pipeline task", err);
            result = ESP_FAIL;
            return result;
        }
        resources->pipeline_task = NULL;
    }
    if (resources->pipeline) {
        esp_gmf_err_t err = esp_gmf_pipeline_destroy(resources->pipeline);
        if (err != ESP_GMF_ERR_OK) {
            log_gmf_error("Destroy AFE pipeline", err);
            return ESP_FAIL;
        }
        resources->pipeline = NULL;
        resources->afe_element = NULL;
    }
    if (resources->pool) {
        esp_gmf_err_t err = esp_gmf_pool_deinit(resources->pool);
        if (err != ESP_GMF_ERR_OK) {
            log_gmf_error("Destroy GMF pool", err);
            result = ESP_FAIL;
            return result;
        }
        resources->pool = NULL;
    }
    if (resources->afe_manager) {
        esp_gmf_err_t err = esp_gmf_afe_manager_destroy(resources->afe_manager);
        if (err != ESP_GMF_ERR_OK) {
            log_gmf_error("Destroy AFE manager", err);
            result = ESP_FAIL;
            return result;
        }
        resources->afe_manager = NULL;
    }
    if (resources->afe_config) {
        afe_config_free(resources->afe_config);
        resources->afe_config = NULL;
    }
    if (resources->models) {
        esp_srmodel_deinit(resources->models);
        resources->models = NULL;
    }
    if (resources->codec_open && resources->adc) {
        int ret = esp_codec_dev_close(resources->adc->codec_dev);
        if (ret != ESP_CODEC_DEV_OK) {
            ESP_LOGE(TAG, "Close microphone codec failed: %d", ret);
            result = ESP_FAIL;
        } else {
            resources->codec_open = false;
        }
    }

    return result;
}

static sr_state_t resources_state(void)
{
    if (s_resources.pipeline_started && s_resources.pipeline_active) {
        return SR_STATE_RUNNING;
    }
    if (s_resources.pipeline && s_resources.pipeline_task && s_resources.pool &&
            s_resources.afe_manager && s_resources.models && s_resources.adc_initialized &&
            s_resources.codec_open && s_resources.pipeline_started && s_resources.afe_suspended) {
        return SR_STATE_PAUSED;
    }
    if (!s_resources.pipeline && !s_resources.pipeline_task && !s_resources.pool &&
            !s_resources.afe_manager && !s_resources.models && !s_resources.codec_open &&
            !s_resources.pipeline_started && !s_resources.afe_suspended) {
        return SR_STATE_STOPPED;
    }
    return SR_STATE_STOPPING;
}

static void complete_teardown(const char *operation)
{
    set_state(SR_STATE_STOPPING);
    for (unsigned attempt = 1; attempt <= SR_CLEANUP_RETRIES; ++attempt) {
        esp_err_t err = cleanup_resources();
        if (err == ESP_OK || resources_state() == SR_STATE_STOPPED) {
            set_state(SR_STATE_STOPPED);
            ESP_LOGI(TAG, "%s completed", operation);
            return;
        }
        ESP_LOGE(TAG, "%s cleanup attempt %u/%u failed", operation, attempt,
                 SR_CLEANUP_RETRIES);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    ESP_LOGE(TAG, "%s cleanup remains incomplete; controls stay disabled", operation);
}

static esp_err_t setup_pipeline_ports(sr_resources_t *resources)
{
    esp_gmf_port_handle_t output = NEW_ESP_GMF_PORT_OUT_BYTE(
                                       output_acquire, output_release, NULL, resources, 2048, portMAX_DELAY);
    ESP_RETURN_ON_FALSE(output, ESP_ERR_NO_MEM, TAG, "Create AFE output port failed");
    if (esp_gmf_pipeline_reg_el_port(resources->pipeline, "ai_afe",
                                     ESP_GMF_IO_DIR_WRITER, output) != ESP_GMF_ERR_OK) {
        esp_gmf_port_deinit(output);
        ESP_LOGE(TAG, "Register AFE output port failed");
        return ESP_FAIL;
    }

    esp_gmf_port_handle_t input = NEW_ESP_GMF_PORT_IN_BYTE(
                                      input_acquire, input_release, NULL, resources, 2048, portMAX_DELAY);
    ESP_RETURN_ON_FALSE(input, ESP_ERR_NO_MEM, TAG, "Create AFE input port failed");
    if (esp_gmf_pipeline_reg_el_port(resources->pipeline, "ai_afe",
                                     ESP_GMF_IO_DIR_READER, input) != ESP_GMF_ERR_OK) {
        esp_gmf_port_deinit(input);
        ESP_LOGE(TAG, "Register AFE input port failed");
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t create_pipeline_task(sr_resources_t *resources)
{
    esp_gmf_task_cfg_t task_config = DEFAULT_ESP_GMF_TASK_CONFIG();
    task_config.name = "sr_pipeline";
    // task_config.thread.stack_in_ext = true;
    // task_config.thread.stack = 32 * 1024;
    // task_config.thread.prio = 7;
    // task_config.thread.core = 0;

    ESP_RETURN_ON_FALSE(esp_gmf_task_init(&task_config, &resources->pipeline_task) == ESP_GMF_ERR_OK,
                        ESP_FAIL, TAG, "Create pipeline task failed");
    ESP_RETURN_ON_FALSE(esp_gmf_pipeline_bind_task(resources->pipeline, resources->pipeline_task) == ESP_GMF_ERR_OK,
                        ESP_FAIL, TAG, "Bind pipeline task failed");
    return ESP_OK;
}

static esp_err_t start_resources(void)
{
    sr_resources_t *resources = &s_resources;
    esp_gmf_element_handle_t pool_afe_element = NULL;
    const char *elements[] = {"ai_afe"};
    esp_err_t ret = ESP_OK;
    s_cleanup_incomplete = false;

    if (!resources->adc_initialized) {
        ESP_GOTO_ON_ERROR(esp_board_manager_init_device_by_name(ESP_BOARD_DEVICE_NAME_AUDIO_ADC),
                          fail, TAG, "Initialize microphone device failed");
        resources->adc_initialized = true;
    }
    if (!resources->adc) {
        ESP_GOTO_ON_ERROR(esp_board_manager_get_device_handle(ESP_BOARD_DEVICE_NAME_AUDIO_ADC,
                                                              (void **)&resources->adc), fail, TAG,
                          "Get microphone device failed");
        ESP_GOTO_ON_FALSE(resources->adc && resources->adc->codec_dev, ESP_ERR_NOT_FOUND, fail, TAG,
                          "Microphone codec is unavailable");
    }

    esp_codec_dev_sample_info_t sample_info = {
        .sample_rate = SR_SAMPLE_RATE,
        .channel = SR_CHANNELS,
        .bits_per_sample = SR_BITS_PER_SAMPLE,
    };
    if (!resources->codec_open) {
        // Open can fail after esp_codec_dev marks the input open, so own rollback conservatively.
        resources->codec_open = true;
        int codec_ret = esp_codec_dev_open(resources->adc->codec_dev, &sample_info);
        if (codec_ret != ESP_CODEC_DEV_OK) {
            if (esp_codec_dev_close(resources->adc->codec_dev) == ESP_CODEC_DEV_OK) {
                resources->codec_open = false;
            }
            ESP_LOGE(TAG, "Open microphone codec failed: %d", codec_ret);
            ret = ESP_FAIL;
            goto fail;
        }
    }

    resources->models = esp_srmodel_init("model");
    ESP_GOTO_ON_FALSE(resources->models, ESP_FAIL, fail, TAG, "Load speech models failed");
    resources->afe_config = afe_config_init(SR_INPUT_FORMAT, resources->models,
                                            AFE_TYPE_SR, AFE_MODE_HIGH_PERF);
    ESP_GOTO_ON_FALSE(resources->afe_config, ESP_ERR_NO_MEM, fail, TAG, "Create AFE config failed");
    resources->afe_config->wakenet_init = true;
    resources->afe_config->vad_init = true;
    resources->afe_config->vad_mode = VAD_MODE_3;
    resources->afe_config->vad_min_noise_ms = 1000;
    resources->afe_config->vad_min_speech_ms = 64;
    resources->afe_config->agc_init = true;
    resources->afe_config->aec_init = false;
    resources->afe_config->se_init = false;
    resources->afe_config->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;

    ESP_GOTO_ON_FALSE(esp_gmf_pool_init(&resources->pool) == ESP_GMF_ERR_OK,
                      ESP_FAIL, fail, TAG, "Create GMF pool failed");

    esp_gmf_afe_manager_cfg_t manager_config = DEFAULT_GMF_AFE_MANAGER_CFG(
                                                   resources->afe_config, NULL, NULL, NULL, NULL);
    // manager_config.feed_task_setting.core = SR_AFE_TASK_CORE;
    // manager_config.feed_task_setting.prio = SR_AFE_TASK_PRIORITY;
    // manager_config.feed_task_setting.stack_size = SR_AFE_TASK_STACK_SIZE;
    // manager_config.fetch_task_setting.core = SR_AFE_TASK_CORE;
    // manager_config.fetch_task_setting.prio = SR_AFE_TASK_PRIORITY;
    // manager_config.fetch_task_setting.stack_size = SR_AFE_TASK_STACK_SIZE;
    ESP_GOTO_ON_FALSE(esp_gmf_afe_manager_create(&manager_config, &resources->afe_manager) == ESP_GMF_ERR_OK,
                      ESP_FAIL, fail, TAG, "Create AFE manager failed");

    esp_gmf_afe_cfg_t element_config = DEFAULT_GMF_AFE_CFG(
                                           resources->afe_manager, NULL, NULL, resources->models);
    element_config.wakeup_time = 15 * 1000;
    element_config.wakeup_end = 15 * 1000;
    ESP_GOTO_ON_FALSE(esp_gmf_afe_init(&element_config, &pool_afe_element) == ESP_GMF_ERR_OK,
                      ESP_FAIL, fail, TAG, "Create AFE element failed");
    if (esp_gmf_pool_register_element(resources->pool, pool_afe_element, "ai_afe") != ESP_GMF_ERR_OK) {
        esp_gmf_obj_delete(pool_afe_element);
        pool_afe_element = NULL;
        ret = ESP_FAIL;
        ESP_LOGE(TAG, "Register AFE element failed");
        goto fail;
    }
    pool_afe_element = NULL;

    ESP_GOTO_ON_FALSE(esp_gmf_pool_new_pipeline(resources->pool, NULL, elements, 1, NULL,
                                                &resources->pipeline) == ESP_GMF_ERR_OK,
                      ESP_FAIL, fail, TAG, "Create AFE pipeline failed");
    ESP_GOTO_ON_ERROR(setup_pipeline_ports(resources), fail, TAG, "Set up AFE ports failed");
    ESP_GOTO_ON_FALSE(esp_gmf_pipeline_get_el_by_name(resources->pipeline, "ai_afe",
                                                      &resources->afe_element) == ESP_GMF_ERR_OK,
                      ESP_FAIL, fail, TAG, "Find AFE element failed");
    ESP_GOTO_ON_FALSE(esp_gmf_afe_set_event_cb(resources->afe_element, afe_event_cb, NULL) == ESP_GMF_ERR_OK,
                      ESP_FAIL, fail, TAG, "Register AFE callback failed");

    esp_gmf_info_sound_t input_info = {
        .sample_rates = SR_SAMPLE_RATE,
        .bits = SR_BITS_PER_SAMPLE,
        .channels = SR_CHANNELS,
    };
    ESP_GOTO_ON_FALSE(esp_gmf_pipeline_report_info(resources->pipeline, ESP_GMF_INFO_SOUND,
                                                   &input_info, sizeof(input_info)) == ESP_GMF_ERR_OK,
                      ESP_FAIL, fail, TAG, "Report microphone format failed");
    ESP_GOTO_ON_ERROR(create_pipeline_task(resources), fail, TAG, "Create AFE task failed");
    ESP_GOTO_ON_FALSE(esp_gmf_pipeline_loading_jobs(resources->pipeline) == ESP_GMF_ERR_OK,
                      ESP_FAIL, fail, TAG, "Load AFE jobs failed");
    // A run timeout may leave the worker active, so cleanup must always stop after this point.
    resources->pipeline_started = true;
    ESP_GOTO_ON_FALSE(esp_gmf_pipeline_run(resources->pipeline) == ESP_GMF_ERR_OK,
                      ESP_FAIL, fail, TAG, "Run AFE pipeline failed");
    resources->pipeline_active = true;
    return ESP_OK;

fail:
    if (pool_afe_element) {
        esp_gmf_obj_delete(pool_afe_element);
    }
    if (cleanup_resources() != ESP_OK && resources_state() != SR_STATE_STOPPED) {
        s_cleanup_incomplete = true;
        ESP_LOGE(TAG, "Start cleanup incomplete; retained resources remain owned");
    }
    return ret;
}

static esp_err_t pause_resources(void)
{
    ESP_RETURN_ON_FALSE(s_resources.pipeline_active, ESP_ERR_INVALID_STATE, TAG,
                        "AFE pipeline is not running");
    ESP_RETURN_ON_FALSE(esp_gmf_pipeline_pause(s_resources.pipeline) == ESP_GMF_ERR_OK,
                        ESP_FAIL, TAG, "Pause AFE pipeline failed");
    ESP_RETURN_ON_FALSE(esp_gmf_afe_manager_suspend(s_resources.afe_manager, true) == ESP_GMF_ERR_OK,
                        ESP_FAIL, TAG, "Pause AFE manager failed");
    s_resources.afe_suspended = true;
    s_resources.pipeline_active = false;
    return ESP_OK;
}

static esp_err_t resume_resources(void)
{
    ESP_RETURN_ON_FALSE(!s_resources.pipeline_active && s_resources.pipeline,
                        ESP_ERR_INVALID_STATE, TAG, "AFE pipeline is not paused");
    ESP_RETURN_ON_FALSE(s_resources.afe_suspended, ESP_ERR_INVALID_STATE, TAG,
                        "AFE manager is not paused");
    ESP_RETURN_ON_FALSE(esp_gmf_afe_manager_suspend(s_resources.afe_manager, false) == ESP_GMF_ERR_OK,
                        ESP_FAIL, TAG, "Resume AFE manager failed");
    s_resources.afe_suspended = false;
    ESP_RETURN_ON_FALSE(esp_gmf_pipeline_resume(s_resources.pipeline) == ESP_GMF_ERR_OK,
                        ESP_FAIL, TAG, "Resume AFE pipeline failed");
    s_resources.pipeline_active = true;
    return ESP_OK;
}

static void recover_transition_failure(const char *operation)
{
    ESP_LOGE(TAG, "%s failed; attempting full teardown", operation);
    complete_teardown(operation);
}

static void consume_wake_events(void)
{
    uint32_t count = 0;
    while (xSemaphoreTake(s_wake_events, 0) == pdTRUE) {
        ++count;
    }
    if (!count) {
        return;
    }
    xSemaphoreTake(s_snapshot_lock, portMAX_DELAY);
    if (s_snapshot.state == SR_STATE_RUNNING) {
        s_snapshot.wake_count += count;
    }
    xSemaphoreGive(s_snapshot_lock);
}

static void process_message(owner_msg_t msg)
{
    sr_snapshot_t snapshot = {0};
    sr_owner_get_snapshot(&snapshot);

    switch (msg) {
    case OWNER_MSG_INTENT_START:
        if (snapshot.state != SR_STATE_STOPPED) {
            ESP_LOGW(TAG, "Ignoring Start in state %d", snapshot.state);
            return;
        }
        set_state(SR_STATE_STARTING);
        if (start_resources() == ESP_OK) {
            set_state(SR_STATE_RUNNING);
            ESP_LOGI(TAG, "Speech recognition running");
        } else {
            if (s_cleanup_incomplete) {
                complete_teardown("Failed Start");
            } else {
                set_state(SR_STATE_STOPPED);
            }
            ESP_LOGE(TAG, "Speech recognition start failed");
        }
        break;
    case OWNER_MSG_INTENT_PAUSE:
        if (snapshot.state != SR_STATE_RUNNING) {
            ESP_LOGW(TAG, "Ignoring Pause in state %d", snapshot.state);
            return;
        }
        if (pause_resources() == ESP_OK) {
            set_state(SR_STATE_PAUSED);
            ESP_LOGI(TAG, "Speech recognition paused");
        } else {
            recover_transition_failure("Pause");
        }
        break;
    case OWNER_MSG_INTENT_RESUME:
        if (snapshot.state != SR_STATE_PAUSED) {
            ESP_LOGW(TAG, "Ignoring Resume in state %d", snapshot.state);
            return;
        }
        if (resume_resources() == ESP_OK) {
            set_state(SR_STATE_RUNNING);
            ESP_LOGI(TAG, "Speech recognition resumed");
        } else {
            recover_transition_failure("Resume");
        }
        break;
    case OWNER_MSG_INTENT_STOP:
        if (snapshot.state != SR_STATE_RUNNING && snapshot.state != SR_STATE_PAUSED) {
            ESP_LOGW(TAG, "Ignoring Stop in state %d", snapshot.state);
            return;
        }
        complete_teardown("Stop");
        break;
    case OWNER_MSG_WAKE_DETECTED:
        consume_wake_events();
        break;
    }
}

static void owner_task(void *arg)
{
    owner_msg_t msg;
    while (true) {
        if (xQueueReceive(s_queue, &msg, portMAX_DELAY) == pdTRUE) {
            process_message(msg);
            consume_wake_events();
        }
    }
}

esp_err_t sr_owner_init(void)
{
    ESP_RETURN_ON_FALSE(!s_queue, ESP_ERR_INVALID_STATE, TAG, "SR owner already initialized");
    s_snapshot_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_snapshot_lock, ESP_ERR_NO_MEM, TAG, "Create snapshot lock failed");
    s_wake_events = xSemaphoreCreateCounting(UINT_MAX, 0);
    if (!s_wake_events) {
        vSemaphoreDelete(s_snapshot_lock);
        s_snapshot_lock = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_queue = xQueueCreate(SR_QUEUE_LENGTH, sizeof(owner_msg_t));
    if (!s_queue) {
        vSemaphoreDelete(s_wake_events);
        vSemaphoreDelete(s_snapshot_lock);
        s_wake_events = NULL;
        s_snapshot_lock = NULL;
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreatePinnedToCore(owner_task, "sr_owner", 6144, NULL, 5, NULL, 0) != pdPASS) {
        vQueueDelete(s_queue);
        vSemaphoreDelete(s_wake_events);
        vSemaphoreDelete(s_snapshot_lock);
        s_queue = NULL;
        s_wake_events = NULL;
        s_snapshot_lock = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t sr_owner_submit(sr_intent_t intent)
{
    ESP_RETURN_ON_FALSE(s_queue, ESP_ERR_INVALID_STATE, TAG, "SR owner is not initialized");
    ESP_RETURN_ON_FALSE(intent >= SR_INTENT_START && intent <= SR_INTENT_STOP,
                        ESP_ERR_INVALID_ARG, TAG, "Unknown SR intent");
    const owner_msg_t msg = (owner_msg_t)(OWNER_MSG_INTENT_START + intent);
    ESP_RETURN_ON_FALSE(xQueueSend(s_queue, &msg, 0) == pdTRUE,
                        ESP_ERR_TIMEOUT, TAG, "SR owner queue is full");
    return ESP_OK;
}

void sr_owner_get_snapshot(sr_snapshot_t *snapshot)
{
    if (!snapshot || !s_snapshot_lock) {
        return;
    }
    xSemaphoreTake(s_snapshot_lock, portMAX_DELAY);
    *snapshot = s_snapshot;
    xSemaphoreGive(s_snapshot_lock);
}
