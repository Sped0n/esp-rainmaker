/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <esp_rmaker_cmd_resp.h>
#include <esp_rmaker_utils.h>
#include <esp_matter_core.h>
#include <esp_matter_controller_utils.h>
#include <esp_matter_controller_cluster_command.h>
#include <esp_matter_controller_client.h>
#include <esp_matter_controller_write_command.h>
#include <esp_matter_controller_read_command.h>
#include <esp_check.h>
#include <esp_log.h>
#include <freertos/semphr.h>
#include <app_rmaker_matter_controller_internal.h>
#include <app_rmaker_matter_json_helpers.h>

#include <cJSON.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <utility>

#include <app/ConcreteAttributePath.h>
#include <app/MessageDef/StatusIB.h>
#include <app/WriteClient.h>
#include <app/server/Server.h>
#include <lib/core/DataModelTypes.h>
#include <lib/core/Optional.h>
#include <lib/support/ScopedBuffer.h>
#include <lib/support/CHIPMem.h>
#include <lib/core/NodeId.h>
#include <lib/core/TLVReader.h>

using namespace esp_matter;
using namespace chip::app;

#define TAG "rmaker_matter_cmd"

#define MATTER_CONTROL_CMD_TYPE_INVOKE_CMD 0x1100
#define MATTER_CONTROL_CMD_TYPE_WRITE_ATTR 0x1101
#define MATTER_CONTROL_CMD_TYPE_READ       0x1102

static_assert(CONFIG_RMAKER_MTCTL_CMDRESP_MTCMD_TIMEOUT_MS > 0,
              "CONFIG_RMAKER_MTCTL_CMDRESP_MTCMD_TIMEOUT_MS must be greater than 0");
static_assert(CONFIG_RMAKER_MTCTL_CMDRESP_CMDFIELD_BUFFER_SIZE > 0,
              "CONFIG_RMAKER_MTCTL_CMDRESP_CMDFIELD_BUFFER_SIZE must be greater than 0");
static_assert(CONFIG_RMAKER_MTCTL_CMDRESP_ATTRVAL_BUFFER_SIZE > 0,
              "CONFIG_RMAKER_MTCTL_CMDRESP_ATTRVAL_BUFFER_SIZE must be greater than 0");
static_assert(CONFIG_RMAKER_MTCTL_CMDRESP_RESPONSE_BUFFER_SIZE > 0,
              "CONFIG_RMAKER_MTCTL_CMDRESP_RESPONSE_BUFFER_SIZE must be greater than 0");

namespace {
char *s_cmd_resp_buffer = nullptr;
cJSON *s_resp_root = nullptr;
cJSON *s_resp_array = nullptr;
SemaphoreHandle_t s_cmd_resp_mutex = nullptr;
const int INVOKE_CMD_HANDLED_EVENT = BIT0;
const int WRITE_ATTR_HANDLED_EVENT = BIT1;
const int READ_HANDLED_EVENT = BIT2;
EventGroupHandle_t s_matter_controller_event_group;
bool s_cmd_resp_enabled = false;
bool s_invoke_cmd_registered = false;
bool s_write_attr_cmd_registered = false;
bool s_read_cmd_registered = false;

class CmdRespLock {
public:
    CmdRespLock() : m_locked(s_cmd_resp_mutex && xSemaphoreTake(s_cmd_resp_mutex, portMAX_DELAY) == pdTRUE) {}
    ~CmdRespLock()
    {
        if (m_locked) {
            xSemaphoreGive(s_cmd_resp_mutex);
        }
    }
    bool locked() const
    {
        return m_locked;
    }

private:
    bool m_locked;
};

static void reset_response_json(void)
{
    if (s_resp_root) {
        cJSON_Delete(s_resp_root);
    }
    s_resp_root = nullptr;
    s_resp_array = nullptr;
}

static esp_err_t start_response_array(const char *array_key)
{
    reset_response_json();
    s_resp_root = cJSON_CreateObject();
    s_resp_array = cJSON_CreateArray();
    if (!s_resp_root || !s_resp_array) {
        reset_response_json();
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddItemToObject(s_resp_root, array_key, s_resp_array);
    return ESP_OK;
}

static esp_err_t serialize_response(size_t *out_len)
{
    if (!s_resp_root || !out_len) {
        return ESP_ERR_INVALID_STATE;
    }

    s_cmd_resp_buffer[0] = '\0';
    if (!cJSON_PrintPreallocated(s_resp_root, s_cmd_resp_buffer, CONFIG_RMAKER_MTCTL_CMDRESP_RESPONSE_BUFFER_SIZE,
                                 false)) {
        reset_response_json();
        return ESP_FAIL;
    }

    size_t json_len = strlen(s_cmd_resp_buffer);
    *out_len = json_len;
    reset_response_json();
    return ESP_OK;
}

static bool json_get_string(cJSON *obj, const char *key, char *buf, size_t buf_size)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsString(item) || !item->valuestring || buf_size == 0) {
        return false;
    }
    strlcpy(buf, item->valuestring, buf_size);
    return true;
}

static bool json_get_object_text(cJSON *obj, const char *key, char *buf, size_t buf_size)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsObject(item) || buf_size == 0) {
        return false;
    }

    buf[0] = '\0';
    if (!cJSON_PrintPreallocated(item, buf, buf_size, false)) {
        return false;
    }
    return true;
}

static void add_tlv_value_to_cjson(chip::TLV::TLVReader *data, const char *key, cJSON *obj)
{
    if (!data || !key || !obj) {
        return;
    }
    cJSON *json = NULL;
    if (app_rmaker_matter_tlv_to_json(*data, &json) == ESP_OK && json) {
        cJSON_AddItemToObject(obj, key, json);
        return;
    }
    cJSON_AddStringToObject(obj, key, "tlv_decode_failed");
}

class RmakerWriteCommand : public controller::write_command {
public:
    RmakerWriteCommand(uint64_t node_id, uint16_t endpoint_id, uint32_t cluster_id, uint32_t attribute_id,
                       const char *attr_val, cJSON *resp_obj)
        : controller::write_command(node_id, endpoint_id, cluster_id, attribute_id, attr_val, chip::NullOptional)
        , m_resp_obj(resp_obj)
    {
    }

    void Deactivate()
    {
        m_active = false;
        m_resp_obj = nullptr;
    }

    void OnResponse(const WriteClient *client, const ConcreteDataAttributePath &path, StatusIB status) override
    {
        (void)client;
        (void)path;
        CHIP_ERROR error = status.ToChipError();
        cJSON *resp_obj = m_active ? m_resp_obj : nullptr;
        if (!resp_obj || cJSON_GetObjectItemCaseSensitive(resp_obj, "status")) {
        return;
    }
    if (error == CHIP_NO_ERROR) {
        cJSON_AddStringToObject(resp_obj, "status", "success");
        } else {
            cJSON_AddStringToObject(resp_obj, "status", "failure");
            cJSON_AddStringToObject(resp_obj, "reason", chip::ErrorStr(error));
        }
    }

    void OnError(const WriteClient *client, CHIP_ERROR error) override
    {
        (void)client;
        cJSON *resp_obj = m_active ? m_resp_obj : nullptr;
        if (resp_obj && !cJSON_GetObjectItemCaseSensitive(resp_obj, "status")) {
        cJSON_AddStringToObject(resp_obj, "status", "failure");
            cJSON_AddStringToObject(resp_obj, "reason", chip::ErrorStr(error));
        }
        xEventGroupSetBits(s_matter_controller_event_group, WRITE_ATTR_HANDLED_EVENT);
    }

    void OnDone(WriteClient *client) override
    {
        (void)client;
        cJSON *resp_obj = m_active ? m_resp_obj : nullptr;
        if (resp_obj && !cJSON_GetObjectItemCaseSensitive(resp_obj, "status")) {
        cJSON_AddStringToObject(resp_obj, "status", "success");
        }
        xEventGroupSetBits(s_matter_controller_event_group, WRITE_ATTR_HANDLED_EVENT);
        chip::Platform::Delete(this);
    }

private:
    cJSON *m_resp_obj;
    bool m_active = true;
};

class RmakerInvokeCommand {
public:
    RmakerInvokeCommand(uint64_t destination_id, uint16_t endpoint_id, uint32_t cluster_id, uint32_t command_id,
                        const char *command_data_field,
                        const chip::Optional<uint16_t> timed_interaction_timeout_ms, cJSON *resp_obj)
        : m_destination_id(destination_id)
        , m_endpoint_id(endpoint_id)
        , m_cluster_id(cluster_id)
        , m_command_id(command_id)
        , m_command_data_field(command_data_field,
                               esp_matter::client::interaction::custom_encodable_type::interaction_type::k_invoke_cmd)
        , m_timed_interaction_timeout_ms(timed_interaction_timeout_ms)
        , m_resp_obj(resp_obj)
        , m_on_device_connected_cb(on_device_connected, this)
        , m_on_device_connection_failure_cb(on_device_connection_failure, this)
    {
    }

    esp_err_t Send()
    {
        if (chip::IsGroupId(m_destination_id)) {
            return ESP_ERR_NOT_SUPPORTED;
        }
#ifdef CONFIG_ESP_MATTER_ENABLE_MATTER_SERVER
        chip::Server &server = chip::Server::GetInstance();
        server.GetCASESessionManager()->FindOrEstablishSession(chip::ScopedNodeId(m_destination_id, get_fabric_index()),
                                                               &m_on_device_connected_cb,
                                                               &m_on_device_connection_failure_cb);
        return ESP_OK;
#else
        auto &controller_instance = esp_matter::controller::matter_controller_client::get_instance();
#ifdef CONFIG_ESP_MATTER_COMMISSIONER_ENABLE
        CHIP_ERROR err = controller_instance.get_commissioner()->GetConnectedDevice(m_destination_id,
                                                                                    &m_on_device_connected_cb,
                                                                                    &m_on_device_connection_failure_cb);
#else
        CHIP_ERROR err = controller_instance.get_controller()->GetConnectedDevice(m_destination_id,
                                                                                  &m_on_device_connected_cb,
                                                                                  &m_on_device_connection_failure_cb);
#endif
        if (err == CHIP_NO_ERROR) {
            return ESP_OK;
        }
        chip::Platform::Delete(this);
        return ESP_FAIL;
#endif
    }

    void Deactivate()
    {
        m_active = false;
        m_resp_obj = nullptr;
    }

private:
    static void on_device_connected(void *context, chip::Messaging::ExchangeManager &exchange_mgr,
                                    const chip::SessionHandle &session_handle)
    {
        RmakerInvokeCommand *cmd = reinterpret_cast<RmakerInvokeCommand *>(context);
        chip::OperationalDeviceProxy device_proxy(&exchange_mgr, session_handle);
        chip::app::CommandPathParams command_path = {cmd->m_endpoint_id, 0, cmd->m_cluster_id, cmd->m_command_id,
                                                     chip::app::CommandPathFlags::kEndpointIdValid
                                                    };
        esp_err_t err = esp_matter::client::interaction::invoke::send_request(
                            cmd, &device_proxy, command_path, cmd->m_command_data_field, on_success, on_error,
                            cmd->m_timed_interaction_timeout_ms);
        if (err != ESP_OK) {
            cmd->set_failure("send_command failed");
            xEventGroupSetBits(s_matter_controller_event_group, INVOKE_CMD_HANDLED_EVENT);
            chip::Platform::Delete(cmd);
        }
    }

    static void on_device_connection_failure(void *context, const chip::ScopedNodeId &peer_id, CHIP_ERROR error)
    {
        (void)peer_id;
        RmakerInvokeCommand *cmd = reinterpret_cast<RmakerInvokeCommand *>(context);
        cmd->set_failure(chip::ErrorStr(error));
        xEventGroupSetBits(s_matter_controller_event_group, INVOKE_CMD_HANDLED_EVENT);
        chip::Platform::Delete(cmd);
    }

    static void on_success(void *ctx, const ConcreteCommandPath &command_path, const StatusIB &status,
                           TLVReader *response_data)
    {
        (void)command_path;
        (void)status;
        RmakerInvokeCommand *cmd = reinterpret_cast<RmakerInvokeCommand *>(ctx);
        if (cmd->m_active && cmd->m_resp_obj) {
            cJSON_AddStringToObject(cmd->m_resp_obj, "status", "success");
            if (response_data) {
                add_tlv_value_to_cjson(response_data, "response_data", cmd->m_resp_obj);
            }
        }
        xEventGroupSetBits(s_matter_controller_event_group, INVOKE_CMD_HANDLED_EVENT);
        chip::Platform::Delete(cmd);
    }

    static void on_error(void *ctx, CHIP_ERROR error)
    {
        RmakerInvokeCommand *cmd = reinterpret_cast<RmakerInvokeCommand *>(ctx);
        cmd->set_failure(error.AsString());
        xEventGroupSetBits(s_matter_controller_event_group, INVOKE_CMD_HANDLED_EVENT);
        chip::Platform::Delete(cmd);
    }

    void set_failure(const char *reason)
    {
        if (m_active && m_resp_obj && !cJSON_GetObjectItemCaseSensitive(m_resp_obj, "status")) {
            cJSON_AddStringToObject(m_resp_obj, "status", "failure");
            cJSON_AddStringToObject(m_resp_obj, "reason", reason ? reason : "failure");
        }
    }

    uint64_t m_destination_id;
    uint16_t m_endpoint_id;
    uint32_t m_cluster_id;
    uint32_t m_command_id;
    esp_matter::client::interaction::custom_encodable_type m_command_data_field;
    chip::Optional<uint16_t> m_timed_interaction_timeout_ms;
    cJSON *m_resp_obj;
    bool m_active = true;
    chip::Callback::Callback<chip::OnDeviceConnected> m_on_device_connected_cb;
    chip::Callback::Callback<chip::OnDeviceConnectionFailure> m_on_device_connection_failure_cb;
};

class RmakerReadCommand : public controller::read_command {
public:
    RmakerReadCommand(uint64_t node_id,
                      chip::Platform::ScopedMemoryBufferWithSize<AttributePathParams> &&attr_paths,
                      chip::Platform::ScopedMemoryBufferWithSize<EventPathParams> &&event_paths,
                      cJSON *resp_root, cJSON *resp_array)
        : controller::read_command(node_id, std::move(attr_paths), std::move(event_paths), nullptr, nullptr, nullptr)
        , m_resp_root(resp_root)
        , m_resp_array(resp_array)
    {
    }

    void Deactivate()
    {
        m_active = false;
        m_resp_root = nullptr;
        m_resp_array = nullptr;
    }

    void OnAttributeData(const chip::app::ConcreteDataAttributePath &path, chip::TLV::TLVReader *data,
                         const chip::app::StatusIB &status) override
    {
        if (!m_active || !m_resp_array) {
        return;
    }
    char endpoint_id_str[8] = {0};
    char cluster_id_str[12] = {0};
    char attribute_id_str[12] = {0};
    snprintf(endpoint_id_str, sizeof(endpoint_id_str), "%u", path.mEndpointId);
        snprintf(cluster_id_str, sizeof(cluster_id_str), "0x%08lx", (unsigned long)path.mClusterId);
        snprintf(attribute_id_str, sizeof(attribute_id_str), "0x%08lx", (unsigned long)path.mAttributeId);

        cJSON *result = cJSON_CreateObject();
        if (!result) {
        return;
    }
    cJSON_AddItemToArray(m_resp_array, result);
    cJSON_AddStringToObject(result, "endpoint_id", endpoint_id_str);
    cJSON_AddStringToObject(result, "cluster_id", cluster_id_str);
    cJSON_AddStringToObject(result, "attribute_id", attribute_id_str);
    CHIP_ERROR error = status.ToChipError();
    if (error != CHIP_NO_ERROR) {
        cJSON_AddStringToObject(result, "status", "failure");
            cJSON_AddStringToObject(result, "reason", chip::ErrorStr(error));
            return;
        }
        if (data) {
        add_tlv_value_to_cjson(data, "attribute_value", result);
        }
    }

    void OnEventData(const chip::app::EventHeader &header, chip::TLV::TLVReader *data,
                     const chip::app::StatusIB *status) override
    {
        if (!m_active || !m_resp_array) {
        return;
    }
    char endpoint_id_str[8] = {0};
    char cluster_id_str[12] = {0};
    char event_id_str[12] = {0};
    snprintf(endpoint_id_str, sizeof(endpoint_id_str), "%u", header.mPath.mEndpointId);
        snprintf(cluster_id_str, sizeof(cluster_id_str), "0x%08lx", (unsigned long)header.mPath.mClusterId);
        snprintf(event_id_str, sizeof(event_id_str), "0x%08lx", (unsigned long)header.mPath.mEventId);

        cJSON *result = cJSON_CreateObject();
        if (!result) {
        return;
    }
    cJSON_AddItemToArray(m_resp_array, result);
    cJSON_AddStringToObject(result, "endpoint_id", endpoint_id_str);
    cJSON_AddStringToObject(result, "cluster_id", cluster_id_str);
    cJSON_AddStringToObject(result, "event_id", event_id_str);
    if (status) {
        CHIP_ERROR error = status->ToChipError();
            if (error != CHIP_NO_ERROR) {
                cJSON_AddStringToObject(result, "status", "failure");
                cJSON_AddStringToObject(result, "reason", chip::ErrorStr(error));
                return;
            }
        }
        if (data) {
        add_tlv_value_to_cjson(data, "event_data", result);
        }
    }

    void OnError(CHIP_ERROR error) override
    {
        if (m_active && m_resp_root && !cJSON_GetObjectItemCaseSensitive(m_resp_root, "status")) {
        cJSON_AddStringToObject(m_resp_root, "status", "failure");
            cJSON_AddStringToObject(m_resp_root, "reason", chip::ErrorStr(error));
        }
        xEventGroupSetBits(s_matter_controller_event_group, READ_HANDLED_EVENT);
    }

    void OnDone(ReadClient *apReadClient) override
    {
        (void)apReadClient;
        xEventGroupSetBits(s_matter_controller_event_group, READ_HANDLED_EVENT);
        chip::Platform::Delete(this);
    }

private:
    cJSON *m_resp_root;
    cJSON *m_resp_array;
    bool m_active = true;
};

esp_err_t invoke_cluster_command(uint64_t destination_id, uint16_t endpoint_id, uint32_t cluster_id,
                                 uint32_t command_id, const char *command_data_field,
                                 const chip::Optional<uint16_t> timed_interaction_timeout_ms, cJSON *resp_obj)
{
    ESP_LOGI(TAG, "Send cluster command [cluster 0x%lx, command 0x%lx] to node %llx endpoint %x", cluster_id, command_id,
             destination_id, endpoint_id);
    RmakerInvokeCommand *cluster_command = chip::Platform::New<RmakerInvokeCommand>(
                                               destination_id, endpoint_id, cluster_id, command_id, command_data_field, timed_interaction_timeout_ms, resp_obj);
    if (!cluster_command) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = ESP_OK;
    {
        lock::ScopedChipStackLock lock(3000);
        xEventGroupClearBits(s_matter_controller_event_group, INVOKE_CMD_HANDLED_EVENT);
        err = cluster_command->Send();
    }
    if (err == ESP_OK) {
        EventBits_t bits = xEventGroupWaitBits(s_matter_controller_event_group, INVOKE_CMD_HANDLED_EVENT, true, true,
                                               pdMS_TO_TICKS(CONFIG_RMAKER_MTCTL_CMDRESP_MTCMD_TIMEOUT_MS));
        if ((bits & INVOKE_CMD_HANDLED_EVENT) == 0) {
            if (resp_obj && !cJSON_GetObjectItemCaseSensitive(resp_obj, "status")) {
                cJSON_AddStringToObject(resp_obj, "status", "failure");
                cJSON_AddStringToObject(resp_obj, "reason", "invoke command timeout");
            }
            cluster_command->Deactivate();
            err = ESP_ERR_TIMEOUT;
        }
    }
    return err;
}

esp_err_t esp_rmaker_matter_controller_invoke_cmd_handler(const void *in_data, size_t in_len,
                                                          void **out_data, size_t *out_len,
                                                          esp_rmaker_cmd_ctx_t *ctx, void *priv)
{
    if (in_data == nullptr || in_len == 0) {
        ESP_LOGE(TAG, "No data received for invoking matter command");
        return ESP_FAIL;
    }

    CmdRespLock lock;
    if (!lock.locked()) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_cmd_resp_buffer == nullptr) {
        ESP_LOGE(TAG, "Command response buffer not allocated");
        return ESP_ERR_INVALID_STATE;
    }
    uint16_t timed_interaction_timeout_ms = 0;
    bool has_timed_interaction_timeout = false;
    ESP_LOGI(TAG, "Receive invoke-command command: %u bytes", (unsigned)in_len);
    ESP_LOGD(TAG, "Invoke-command payload: %.*s", (int)in_len, (char *)in_data);

    cJSON *root = cJSON_ParseWithLength((const char *)in_data, in_len);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        ESP_LOGE(TAG, "Failed to parse input JSON");
        return ESP_FAIL;
    }

    uint32_t cluster_id = chip::kInvalidClusterId, command_id = chip::kInvalidCommandId;
    char command_fields_buffer[CONFIG_RMAKER_MTCTL_CMDRESP_CMDFIELD_BUFFER_SIZE] = "{}";
    char id_buffer[19] = {0};
    cJSON *request_payload = cJSON_GetObjectItemCaseSensitive(root, "request_payload");
    if (!cJSON_IsObject(request_payload)) {
        cJSON_Delete(root);
        ESP_LOGE(TAG, "No 'request_payload' found in input");
        return ESP_FAIL;
    }
    if (json_get_string(request_payload, "cluster_id", id_buffer, sizeof(id_buffer))) {
        cluster_id = string_to_uint32(id_buffer);
    }
    if (json_get_string(request_payload, "command_id", id_buffer, sizeof(id_buffer))) {
        command_id = string_to_uint32(id_buffer);
    }
    if (cluster_id == chip::kInvalidClusterId || command_id == chip::kInvalidCommandId) {
        cJSON_Delete(root);
        return ESP_FAIL;
    }
    cJSON *timed_interaction_timeout = cJSON_GetObjectItemCaseSensitive(request_payload, "timed_interaction_timeout");
    if (cJSON_IsNumber(timed_interaction_timeout)) {
        timed_interaction_timeout_ms = (uint16_t)timed_interaction_timeout->valueint;
        has_timed_interaction_timeout = true;
    }
    cJSON *command_fields = cJSON_GetObjectItemCaseSensitive(request_payload, "command_fields");
    if (command_fields && !json_get_object_text(request_payload, "command_fields", command_fields_buffer,
                                                sizeof(command_fields_buffer))) {
        cJSON_Delete(root);
        ESP_LOGE(TAG, "Invalid command_fields");
        return ESP_FAIL;
    }

    cJSON *objects = cJSON_GetObjectItemCaseSensitive(root, "objects");
    if (!cJSON_IsArray(objects)) {
        cJSON_Delete(root);
        ESP_LOGE(TAG, "No 'objects' found in input");
        return ESP_FAIL;
    }
    esp_err_t ret = start_response_array("responses");
    if (ret != ESP_OK) {
        cJSON_Delete(root);
        return ret;
    }
    cJSON *object = nullptr;
    cJSON_ArrayForEach(object, objects) {
        if (!cJSON_IsObject(object)) {
            continue;
        }
        uint64_t node_id = chip::kUndefinedNodeId;
        uint16_t endpoint_id = chip::kInvalidEndpointId;
        cJSON *resp_obj = cJSON_CreateObject();
        if (!resp_obj) {
            cJSON_Delete(root);
            reset_response_json();
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddItemToArray(s_resp_array, resp_obj);
        if (json_get_string(object, "matter_node_id", id_buffer, sizeof(id_buffer))) {
            cJSON_AddStringToObject(resp_obj, "matter_node_id", id_buffer);
            node_id = string_to_uint64(id_buffer);
        }
        if (json_get_string(object, "matter_endpoint_id", id_buffer, sizeof(id_buffer))) {
            cJSON_AddStringToObject(resp_obj, "matter_endpoint_id", id_buffer);
            endpoint_id = string_to_uint16(id_buffer);
        }
        if (node_id != chip::kUndefinedNodeId && endpoint_id != chip::kInvalidEndpointId) {
            esp_err_t err = has_timed_interaction_timeout ?
                            invoke_cluster_command(node_id, endpoint_id, cluster_id, command_id, command_fields_buffer,
                                                   chip::MakeOptional(timed_interaction_timeout_ms), resp_obj) :
                            invoke_cluster_command(node_id, endpoint_id, cluster_id, command_id, command_fields_buffer,
                                                   chip::NullOptional, resp_obj);
            if (err != ESP_OK && !cJSON_GetObjectItemCaseSensitive(resp_obj, "status")) {
                cJSON_AddStringToObject(resp_obj, "status", "failure");
            }
        } else {
            cJSON_AddStringToObject(resp_obj, "status", "failure");
            cJSON_AddStringToObject(resp_obj, "reason", "invalid object");
        }
    }
    cJSON_Delete(root);
    ret = serialize_response(out_len);
    if (ret != ESP_OK) {
        return ret;
    }
    *out_data = s_cmd_resp_buffer;
    ESP_LOGI(TAG, "Returning invoke-command response: %u bytes", (unsigned)*out_len);
    ESP_LOGD(TAG, "Invoke-command response: %s", s_cmd_resp_buffer);
    return ESP_OK;
}

esp_err_t write_attr_command(uint64_t node_id, uint16_t endpoint_id, uint32_t cluster_id,
                             uint32_t attribute_id, const char *attr_val, cJSON *resp_obj)
{
    ESP_LOGI(TAG, "Send write_attr command [cluster 0x%lx, attribute 0x%lx] to node %llx endpoint %x", cluster_id,
             attribute_id,
             node_id, endpoint_id);
    RmakerWriteCommand *write_command = chip::Platform::New<RmakerWriteCommand>(node_id, endpoint_id, cluster_id,
                                                                                attribute_id, attr_val,
                                                                                resp_obj);
    ESP_RETURN_ON_FALSE(write_command, ESP_ERR_NO_MEM, TAG, "Failed to allocate write command");
    esp_err_t err = ESP_OK;
    {
        lock::ScopedChipStackLock lock(3000);
        xEventGroupClearBits(s_matter_controller_event_group, WRITE_ATTR_HANDLED_EVENT);
        err = write_command->send_command();
    }
    if (err == ESP_OK) {
        EventBits_t bits = xEventGroupWaitBits(s_matter_controller_event_group, WRITE_ATTR_HANDLED_EVENT, true, true,
                                               pdMS_TO_TICKS(CONFIG_RMAKER_MTCTL_CMDRESP_MTCMD_TIMEOUT_MS));
        if ((bits & WRITE_ATTR_HANDLED_EVENT) == 0) {
            if (resp_obj && !cJSON_GetObjectItemCaseSensitive(resp_obj, "status")) {
                cJSON_AddStringToObject(resp_obj, "status", "failure");
                cJSON_AddStringToObject(resp_obj, "reason", "write attribute timeout");
            }
            write_command->Deactivate();
            err = ESP_ERR_TIMEOUT;
        }
    }
    return err;
}

esp_err_t read_attr_or_event_command(uint64_t node_id,
                                     chip::Platform::ScopedMemoryBufferWithSize<AttributePathParams> &&attr_paths,
                                     chip::Platform::ScopedMemoryBufferWithSize<EventPathParams> &&event_paths)
{
    ESP_LOGI(TAG, "Send read attribute/event command to node %llx", node_id);
    RmakerReadCommand *cmd = chip::Platform::New<RmakerReadCommand>(node_id, std::move(attr_paths), std::move(event_paths),
                                                                    s_resp_root, s_resp_array);
    if (!cmd) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = ESP_OK;
    {
        lock::ScopedChipStackLock lock(3000);
        xEventGroupClearBits(s_matter_controller_event_group, READ_HANDLED_EVENT);
        err = cmd->send_command();
    }
    if (err != ESP_OK) {
        if (s_resp_root) {
            cJSON_AddStringToObject(s_resp_root, "status", "failure");
            cJSON_AddStringToObject(s_resp_root, "reason", "send_command failed");
        }
    } else {
        EventBits_t bits = xEventGroupWaitBits(s_matter_controller_event_group, READ_HANDLED_EVENT, true, true,
                                               pdMS_TO_TICKS(CONFIG_RMAKER_MTCTL_CMDRESP_MTCMD_TIMEOUT_MS));
        if ((bits & READ_HANDLED_EVENT) == 0) {
            if (s_resp_root) {
                cJSON_AddStringToObject(s_resp_root, "status", "failure");
                cJSON_AddStringToObject(s_resp_root, "reason", "read command timeout");
            }
            cmd->Deactivate();
            err = ESP_ERR_TIMEOUT;
        }
    }
    return err;
}

esp_err_t esp_rmaker_matter_controller_write_attr_handler(const void *in_data, size_t in_len,
                                                          void **out_data, size_t *out_len,
                                                          esp_rmaker_cmd_ctx_t *ctx, void *priv)
{
    if (in_data == nullptr || in_len == 0) {
        ESP_LOGE(TAG, "No data received for sending writing attribute command");
        return ESP_FAIL;
    }

    CmdRespLock lock;
    if (!lock.locked()) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_cmd_resp_buffer == nullptr) {
        ESP_LOGE(TAG, "Command response buffer not allocated");
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(TAG, "Receive write-attribute command: %u bytes", (unsigned)in_len);
    ESP_LOGD(TAG, "Write-attribute payload: %.*s", (int)in_len, (char *)in_data);

    cJSON *root = cJSON_ParseWithLength((const char *)in_data, in_len);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        ESP_LOGE(TAG, "Failed to parse input JSON");
        return ESP_FAIL;
    }

    uint32_t cluster_id = chip::kInvalidClusterId, attribute_id = chip::kInvalidAttributeId;
    char attr_val_buffer[CONFIG_RMAKER_MTCTL_CMDRESP_ATTRVAL_BUFFER_SIZE] = {0};
    char id_buffer[19] = {0};
    cJSON *request_payload = cJSON_GetObjectItemCaseSensitive(root, "request_payload");
    if (!cJSON_IsObject(request_payload)) {
        cJSON_Delete(root);
        ESP_LOGE(TAG, "No 'request_payload' found in input");
        return ESP_FAIL;
    }
    if (json_get_string(request_payload, "cluster_id", id_buffer, sizeof(id_buffer))) {
        cluster_id = string_to_uint32(id_buffer);
    }
    if (json_get_string(request_payload, "attribute_id", id_buffer, sizeof(id_buffer))) {
        attribute_id = string_to_uint32(id_buffer);
    }
    if (cluster_id == chip::kInvalidClusterId || attribute_id == chip::kInvalidAttributeId) {
        cJSON_Delete(root);
        return ESP_FAIL;
    }
    if (!json_get_object_text(request_payload, "attribute_value", attr_val_buffer, sizeof(attr_val_buffer))) {
        cJSON_Delete(root);
        ESP_LOGE(TAG, "Invalid attribute_value");
        return ESP_FAIL;
    }

    cJSON *objects = cJSON_GetObjectItemCaseSensitive(root, "objects");
    if (!cJSON_IsArray(objects)) {
        cJSON_Delete(root);
        ESP_LOGE(TAG, "No 'objects' found in input");
        return ESP_FAIL;
    }
    esp_err_t ret = start_response_array("responses");
    if (ret != ESP_OK) {
        cJSON_Delete(root);
        return ret;
    }
    cJSON *object = nullptr;
    cJSON_ArrayForEach(object, objects) {
        if (!cJSON_IsObject(object)) {
            continue;
        }
        uint64_t node_id = chip::kUndefinedNodeId;
        uint16_t endpoint_id = chip::kInvalidEndpointId;
        cJSON *resp_obj = cJSON_CreateObject();
        if (!resp_obj) {
            cJSON_Delete(root);
            reset_response_json();
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddItemToArray(s_resp_array, resp_obj);
        if (json_get_string(object, "matter_node_id", id_buffer, sizeof(id_buffer))) {
            cJSON_AddStringToObject(resp_obj, "matter_node_id", id_buffer);
            node_id = string_to_uint64(id_buffer);
        }
        if (json_get_string(object, "matter_endpoint_id", id_buffer, sizeof(id_buffer))) {
            cJSON_AddStringToObject(resp_obj, "matter_endpoint_id", id_buffer);
            endpoint_id = string_to_uint16(id_buffer);
        }
        if (node_id != chip::kUndefinedNodeId && endpoint_id != chip::kInvalidEndpointId) {
            if (write_attr_command(node_id, endpoint_id, cluster_id, attribute_id, attr_val_buffer, resp_obj) != ESP_OK &&
                    !cJSON_GetObjectItemCaseSensitive(resp_obj, "status")) {
                cJSON_AddStringToObject(resp_obj, "status", "failure");
            }
        } else {
            cJSON_AddStringToObject(resp_obj, "status", "failure");
            cJSON_AddStringToObject(resp_obj, "reason", "invalid object");
        }
    }
    cJSON_Delete(root);
    ret = serialize_response(out_len);
    if (ret != ESP_OK) {
        return ret;
    }
    *out_data = s_cmd_resp_buffer;
    ESP_LOGI(TAG, "Returning write-attribute response: %u bytes", (unsigned)*out_len);
    ESP_LOGD(TAG, "Write-attribute response: %s", s_cmd_resp_buffer);
    return ESP_OK;
}

esp_err_t esp_rmaker_matter_controller_read_handler(const void *in_data, size_t in_len,
                                                    void **out_data, size_t *out_len,
                                                    esp_rmaker_cmd_ctx_t *ctx, void *priv)
{

    if (in_data == nullptr || in_len == 0) {
        ESP_LOGE(TAG, "No data received for sending reading attribute/event command");
        return ESP_FAIL;
    }

    CmdRespLock lock;
    if (!lock.locked()) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_cmd_resp_buffer == nullptr) {
        ESP_LOGE(TAG, "Command response buffer not allocated");
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(TAG, "Receive read attribute/event command: %u bytes", (unsigned)in_len);
    ESP_LOGD(TAG, "Read attribute/event payload: %.*s", (int)in_len, (char *)in_data);

    cJSON *root = cJSON_ParseWithLength((const char *)in_data, in_len);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        ESP_LOGE(TAG, "Failed to parse input JSON");
        return ESP_FAIL;
    }

    uint64_t node_id = chip::kUndefinedNodeId;
    chip::Platform::ScopedMemoryBufferWithSize<AttributePathParams> attr_paths;
    chip::Platform::ScopedMemoryBufferWithSize<EventPathParams> event_paths;
    char id_buffer[19] = {0};
    char node_id_str[19] = {0};
    if (json_get_string(root, "matter_node_id", id_buffer, sizeof(id_buffer))) {
        node_id = string_to_uint64(id_buffer);
        strlcpy(node_id_str, id_buffer, sizeof(node_id_str));
    }

    int attr_path_count = 0;
    int event_path_count = 0;
    cJSON *attribute_paths = cJSON_GetObjectItemCaseSensitive(root, "attribute_paths");
    if (cJSON_IsArray(attribute_paths)) {
        attr_path_count = cJSON_GetArraySize(attribute_paths);
        attr_paths.Alloc(attr_path_count);
        if (!attr_paths.Get()) {
            cJSON_Delete(root);
            return ESP_ERR_NO_MEM;
        }
        int i = 0;
        cJSON *path = nullptr;
        cJSON_ArrayForEach(path, attribute_paths) {
            uint16_t endpoint_id = chip::kInvalidEndpointId;
            uint32_t cluster_id = chip::kInvalidClusterId, attribute_id = chip::kInvalidAttributeId;
            if (cJSON_IsObject(path)) {
                if (json_get_string(path, "endpoint_id", id_buffer, sizeof(id_buffer))) {
                    endpoint_id = string_to_uint16(id_buffer);
                }
                if (json_get_string(path, "cluster_id", id_buffer, sizeof(id_buffer))) {
                    cluster_id = string_to_uint32(id_buffer);
                }
                if (json_get_string(path, "attribute_id", id_buffer, sizeof(id_buffer))) {
                    attribute_id = string_to_uint32(id_buffer);
                }
            }
            attr_paths[i++] = AttributePathParams(endpoint_id, cluster_id, attribute_id);
        }
    }
    cJSON *event_paths_json = cJSON_GetObjectItemCaseSensitive(root, "event_paths");
    if (cJSON_IsArray(event_paths_json)) {
        event_path_count = cJSON_GetArraySize(event_paths_json);
        event_paths.Alloc(event_path_count);
        if (!event_paths.Get()) {
            cJSON_Delete(root);
            return ESP_ERR_NO_MEM;
        }
        int i = 0;
        cJSON *path = nullptr;
        cJSON_ArrayForEach(path, event_paths_json) {
            uint16_t endpoint_id = chip::kInvalidEndpointId;
            uint32_t cluster_id = chip::kInvalidClusterId, event_id = chip::kInvalidEventId;
            if (cJSON_IsObject(path)) {
                if (json_get_string(path, "endpoint_id", id_buffer, sizeof(id_buffer))) {
                    endpoint_id = string_to_uint16(id_buffer);
                }
                if (json_get_string(path, "cluster_id", id_buffer, sizeof(id_buffer))) {
                    cluster_id = string_to_uint32(id_buffer);
                }
                if (json_get_string(path, "event_id", id_buffer, sizeof(id_buffer))) {
                    event_id = string_to_uint32(id_buffer);
                }
            }
            event_paths[i++] = EventPathParams(endpoint_id, cluster_id, event_id);
        }
    }
    if (node_id == chip::kUndefinedNodeId) {
        cJSON_Delete(root);
        ESP_LOGE(TAG, "Invalid or missing matter_node_id");
        return ESP_FAIL;
    }
    if (attr_path_count == 0 && event_path_count == 0) {
        cJSON_Delete(root);
        ESP_LOGE(TAG, "At least one attribute_paths or event_paths entry required");
        return ESP_FAIL;
    }

    reset_response_json();
    s_resp_root = cJSON_CreateObject();
    s_resp_array = cJSON_CreateArray();
    if (!s_resp_root || !s_resp_array) {
        cJSON_Delete(root);
        reset_response_json();
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(s_resp_root, "matter_node_id", node_id_str);
    cJSON_AddItemToObject(s_resp_root, "read_results", s_resp_array);
    if (read_attr_or_event_command(node_id, std::move(attr_paths), std::move(event_paths)) != ESP_OK &&
            !cJSON_GetObjectItemCaseSensitive(s_resp_root, "status")) {
        cJSON_AddStringToObject(s_resp_root, "status", "failure");
        cJSON_AddStringToObject(s_resp_root, "reason", "read_attr_or_event_command failed");
    }
    cJSON_Delete(root);
    esp_err_t ret = serialize_response(out_len);
    if (ret != ESP_OK) {
        return ret;
    }
    *out_data = s_cmd_resp_buffer;
    ESP_LOGI(TAG, "Returning read attribute/event response: %u bytes", (unsigned)*out_len);
    ESP_LOGD(TAG, "Read attribute/event response: %s", s_cmd_resp_buffer);
    return ESP_OK;
}
} // namespace


esp_err_t app_rmaker_matter_cmd_resp_enable(void)
{
    if (s_cmd_resp_enabled) {
        return ESP_OK;
    }
    if (s_cmd_resp_buffer == nullptr) {
        s_cmd_resp_buffer = (char *)MEM_CALLOC_EXTRAM(1, CONFIG_RMAKER_MTCTL_CMDRESP_RESPONSE_BUFFER_SIZE);
        if (s_cmd_resp_buffer == NULL) {
            ESP_LOGE(TAG, "Failed to allocate command response buffer");
            return ESP_ERR_NO_MEM;
        }
        ESP_LOGI(TAG, "Allocated %d bytes for command response buffer", CONFIG_RMAKER_MTCTL_CMDRESP_RESPONSE_BUFFER_SIZE);
    }
    if (s_cmd_resp_mutex == nullptr) {
        s_cmd_resp_mutex = xSemaphoreCreateMutex();
        if (!s_cmd_resp_mutex) {
            ESP_LOGE(TAG, "Failed to allocate command response mutex");
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_matter_controller_event_group == nullptr) {
        s_matter_controller_event_group = xEventGroupCreate();
        if (!s_matter_controller_event_group) {
            ESP_LOGE(TAG, "Failed to allocate controller event group");
            return ESP_ERR_NO_MEM;
        }
    }
    esp_err_t ret = ESP_OK;

    if (!s_invoke_cmd_registered) {
        ESP_GOTO_ON_ERROR(esp_rmaker_cmd_register(MATTER_CONTROL_CMD_TYPE_INVOKE_CMD,
                                                  ESP_RMAKER_USER_ROLE_SUPER_ADMIN |
                                                  ESP_RMAKER_USER_ROLE_PRIMARY_USER |
                                                  ESP_RMAKER_USER_ROLE_SECONDARY_USER,
                                                  esp_rmaker_matter_controller_invoke_cmd_handler,
                                                  false, nullptr),
                          clear, TAG, "Failed to register invoke command handler");
        s_invoke_cmd_registered = true;
    }

    if (!s_write_attr_cmd_registered) {
        ESP_GOTO_ON_ERROR(esp_rmaker_cmd_register(MATTER_CONTROL_CMD_TYPE_WRITE_ATTR,
                                                  ESP_RMAKER_USER_ROLE_SUPER_ADMIN |
                                                  ESP_RMAKER_USER_ROLE_PRIMARY_USER |
                                                  ESP_RMAKER_USER_ROLE_SECONDARY_USER,
                                                  esp_rmaker_matter_controller_write_attr_handler,
                                                  false, nullptr),
                          clear, TAG, "Failed to register write attribute handler");
        s_write_attr_cmd_registered = true;
    }

    if (!s_read_cmd_registered) {
        ESP_GOTO_ON_ERROR(esp_rmaker_cmd_register(MATTER_CONTROL_CMD_TYPE_READ,
                                                  ESP_RMAKER_USER_ROLE_SUPER_ADMIN |
                                                  ESP_RMAKER_USER_ROLE_PRIMARY_USER |
                                                  ESP_RMAKER_USER_ROLE_SECONDARY_USER,
                                                  esp_rmaker_matter_controller_read_handler,
                                                  false, nullptr),
                          clear, TAG, "Failed to register read handler");
        s_read_cmd_registered = true;
    }
clear:
    if (ret != ESP_OK) {
        bool any_registered = s_invoke_cmd_registered || s_write_attr_cmd_registered || s_read_cmd_registered;
        if (any_registered) {
            ESP_LOGW(TAG, "Command response enable incomplete; keeping resources for registered handlers");
        } else if (s_cmd_resp_buffer) {
            free(s_cmd_resp_buffer);
            s_cmd_resp_buffer = nullptr;
        }
        if (!any_registered && s_matter_controller_event_group) {
            vEventGroupDelete(s_matter_controller_event_group);
            s_matter_controller_event_group = NULL;
        }
        if (!any_registered && s_cmd_resp_mutex) {
            vSemaphoreDelete(s_cmd_resp_mutex);
            s_cmd_resp_mutex = nullptr;
        }
    } else {
        s_cmd_resp_enabled = s_invoke_cmd_registered && s_write_attr_cmd_registered && s_read_cmd_registered;
    }
    return ret;
}
