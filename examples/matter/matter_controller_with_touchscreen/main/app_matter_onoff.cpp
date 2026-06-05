#include <app_matter_onoff.h>

#include <stdlib.h>

#include <app/data-model/Decode.h>
#include <platform/PlatformManager.h>
#include <app_controller.h>
#include <app_matter_ctrl.h>
#include <app_matter_device_list.h>
#include <esp_check.h>
#include <esp_log.h>
#include <esp_matter.h>
#include <esp_matter_controller_cluster_command.h>
#include <esp_matter_controller_subscribe_command.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <ui_matter_ctrl.h>

using namespace chip::app::Clusters;

static const char *TAG = "matter_onoff";
static uint32_t s_subscription_generation;
static TaskHandle_t s_offline_retry_task_handle;

static constexpr uint32_t kOfflineRetryIntervalMs = 600000;

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
        for (matter_device_list_node_t *node = matter_device_list.dev_list; node; node = node->next) {
            if (node->node_id == node_id && node->endpoint_id == endpoint_id && node->is_online != online) {
                node->is_online = online;
                if (online) {
                    ++matter_device_list.online_num;
                } else if (matter_device_list.online_num > 0) {
                    --matter_device_list.online_num;
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

    bool changed = false;

    matter_device_list_lock();
    for (matter_device_list_node_t *node = matter_device_list.dev_list; node; node = node->next) {
        if (node->node_id != remote_node_id || node->endpoint_id != path.mEndpointId) {
            continue;
        }

        if (!node->is_online) {
            node->is_online = true;
            ++matter_device_list.online_num;
            changed = true;
        }
        if (node->onoff != onoff) {
            node->onoff = onoff;
            changed = true;
        }
        break;
    }
    matter_device_list_unlock();

    if (changed) {
        ui_matter_config_update_cb(UI_MATTER_EVT_REFRESH);
    }
}

static void toggle_state_work(intptr_t arg)
{
    onoff_target_t *target = (onoff_target_t *)arg;
    ESP_RETURN_VOID_ON_FALSE(target, TAG, "OnOff toggle target is NULL");

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
                                                       node_id, endpoint_id, OnOff::Id, OnOff::Commands::Toggle::Id, nullptr, chip::NullOptional, on_success,
                                                       on_error);
    esp_err_t ret = ESP_OK;
    esp_err_t err = ESP_OK;
    ESP_GOTO_ON_FALSE(cmd, ESP_ERR_NO_MEM, cleanup, TAG, "Failed to allocate Toggle command");

    err = cmd->send_command();
    if (err != ESP_OK) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(err);
    }

cleanup:
    (void)ret;
    free(target);
}

static void subscribe_onoff_state_work(intptr_t arg)
{
    onoff_target_t *target = (onoff_target_t *)arg;
    ESP_RETURN_VOID_ON_FALSE(target, TAG, "OnOff subscribe target is NULL");
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
    ESP_RETURN_ON_FALSE(cmd, ESP_ERR_NO_MEM, TAG, "Failed to allocate OnOff subscribe command");
    return cmd->send_command();
}

esp_err_t matter_onoff_toggle(uint64_t node_id, uint16_t endpoint_id)
{
    onoff_target_t *target = (onoff_target_t *)calloc(1, sizeof(onoff_target_t));
    ESP_RETURN_ON_FALSE(target, ESP_ERR_NO_MEM, TAG, "Failed to allocate OnOff toggle target");
    target->node_id = node_id;
    target->endpoint_id = endpoint_id;

    CHIP_ERROR err = chip::DeviceLayer::PlatformMgr().ScheduleWork(toggle_state_work, (intptr_t)target);
    esp_err_t ret = ESP_OK;
    ESP_GOTO_ON_FALSE(err == CHIP_NO_ERROR, ESP_FAIL, cleanup, TAG, "Failed to schedule OnOff toggle: %s",
                      chip::ErrorStr(err));

cleanup:
    if (ret != ESP_OK) {
        free(target);
    }
    return ret;
}

static esp_err_t subscribe_onoff_state(uint64_t node_id, uint16_t endpoint_id, uint32_t generation)
{
    onoff_target_t *target = (onoff_target_t *)calloc(1, sizeof(onoff_target_t));
    ESP_RETURN_ON_FALSE(target, ESP_ERR_NO_MEM, TAG, "Failed to allocate OnOff subscribe target");
    target->node_id = node_id;
    target->endpoint_id = endpoint_id;
    target->generation = generation;

    CHIP_ERROR err = chip::DeviceLayer::PlatformMgr().ScheduleWork(subscribe_onoff_state_work, (intptr_t)target);
    esp_err_t ret = ESP_OK;
    ESP_GOTO_ON_FALSE(err == CHIP_NO_ERROR, ESP_FAIL, cleanup, TAG, "Failed to schedule OnOff subscribe: %s",
                      chip::ErrorStr(err));

cleanup:
    if (ret != ESP_OK) {
        free(target);
    }
    return ret;
}

void matter_onoff_subscribe_all(void)
{
    matter_device_list_lock();
    size_t capacity = matter_device_list.device_num;
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
    for (matter_device_list_node_t *node = matter_device_list.dev_list; node && count < capacity; node = node->next) {
        if (node->device_type != matter_device_list_type_unknown) {
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
    if (!matter_ctrl_is_provisioned() || !app_controller_is_ready()) {
        return;
    }

    matter_device_list_lock();
    size_t capacity = matter_device_list.device_num;
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
    for (matter_device_list_node_t *node = matter_device_list.dev_list; node && count < capacity; node = node->next) {
        if (!node->is_online && node->device_type != matter_device_list_type_unknown) {
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

void matter_onoff_ensure_retry_task_started(void)
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
