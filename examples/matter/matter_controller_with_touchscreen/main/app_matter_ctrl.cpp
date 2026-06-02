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
#include "esp_matter_controller_read_command.h"
#include "esp_matter_controller_subscribe_command.h"
#include "ui_matter_ctrl.h"

using namespace chip::app::Clusters;

static const char *TAG = "app_matter_ctrl";
static SemaphoreHandle_t s_device_list_mutex;
static char s_qr_payload[160];
static bool s_is_provisioned;
static bool s_empty_list_update_requested;

static constexpr uint32_t kOnOffLightDeviceTypeId = 0x0100;
static constexpr uint32_t kOnOffLightSwitchDeviceTypeId = 0x0103;
static constexpr uint32_t kOnOffPluginUnitDeviceTypeId = 0x010A;
static constexpr uint32_t kOnOffLightDeviceTypeIdV2 = 0x010D;

device_to_control_t device_to_control = {0};

typedef struct {
    uint64_t node_id;
    uint16_t endpoint_id;
} onoff_target_t;

static esp_err_t send_onoff_read(uint64_t node_id, uint16_t endpoint_id);
static esp_err_t send_onoff_subscribe(uint64_t node_id, uint16_t endpoint_id);

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

    matter_device_list_lock();
    for (node_endpoint_id_list_t *node = device_to_control.dev_list; node; node = node->next) {
        if (node->node_id == remote_node_id && node->endpoint_id == path.mEndpointId) {
            node->OnOff = onoff;
            node->onoff_known = true;
            lv_obj = node->lv_obj;
            device_type = node->device_type;
            break;
        }
    }
    matter_device_list_unlock();

    if (lv_obj) {
        ui_set_onoff_state(lv_obj, device_type, onoff);
    }
}

static esp_err_t send_onoff_read(uint64_t node_id, uint16_t endpoint_id)
{
    chip::Platform::ScopedMemoryBufferWithSize<chip::app::AttributePathParams> attr_paths;
    chip::Platform::ScopedMemoryBufferWithSize<chip::app::EventPathParams> event_paths;

    attr_paths.Alloc(1);
    if (!attr_paths.Get()) {
        return ESP_ERR_NO_MEM;
    }
    attr_paths[0] = chip::app::AttributePathParams(endpoint_id, OnOff::Id, OnOff::Attributes::OnOff::Id);

    esp_matter::controller::read_command *cmd = chip::Platform::New<esp_matter::controller::read_command>(
        node_id, std::move(attr_paths), std::move(event_paths), on_onoff_attribute_report, nullptr, nullptr);
    if (!cmd) {
        return ESP_ERR_NO_MEM;
    }
    return cmd->send_command();
}

static void request_onoff_state_work(intptr_t arg)
{
    onoff_target_t *target = (onoff_target_t *)arg;
    if (!target) {
        return;
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(send_onoff_read(target->node_id, target->endpoint_id));
    free(target);
}

static void toggle_state_work(intptr_t arg)
{
    onoff_target_t *target = (onoff_target_t *)arg;
    if (!target) {
        return;
    }

    uint64_t node_id = target->node_id;
    uint16_t endpoint_id = target->endpoint_id;
    auto on_success = [](void *, const chip::app::ConcreteCommandPath &, const chip::app::StatusIB &status,
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
    ESP_ERROR_CHECK_WITHOUT_ABORT(send_onoff_subscribe(target->node_id, target->endpoint_id));
    free(target);
}

static void shutdown_onoff_subscriptions_work(intptr_t arg)
{
    (void)arg;
    esp_matter::controller::send_shutdown_all_subscriptions();
}

static esp_err_t send_onoff_subscribe(uint64_t node_id, uint16_t endpoint_id)
{
    esp_matter::controller::subscribe_command *cmd = chip::Platform::New<esp_matter::controller::subscribe_command>(
        node_id, endpoint_id, OnOff::Id, OnOff::Attributes::OnOff::Id,
        esp_matter::controller::SUBSCRIBE_ATTRIBUTE, 1, 30, true, on_onoff_attribute_report, nullptr, nullptr, nullptr,
        true);
    if (!cmd) {
        return ESP_ERR_NO_MEM;
    }
    return cmd->send_command();
}

static esp_err_t request_onoff_state(uint64_t node_id, uint16_t endpoint_id)
{
    onoff_target_t *target = (onoff_target_t *)calloc(1, sizeof(onoff_target_t));
    if (!target) {
        return ESP_ERR_NO_MEM;
    }
    target->node_id = node_id;
    target->endpoint_id = endpoint_id;

    CHIP_ERROR err = chip::DeviceLayer::PlatformMgr().ScheduleWork(request_onoff_state_work, (intptr_t)target);
    if (err != CHIP_NO_ERROR) {
        free(target);
        ESP_LOGW(TAG, "Failed to schedule OnOff read: %s", chip::ErrorStr(err));
        return ESP_FAIL;
    }
    return ESP_OK;
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

static esp_err_t subscribe_onoff_state(uint64_t node_id, uint16_t endpoint_id)
{
    onoff_target_t *target = (onoff_target_t *)calloc(1, sizeof(onoff_target_t));
    if (!target) {
        return ESP_ERR_NO_MEM;
    }
    target->node_id = node_id;
    target->endpoint_id = endpoint_id;

    CHIP_ERROR err = chip::DeviceLayer::PlatformMgr().ScheduleWork(subscribe_onoff_state_work, (intptr_t)target);
    if (err != CHIP_NO_ERROR) {
        free(target);
        ESP_LOGW(TAG, "Failed to schedule OnOff subscribe: %s", chip::ErrorStr(err));
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void request_online_onoff_states(void)
{
    matter_device_list_lock();
    size_t capacity = device_to_control.online_num;
    if (capacity == 0) {
        matter_device_list_unlock();
        return;
    }
    onoff_target_t *targets = (onoff_target_t *)calloc(capacity, sizeof(onoff_target_t));
    if (!targets) {
        matter_device_list_unlock();
        ESP_LOGW(TAG, "Failed to allocate OnOff read targets");
        return;
    }

    size_t count = 0;
    for (node_endpoint_id_list_t *node = device_to_control.dev_list; node && count < capacity; node = node->next) {
        if (node->is_online && node->device_type != CONTROL_UNKNOWN_DEVICE) {
            targets[count].node_id = node->node_id;
            targets[count].endpoint_id = node->endpoint_id;
            ++count;
        }
    }
    matter_device_list_unlock();

    for (size_t i = 0; i < count; ++i) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(request_onoff_state(targets[i].node_id, targets[i].endpoint_id));
    }
    free(targets);
}

static void subscribe_online_onoff_states(void)
{
    matter_device_list_lock();
    size_t capacity = device_to_control.online_num;
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
        if (node->is_online && node->device_type != CONTROL_UNKNOWN_DEVICE) {
            targets[count].node_id = node->node_id;
            targets[count].endpoint_id = node->endpoint_id;
            ++count;
        }
    }
    matter_device_list_unlock();

    CHIP_ERROR err = chip::DeviceLayer::PlatformMgr().ScheduleWork(shutdown_onoff_subscriptions_work);
    if (err != CHIP_NO_ERROR) {
        ESP_LOGW(TAG, "Failed to schedule subscription shutdown: %s", chip::ErrorStr(err));
    }
    for (size_t i = 0; i < count; ++i) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(subscribe_onoff_state(targets[i].node_id, targets[i].endpoint_id));
    }
    free(targets);
}

extern "C" void matter_device_list_lock(void)
{
    if (!s_device_list_mutex) {
        s_device_list_mutex = xSemaphoreCreateRecursiveMutex();
    }
    xSemaphoreTakeRecursive(s_device_list_mutex, portMAX_DELAY);
}

extern "C" void matter_device_list_unlock(void)
{
    xSemaphoreGiveRecursive(s_device_list_mutex);
}

extern "C" void matter_ctrl_lv_obj_clear(void)
{
    matter_device_list_lock();
    for (node_endpoint_id_list_t *node = device_to_control.dev_list; node; node = node->next) {
        node->lv_obj = NULL;
    }
    matter_device_list_unlock();
}

extern "C" void matter_factory_reset(void)
{
    esp_matter::factory_reset();
}

extern "C" void matter_ctrl_change_state(intptr_t arg)
{
    node_endpoint_id_list_t *node = (node_endpoint_id_list_t *)arg;
    if (!node || !node->is_online) {
        return;
    }

    ESP_ERROR_CHECK_WITHOUT_ABORT(request_toggle_state(node->node_id, node->endpoint_id));
}

extern "C" void matter_ctrl_refresh_device_list(void)
{
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
            entry->is_online = dev->reachable;
            entry->is_rainmaker_device = dev->is_rainmaker_device;
            entry->device_type = mapped_type;
            strlcpy(entry->name, dev->endpoints[i].device_name, sizeof(entry->name));
            *tail = entry;
            tail = &entry->next;
            ++device_to_control.device_num;
            if (entry->is_online) {
                ++device_to_control.online_num;
            }
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
    request_online_onoff_states();
    subscribe_online_onoff_states();
}

extern "C" bool matter_ctrl_can_refresh_device_list(void)
{
    return s_is_provisioned && app_controller_is_ready() && app_rmaker_matter_controller_can_update_device_list();
}

extern "C" void matter_ctrl_request_device_list_update(void)
{
    if (!matter_ctrl_can_refresh_device_list()) {
        ESP_LOGW(TAG, "Device-list refresh requested before controller setup is ready");
        return;
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(app_rmaker_update_matter_device_list());
}

extern "C" const char *matter_ctrl_get_qr_payload(void)
{
    return s_qr_payload;
}

extern "C" void matter_ctrl_set_qr_payload(const char *payload)
{
    if (!payload) {
        s_qr_payload[0] = 0;
        return;
    }
    strlcpy(s_qr_payload, payload, sizeof(s_qr_payload));
    ui_matter_config_update_cb(UI_MATTER_EVT_LOADING);
}

extern "C" void matter_ctrl_set_provisioned(bool provisioned)
{
    s_is_provisioned = provisioned;
    ui_matter_config_update_cb(provisioned ? UI_MATTER_EVT_REFRESH : UI_MATTER_EVT_LOADING);
}
