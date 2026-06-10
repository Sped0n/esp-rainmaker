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
#include <app_rmaker_matter_json_tlv.h>
#include <esp_matter_controller_subscribe_command.h>
#include <esp_matter_controller_utils.h>
#include <esp_matter_core.h>

using namespace esp_matter;
using namespace esp_matter::controller;

#define TAG "rmaker_matter_attr"

/* Filter: ignore endpoint 0, global attributes 0xFFF8-0xFFFD, cluster 0x1D */
#define MATTER_ATTR_FILTER_EP0            0
#define MATTER_ATTR_FILTER_GLOBAL_ATTR_MIN 0xFFF8
#define MATTER_ATTR_FILTER_GLOBAL_ATTR_MAX 0xFFFD
#define MATTER_ATTR_FILTER_CLUSTER_1D      0x1D

#define MATTER_ATTR_VALUE_MAX_LEN         384
#define MATTER_ATTR_MAX_REMOVED_PER_UPDATE 32
#define MATTER_ATTR_TASK_PRIO             5

typedef enum {
    ATTR_REPORT_MSG_ATTRIBUTE_DATA = 0,
    ATTR_REPORT_MSG_SUBSCRIPTION_TERMINATED,
    ATTR_REPORT_MSG_RESUBSCRIBE_RETRY,
    ATTR_REPORT_MSG_SUBSCRIBE_CONNECT_FAILED,
    ATTR_REPORT_MSG_SUBSCRIPTION_ESTABLISHED,
} attr_report_msg_type_t;

typedef struct {
    attr_report_msg_type_t msg_type;
    uint64_t node_id;
    uint16_t endpoint_id;
    uint32_t cluster_id;
    uint32_t attribute_id;
    char value[MATTER_ATTR_VALUE_MAX_LEN];
} attr_report_msg_t;

#define MATTER_ATTR_QUEUE_ITEM_SIZE       sizeof(attr_report_msg_t)

#define MATTER_ATTR_FIB_MAX_SEC 3600u
/** First resubscribe delay after going offline (then Fibonacci growth, capped by MATTER_ATTR_FIB_MAX_SEC). */
#define MATTER_ATTR_FIB_FIRST_SEC 60u

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

static bool is_node_in_list(matter_device_t *list, uint64_t node_id)
{
    for (matter_device_t *d = list; d != NULL; d = d->next) {
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
            matter_device_t *dev_list = app_rmaker_get_matter_device_list();
            if (!dev_list || !is_node_in_list(dev_list, msg.node_id)) {
                if (dev_list) {
                    app_rmaker_free_matter_device_list(dev_list);
                }
                xSemaphoreTake(s_state_mutex, portMAX_DELAY);
                node_state_t *ns_stop = find_node_state(msg.node_id);
                if (ns_stop) {
                    stop_resubscribe_timer(ns_stop);
                }
                xSemaphoreGive(s_state_mutex);
                continue;
            }
            app_rmaker_free_matter_device_list(dev_list);

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

            xSemaphoreTake(s_state_mutex, portMAX_DELAY);
            ns = find_node_state(retry_node_id);
            if (ns && !ns->online) {
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "Resubscribe send_command failed for 0x%llX: %s",
                             (unsigned long long)retry_node_id, esp_err_to_name(err));
                    schedule_resubscribe_attempt(ns);
                }
                /* Online true when subscription established; connect failure uses connect_failure_cb. */
                app_rmaker_matter_attr_json_publish_online_delta(ns->node_id, ns->rainmaker_node_id, false);
            }
            xSemaphoreGive(s_state_mutex);
            continue;
        }

        if (msg.msg_type == ATTR_REPORT_MSG_SUBSCRIPTION_ESTABLISHED) {
            xSemaphoreTake(s_state_mutex, portMAX_DELAY);
            node_state_t *ns_est = find_node_state(msg.node_id);
            if (ns_est) {
                stop_resubscribe_timer(ns_est);
                reset_fib_backoff(ns_est);
                app_rmaker_matter_attr_json_publish_online_delta(ns_est->node_id, ns_est->rainmaker_node_id, true);
            }
            xSemaphoreGive(s_state_mutex);
            continue;
        }

        if (msg.msg_type == ATTR_REPORT_MSG_SUBSCRIPTION_TERMINATED) {
            xSemaphoreTake(s_state_mutex, portMAX_DELAY);
            node_state_t *nst = find_node_state(msg.node_id);
            if (nst) {
                cJSON *empty = cJSON_CreateObject();
                if (empty) {
                    cJSON_Delete(nst->root);
                    nst->root = empty;
                } else {
                    ESP_LOGW(TAG, "Failed to alloc empty attr root for 0x%llX", (unsigned long long)msg.node_id);
                }
                app_rmaker_matter_attr_json_publish_online_delta(nst->node_id, nst->rainmaker_node_id, false);
                schedule_resubscribe_attempt(nst);
            }
            xSemaphoreGive(s_state_mutex);
            continue;
        }

        if (msg.msg_type != ATTR_REPORT_MSG_ATTRIBUTE_DATA) {
            ESP_LOGW(TAG, "Unknown attr report msg_type %d", (int)msg.msg_type);
            continue;
        }

        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        node_state_t *ns = find_node_state(msg.node_id);
        if (!ns) {
            xSemaphoreGive(s_state_mutex);
            continue;
        }
        stop_resubscribe_timer(ns);
        reset_fib_backoff(ns);
        if (app_rmaker_matter_attr_json_update_tree(ns->root, msg.endpoint_id, msg.cluster_id, msg.attribute_id,
                                                    msg.value)) {
            cJSON *endpoints = cJSON_CreateObject();
            cJSON *wrapper = cJSON_CreateObject();
            cJSON *root = cJSON_CreateObject();
            if (!endpoints || !wrapper || !root) {
                cJSON_Delete(endpoints);
                cJSON_Delete(wrapper);
                cJSON_Delete(root);
                xSemaphoreGive(s_state_mutex);
                continue;
            }
            if (!app_rmaker_matter_attr_json_update_tree(endpoints, msg.endpoint_id, msg.cluster_id, msg.attribute_id,
                                                        msg.value)) {
                cJSON_Delete(endpoints);
                cJSON_Delete(wrapper);
                cJSON_Delete(root);
                xSemaphoreGive(s_state_mutex);
                continue;
            }
            char node_key[32];
            snprintf(node_key, sizeof(node_key), "%016llx", (unsigned long long)ns->node_id);
            cJSON_AddItemToObject(wrapper, "rainmaker_node_id", cJSON_CreateString(ns->rainmaker_node_id));
            cJSON_AddItemToObject(wrapper, "endpoints", endpoints);
            cJSON_AddItemToObject(root, node_key, wrapper);
            app_rmaker_matter_attr_json_publish_matter_devices_delta(root);
        }
        xSemaphoreGive(s_state_mutex);
    }
}

static void on_attribute_data_cb(uint64_t remote_node_id, const chip::app::ConcreteDataAttributePath &path,
                                  chip::TLV::TLVReader *data)
{
    if (should_ignore_attribute(path.mEndpointId, path.mClusterId, path.mAttributeId)) {
        return;
    }
    report_online(remote_node_id);

    attr_report_msg_t msg = {};
    msg.msg_type = ATTR_REPORT_MSG_ATTRIBUTE_DATA;
    msg.node_id = remote_node_id;
    msg.endpoint_id = path.mEndpointId;
    msg.cluster_id = path.mClusterId;
    msg.attribute_id = path.mAttributeId;
    if (data) {
        app_rmaker_matter_tlv_to_json_string(data, msg.value, sizeof(msg.value));
    } else {
        snprintf(msg.value, sizeof(msg.value), "null");
    }
    if (s_attr_report_queue && xQueueSend(s_attr_report_queue, &msg, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Attr report queue full, drop node 0x%llX ep %u", (unsigned long long)remote_node_id,
                 path.mEndpointId);
    }
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

esp_err_t app_rmaker_matter_controller_attr_report_enable(void)
{
    if (s_attr_report_queue != NULL) {
        s_attr_report_initialized = true;
        return ESP_OK;
    }
    s_state_mutex = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_state_mutex, ESP_ERR_NO_MEM, TAG, "Failed to create state mutex");
#if CONFIG_RAINMAKER_MATTER_CONTROLLER_MEM_ALLOC_MODE_EXTERNAL
    s_attr_report_queue = xQueueCreateWithCaps(CONFIG_RAINMAKER_MATTER_CONTROLLER_ATTR_QUEUE_SIZE,
                                               MATTER_ATTR_QUEUE_ITEM_SIZE, MALLOC_CAP_SPIRAM);
#else
    s_attr_report_queue = xQueueCreate(CONFIG_RAINMAKER_MATTER_CONTROLLER_ATTR_QUEUE_SIZE,
                                       MATTER_ATTR_QUEUE_ITEM_SIZE);
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
    s_attr_report_initialized = true;
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

void app_rmaker_matter_controller_attr_report_on_device_list_update(void)
{
    if (!s_attr_report_initialized || !s_state_mutex || !s_attr_report_queue) {
        ESP_LOGD(TAG, "Ignoring device-list update before attr-report init");
        return;
    }
    matter_device_t *dev_list = app_rmaker_get_matter_device_list();
    uint64_t removed_node_ids[MATTER_ATTR_MAX_REMOVED_PER_UPDATE];
    size_t removed_count = 0;

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
                free_node_state(n);
                if (removed_count < MATTER_ATTR_MAX_REMOVED_PER_UPDATE) {
                    removed_node_ids[removed_count++] = node_id;
                } else {
                    ESP_LOGW(TAG, "Removed node backlog > %d; call shutdown manually for 0x%llX",
                             MATTER_ATTR_MAX_REMOVED_PER_UPDATE, (unsigned long long)node_id);
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
        for (matter_device_t *d = dev_list; d != NULL; d = d->next) {
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
            cJSON *removal = cJSON_CreateObject();
            if (removal) {
                for (size_t i = 0; i < removed_count; i++) {
                    char node_key[32];
                    snprintf(node_key, sizeof(node_key), "%016llx", (unsigned long long)removed_node_ids[i]);
                    cJSON_AddItemToObject(removal, node_key, cJSON_CreateNull());
                }
                app_rmaker_matter_attr_json_publish_matter_devices_delta(removal);
            }
        }

        xSemaphoreGive(s_state_mutex);
    }

    /* Must not hold s_state_mutex: shutdown runs subscribe_done/on_subscribe_done_cb which takes the same mutex. */
    for (size_t i = 0; i < removed_count; i++) {
        esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
        send_shutdown_subscriptions(removed_node_ids[i]);
    }

    app_rmaker_free_matter_device_list(dev_list);
}
