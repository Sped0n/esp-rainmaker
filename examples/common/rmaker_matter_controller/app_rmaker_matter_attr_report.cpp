/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <cJSON.h>
#include <esp_check.h>
#include <esp_err.h>
#if CONFIG_RAINMAKER_MATTER_CONTROLLER_MEM_ALLOC_MODE_EXTERNAL
#include <esp_heap_caps.h>
#endif
#include <esp_log.h>
#include <esp_rmaker_core.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#if CONFIG_RAINMAKER_MATTER_CONTROLLER_MEM_ALLOC_MODE_EXTERNAL
#include <freertos/idf_additions.h>
#endif
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include <app_rmaker_matter_controller.h>
#include <app_rmaker_matter_controller_internal.h>
#include <app_rmaker_matter_device_list.h>
#include <app_rmaker_matter_attr_json.h>
#include <app_rmaker_matter_json_helpers.h>
#include <esp_matter_controller_subscribe_command.h>
#include <esp_matter_controller_utils.h>
#include <esp_matter_core.h>

using namespace esp_matter;
using namespace esp_matter::controller;

#define TAG "rmaker_matter_attr"

/* Filter: ignore endpoint 0, global attributes 0xFFF8-0xFFFD, cluster 0x1D */
#define MATTER_ATTR_FILTER_EP0             0
#define MATTER_ATTR_FILTER_GLOBAL_ATTR_MIN 0xFFF8
#define MATTER_ATTR_FILTER_GLOBAL_ATTR_MAX 0xFFFD
#define MATTER_ATTR_FILTER_CLUSTER_1D      0x1D

#define MATTER_ATTR_MAX_REMOVED_PER_UPDATE 32
#define MATTER_ATTR_TASK_PRIO              5
#define MATTER_ATTR_QUEUE_SIZE             32
#define MATTER_ATTR_VALUE_MAX_LEN          384
#define MATTER_ATTR_BATCH_COALESCE_MS      100

typedef enum {
    ATTR_REPORT_MSG_ATTRIBUTE_DATA = 0,
    ATTR_REPORT_MSG_SUBSCRIPTION_TERMINATED,
    ATTR_REPORT_MSG_RESUBSCRIBE_RETRY,
    ATTR_REPORT_MSG_SUBSCRIBE_CONNECT_FAILED,
    ATTR_REPORT_MSG_SUBSCRIPTION_ESTABLISHED,
    ATTR_REPORT_MSG_FLUSH_BATCH,
} attr_report_msg_type_t;

typedef struct {
    attr_report_msg_type_t msg_type;
    uint64_t node_id;
    uint16_t endpoint_id;
    uint32_t cluster_id;
    uint32_t attribute_id;
    char value[MATTER_ATTR_VALUE_MAX_LEN];
} attr_report_msg_t;

#define MATTER_ATTR_QUEUE_ITEM_SIZE sizeof(attr_report_msg_t)

#define MATTER_ATTR_FIB_MAX_SEC 3600u
/** First resubscribe delay after going offline (then Fibonacci growth, capped by MATTER_ATTR_FIB_MAX_SEC). */
#define MATTER_ATTR_FIB_FIRST_SEC 60u

static_assert(CONFIG_RAINMAKER_MATTER_CONTROLLER_ATTR_BATCH_BUCKET_CAPACITY > 0,
              "CONFIG_RAINMAKER_MATTER_CONTROLLER_ATTR_BATCH_BUCKET_CAPACITY must be greater than 0");
static_assert(CONFIG_RAINMAKER_MATTER_CONTROLLER_ATTR_BATCH_REFILL_MS > 0,
              "CONFIG_RAINMAKER_MATTER_CONTROLLER_ATTR_BATCH_REFILL_MS must be greater than 0");

typedef struct node_state {
    uint64_t node_id;
    char rainmaker_node_id[ESP_RAINMAKER_NODE_ID_MAX_LEN];
    cJSON *root; /* endpoints -> 0xEP -> clusters -> servers -> 0xCID -> attributes -> 0xAID -> value */
    bool online; /* true after subscribe OnSubscriptionEstablished until subscription terminated */
    uint32_t fib_prev_sec;       /* Fibonacci backoff; first delay MATTER_ATTR_FIB_FIRST_SEC (e.g. 60,60,120,...) */
    uint32_t fib_cur_sec;
    esp_timer_handle_t resubscribe_timer;
    struct node_state *next;
} node_state_t;

static QueueHandle_t s_attr_report_queue = NULL;
static TaskHandle_t s_attr_report_task = NULL;
static SemaphoreHandle_t s_state_mutex = NULL;
static node_state_t *s_node_states = NULL;
static bool s_attr_report_initialized = false;
static cJSON *s_pending_attr_delta = NULL;
static esp_timer_handle_t s_batch_timer = NULL;
static uint32_t s_last_token_update_ms = 0;
static uint8_t s_tokens = CONFIG_RAINMAKER_MATTER_CONTROLLER_ATTR_BATCH_BUCKET_CAPACITY;
static bool s_batch_timer_armed = false;
static app_rmaker_matter_attr_report_cb_t s_attr_report_cb = NULL;
static void *s_attr_report_cb_priv = NULL;

static size_t subscribed_node_limit(void)
{
    return CHIP_CONFIG_CONTROLLER_MAX_ACTIVE_DEVICES < CONFIG_MAX_EXCHANGE_CONTEXTS ?
           CHIP_CONFIG_CONTROLLER_MAX_ACTIVE_DEVICES : CONFIG_MAX_EXCHANGE_CONTEXTS;
}

static bool should_ignore_attribute(uint16_t endpoint_id, uint32_t cluster_id, uint32_t attribute_id)
{
    if (endpoint_id == MATTER_ATTR_FILTER_EP0) {
        return true;
    }
    if (attribute_id >= MATTER_ATTR_FILTER_GLOBAL_ATTR_MIN && attribute_id <= MATTER_ATTR_FILTER_GLOBAL_ATTR_MAX) {
        return true;
    }
    if (cluster_id == MATTER_ATTR_FILTER_CLUSTER_1D) {
        return true;
    }
    return false;
}

static node_state_t *find_node_state(uint64_t node_id)
{
    for (node_state_t *n = s_node_states; n != NULL; n = n->next) {
        if (n->node_id == node_id) {
            return n;
        }
    }
    return NULL;
}

static void free_node_state(node_state_t *ns)
{
    if (ns->resubscribe_timer) {
        esp_timer_stop(ns->resubscribe_timer);
        esp_timer_delete(ns->resubscribe_timer);
        ns->resubscribe_timer = NULL;
    }
    if (ns->root) {
        cJSON_Delete(ns->root);
        ns->root = NULL;
    }
    free(ns);
}

static node_state_t *create_node_state(uint64_t node_id, const char *rainmaker_node_id)
{
#if CONFIG_RAINMAKER_MATTER_CONTROLLER_MEM_ALLOC_MODE_EXTERNAL
    node_state_t *ns = (node_state_t *)heap_caps_calloc_prefer(1, sizeof(node_state_t), 2,
                                                               MALLOC_CAP_DEFAULT | MALLOC_CAP_SPIRAM,
                                                               MALLOC_CAP_DEFAULT | MALLOC_CAP_INTERNAL);
#else
    node_state_t *ns = (node_state_t *)calloc(1, sizeof(node_state_t));
#endif
    if (!ns) {
        return NULL;
    }
    ns->node_id = node_id;
    if (rainmaker_node_id) {
        strncpy(ns->rainmaker_node_id, rainmaker_node_id, sizeof(ns->rainmaker_node_id) - 1);
        ns->rainmaker_node_id[sizeof(ns->rainmaker_node_id) - 1] = '\0';
    }
    ns->root = cJSON_CreateObject();
    if (!ns->root) {
        free(ns);
        return NULL;
    }
    ns->online = false; /* set true in attr_report_task on ATTR_REPORT_MSG_SUBSCRIPTION_ESTABLISHED */
    ns->fib_prev_sec = 0;
    ns->fib_cur_sec = MATTER_ATTR_FIB_FIRST_SEC;
    ns->resubscribe_timer = NULL;
    return ns;
}

static void reset_fib_backoff(node_state_t *ns)
{
    ns->fib_prev_sec = 0;
    ns->fib_cur_sec = MATTER_ATTR_FIB_FIRST_SEC;
}

static void stop_resubscribe_timer(node_state_t *ns)
{
    if (ns && ns->resubscribe_timer) {
        esp_timer_stop(ns->resubscribe_timer);
    }
}

static void resubscribe_timer_cb(void *arg)
{
    node_state_t *ns = (node_state_t *)arg;
    if (!ns || !s_attr_report_queue) {
        return;
    }
    attr_report_msg_t refresh = {};
    refresh.msg_type = ATTR_REPORT_MSG_RESUBSCRIBE_RETRY;
    refresh.node_id = ns->node_id;
    if (xQueueSend(s_attr_report_queue, &refresh, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Attr report queue full, drop resubscribe retry for 0x%llX",
                 (unsigned long long)ns->node_id);
    }
}

static esp_err_t ensure_resubscribe_timer(node_state_t *ns)
{
    if (ns->resubscribe_timer) {
        return ESP_OK;
    }
    esp_timer_create_args_t args = {
        .callback = &resubscribe_timer_cb,
        .arg = ns,
        .name = "mt_attr_rs",
    };
    return esp_timer_create(&args, &ns->resubscribe_timer);
}

/* Requires s_state_mutex held. Schedules next one-shot retry; advances Fibonacci state. */
static void schedule_resubscribe_attempt(node_state_t *ns)
{
    if (!ns) {
        return;
    }
    ESP_LOGI(TAG, "Scheduling resubscribe attempt for 0x%llX", (unsigned long long)ns->node_id);
    if (ensure_resubscribe_timer(ns) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to create resubscribe timer for 0x%llX", (unsigned long long)ns->node_id);
        return;
    }
    uint32_t delay_sec = ns->fib_cur_sec;
    if (delay_sec > MATTER_ATTR_FIB_MAX_SEC) {
        delay_sec = MATTER_ATTR_FIB_MAX_SEC;
    }
    esp_timer_stop(ns->resubscribe_timer);
    esp_err_t err = esp_timer_start_once(ns->resubscribe_timer, (uint64_t)delay_sec * 1000000ULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_timer_start_once failed for 0x%llX: %s", (unsigned long long)ns->node_id,
                 esp_err_to_name(err));
        return;
    }
    uint32_t nxt = ns->fib_prev_sec + ns->fib_cur_sec;
    if (nxt < ns->fib_cur_sec || nxt > MATTER_ATTR_FIB_MAX_SEC) {
        nxt = MATTER_ATTR_FIB_MAX_SEC;
    }
    ns->fib_prev_sec = ns->fib_cur_sec;
    if (ns->fib_prev_sec > MATTER_ATTR_FIB_MAX_SEC) {
        ns->fib_prev_sec = MATTER_ATTR_FIB_MAX_SEC;
    }
    ns->fib_cur_sec = nxt;
}

static esp_err_t send_wildcard_subscribe(uint64_t node_id);

static void dispatch_node_online_event(uint64_t node_id, bool online)
{
    app_rmaker_matter_attr_report_cb_t cb = NULL;
    void *priv_data = NULL;

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    cb = s_attr_report_cb;
    priv_data = s_attr_report_cb_priv;
    xSemaphoreGive(s_state_mutex);

    if (!cb) {
        return;
    }

    app_rmaker_matter_attr_report_event_t event = {};
    event.node_id = node_id;
    event.online = online;
    cb(&event, priv_data);
}

static void refill_batch_tokens_locked(uint32_t now)
{
    uint8_t capacity = CONFIG_RAINMAKER_MATTER_CONTROLLER_ATTR_BATCH_BUCKET_CAPACITY;
    if (s_tokens > capacity) {
        s_tokens = capacity;
    }
    if (s_last_token_update_ms == 0) {
        s_last_token_update_ms = now;
        s_tokens = capacity;
        return;
    }
    uint32_t refill_ms = CONFIG_RAINMAKER_MATTER_CONTROLLER_ATTR_BATCH_REFILL_MS;
    uint32_t elapsed_ms = now - s_last_token_update_ms;
    if (elapsed_ms < refill_ms) {
        return;
    }
    uint32_t refill_count = elapsed_ms / refill_ms;
    if (refill_count > 0) {
        s_tokens = (refill_count >= capacity || s_tokens + refill_count >= capacity) ? capacity : s_tokens + refill_count;
        s_last_token_update_ms += refill_count * refill_ms;
    }
}

static void batch_timer_cb(void *arg)
{
    (void)arg;
    attr_report_msg_t msg = {};
    msg.msg_type = ATTR_REPORT_MSG_FLUSH_BATCH;
    if (s_attr_report_queue && xQueueSend(s_attr_report_queue, &msg, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Attr report queue full, drop batch flush");
    }
}

static esp_err_t ensure_batch_timer_locked(void)
{
    if (s_batch_timer) {
        return ESP_OK;
    }
    esp_timer_create_args_t args = {
        .callback = &batch_timer_cb,
        .name = "mt_attr_batch",
    };
    return esp_timer_create(&args, &s_batch_timer);
}

static void stop_batch_timer_locked(void)
{
    if (s_batch_timer) {
        esp_timer_stop(s_batch_timer);
    }
    s_batch_timer_armed = false;
}

static void arm_batch_timer_locked(void)
{
    if (app_rmaker_matter_json_pending_is_empty(s_pending_attr_delta)) {
        stop_batch_timer_locked();
        return;
    }
    if (s_batch_timer_armed) {
        return;
    }
    if (ensure_batch_timer_locked() != ESP_OK) {
        ESP_LOGW(TAG, "Failed to create attr batch timer");
        return;
    }

    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000ULL);
    refill_batch_tokens_locked(now);
    uint32_t delay_ms = MATTER_ATTR_BATCH_COALESCE_MS;
    if (s_tokens == 0) {
        uint32_t elapsed_ms = now - s_last_token_update_ms;
        delay_ms = CONFIG_RAINMAKER_MATTER_CONTROLLER_ATTR_BATCH_REFILL_MS - elapsed_ms;
        if (delay_ms == 0) {
            delay_ms = 1;
        }
    }
    if (esp_timer_start_once(s_batch_timer, (uint64_t)delay_ms * 1000ULL) == ESP_OK) {
        s_batch_timer_armed = true;
    } else {
        ESP_LOGW(TAG, "Failed to arm attr batch timer");
    }
}

static cJSON *flush_pending_attrs_all_locked(bool force)
{
    s_batch_timer_armed = false;
    if (app_rmaker_matter_json_pending_is_empty(s_pending_attr_delta)) {
        return NULL;
    }
    refill_batch_tokens_locked((uint32_t)(esp_timer_get_time() / 1000ULL));
    if (!force && s_tokens == 0) {
        arm_batch_timer_locked();
        return NULL;
    }
    if (s_tokens > 0) {
        s_tokens--;
    }
    cJSON *payload = app_rmaker_matter_json_detach_pending_all(&s_pending_attr_delta);
    stop_batch_timer_locked();
    return payload;
}

static cJSON *flush_pending_attrs_for_node_locked(uint64_t node_id, bool force)
{
    if (app_rmaker_matter_json_pending_is_empty(s_pending_attr_delta)) {
        return NULL;
    }
    refill_batch_tokens_locked((uint32_t)(esp_timer_get_time() / 1000ULL));
    if (s_tokens == 0 && !force) {
        arm_batch_timer_locked();
        return NULL;
    }
    cJSON *payload = app_rmaker_matter_json_detach_pending_node(&s_pending_attr_delta, node_id);
    if (payload && s_tokens > 0) {
        s_tokens--;
    }
    if (app_rmaker_matter_json_pending_is_empty(s_pending_attr_delta)) {
        stop_batch_timer_locked();
    }
    return payload;
}

static void queue_attribute_report(uint64_t node_id, uint16_t endpoint_id, uint32_t cluster_id, uint32_t attribute_id,
                                   chip::TLV::TLVReader *data)
{
    if (!s_attr_report_queue) {
        return;
    }

    attr_report_msg_t msg = {};
    msg.msg_type = ATTR_REPORT_MSG_ATTRIBUTE_DATA;
    msg.node_id = node_id;
    msg.endpoint_id = endpoint_id;
    msg.cluster_id = cluster_id;
    msg.attribute_id = attribute_id;

    if (data) {
        app_rmaker_matter_tlv_to_json_string(data, msg.value, sizeof(msg.value));
    } else {
        snprintf(msg.value, sizeof(msg.value), "null");
    }

    if (xQueueSend(s_attr_report_queue, &msg, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Attr report queue full, drop node 0x%llX ep %u", (unsigned long long)node_id,
                 endpoint_id);
    }
}

static void process_attribute_report(const attr_report_msg_t *msg)
{
    app_rmaker_matter_attr_report_cb_t attr_report_cb = NULL;
    void *attr_report_cb_priv = NULL;
    app_rmaker_matter_attr_report_event_t event = {};

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    node_state_t *ns = find_node_state(msg->node_id);
    if (!ns) {
        xSemaphoreGive(s_state_mutex);
        return;
    }
    stop_resubscribe_timer(ns);
    reset_fib_backoff(ns);
    attr_report_cb = s_attr_report_cb;
    attr_report_cb_priv = s_attr_report_cb_priv;
    event.node_id = ns->node_id;
    event.endpoint_id = msg->endpoint_id;
    event.cluster_id = msg->cluster_id;
    event.attribute_id = msg->attribute_id;
    event.value_json = msg->value;

    if (app_rmaker_matter_attr_json_update_tree(ns->root, msg->endpoint_id, msg->cluster_id, msg->attribute_id,
                                                msg->value)) {
        if (app_rmaker_matter_json_merge_pending_attr(&s_pending_attr_delta, ns->node_id, ns->rainmaker_node_id,
                                                      msg->endpoint_id, msg->cluster_id, msg->attribute_id,
                                                      msg->value)) {
            arm_batch_timer_locked();
        }
    }
    xSemaphoreGive(s_state_mutex);

    if (attr_report_cb) {
        attr_report_cb(&event, attr_report_cb_priv);
    }
}

static bool is_node_in_list(const matter_device_t *list, uint64_t node_id)
{
    for (const matter_device_t *d = list; d != NULL; d = d->next) {
        if (d->node_id == node_id) {
            return true;
        }
    }
    return false;
}

static void on_subscribe_connect_failure_cb(void *context)
{
    (void)context;
    ESP_LOGW(TAG, "Subscribe connect failed");
    attr_report_msg_t refresh = {};
    refresh.msg_type = ATTR_REPORT_MSG_SUBSCRIBE_CONNECT_FAILED;
    refresh.node_id = 0; /* esp-matter 1.5 callback does not expose node id; retry all offline nodes. */
    if (s_attr_report_queue) {
        xQueueSend(s_attr_report_queue, &refresh, 0);
    }
}

/* Called when subscription is terminated (device offline or subscription ended). */
static void on_subscribe_done_cb(uint64_t node_id, uint32_t subscription_id)
{
    (void)subscription_id;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    node_state_t *ns = find_node_state(node_id);
    if (ns) {
        ns->online = false;
    }
    xSemaphoreGive(s_state_mutex);
    /* Post a sentinel message so the task publishes the updated report (with this node offline). */
    attr_report_msg_t refresh = {};
    refresh.msg_type = ATTR_REPORT_MSG_SUBSCRIPTION_TERMINATED;
    refresh.node_id = node_id;
    if (s_attr_report_queue) {
        xQueueSend(s_attr_report_queue, &refresh, 0);
    }
}

void report_online(uint64_t remote_node_id)
{
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    node_state_t *ns = find_node_state(remote_node_id);
    if (!ns || ns->online) {
        xSemaphoreGive(s_state_mutex);
        return;
    }
    if (!s_attr_report_queue) {
        xSemaphoreGive(s_state_mutex);
        return;
    }
    attr_report_msg_t m = {};
    m.msg_type = ATTR_REPORT_MSG_SUBSCRIPTION_ESTABLISHED;
    m.node_id = remote_node_id;
    if (xQueueSend(s_attr_report_queue, &m, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Attr report queue full, drop subscription-established for 0x%llX",
                 (unsigned long long)remote_node_id);
    } else {
        ns->online = true;
    }
    xSemaphoreGive(s_state_mutex);
}

static void attr_report_task(void *arg)
{
    attr_report_msg_t msg;
    while (xQueueReceive(s_attr_report_queue, &msg, portMAX_DELAY) == pdTRUE) {
        if (msg.msg_type == ATTR_REPORT_MSG_ATTRIBUTE_DATA) {
            process_attribute_report(&msg);
            continue;
        }

        if (msg.msg_type == ATTR_REPORT_MSG_FLUSH_BATCH) {
            xSemaphoreTake(s_state_mutex, portMAX_DELAY);
            cJSON *payload = flush_pending_attrs_all_locked(false);
            xSemaphoreGive(s_state_mutex);
            app_rmaker_matter_attr_json_publish_matter_devices_delta(payload);
            continue;
        }

        if (msg.msg_type == ATTR_REPORT_MSG_SUBSCRIBE_CONNECT_FAILED) {
            xSemaphoreTake(s_state_mutex, portMAX_DELAY);
            if (msg.node_id == 0) {
                for (node_state_t *ns_cf = s_node_states; ns_cf != NULL; ns_cf = ns_cf->next) {
                    if (!ns_cf->online) {
                        schedule_resubscribe_attempt(ns_cf);
                    }
                }
            } else {
                node_state_t *ns_cf = find_node_state(msg.node_id);
                if (ns_cf && !ns_cf->online) {
                    ESP_LOGW(TAG, "Subscribe connect failed for 0x%llX, scheduling backoff retry",
                             (unsigned long long)msg.node_id);
                    schedule_resubscribe_attempt(ns_cf);
                }
            }
            xSemaphoreGive(s_state_mutex);
            continue;
        }
        if (msg.msg_type == ATTR_REPORT_MSG_RESUBSCRIBE_RETRY) {
            uint64_t retry_node_id;
            xSemaphoreTake(s_state_mutex, portMAX_DELAY);
            node_state_t *ns = find_node_state(msg.node_id);
            if (!ns) {
                xSemaphoreGive(s_state_mutex);
                continue;
            }
            if (ns->online) {
                stop_resubscribe_timer(ns);
                xSemaphoreGive(s_state_mutex);
                continue;
            }
            retry_node_id = ns->node_id;
            xSemaphoreGive(s_state_mutex);

            esp_err_t err = send_wildcard_subscribe(retry_node_id);

            cJSON *pending_payload = NULL;
            uint64_t offline_node_id = 0;
            char offline_rainmaker_node_id[ESP_RAINMAKER_NODE_ID_MAX_LEN] = {};
            bool publish_offline = false;

            xSemaphoreTake(s_state_mutex, portMAX_DELAY);
            ns = find_node_state(retry_node_id);
            if (ns && !ns->online) {
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "Resubscribe send_command failed for 0x%llX: %s",
                             (unsigned long long)retry_node_id, esp_err_to_name(err));
                    schedule_resubscribe_attempt(ns);
                }
                /* Online true when subscription established; connect failure uses connect_failure_cb. */
                pending_payload = flush_pending_attrs_for_node_locked(ns->node_id, true);
                offline_node_id = ns->node_id;
                strncpy(offline_rainmaker_node_id, ns->rainmaker_node_id, sizeof(offline_rainmaker_node_id) - 1);
                publish_offline = true;
            }
            xSemaphoreGive(s_state_mutex);
            app_rmaker_matter_attr_json_publish_matter_devices_delta(pending_payload);
            if (publish_offline) {
                app_rmaker_matter_attr_json_publish_online_delta(offline_node_id, offline_rainmaker_node_id, false);
                dispatch_node_online_event(offline_node_id, false);
            }
            continue;
        }

        if (msg.msg_type == ATTR_REPORT_MSG_SUBSCRIPTION_ESTABLISHED) {
            cJSON *pending_payload = NULL;
            uint64_t online_node_id = 0;
            char online_rainmaker_node_id[ESP_RAINMAKER_NODE_ID_MAX_LEN] = {};
            bool publish_online = false;

            xSemaphoreTake(s_state_mutex, portMAX_DELAY);
            node_state_t *ns_est = find_node_state(msg.node_id);
            if (ns_est) {
                stop_resubscribe_timer(ns_est);
                reset_fib_backoff(ns_est);
                pending_payload = flush_pending_attrs_for_node_locked(ns_est->node_id, true);
                online_node_id = ns_est->node_id;
                strncpy(online_rainmaker_node_id, ns_est->rainmaker_node_id, sizeof(online_rainmaker_node_id) - 1);
                publish_online = true;
            }
            xSemaphoreGive(s_state_mutex);
            app_rmaker_matter_attr_json_publish_matter_devices_delta(pending_payload);
            if (publish_online) {
                app_rmaker_matter_attr_json_publish_online_delta(online_node_id, online_rainmaker_node_id, true);
                dispatch_node_online_event(online_node_id, true);
            }
            continue;
        }

        if (msg.msg_type == ATTR_REPORT_MSG_SUBSCRIPTION_TERMINATED) {
            cJSON *pending_payload = NULL;
            uint64_t offline_node_id = 0;
            char offline_rainmaker_node_id[ESP_RAINMAKER_NODE_ID_MAX_LEN] = {};
            bool publish_offline = false;

            xSemaphoreTake(s_state_mutex, portMAX_DELAY);
            node_state_t *nst = find_node_state(msg.node_id);
            if (nst) {
                pending_payload = flush_pending_attrs_for_node_locked(nst->node_id, true);
                cJSON *empty = cJSON_CreateObject();
                if (empty) {
                    cJSON_Delete(nst->root);
                    nst->root = empty;
                } else {
                    ESP_LOGW(TAG, "Failed to alloc empty attr root for 0x%llX", (unsigned long long)msg.node_id);
                }
                offline_node_id = nst->node_id;
                strncpy(offline_rainmaker_node_id, nst->rainmaker_node_id, sizeof(offline_rainmaker_node_id) - 1);
                publish_offline = true;
                schedule_resubscribe_attempt(nst);
            }
            xSemaphoreGive(s_state_mutex);
            app_rmaker_matter_attr_json_publish_matter_devices_delta(pending_payload);
            if (publish_offline) {
                app_rmaker_matter_attr_json_publish_online_delta(offline_node_id, offline_rainmaker_node_id, false);
                dispatch_node_online_event(offline_node_id, false);
            }
            continue;
        }

        ESP_LOGW(TAG, "Unknown attr report msg_type %d", (int)msg.msg_type);
    }
}

static void on_attribute_data_cb(uint64_t remote_node_id, const chip::app::ConcreteDataAttributePath &path,
                                 chip::TLV::TLVReader *data)
{
    if (should_ignore_attribute(path.mEndpointId, path.mClusterId, path.mAttributeId)) {
        return;
    }
    report_online(remote_node_id);

    queue_attribute_report(remote_node_id, path.mEndpointId, path.mClusterId, path.mAttributeId, data);
}

static esp_err_t send_wildcard_subscribe(uint64_t node_id)
{
    esp_matter::lock::ScopedChipStackLock chip_lock(portMAX_DELAY);
    subscribe_command *cmd = chip::Platform::New<subscribe_command>(
                                 node_id, 0xFFFF, 0xFFFFFFFF, 0xFFFFFFFF, SUBSCRIBE_ATTRIBUTE, 0, 60, true, on_attribute_data_cb, nullptr,
                                 on_subscribe_done_cb, on_subscribe_connect_failure_cb);
    if (!cmd) {
        return ESP_ERR_NO_MEM;
    }
    /* send_command returns ESP_OK when session setup was queued; failure is async via connect_failure_cb.
     * On immediate ESP_FAIL, subscribe_command already deletes itself. */
    return cmd->send_command();
}

esp_err_t app_rmaker_matter_attr_report_enable(void)
{
    if (s_attr_report_queue != NULL) {
        s_attr_report_initialized = true;
        return ESP_OK;
    }
    s_state_mutex = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_state_mutex, ESP_ERR_NO_MEM, TAG, "Failed to create state mutex");
#if CONFIG_RAINMAKER_MATTER_CONTROLLER_MEM_ALLOC_MODE_EXTERNAL
    s_attr_report_queue = xQueueCreateWithCaps(MATTER_ATTR_QUEUE_SIZE, MATTER_ATTR_QUEUE_ITEM_SIZE,
                                               MALLOC_CAP_SPIRAM);
#else
    s_attr_report_queue = xQueueCreate(MATTER_ATTR_QUEUE_SIZE, MATTER_ATTR_QUEUE_ITEM_SIZE);
#endif
    if (!s_attr_report_queue) {
        vSemaphoreDelete(s_state_mutex);
        s_state_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }
    BaseType_t created = xTaskCreate(attr_report_task, "matter_attr_rpt",
                                     CONFIG_RAINMAKER_MATTER_CONTROLLER_ATTR_REPORT_TASK_STACK, NULL,
                                     MATTER_ATTR_TASK_PRIO, &s_attr_report_task);
    if (created != pdPASS) {
#if CONFIG_RAINMAKER_MATTER_CONTROLLER_MEM_ALLOC_MODE_EXTERNAL
        vQueueDeleteWithCaps(s_attr_report_queue);
#else
        vQueueDelete(s_attr_report_queue);
#endif
        s_attr_report_queue = NULL;
        vSemaphoreDelete(s_state_mutex);
        s_state_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_last_token_update_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    s_tokens = CONFIG_RAINMAKER_MATTER_CONTROLLER_ATTR_BATCH_BUCKET_CAPACITY;
    s_attr_report_initialized = true;
    return ESP_OK;
}

esp_err_t app_rmaker_matter_controller_register_attr_report_callback(app_rmaker_matter_attr_report_cb_t cb,
                                                                     void *priv_data)
{
    if (s_state_mutex) {
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    }
    s_attr_report_cb = cb;
    s_attr_report_cb_priv = cb ? priv_data : NULL;
    if (s_state_mutex) {
        xSemaphoreGive(s_state_mutex);
    }
    return ESP_OK;
}

static esp_err_t subscribe_node(uint64_t node_id, const char *rainmaker_node_id)
{
    node_state_t *ns = create_node_state(node_id, rainmaker_node_id);
    if (!ns) {
        return ESP_ERR_NO_MEM;
    }
    ns->next = s_node_states;
    s_node_states = ns;
    esp_err_t err = send_wildcard_subscribe(node_id);
    if (err != ESP_OK) {
        s_node_states = ns->next;
        free_node_state(ns);
        return err;
    }
    return ESP_OK;
}

void app_rmaker_matter_attr_report_on_device_list_update(const matter_device_t *dev_list)
{
    if (app_rmaker_matter_attr_report_enable() != ESP_OK) {
        ESP_LOGW(TAG, "Ignoring device-list update: failed to initialize attr-report runtime");
        return;
    }
    uint64_t removed_node_ids[MATTER_ATTR_MAX_REMOVED_PER_UPDATE];
    cJSON *removed_pending[MATTER_ATTR_MAX_REMOVED_PER_UPDATE] = {};
    size_t removed_count = 0;
    cJSON *removal = NULL;

    {
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);

        /* Remove states for nodes no longer in the list (CHIP shutdown done after mutex is released). */
        node_state_t *n = s_node_states;
        node_state_t *prev = NULL;
        while (n != NULL) {
            node_state_t *next = n->next;
            if (!is_node_in_list(dev_list, n->node_id)) {
                uint64_t node_id = n->node_id;
                if (prev) {
                    prev->next = next;
                } else {
                    s_node_states = next;
                }
                if (removed_count < MATTER_ATTR_MAX_REMOVED_PER_UPDATE) {
                    removed_pending[removed_count] = flush_pending_attrs_for_node_locked(node_id, true);
                    removed_node_ids[removed_count++] = node_id;
                    free_node_state(n);
                } else {
                    ESP_LOGW(TAG, "Removed node backlog > %d; call shutdown manually for 0x%llX",
                             MATTER_ATTR_MAX_REMOVED_PER_UPDATE, (unsigned long long)node_id);
                    free_node_state(n);
                }
                n = next;
                continue;
            }
            prev = n;
            n = next;
        }

        /* Subscribe to nodes in the list that we don't have yet */
        size_t subscribed_count = 0;
        size_t limit = subscribed_node_limit();
        for (const matter_device_t *d = dev_list; d != NULL; d = d->next) {
            if (find_node_state(d->node_id)) {
                subscribed_count++;
                continue;
            }
            if (subscribed_count < limit) {
                esp_err_t err = subscribe_node(d->node_id, d->rainmaker_node_id);
                if (err == ESP_OK) {
                    subscribed_count++;
                } else {
                    ESP_LOGW(TAG, "Failed to subscribe 0x%llX: %s", (unsigned long long)d->node_id,
                             esp_err_to_name(err));
                }
            } else {
                ESP_LOGW(TAG, "Skip attr subscription for 0x%llX: node subscription limit %u reached",
                         (unsigned long long)d->node_id, (unsigned)limit);
            }
        }

        if (removed_count > 0) {
            removal = cJSON_CreateObject();
            if (removal) {
                for (size_t i = 0; i < removed_count; i++) {
                    char node_key[32];
                    snprintf(node_key, sizeof(node_key), "%016llx", (unsigned long long)removed_node_ids[i]);
                    cJSON_AddItemToObject(removal, node_key, cJSON_CreateNull());
                }
            }
        }

        xSemaphoreGive(s_state_mutex);
    }

    for (size_t i = 0; i < removed_count; i++) {
        app_rmaker_matter_attr_json_publish_matter_devices_delta(removed_pending[i]);
    }
    app_rmaker_matter_attr_json_publish_matter_devices_delta(removal);

    /* Must not hold s_state_mutex: shutdown runs subscribe_done/on_subscribe_done_cb which takes the same mutex. */
    for (size_t i = 0; i < removed_count; i++) {
        esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
        send_shutdown_subscriptions(removed_node_ids[i]);
    }
}
