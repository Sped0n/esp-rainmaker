/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <app_matter_ctrl.h>
#include <app_matter_device_list.h>

#include <app_controller.h>
#include <app_rmaker_matter_controller.h>
#include <app_rmaker_matter_device_list.h>
#include <esp_check.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <sdkconfig.h>

#include <lib/core/CHIPConfig.h>

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "matter_device_list";
static SemaphoreHandle_t s_device_list_mutex;
static bool s_empty_list_update_requested;

static constexpr uint32_t kOnOffLightDeviceTypeId = 0x0100;
static constexpr uint32_t kOnOffLightSwitchDeviceTypeId = 0x0103;
static constexpr uint32_t kOnOffPluginUnitDeviceTypeId = 0x010A;
static constexpr uint32_t kOnOffLightDeviceTypeIdV2 = 0x010D;

static constexpr size_t kMatterControllerMaxActiveDevices = CHIP_CONFIG_CONTROLLER_MAX_ACTIVE_DEVICES;
static constexpr size_t kMatterMaxExchangeContexts = CONFIG_MAX_EXCHANGE_CONTEXTS;
static constexpr size_t kMatterDeviceListMaxDevices = kMatterControllerMaxActiveDevices < kMatterMaxExchangeContexts
                                                      ? kMatterControllerMaxActiveDevices
                                                      : kMatterMaxExchangeContexts;
static_assert(kMatterDeviceListMaxDevices > 0, "Matter controller device-list limit must be non-zero");

matter_device_list_state_t matter_device_list = {0};

static size_t map_device_type(uint32_t device_type_id)
{
    switch (device_type_id) {
    case kOnOffLightDeviceTypeId:
    case kOnOffLightDeviceTypeIdV2:
        return matter_device_list_type_light;
    case kOnOffPluginUnitDeviceTypeId:
        return matter_device_list_type_plug;
    case kOnOffLightSwitchDeviceTypeId:
        return matter_device_list_type_switch;
    default:
        return matter_device_list_type_unknown;
    }
}

void matter_device_list_lock(void)
{
    if (!s_device_list_mutex) {
        s_device_list_mutex = xSemaphoreCreateRecursiveMutex();
    }
    ESP_RETURN_VOID_ON_FALSE(s_device_list_mutex, TAG, "Failed to create device-list mutex");
    xSemaphoreTakeRecursive(s_device_list_mutex, portMAX_DELAY);
}

void matter_device_list_unlock(void)
{
    xSemaphoreGiveRecursive(s_device_list_mutex);
}

void matter_device_list_rebuild(void)
{
    matter_device_t *dev_list = app_rmaker_get_matter_device_list();

    if (!dev_list) {
        ESP_LOGW(TAG, "No Matter devices returned by app_rmaker_get_matter_device_list()");
        if (matter_ctrl_is_provisioned() && app_controller_is_ready() && !s_empty_list_update_requested) {
            s_empty_list_update_requested = true;
            app_rmaker_update_matter_device_list();
        }
    }

    matter_device_list_lock();

    matter_device_list_node_t *node = matter_device_list.dev_list;
    while (node) {
        matter_device_list_node_t *next = node->next;
        free(node);
        node = next;
    }
    matter_device_list.dev_list = NULL;
    matter_device_list.device_num = 0;
    matter_device_list.online_num = 0;

    matter_device_list_node_t **tail = &matter_device_list.dev_list;
    size_t skipped_unsupported = 0;
    size_t skipped_limit = 0;
    for (matter_device_t *dev = dev_list; dev; dev = dev->next) {
        ESP_LOGI(TAG, "Refresh device: node=0x%" PRIx32 "%08" PRIx32 " reachable=%d endpoint_count=%u rainmaker=%d",
                 (uint32_t)(dev->node_id >> 32), (uint32_t)(dev->node_id & 0xFFFFFFFF), dev->reachable,
                 dev->endpoint_count, dev->is_rainmaker_device);
        for (uint8_t i = 0; i < dev->endpoint_count; ++i) {
            size_t mapped_type = map_device_type(dev->endpoints[i].device_type_id);
            ESP_LOGI(TAG, "  endpoint=%u device_type_id=0x%" PRIx32 " mapped_type=%zu name=%s",
                     dev->endpoints[i].endpoint_id, dev->endpoints[i].device_type_id, mapped_type,
                     dev->endpoints[i].device_name);
            if (mapped_type == matter_device_list_type_unknown) {
                ++skipped_unsupported;
                continue;
            }
            if (matter_device_list.device_num >= kMatterDeviceListMaxDevices) {
                ++skipped_limit;
                continue;
            }

            matter_device_list_node_t *entry = (matter_device_list_node_t *)calloc(1, sizeof(matter_device_list_node_t));
            if (!entry) {
                continue;
            }

            entry->node_id = dev->node_id;
            entry->endpoint_id = dev->endpoints[i].endpoint_id;
            entry->device_type = mapped_type;
            strlcpy(entry->name, dev->endpoints[i].device_name, sizeof(entry->name));
            *tail = entry;
            tail = &entry->next;
            ++matter_device_list.device_num;
        }
    }

    matter_device_list_unlock();

    if (skipped_unsupported > 0) {
        ESP_LOGI(TAG, "Skipped %zu unsupported endpoints", skipped_unsupported);
    }
    if (skipped_limit > 0) {
        ESP_LOGW(TAG,
                 "Skipped %zu endpoints due to controller limit %zu (active_devices=%zu exchange_contexts=%zu)",
                 skipped_limit, kMatterDeviceListMaxDevices, kMatterControllerMaxActiveDevices,
                 kMatterMaxExchangeContexts);
    }

    app_rmaker_free_matter_device_list(dev_list);
    if (dev_list) {
        s_empty_list_update_requested = false;
    }

    ESP_LOGI(TAG, "Rebuild complete: device_num=%zu online_num=%zu", matter_device_list.device_num,
             matter_device_list.online_num);
}

bool matter_device_list_is_fetchable(void)
{
    return matter_ctrl_is_provisioned() && app_controller_is_ready() &&
           app_rmaker_matter_controller_can_update_device_list();
}

void matter_device_list_fetch(void)
{
    ESP_RETURN_VOID_ON_FALSE(matter_device_list_is_fetchable(), TAG,
                             "Device-list refresh requested before controller setup is ready");
    ESP_ERROR_CHECK_WITHOUT_ABORT(app_rmaker_update_matter_device_list());
}
