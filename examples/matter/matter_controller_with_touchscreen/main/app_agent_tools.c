/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <app_matter_device_types.h>
#include <app_matter_view_model.h>
#include <devices/onoff.h>
#include <esp_check.h>

#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "app_agent_tools.h"

static esp_err_t tool_error(cJSON **result, esp_err_t error, const char *message)
{
    *result = cJSON_CreateObject();
    if (*result == NULL || cJSON_AddStringToObject(*result, "error", message) == NULL) {
        cJSON_Delete(*result);
        *result = NULL;
        return ESP_ERR_NO_MEM;
    }
    return error;
}

static bool parse_node_id(const cJSON *value, uint64_t *node_id)
{
    const char *text = cJSON_GetStringValue(value);
    if (text == NULL || text[0] == '\0' || text[0] == '-' || text[0] == ' ' || text[0] == '\t') {
        return false;
    }
    errno = 0;
    char *end = NULL;
    unsigned long long parsed = strtoull(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0') {
        return false;
    }
    *node_id = parsed;
    return true;
}

static esp_err_t copy_devices(matter_device_vm_item_t **devices, size_t *count)
{
    matter_device_vm_status_t status = {};
    if (!matter_vm_get_status(&status)) {
        return ESP_FAIL;
    }
    *count = 0;
    *devices = NULL;
    if (status.device_count == 0) {
        return ESP_OK;
    }
    *devices = calloc(status.device_count, sizeof(**devices));
    if (*devices == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (!matter_vm_copy_devices(*devices, status.device_count, count)) {
        free(*devices);
        *devices = NULL;
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t parse_node_ids(const cJSON *arguments, uint64_t **node_ids, size_t *node_count,
                                cJSON **result)
{
    if (!cJSON_IsObject(arguments)) {
        return tool_error(result, ESP_ERR_INVALID_ARG, "Tool input must be an object");
    }
    cJSON *values = cJSON_GetObjectItemCaseSensitive(arguments, "node_ids");
    size_t count = cJSON_IsArray(values) ? (size_t)cJSON_GetArraySize(values) : 0;
    if (!cJSON_IsArray(values) || count == 0 || count > 64) {
        return tool_error(result, ESP_ERR_INVALID_ARG,
                          "node_ids must be a non-empty array of strings");
    }
    uint64_t *ids = calloc(count, sizeof(*ids));
    if (ids == NULL) {
        return ESP_ERR_NO_MEM;
    }
    for (size_t i = 0; i < count; ++i) {
        cJSON *item = cJSON_GetArrayItem(values, i);
        if (!parse_node_id(item, &ids[i])) {
            free(ids);
            return tool_error(result, ESP_ERR_INVALID_ARG,
                              "node_ids contains an invalid unsigned integer string");
        }
        for (size_t j = 0; j < i; ++j) {
            if (ids[j] == ids[i]) {
                free(ids);
                return tool_error(result, ESP_ERR_INVALID_ARG, "node_ids contains a duplicate");
            }
        }
    }
    *node_ids = ids;
    *node_count = count;
    return ESP_OK;
}

typedef enum {
    DEVICE_LIST_FILTER_NONE = 0,
    DEVICE_LIST_FILTER_ON = 1 << 0,
    DEVICE_LIST_FILTER_OFF = 1 << 1,
    DEVICE_LIST_FILTER_ONLINE = 1 << 2,
    DEVICE_LIST_FILTER_OFFLINE = 1 << 3,
} device_list_filter_t;

static esp_err_t parse_device_filter(const cJSON *arguments, device_list_filter_t *filter, cJSON **result)
{
    *filter = DEVICE_LIST_FILTER_NONE;
    if (!cJSON_IsObject(arguments)) {
        return tool_error(result, ESP_ERR_INVALID_ARG, "Tool input must be an object");
    }
    cJSON *values = cJSON_GetObjectItemCaseSensitive(arguments, "filter");
    if (values == NULL) {
        return ESP_OK;
    }
    if (!cJSON_IsArray(values) || cJSON_GetArraySize(values) == 0) {
        return tool_error(result, ESP_ERR_INVALID_ARG, "filter must be a non-empty array");
    }

    bool none = false;
    cJSON *value = NULL;
    cJSON_ArrayForEach(value, values) {
        const char *name = cJSON_GetStringValue(value);
        device_list_filter_t bit = DEVICE_LIST_FILTER_NONE;
        if (name != NULL && strcmp(name, "none") == 0) {
            if (none) {
                return tool_error(result, ESP_ERR_INVALID_ARG, "Duplicate device filter");
            }
            none = true;
        } else if (name != NULL && strcmp(name, "on") == 0) {
            bit = DEVICE_LIST_FILTER_ON;
        } else if (name != NULL && strcmp(name, "off") == 0) {
            bit = DEVICE_LIST_FILTER_OFF;
        } else if (name != NULL && strcmp(name, "online") == 0) {
            bit = DEVICE_LIST_FILTER_ONLINE;
        } else if (name != NULL && strcmp(name, "offline") == 0) {
            bit = DEVICE_LIST_FILTER_OFFLINE;
        } else {
            return tool_error(result, ESP_ERR_INVALID_ARG, "Unknown device filter");
        }
        if ((*filter & bit) != 0) {
            return tool_error(result, ESP_ERR_INVALID_ARG, "Duplicate device filter");
        }
        *filter = (device_list_filter_t)(*filter | bit);
    }
    if ((none && *filter != DEVICE_LIST_FILTER_NONE) ||
            ((*filter & DEVICE_LIST_FILTER_ON) && (*filter & DEVICE_LIST_FILTER_OFF)) ||
            ((*filter & DEVICE_LIST_FILTER_ONLINE) && (*filter & DEVICE_LIST_FILTER_OFFLINE))) {
        return tool_error(result, ESP_ERR_INVALID_ARG, "Device filters are contradictory");
    }
    return ESP_OK;
}

static cJSON *device_json(const matter_device_vm_item_t *device)
{
    cJSON *item = cJSON_CreateObject();
    char node_id[19];
    snprintf(node_id, sizeof(node_id), "0x%016" PRIx64, device->node_id);
    if (item == NULL ||
            cJSON_AddStringToObject(item, "name", device->name) == NULL ||
            cJSON_AddStringToObject(item, "node_id", node_id) == NULL ||
            cJSON_AddNumberToObject(item, "endpoint_id", device->endpoint_id) == NULL ||
            cJSON_AddBoolToObject(item, "online", device->is_online) == NULL ||
            cJSON_AddBoolToObject(item, "on", device->state.onoff.onoff) == NULL) {
        cJSON_Delete(item);
        return NULL;
    }
    return item;
}

static esp_err_t get_device_list(const cJSON *arguments, cJSON **result, void *user_data)
{
    (void)user_data;
    device_list_filter_t filter = DEVICE_LIST_FILTER_NONE;
    esp_err_t err = parse_device_filter(arguments, &filter, result);
    if (err != ESP_OK) {
        return err;
    }
    matter_device_vm_item_t *devices = NULL;
    size_t count = 0;
    err = copy_devices(&devices, &count);
    if (err != ESP_OK) {
        return tool_error(result, err, "Matter device list is unavailable");
    }

    *result = cJSON_CreateObject();
    cJSON *array = *result == NULL ? NULL : cJSON_AddArrayToObject(*result, "devices");
    if (array == NULL) {
        free(devices);
        cJSON_Delete(*result);
        *result = NULL;
        return ESP_ERR_NO_MEM;
    }
    for (size_t i = 0; i < count; ++i) {
        if (!matter_device_type_is_onoff(devices[i].device_type)) {
            continue;
        }
        if (((filter & DEVICE_LIST_FILTER_ON) && !devices[i].state.onoff.onoff) ||
                ((filter & DEVICE_LIST_FILTER_OFF) && devices[i].state.onoff.onoff) ||
                ((filter & DEVICE_LIST_FILTER_ONLINE) && !devices[i].is_online) ||
                ((filter & DEVICE_LIST_FILTER_OFFLINE) && devices[i].is_online)) {
            continue;
        }
        cJSON *item = device_json(&devices[i]);
        if (item == NULL || !cJSON_AddItemToArray(array, item)) {
            cJSON_Delete(item);
            free(devices);
            cJSON_Delete(*result);
            *result = NULL;
            return ESP_ERR_NO_MEM;
        }
    }
    free(devices);
    return ESP_OK;
}

static esp_err_t get_onoff(const cJSON *arguments, cJSON **result, void *user_data)
{
    (void)user_data;
    uint64_t *node_ids = NULL;
    size_t node_count = 0;
    esp_err_t err = parse_node_ids(arguments, &node_ids, &node_count, result);
    if (err != ESP_OK) {
        return err;
    }

    matter_device_vm_item_t *devices = NULL;
    size_t count = 0;
    err = copy_devices(&devices, &count);
    if (err != ESP_OK) {
        free(node_ids);
        return tool_error(result, err, "Matter device list is unavailable");
    }
    *result = cJSON_CreateObject();
    cJSON *array = *result == NULL ? NULL : cJSON_AddArrayToObject(*result, "devices");
    for (size_t id_index = 0; array != NULL && id_index < node_count; ++id_index) {
        size_t matches = 0;
        for (size_t i = 0; i < count; ++i) {
            if (devices[i].node_id != node_ids[id_index] ||
                    !matter_device_type_is_onoff(devices[i].device_type)) {
                continue;
            }
            cJSON *item = device_json(&devices[i]);
            if (item == NULL || cJSON_AddStringToObject(item, "status", "found") == NULL ||
                    !cJSON_AddItemToArray(array, item)) {
                cJSON_Delete(item);
                array = NULL;
                break;
            }
            ++matches;
        }
        if (array == NULL) {
            break;
        }
        if (matches == 0) {
            char node_id[19];
            snprintf(node_id, sizeof(node_id), "0x%016" PRIx64, node_ids[id_index]);
            cJSON *item = cJSON_CreateObject();
            if (item == NULL || cJSON_AddStringToObject(item, "node_id", node_id) == NULL ||
                    cJSON_AddStringToObject(item, "status", "not_found") == NULL ||
                    !cJSON_AddItemToArray(array, item)) {
                cJSON_Delete(item);
                array = NULL;
                break;
            }
        }
    }
    free(devices);
    free(node_ids);
    if (array == NULL) {
        cJSON_Delete(*result);
        *result = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static esp_err_t control_onoff(const cJSON *arguments, cJSON **result, void *user_data)
{
    (void)user_data;
    uint64_t node_id = 0;
    if (!cJSON_IsObject(arguments) ||
            !parse_node_id(cJSON_GetObjectItemCaseSensitive(arguments, "node_id"), &node_id)) {
        return tool_error(result, ESP_ERR_INVALID_ARG,
                          "node_id must be an unsigned integer string");
    }
    const char *action = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(arguments, "action"));
    if (action == NULL || (strcmp(action, "on") != 0 && strcmp(action, "off") != 0 &&
                           strcmp(action, "toggle") != 0)) {
        return tool_error(result, ESP_ERR_INVALID_ARG, "action must be on, off, or toggle");
    }

    matter_device_vm_item_t *devices = NULL;
    size_t count = 0;
    esp_err_t err = copy_devices(&devices, &count);
    if (err != ESP_OK) {
        return tool_error(result, err, "Matter device list is unavailable");
    }
    *result = cJSON_CreateObject();
    cJSON *array = *result == NULL ? NULL : cJSON_AddArrayToObject(*result, "devices");
    size_t matches = 0;
    for (size_t i = 0; array != NULL && i < count; ++i) {
        if (devices[i].node_id != node_id ||
                !matter_device_type_is_onoff(devices[i].device_type)) {
            continue;
        }
        ++matches;
        bool desired = strcmp(action, "toggle") == 0 ? !devices[i].state.onoff.onoff :
                       strcmp(action, "on") == 0;
        bool invoke = devices[i].is_online;
        esp_err_t control_err = ESP_OK;
        if (invoke) {
            control_err = matter_onoff_set(devices[i].node_id, devices[i].endpoint_id, desired);
        }
        const char *status = !devices[i].is_online ? "offline" :
                             control_err != ESP_OK ? "error" : "accepted";
        cJSON *item = device_json(&devices[i]);
        if (item == NULL ||
                cJSON_AddStringToObject(item, "requested_state", desired ? "on" : "off") == NULL ||
                cJSON_AddBoolToObject(item, "command_invoked", invoke && control_err == ESP_OK) == NULL ||
                cJSON_AddStringToObject(item, "status", status) == NULL ||
                !cJSON_AddItemToArray(array, item)) {
            cJSON_Delete(item);
            array = NULL;
            break;
        }
    }
    if (array != NULL && matches == 0) {
        char node_id_text[19];
        snprintf(node_id_text, sizeof(node_id_text), "0x%016" PRIx64, node_id);
        cJSON *item = cJSON_CreateObject();
        if (item == NULL || cJSON_AddStringToObject(item, "node_id", node_id_text) == NULL ||
                cJSON_AddStringToObject(item, "status", "not_found") == NULL ||
                !cJSON_AddItemToArray(array, item)) {
            cJSON_Delete(item);
            array = NULL;
        }
    }
    free(devices);
    if (array == NULL) {
        cJSON_Delete(*result);
        *result = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static const agent_ng_tool_t s_tools[] = {
    {.name = "get_device_list", .callback = get_device_list},
    {.name = "get_onoff", .callback = get_onoff},
    {.name = "control_onoff", .callback = control_onoff},
};

const agent_ng_tool_t *app_agent_tools_get(size_t *count)
{
    if (count != NULL) {
        *count = sizeof(s_tools) / sizeof(s_tools[0]);
    }
    return s_tools;
}
