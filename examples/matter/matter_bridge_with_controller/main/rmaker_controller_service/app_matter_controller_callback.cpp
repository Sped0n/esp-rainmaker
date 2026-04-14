/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <esp_check.h>
#include <esp_err.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_rmaker_utils.h>

#include <cstdio>

#include <app/server/Server.h>
#include <credentials/FabricTable.h>

#include <controller_rest_apis.h>

#include <app_matter_controller_callback.h>
#include <app_matter_controller.h>
#include <app_matter_device_manager.h>
#include <app_matter_noc_manager.h>
#include <matter_controller_std.h>


#define TAG "MatterController"

static constexpr size_t kMaxLocalCommissionedFabrics = 6;

static esp_err_t get_local_commissioned_fabric_ids(uint64_t *fabric_ids, size_t fabric_ids_len, size_t *fabric_count)
{
    ESP_RETURN_ON_FALSE(fabric_ids && fabric_ids_len > 0 && fabric_count, ESP_ERR_INVALID_ARG, TAG,
                        "Invalid fabric id output buffer");

    auto &fabric_table = chip::Server::GetInstance().GetFabricTable();
    size_t count = 0;
    for (auto it = fabric_table.begin(); it != fabric_table.end(); ++it) {
        if (!it->IsInitialized()) {
            continue;
        }
        ESP_RETURN_ON_FALSE(count < fabric_ids_len, ESP_ERR_NO_MEM, TAG, "Local fabric buffer is too small");
        fabric_ids[count++] = static_cast<uint64_t>(it->GetFabricId());
        ESP_LOGI(TAG, "Found local commissioned fabric[%u]=0x%016llX", static_cast<unsigned>(count - 1),
                 static_cast<unsigned long long>(fabric_ids[count - 1]));
    }

    *fabric_count = count;
    if (count == 0) {
        ESP_LOGW(TAG, "No local commissioned fabrics found");
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_OK;
}

static esp_err_t app_matter_controller_discover_rmaker_group(matter_controller_handle_t *handle)
{
    ESP_RETURN_ON_FALSE(handle && handle->base_url && handle->access_token, ESP_ERR_INVALID_ARG, TAG,
                        "Controller service is not ready for group discovery");
    ESP_RETURN_ON_FALSE(handle->matter_fabric_id == 0, ESP_ERR_INVALID_STATE, TAG,
                        "Matter fabric id already set before discovery");

    uint64_t local_fabric_ids[kMaxLocalCommissionedFabrics] = { 0 };
    size_t local_fabric_count = 0;
    ESP_RETURN_ON_ERROR(get_local_commissioned_fabric_ids(local_fabric_ids, kMaxLocalCommissionedFabrics,
                                                          &local_fabric_count),
                        TAG, "Failed to get local commissioned fabric ids");
    ESP_LOGI(TAG, "Trying RainMaker group discovery across %u local commissioned fabrics",
             static_cast<unsigned>(local_fabric_count));

    char discovered_group_id[64] = { 0 };
    uint64_t matched_fabric_id = 0;
    esp_err_t err = ESP_ERR_NOT_FOUND;
    esp_err_t last_lookup_err = ESP_ERR_NOT_FOUND;
    for (size_t i = local_fabric_count; i > 0; --i) {
        const uint64_t fabric_id = local_fabric_ids[i - 1];
        ESP_LOGI(TAG, "Trying RainMaker group discovery for local fabric 0x%016llX",
                 static_cast<unsigned long long>(fabric_id));
        err = fetch_rainmaker_group_id(handle->base_url, handle->access_token, fabric_id, discovered_group_id,
                                       sizeof(discovered_group_id));
        if (err == ESP_OK) {
            matched_fabric_id = fabric_id;
            break;
        }
        last_lookup_err = err;
        ESP_LOGW(TAG, "RainMaker group lookup for local fabric 0x%016llX failed: %s",
                 static_cast<unsigned long long>(fabric_id), esp_err_to_name(err));
    }
    if (err != ESP_OK) {
        return last_lookup_err;
    }

    if (handle->rmaker_group_id) {
        free(handle->rmaker_group_id);
        handle->rmaker_group_id = NULL;
    }

    const size_t group_id_len = strlen(discovered_group_id);
    handle->rmaker_group_id = (char *) MEM_CALLOC_EXTRAM(group_id_len + 1, 1);
    ESP_RETURN_ON_FALSE(handle->rmaker_group_id, ESP_ERR_NO_MEM, TAG, "Failed to allocate discovered group id");

    memcpy(handle->rmaker_group_id, discovered_group_id, group_id_len);
    handle->matter_fabric_id = matched_fabric_id;

    ESP_LOGI(TAG, "Discovered RainMaker group %s for local fabric 0x%016llX", handle->rmaker_group_id,
             static_cast<unsigned long long>(handle->matter_fabric_id));

    esp_rmaker_param_t *param =
        esp_rmaker_device_get_param_by_type(handle->service, ESP_RMAKER_PARAM_RMAKER_GROUP_ID);
    ESP_RETURN_ON_FALSE(param, ESP_ERR_NOT_FOUND, TAG, "Cannot find the rmaker group id param");
    return esp_rmaker_param_update_and_report(param, esp_rmaker_str(handle->rmaker_group_id));
}

static esp_err_t app_matter_controller_authorize(matter_controller_handle_t *handle)
{
    esp_err_t err = ESP_OK;
    if (!handle->base_url || !handle->user_token || handle->access_token) {
        return ESP_ERR_INVALID_ARG;
    }
    handle->access_token = (char *)MEM_CALLOC_EXTRAM(1800, 1);
    if (!handle->access_token) {
        return ESP_ERR_NO_MEM;
    }
    if ((err = fetch_access_token(handle->base_url, handle->user_token, handle->access_token, 1800)) != ESP_OK) {
        free(handle->access_token);
        ESP_LOGE(TAG, "Failed on fetch access token");
    }
    return err;
}

static esp_err_t app_matter_controller_fetch_matter_fabric_id(matter_controller_handle_t *handle)
{
    if (!handle->base_url || !handle->access_token || !handle->rmaker_group_id || handle->matter_fabric_id != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    return fetch_matter_fabric_id(handle->base_url, handle->access_token, handle->rmaker_group_id,
                                  &handle->matter_fabric_id);
}

static esp_err_t app_matter_controller_setup_controller(matter_controller_handle_t *handle)
{
    esp_err_t err = ESP_OK;
    if (handle->matter_noc_installed) {
        err = app_matter_noc_manager_sync_commissioned_node(handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to sync commissioned node: %s", esp_err_to_name(err));
            return ESP_ERR_INVALID_STATE;
        }
    }

    err = app_matter_noc_manager_install(handle, !handle->matter_noc_installed);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install controller NOC: %s", esp_err_to_name(err));
    }
    return err;
}

static esp_err_t app_matter_controller_update_noc(matter_controller_handle_t *handle)
{
    esp_err_t err = app_matter_noc_manager_install(handle, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to force update controller NOC: %s", esp_err_to_name(err));
    }
    return err;
}

static esp_err_t app_matter_controller_update_device_list(matter_controller_handle_t *handle)
{
    return update_device_list(handle);
}

esp_err_t app_matter_controller_callback(matter_controller_handle_t *handle, matter_controller_callback_type_t type)
{
    switch (type) {
    case MATTER_CONTROLLER_CALLBACK_TYPE_AUTHORIZE: {
        return app_matter_controller_authorize(handle);
        break;
    }
    case MATTER_CONTROLLER_CALLBACK_TYPE_DISCOVER_RMAKER_GROUP: {
        return app_matter_controller_discover_rmaker_group(handle);
        break;
    }
    case MATTER_CONTROLLER_CALLBACK_TYPE_QUERY_MATTER_FABRIC_ID: {
        return app_matter_controller_fetch_matter_fabric_id(handle);
        break;
    }
    case MATTER_CONTROLLER_CALLBACK_TYPE_SETUP_CONTROLLER: {
        return app_matter_controller_setup_controller(handle);
        break;
    }
    case MATTER_CONTROLLER_CALLBACK_TYPE_UPDATE_CONTROLLER_NOC: {
        return app_matter_controller_update_noc(handle);
        break;
    }
    case MATTER_CONTROLLER_CALLBACK_TYPE_UPDATE_DEVICE: {
        return app_matter_controller_update_device_list(handle);
        break;
    }
    default:
        break;
    }
    return ESP_OK;
}
