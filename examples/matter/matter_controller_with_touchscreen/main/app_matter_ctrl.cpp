#include "app_matter_ctrl.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <utility>

#include <app/data-model/Decode.h>
#include <platform/PlatformManager.h>
#include "app_controller.h"
#include "app_rmaker_matter_controller.h"
#include "app_rmaker_matter_device_list.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_matter.h"
#include "esp_matter_controller_cluster_command.h"
#include "esp_matter_controller_subscribe_command.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ui_matter_ctrl.h"

using namespace chip::app::Clusters;

static const char *TAG = "app_matter_ctrl";
static SemaphoreHandle_t s_device_list_mutex;
static char s_qr_payload[160];
static bool s_is_provisioned;
static bool s_empty_list_update_requested;
static uint32_t s_subscription_generation;
static TaskHandle_t s_offline_retry_task_handle;

static constexpr uint32_t kOnOffLightDeviceTypeId = 0x0100;
static constexpr uint32_t kOnOffLightSwitchDeviceTypeId = 0x0103;
static constexpr uint32_t kOnOffPluginUnitDeviceTypeId = 0x010A;
static constexpr uint32_t kOnOffLightDeviceTypeIdV2 = 0x010D;

static constexpr uint32_t kOfflineRetryIntervalMs = 600000;

device_to_control_t device_to_control = {0};

typedef struct {
    uint64_t node_id;
    uint16_t endpoint_id;
    uint32_t generation;
} onoff_target_t;

static void on_onoff_attribute_report(uint64_t remote_node_id, const chip::app::ConcreteDataAttributePath &path,
                                      chip::TLV::TLVReader *data);
static esp_err_t send_onoff_subscribe(uint64_t node_id, uint16_t endpoint_id, uint32_t generation);

static void mark_endpoint_online(uint64_t node_id, uint16_t endpoint_id, bool online, uint32_t generation)
{
    bool changed = false;

    matter_device_list_lock();
    if (generation == s_subscription_generation) {
        for (node_endpoint_id_list_t *node = device_to_control.dev_list; node; node = node->next) {
            if (node->node_id == node_id && node->endpoint_id == endpoint_id && node->is_online != online) {
                node->is_online = online;
                if (online) {
                    ++device_to_control.online_num;
                } else if (device_to_control.online_num > 0) {
                    --device_to_control.online_num;
                    node->onoff = false;
                }
                changed = true;
                break;
            }
        }
    }
    matter_device_list_unlock();

    if (changed) {
        ui_matter_config_update_cb(UI_MATTER_EVT_REFRESH);
    }
}

class onoff_subscribe_command : public esp_matter::controller::subscribe_command {
public:
    onoff_subscribe_command(uint64_t node_id, uint16_t endpoint_id, uint32_t generation)
        : subscribe_command(node_id, endpoint_id, OnOff::Id, OnOff::Attributes::OnOff::Id,
                            esp_matter::controller::SUBSCRIBE_ATTRIBUTE, 1, 30, true, on_onoff_attribute_report,
                            nullptr, nullptr, on_connection_failure, true)
        , m_node_id(node_id)
        , m_endpoint_id(endpoint_id)
        , m_generation(generation)
    {
    }

    void OnSubscriptionEstablished(chip::SubscriptionId subscription_id) override
    {
        subscribe_command::OnSubscriptionEstablished(subscription_id);
        mark_endpoint_online(m_node_id, m_endpoint_id, true, m_generation);
    }

    void OnDone(chip::app::ReadClient *client) override
    {
        mark_endpoint_online(m_node_id, m_endpoint_id, false, m_generation);
        subscribe_command::OnDone(client);
    }

private:
    static void on_connection_failure(void *command)
    {
        auto *cmd = static_cast<onoff_subscribe_command *>(command);
        mark_endpoint_online(cmd->m_node_id, cmd->m_endpoint_id, false, cmd->m_generation);
    }

    uint64_t m_node_id;
    uint16_t m_endpoint_id;
    uint32_t m_generation;
};

static void free_device_list_locked(void)
{
    node_endpoint_id_list_t *node = device_to_control.dev_list;
    while (node) {
        node_endpoint_id_list_t *next = node->next;
        free(node);
        node = next;
    }
    device_to_control.dev_list = NULL;
    device_to_control.device_num = 0;
    device_to_control.online_num = 0;
}

static size_t map_device_type(uint32_t device_type_id)
{
    switch (device_type_id) {
    case kOnOffLightDeviceTypeId:
    case kOnOffLightDeviceTypeIdV2:
        return CONTROL_LIGHT_DEVICE;
    case kOnOffPluginUnitDeviceTypeId:
        return CONTROL_PLUG_DEVICE;
    case kOnOffLightSwitchDeviceTypeId:
        return CONTROL_SWITCH_DEVICE;
    default:
        return CONTROL_UNKNOWN_DEVICE;
    }
}

static void on_onoff_attribute_report(uint64_t remote_node_id, const chip::app::ConcreteDataAttributePath &path,
                                      chip::TLV::TLVReader *data)
{
    if (!data || path.mClusterId != OnOff::Id || path.mAttributeId != OnOff::Attributes::OnOff::Id) {
        return;
    }

    bool onoff = false;
    CHIP_ERROR err = chip::app::DataModel::Decode(*data, onoff);
    if (err != CHIP_NO_ERROR) {
        ESP_LOGW(TAG, "Failed to decode OnOff attribute: %s", chip::ErrorStr(err));
        return;
    }

    lv_obj_t *lv_obj = NULL;
    size_t device_type = CONTROL_UNKNOWN_DEVICE;
    bool online_changed = false;

    matter_device_list_lock();
    for (node_endpoint_id_list_t *node = device_to_control.dev_list; node; node = node->next) {
        if (node->node_id == remote_node_id && node->endpoint_id == path.mEndpointId) {
            if (!node->is_online) {
                node->is_online = true;
                ++device_to_control.online_num;
                online_changed = true;
            }
            node->onoff = onoff;
            lv_obj = node->lv_obj;
            device_type = node->device_type;
            break;
        }
    }
    matter_device_list_unlock();

    if (online_changed) {
        ui_matter_config_update_cb(UI_MATTER_EVT_REFRESH);
    } else if (lv_obj) {
        ui_set_onoff_state(lv_obj, device_type, onoff);
    }
}

static void toggle_state_work(intptr_t arg)
{
    onoff_target_t *target = (onoff_target_t *)arg;
    if (!target) {
        return;
    }

    uint64_t node_id = target->node_id;
    uint16_t endpoint_id = target->endpoint_id;
    auto on_success = [](void *, const chip::app::ConcreteCommandPath &, const chip::app::StatusIB & status,
    chip::TLV::TLVReader *) {
        if (!status.IsSuccess()) {
            ESP_LOGW(TAG, "Toggle command returned failure status");
        }
    };
    auto on_error = [](void *, CHIP_ERROR error) {
        ESP_LOGW(TAG, "Toggle command failed: %s", chip::ErrorStr(error));
    };

    esp_matter::controller::cluster_command *cmd = chip::Platform::New<esp_matter::controller::cluster_command>(
                                                       node_id, endpoint_id, OnOff::Id, OnOff::Commands::Toggle::Id, nullptr, chip::NullOptional, on_success, on_error);
    if (!cmd) {
        ESP_LOGW(TAG, "Failed to allocate Toggle command");
        free(target);
        return;
    }

    esp_err_t err = cmd->send_command();
    if (err != ESP_OK) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(err);
    }
    free(target);
}

static void subscribe_onoff_state_work(intptr_t arg)
{
    onoff_target_t *target = (onoff_target_t *)arg;
    if (!target) {
        return;
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(send_onoff_subscribe(target->node_id, target->endpoint_id, target->generation));
    free(target);
}

static void shutdown_onoff_subscriptions_work(intptr_t arg)
{
    (void)arg;
    esp_matter::controller::send_shutdown_all_subscriptions();
}

static esp_err_t send_onoff_subscribe(uint64_t node_id, uint16_t endpoint_id, uint32_t generation)
{
    onoff_subscribe_command *cmd = chip::Platform::New<onoff_subscribe_command>(node_id, endpoint_id, generation);
    if (!cmd) {
        return ESP_ERR_NO_MEM;
    }
    return cmd->send_command();
}

static esp_err_t request_toggle_state(uint64_t node_id, uint16_t endpoint_id)
{
    onoff_target_t *target = (onoff_target_t *)calloc(1, sizeof(onoff_target_t));
    if (!target) {
        return ESP_ERR_NO_MEM;
    }
    target->node_id = node_id;
    target->endpoint_id = endpoint_id;

    CHIP_ERROR err = chip::DeviceLayer::PlatformMgr().ScheduleWork(toggle_state_work, (intptr_t)target);
    if (err != CHIP_NO_ERROR) {
        free(target);
        ESP_LOGW(TAG, "Failed to schedule OnOff toggle: %s", chip::ErrorStr(err));
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t subscribe_onoff_state(uint64_t node_id, uint16_t endpoint_id, uint32_t generation)
{
    onoff_target_t *target = (onoff_target_t *)calloc(1, sizeof(onoff_target_t));
    if (!target) {
        return ESP_ERR_NO_MEM;
    }
    target->node_id = node_id;
    target->endpoint_id = endpoint_id;
    target->generation = generation;

    CHIP_ERROR err = chip::DeviceLayer::PlatformMgr().ScheduleWork(subscribe_onoff_state_work, (intptr_t)target);
    if (err != CHIP_NO_ERROR) {
        free(target);
        ESP_LOGW(TAG, "Failed to schedule OnOff subscribe: %s", chip::ErrorStr(err));
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void subscribe_online_onoff_states(void)
{
    matter_device_list_lock();
    size_t capacity = device_to_control.device_num;
    if (capacity == 0) {
        matter_device_list_unlock();
        return;
    }
    onoff_target_t *targets = (onoff_target_t *)calloc(capacity, sizeof(onoff_target_t));
    if (!targets) {
        matter_device_list_unlock();
        ESP_LOGW(TAG, "Failed to allocate OnOff subscribe targets");
        return;
    }

    size_t count = 0;
    for (node_endpoint_id_list_t *node = device_to_control.dev_list; node && count < capacity; node = node->next) {
        if (node->device_type != CONTROL_UNKNOWN_DEVICE) {
            targets[count].node_id = node->node_id;
            targets[count].endpoint_id = node->endpoint_id;
            ++count;
        }
    }
    matter_device_list_unlock();

    uint32_t generation = ++s_subscription_generation;
    CHIP_ERROR err = chip::DeviceLayer::PlatformMgr().ScheduleWork(shutdown_onoff_subscriptions_work);
    if (err != CHIP_NO_ERROR) {
        ESP_LOGW(TAG, "Failed to schedule subscription shutdown: %s", chip::ErrorStr(err));
    }
    for (size_t i = 0; i < count; ++i) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(subscribe_onoff_state(targets[i].node_id, targets[i].endpoint_id, generation));
    }
    free(targets);
}

static void retry_offline_onoff_subscriptions(void)
{
    if (!s_is_provisioned || !app_controller_is_ready()) {
        return;
    }

    matter_device_list_lock();
    size_t capacity = device_to_control.device_num;
    if (capacity == 0) {
        matter_device_list_unlock();
        return;
    }

    onoff_target_t *targets = (onoff_target_t *)calloc(capacity, sizeof(onoff_target_t));
    if (!targets) {
        matter_device_list_unlock();
        ESP_LOGW(TAG, "Failed to allocate offline subscription retry targets");
        return;
    }

    uint32_t generation = s_subscription_generation;
    size_t count = 0;
    for (node_endpoint_id_list_t *node = device_to_control.dev_list; node && count < capacity; node = node->next) {
        if (!node->is_online && node->device_type != CONTROL_UNKNOWN_DEVICE) {
            targets[count].node_id = node->node_id;
            targets[count].endpoint_id = node->endpoint_id;
            ++count;
        }
    }
    matter_device_list_unlock();

    for (size_t i = 0; i < count; ++i) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(subscribe_onoff_state(targets[i].node_id, targets[i].endpoint_id, generation));
    }
    free(targets);
}

static void offline_subscription_retry_task(void *arg)
{
    (void)arg;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(kOfflineRetryIntervalMs));
        retry_offline_onoff_subscriptions();
    }
}

static void ensure_offline_retry_task_started(void)
{
    if (s_offline_retry_task_handle) {
        return;
    }

    BaseType_t ret = xTaskCreate(offline_subscription_retry_task, "matter_offline_retry", 4096, NULL, tskIDLE_PRIORITY,
                                 &s_offline_retry_task_handle);
    if (ret != pdPASS) {
        s_offline_retry_task_handle = NULL;
        ESP_LOGW(TAG, "Failed to start offline subscription retry task");
    }
}

void matter_device_list_lock(void)
{
    if (!s_device_list_mutex) {
        s_device_list_mutex = xSemaphoreCreateRecursiveMutex();
    }
    xSemaphoreTakeRecursive(s_device_list_mutex, portMAX_DELAY);
}

void matter_device_list_unlock(void)
{
    xSemaphoreGiveRecursive(s_device_list_mutex);
}

void matter_ctrl_lv_obj_clear(void)
{
    matter_device_list_lock();
    for (node_endpoint_id_list_t *node = device_to_control.dev_list; node; node = node->next) {
        node->lv_obj = NULL;
    }
    matter_device_list_unlock();
}

void matter_factory_reset(void)
{
    esp_matter::factory_reset();
}

void matter_ctrl_change_state(intptr_t arg)
{
    node_endpoint_id_list_t *node = (node_endpoint_id_list_t *)arg;
    if (!node || !node->is_online) {
        return;
    }

    ESP_ERROR_CHECK_WITHOUT_ABORT(request_toggle_state(node->node_id, node->endpoint_id));
}

void matter_ctrl_refresh_device_list(void)
{
    if (s_is_provisioned) {
        ensure_offline_retry_task_started();
    }

    matter_device_t *dev_list = app_rmaker_get_matter_device_list();

    if (!dev_list) {
        ESP_LOGW(TAG, "No Matter devices returned by app_rmaker_get_matter_device_list()");
        if (s_is_provisioned && app_controller_is_ready() && !s_empty_list_update_requested) {
            s_empty_list_update_requested = true;
            app_rmaker_update_matter_device_list();
        }
    }

    matter_device_list_lock();
    free_device_list_locked();

    node_endpoint_id_list_t **tail = &device_to_control.dev_list;
    for (matter_device_t *dev = dev_list; dev; dev = dev->next) {
        ESP_LOGI(TAG, "Refresh device: node=0x%" PRIx32 "%08" PRIx32 " reachable=%d endpoint_count=%u rainmaker=%d",
                 (uint32_t)(dev->node_id >> 32), (uint32_t)(dev->node_id & 0xFFFFFFFF), dev->reachable,
                 dev->endpoint_count, dev->is_rainmaker_device);
        for (uint8_t i = 0; i < dev->endpoint_count; ++i) {
            size_t mapped_type = map_device_type(dev->endpoints[i].device_type_id);
            ESP_LOGI(TAG, "  endpoint=%u device_type_id=0x%" PRIx32 " mapped_type=%u name=%s",
                     dev->endpoints[i].endpoint_id, dev->endpoints[i].device_type_id, mapped_type,
                     dev->endpoints[i].device_name);
            node_endpoint_id_list_t *entry = (node_endpoint_id_list_t *)calloc(1, sizeof(node_endpoint_id_list_t));
            if (!entry) {
                continue;
            }
            entry->node_id = dev->node_id;
            entry->endpoint_id = dev->endpoints[i].endpoint_id;
            entry->device_type = mapped_type;
            strlcpy(entry->name, dev->endpoints[i].device_name, sizeof(entry->name));
            *tail = entry;
            tail = &entry->next;
            ++device_to_control.device_num;
        }
    }
    matter_device_list_unlock();

    app_rmaker_free_matter_device_list(dev_list);
    if (dev_list) {
        s_empty_list_update_requested = false;
    }

    ESP_LOGI(TAG, "Refresh complete: device_num=%u online_num=%u", device_to_control.device_num,
             device_to_control.online_num);

    ui_matter_config_update_cb(s_is_provisioned ? UI_MATTER_EVT_REFRESH : UI_MATTER_EVT_LOADING);
    subscribe_online_onoff_states();
}

bool matter_ctrl_can_refresh_device_list(void)
{
    return s_is_provisioned && app_controller_is_ready() && app_rmaker_matter_controller_can_update_device_list();
}

void matter_ctrl_request_device_list_update(void)
{
    if (!matter_ctrl_can_refresh_device_list()) {
        ESP_LOGW(TAG, "Device-list refresh requested before controller setup is ready");
        return;
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(app_rmaker_update_matter_device_list());
}

const char *matter_ctrl_get_qr_payload(void)
{
    return s_qr_payload;
}

void matter_ctrl_set_qr_payload(const char *payload)
{
    if (!payload) {
        s_qr_payload[0] = 0;
        return;
    }
    strlcpy(s_qr_payload, payload, sizeof(s_qr_payload));
    ui_matter_config_update_cb(UI_MATTER_EVT_LOADING);
}

void matter_ctrl_set_provisioned(bool provisioned)
{
    s_is_provisioned = provisioned;
    if (provisioned) {
        ensure_offline_retry_task_started();
    }
    ui_matter_config_update_cb(provisioned ? UI_MATTER_EVT_REFRESH : UI_MATTER_EVT_LOADING);
}
