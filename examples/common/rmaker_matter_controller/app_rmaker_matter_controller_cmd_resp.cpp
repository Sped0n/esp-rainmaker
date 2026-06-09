/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <esp_rmaker_cmd_resp.h>
#include <esp_rmaker_utils.h>
#include <esp_matter_core.h>
#include <esp_matter_controller_utils.h>
#include <esp_matter_controller_cluster_command.h>
#include <esp_matter_controller_write_command.h>
#include <esp_matter_controller_read_command.h>
#include <esp_check.h>
#include <esp_log.h>
#include <freertos/semphr.h>
#include <app_rmaker_matter_rmctl_internal.h>

#include <cJSON.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <utility>

#include <app/ConcreteAttributePath.h>
#include <app/MessageDef/StatusIB.h>
#include <app/WriteClient.h>
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
#define MATTER_CONTROL_CMD_TYPE_READ 0x1102

#define MAX_COMMAND_FIELD_BUFFER_SIZE 120
#define MAX_ATTRIBUTE_VALUE_BUFFER_SIZE MAX_COMMAND_FIELD_BUFFER_SIZE
#define MAX_CMD_RESP_BUFFER_SIZE 5000

namespace {
char *s_cmd_resp_buffer = nullptr;
cJSON *s_resp_root = nullptr;
cJSON *s_resp_array = nullptr;
cJSON *s_current_resp_obj = nullptr;
SemaphoreHandle_t s_cmd_resp_mutex = nullptr;
const int INVOKE_CMD_HANDLED_EVENT = BIT0;
const int WRITE_ATTR_HANDLED_EVENT = BIT1;
const int READ_HANDLED_EVENT = BIT2;
EventGroupHandle_t s_matter_controller_event_group;

class CmdRespLock {
public:
    CmdRespLock() : m_locked(s_cmd_resp_mutex && xSemaphoreTake(s_cmd_resp_mutex, portMAX_DELAY) == pdTRUE) {}
    ~CmdRespLock()
    {
        if (m_locked) {
            xSemaphoreGive(s_cmd_resp_mutex);
        }
    }
    bool locked() const { return m_locked; }

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
    s_current_resp_obj = nullptr;
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

    char *json = cJSON_PrintUnformatted(s_resp_root);
    if (!json) {
        reset_response_json();
        return ESP_ERR_NO_MEM;
    }

    size_t json_len = strnlen(json, MAX_CMD_RESP_BUFFER_SIZE);
    if (json_len >= MAX_CMD_RESP_BUFFER_SIZE) {
        cJSON_free(json);
        reset_response_json();
        return ESP_FAIL;
    }

    memset(s_cmd_resp_buffer, 0, MAX_CMD_RESP_BUFFER_SIZE);
    memcpy(s_cmd_resp_buffer, json, json_len + 1);
    *out_len = json_len;
    cJSON_free(json);
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

    char *json = cJSON_PrintUnformatted(item);
    if (!json) {
        return false;
    }
    size_t json_len = strnlen(json, buf_size);
    if (json_len >= buf_size) {
        cJSON_free(json);
        return false;
    }
    memcpy(buf, json, json_len + 1);
    cJSON_free(json);
    return true;
}

static bool appendf(char *buf, size_t buf_size, size_t &pos, const char *fmt, ...)
{
    if (!buf || buf_size == 0 || pos >= buf_size) {
        return false;
    }
    va_list args;
    va_start(args, fmt);
    int written = vsnprintf(buf + pos, buf_size - pos, fmt, args);
    va_end(args);
    if (written < 0) {
        return false;
    }
    if (static_cast<size_t>(written) >= (buf_size - pos)) {
        pos = buf_size - 1;
        buf[pos] = '\0';
        return false;
    }
    pos += static_cast<size_t>(written);
    return true;
}

static void append_json_escaped_charspan(const chip::CharSpan &str, char *buf, size_t buf_size, size_t &pos)
{
    appendf(buf, buf_size, pos, "\"");
    for (size_t i = 0; i < str.size(); ++i) {
        const char c = str.data()[i];
        if (c == '"' || c == '\\') {
            appendf(buf, buf_size, pos, "\\%c", c);
        } else if (c == '\n') {
            appendf(buf, buf_size, pos, "\\n");
        } else if (c == '\r') {
            appendf(buf, buf_size, pos, "\\r");
        } else if (c == '\t') {
            appendf(buf, buf_size, pos, "\\t");
        } else if (static_cast<unsigned char>(c) < 0x20) {
            appendf(buf, buf_size, pos, "\\u%04x", static_cast<unsigned char>(c));
        } else {
            appendf(buf, buf_size, pos, "%c", c);
        }
    }
    appendf(buf, buf_size, pos, "\"");
}

static bool decode_tlv_reader_to_string(chip::TLV::TLVReader &reader, char *buf, size_t buf_size, size_t &pos, int depth)
{
    if (depth > 8) {
        return appendf(buf, buf_size, pos, "\"max_depth\"");
    }

    switch (reader.GetType()) {
    case chip::TLV::kTLVType_Boolean: {
        bool v = false;
        if (reader.Get(v) == CHIP_NO_ERROR) {
            return appendf(buf, buf_size, pos, "%s", v ? "true" : "false");
        }
        return appendf(buf, buf_size, pos, "\"bool_decode_failed\"");
    }
    case chip::TLV::kTLVType_SignedInteger: {
        int64_t v = 0;
        if (reader.Get(v) == CHIP_NO_ERROR) {
            return appendf(buf, buf_size, pos, "%" PRId64, v);
        }
        return appendf(buf, buf_size, pos, "\"int_decode_failed\"");
    }
    case chip::TLV::kTLVType_UnsignedInteger: {
        uint64_t v = 0;
        if (reader.Get(v) == CHIP_NO_ERROR) {
            return appendf(buf, buf_size, pos, "%" PRIu64, v);
        }
        return appendf(buf, buf_size, pos, "\"uint_decode_failed\"");
    }
    case chip::TLV::kTLVType_FloatingPointNumber: {
        double v = 0.0;
        if (reader.Get(v) == CHIP_NO_ERROR) {
            return appendf(buf, buf_size, pos, "%g", v);
        }
        float fv = 0.0f;
        if (reader.Get(fv) == CHIP_NO_ERROR) {
            return appendf(buf, buf_size, pos, "%g", static_cast<double>(fv));
        }
        return appendf(buf, buf_size, pos, "\"float_decode_failed\"");
    }
    case chip::TLV::kTLVType_UTF8String: {
        chip::CharSpan str;
        if (reader.Get(str) == CHIP_NO_ERROR) {
            append_json_escaped_charspan(str, buf, buf_size, pos);
            return true;
        }
        return appendf(buf, buf_size, pos, "\"string_decode_failed\"");
    }
    case chip::TLV::kTLVType_ByteString: {
        chip::ByteSpan bytes;
        if (reader.Get(bytes) == CHIP_NO_ERROR) {
            appendf(buf, buf_size, pos, "\"0x");
            for (size_t i = 0; i < bytes.size(); ++i) {
                if (!appendf(buf, buf_size, pos, "%02x", bytes.data()[i])) {
                    appendf(buf, buf_size, pos, "...");
                    break;
                }
            }
            return appendf(buf, buf_size, pos, "\"");
        }
        return appendf(buf, buf_size, pos, "\"bytes_decode_failed\"");
    }
    case chip::TLV::kTLVType_Null:
        return appendf(buf, buf_size, pos, "null");
    case chip::TLV::kTLVType_Structure:
    case chip::TLV::kTLVType_Array:
    case chip::TLV::kTLVType_List: {
        const bool is_object_like = (reader.GetType() == chip::TLV::kTLVType_Structure);
        appendf(buf, buf_size, pos, "%c", is_object_like ? '{' : '[');

        chip::TLV::TLVType outer_container_type = chip::TLV::kTLVType_NotSpecified;
        if (reader.EnterContainer(outer_container_type) != CHIP_NO_ERROR) {
            return appendf(buf, buf_size, pos, "%c", is_object_like ? '}' : ']');
        }

        bool first = true;
        while (reader.Next() == CHIP_NO_ERROR) {
            if (!first) {
                appendf(buf, buf_size, pos, ",");
            }
            first = false;

            if (is_object_like) {
                if (chip::TLV::IsContextTag(reader.GetTag())) {
                    appendf(buf, buf_size, pos, "\"%" PRIu32 "\":",
                            static_cast<uint32_t>(chip::TLV::TagNumFromTag(reader.GetTag())));
                } else {
                    appendf(buf, buf_size, pos, "\"tag\":");
                }
            }

            if (!decode_tlv_reader_to_string(reader, buf, buf_size, pos, depth + 1)) {
                break;
            }
        }
        reader.ExitContainer(outer_container_type);
        return appendf(buf, buf_size, pos, "%c", is_object_like ? '}' : ']');
    }
    default:
        return appendf(buf, buf_size, pos, "\"unsupported_tlv_type\"");
    }
}

void decode_tlv_value_to_cjson(chip::TLV::TLVReader *data, const char *key, cJSON *obj)
{
    if (!data || !key || !obj) {
        return;
    }

    switch (data->GetType()) {
    case chip::TLV::kTLVType_Boolean: {
        bool v = false;
        if (data->Get(v) == CHIP_NO_ERROR) {
            cJSON_AddBoolToObject(obj, key, v);
            return;
        }
        break;
    }
    case chip::TLV::kTLVType_SignedInteger: {
        int64_t v = 0;
        if (data->Get(v) == CHIP_NO_ERROR) {
            if (v >= INT_MIN && v <= INT_MAX) {
                cJSON_AddNumberToObject(obj, key, static_cast<int>(v));
            } else {
                char num_buf[32] = {0};
                snprintf(num_buf, sizeof(num_buf), "%" PRId64, v);
                cJSON_AddStringToObject(obj, key, num_buf);
            }
            return;
        }
        break;
    }
    case chip::TLV::kTLVType_UnsignedInteger: {
        uint64_t v = 0;
        if (data->Get(v) == CHIP_NO_ERROR) {
            if (v <= static_cast<uint64_t>(INT_MAX)) {
                cJSON_AddNumberToObject(obj, key, static_cast<int>(v));
            } else {
                char num_buf[32] = {0};
                snprintf(num_buf, sizeof(num_buf), "%" PRIu64, v);
                cJSON_AddStringToObject(obj, key, num_buf);
            }
            return;
        }
        break;
    }
    case chip::TLV::kTLVType_FloatingPointNumber: {
        double dv = 0.0;
        if (data->Get(dv) == CHIP_NO_ERROR) {
            cJSON_AddNumberToObject(obj, key, dv);
            return;
        }
        float fv = 0.0f;
        if (data->Get(fv) == CHIP_NO_ERROR) {
            cJSON_AddNumberToObject(obj, key, fv);
            return;
        }
        break;
    }
    case chip::TLV::kTLVType_UTF8String: {
        chip::CharSpan str;
        if (data->Get(str) == CHIP_NO_ERROR) {
            char str_buf[192] = {0};
            size_t copy_len = str.size();
            if (copy_len >= sizeof(str_buf)) {
                copy_len = sizeof(str_buf) - 1;
            }
            memcpy(str_buf, str.data(), copy_len);
            str_buf[copy_len] = '\0';
            cJSON_AddStringToObject(obj, key, str_buf);
            return;
        }
        break;
    }
    case chip::TLV::kTLVType_ByteString: {
        chip::ByteSpan bytes;
        if (data->Get(bytes) == CHIP_NO_ERROR) {
            char hex_buf[192] = {0};
            size_t pos = 0;
            appendf(hex_buf, sizeof(hex_buf), pos, "0x");
            for (size_t i = 0; i < bytes.size(); ++i) {
                if (!appendf(hex_buf, sizeof(hex_buf), pos, "%02x", bytes.data()[i])) {
                    appendf(hex_buf, sizeof(hex_buf), pos, "...");
                    break;
                }
            }
            cJSON_AddStringToObject(obj, key, hex_buf);
            return;
        }
        break;
    }
    case chip::TLV::kTLVType_Null:
        cJSON_AddNullToObject(obj, key);
        return;
    case chip::TLV::kTLVType_Structure:
    case chip::TLV::kTLVType_Array:
    case chip::TLV::kTLVType_List: {
        char json_buf[256] = {0};
        chip::TLV::TLVReader reader_cpy;
        reader_cpy.Init(*data);
        size_t pos = 0;
        if (!decode_tlv_reader_to_string(reader_cpy, json_buf, sizeof(json_buf), pos, 0)) {
            cJSON_AddStringToObject(obj, key, "decode_truncated");
            return;
        }
        cJSON *decoded = cJSON_Parse(json_buf);
        if (decoded) {
            cJSON_AddItemToObject(obj, key, decoded);
        } else {
            cJSON_AddStringToObject(obj, key, json_buf);
        }
        return;
    }
    default:
        break;
    }
    cJSON_AddStringToObject(obj, key, "tlv_decode_failed");
}

void invoke_cmd_success_fcn(void *ctx, const ConcreteCommandPath &command_path, const StatusIB &status,
                            TLVReader *response_data)
{
    if (s_current_resp_obj) {
        cJSON_AddStringToObject(s_current_resp_obj, "status", "success");
        if (response_data) {
            decode_tlv_value_to_cjson(response_data, "response_data", s_current_resp_obj);
        }
    }
    xEventGroupSetBits(s_matter_controller_event_group, INVOKE_CMD_HANDLED_EVENT);
}

void invoke_cmd_failure_fcn(void *ctx, CHIP_ERROR error)
{
    if (s_current_resp_obj) {
        cJSON_AddStringToObject(s_current_resp_obj, "status", "failure");
        cJSON_AddStringToObject(s_current_resp_obj, "reason", error.AsString());
    }
    xEventGroupSetBits(s_matter_controller_event_group, INVOKE_CMD_HANDLED_EVENT);
}

static void read_attribute_data_cb(uint64_t remote_node_id,
                                    const chip::app::ConcreteDataAttributePath &path,
                                    chip::TLV::TLVReader *data)
{
    char endpoint_id_str[8] = {0};
    char cluster_id_str[12] = {0};
    char attribute_id_str[12] = {0};
    snprintf(endpoint_id_str, sizeof(endpoint_id_str), "%u", path.mEndpointId);
    snprintf(cluster_id_str, sizeof(cluster_id_str), "0x%08lx", (unsigned long)path.mClusterId);
    snprintf(attribute_id_str, sizeof(attribute_id_str), "0x%08lx", (unsigned long)path.mAttributeId);

    if (!s_resp_array) {
        return;
    }

    cJSON *result = cJSON_CreateObject();
    if (!result) {
        return;
    }
    cJSON_AddItemToArray(s_resp_array, result);
    cJSON_AddStringToObject(result, "endpoint_id", endpoint_id_str);
    cJSON_AddStringToObject(result, "cluster_id", cluster_id_str);
    cJSON_AddStringToObject(result, "attribute_id", attribute_id_str);
    if (data) {
        decode_tlv_value_to_cjson(data, "attribute_value", result);
    }
}

static void read_event_data_cb(uint64_t remote_node_id,
                               const chip::app::EventHeader &header,
                               chip::TLV::TLVReader *data)
{
    char endpoint_id_str[8] = {0};
    char cluster_id_str[12] = {0};
    char event_id_str[12] = {0};
    snprintf(endpoint_id_str, sizeof(endpoint_id_str), "%u", header.mPath.mEndpointId);
    snprintf(cluster_id_str, sizeof(cluster_id_str), "0x%08lx", (unsigned long)header.mPath.mClusterId);
    snprintf(event_id_str, sizeof(event_id_str), "0x%08lx", (unsigned long)header.mPath.mEventId);

    if (!s_resp_array) {
        return;
    }

    cJSON *result = cJSON_CreateObject();
    if (!result) {
        return;
    }
    cJSON_AddItemToArray(s_resp_array, result);
    cJSON_AddStringToObject(result, "endpoint_id", endpoint_id_str);
    cJSON_AddStringToObject(result, "cluster_id", cluster_id_str);
    cJSON_AddStringToObject(result, "event_id", event_id_str);
    if (data) {
        decode_tlv_value_to_cjson(data, "event_data", result);
    }
}

static void read_attribute_done_cb(uint64_t remote_node_id,
                                    const chip::Platform::ScopedMemoryBufferWithSize<AttributePathParams> &attr_path,
                                    const chip::Platform::ScopedMemoryBufferWithSize<EventPathParams> &event_path)
{
    (void)remote_node_id;
    (void)attr_path;
    (void)event_path;
    xEventGroupSetBits(s_matter_controller_event_group, READ_HANDLED_EVENT);
}

esp_err_t invoke_cluster_command(uint64_t destination_id, uint16_t endpoint_id, uint32_t cluster_id,
                                 uint32_t command_id, const char *command_data_field,
                                 const chip::Optional<uint16_t> timed_interaction_timeout_ms)
{
    ESP_LOGI(TAG, "Send cluster command [cluster 0x%lx, command 0x%lx] to node %llx endpoint %x", cluster_id, command_id,
             destination_id, endpoint_id);
    controller::cluster_command *cluster_command =
        chip::Platform::New<controller::cluster_command>(destination_id, endpoint_id, cluster_id, command_id, command_data_field,
                                                         timed_interaction_timeout_ms, invoke_cmd_success_fcn, invoke_cmd_failure_fcn);
    if (!cluster_command) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = ESP_OK;
    {
        lock::ScopedChipStackLock lock(3000);
        xEventGroupClearBits(s_matter_controller_event_group, INVOKE_CMD_HANDLED_EVENT);
        err = cluster_command->send_command();
    }
    if (err == ESP_OK) {
        xEventGroupWaitBits(s_matter_controller_event_group, INVOKE_CMD_HANDLED_EVENT, true, true, 20000 / portTICK_PERIOD_MS);
    } else {
        chip::Platform::Delete(cluster_command);
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
    ESP_LOGI(TAG, "Receive invoke-command command: %.*s", (int)in_len, (char *)in_data);

    cJSON *root = cJSON_ParseWithLength((const char *)in_data, in_len);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        ESP_LOGE(TAG, "Failed to parse input JSON");
        return ESP_FAIL;
    }

    uint32_t cluster_id = chip::kInvalidClusterId, command_id = chip::kInvalidCommandId;
    char command_fields_buffer[MAX_COMMAND_FIELD_BUFFER_SIZE] = "{}";
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
        s_current_resp_obj = cJSON_CreateObject();
        if (!s_current_resp_obj) {
            cJSON_Delete(root);
            reset_response_json();
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddItemToArray(s_resp_array, s_current_resp_obj);
        if (json_get_string(object, "matter_node_id", id_buffer, sizeof(id_buffer))) {
            cJSON_AddStringToObject(s_current_resp_obj, "matter_node_id", id_buffer);
            node_id = string_to_uint64(id_buffer);
        }
        if (json_get_string(object, "matter_endpoint_id", id_buffer, sizeof(id_buffer))) {
            cJSON_AddStringToObject(s_current_resp_obj, "matter_endpoint_id", id_buffer);
            endpoint_id = string_to_uint16(id_buffer);
        }
        if (node_id != chip::kUndefinedNodeId && endpoint_id != chip::kInvalidEndpointId) {
            esp_err_t err = has_timed_interaction_timeout ?
                invoke_cluster_command(node_id, endpoint_id, cluster_id, command_id, command_fields_buffer,
                                       chip::MakeOptional(timed_interaction_timeout_ms)) :
                invoke_cluster_command(node_id, endpoint_id, cluster_id, command_id, command_fields_buffer,
                                       chip::NullOptional);
            if (err != ESP_OK) {
                cJSON_AddStringToObject(s_current_resp_obj, "status", "failure");
            }
        } else {
            cJSON_AddStringToObject(s_current_resp_obj, "status", "failure");
            cJSON_AddStringToObject(s_current_resp_obj, "reason", "invalid object");
        }
    }
    cJSON_Delete(root);
    ret = serialize_response(out_len);
    if (ret != ESP_OK) {
        return ret;
    }
    *out_data = s_cmd_resp_buffer;
    ESP_LOGI(TAG, "Returning response: %s", s_cmd_resp_buffer);
    return ESP_OK;
}

esp_err_t write_attr_command(uint64_t node_id, uint16_t endpoint_id, uint32_t cluster_id,
                             uint32_t attribute_id, const char *attr_val)
{
    ESP_LOGI(TAG, "Send write_attr command [cluster 0x%lx, attribute 0x%lx] to node %llx endpoint %x", cluster_id, attribute_id,
             node_id, endpoint_id);
    controller::write_command *write_command =
        chip::Platform::New<controller::write_command>(node_id, endpoint_id, cluster_id, attribute_id, attr_val,
                                                       chip::NullOptional);
    if (!write_command) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = ESP_OK;
    {
        lock::ScopedChipStackLock lock(3000);
        xEventGroupClearBits(s_matter_controller_event_group, WRITE_ATTR_HANDLED_EVENT);
        err = write_command->send_command();
    }
    if (err == ESP_OK) {
        if (s_current_resp_obj) {
            cJSON_AddStringToObject(s_current_resp_obj, "status", "success");
        }
    } else {
        chip::Platform::Delete(write_command);
    }
    return err;
}

esp_err_t read_attr_or_event_command(uint64_t node_id,
                                     chip::Platform::ScopedMemoryBufferWithSize<AttributePathParams> &&attr_paths,
                                     chip::Platform::ScopedMemoryBufferWithSize<EventPathParams> &&event_paths)
{
    ESP_LOGI(TAG, "Send read attribute/event command to node %llx", node_id);
    controller::read_command *cmd = chip::Platform::New<controller::read_command>(
        node_id, std::move(attr_paths), std::move(event_paths),
        read_attribute_data_cb, read_attribute_done_cb, read_event_data_cb);
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
        chip::Platform::Delete(cmd);
    } else {
        xEventGroupWaitBits(s_matter_controller_event_group, READ_HANDLED_EVENT, true, true, 20000 / portTICK_PERIOD_MS);
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
    ESP_LOGI(TAG, "Receive write-attribute command: %.*s", (int)in_len, (char *)in_data);

    cJSON *root = cJSON_ParseWithLength((const char *)in_data, in_len);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        ESP_LOGE(TAG, "Failed to parse input JSON");
        return ESP_FAIL;
    }

    uint32_t cluster_id = chip::kInvalidClusterId, attribute_id = chip::kInvalidAttributeId;
    char attr_val_buffer[MAX_ATTRIBUTE_VALUE_BUFFER_SIZE] = {0};
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
        s_current_resp_obj = cJSON_CreateObject();
        if (!s_current_resp_obj) {
            cJSON_Delete(root);
            reset_response_json();
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddItemToArray(s_resp_array, s_current_resp_obj);
        if (json_get_string(object, "matter_node_id", id_buffer, sizeof(id_buffer))) {
            cJSON_AddStringToObject(s_current_resp_obj, "matter_node_id", id_buffer);
            node_id = string_to_uint64(id_buffer);
        }
        if (json_get_string(object, "matter_endpoint_id", id_buffer, sizeof(id_buffer))) {
            cJSON_AddStringToObject(s_current_resp_obj, "matter_endpoint_id", id_buffer);
            endpoint_id = string_to_uint16(id_buffer);
        }
        if (node_id != chip::kUndefinedNodeId && endpoint_id != chip::kInvalidEndpointId) {
            if (write_attr_command(node_id, endpoint_id, cluster_id, attribute_id, attr_val_buffer) != ESP_OK) {
                cJSON_AddStringToObject(s_current_resp_obj, "status", "failure");
            }
        } else {
            cJSON_AddStringToObject(s_current_resp_obj, "status", "failure");
            cJSON_AddStringToObject(s_current_resp_obj, "reason", "invalid object");
        }
    }
    cJSON_Delete(root);
    ret = serialize_response(out_len);
    if (ret != ESP_OK) {
        return ret;
    }
    *out_data = s_cmd_resp_buffer;
    ESP_LOGI(TAG, "Returning response: %s", s_cmd_resp_buffer);
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
    ESP_LOGI(TAG, "Receive read attribute/event command: %.*s", (int)in_len, (char *)in_data);

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
    if (read_attr_or_event_command(node_id, std::move(attr_paths), std::move(event_paths)) != ESP_OK) {
        cJSON_AddStringToObject(s_resp_root, "status", "failure");
        cJSON_AddStringToObject(s_resp_root, "reason", "read_attr_or_event_command failed");
    }
    cJSON_Delete(root);
    esp_err_t ret = serialize_response(out_len);
    if (ret != ESP_OK) {
        return ret;
    }
    *out_data = s_cmd_resp_buffer;
    ESP_LOGI(TAG, "Returning response: %s", s_cmd_resp_buffer);
    return ESP_OK;
}
} // namespace


esp_err_t app_rmaker_matter_controller_cmd_resp_enable(void)
{
    if (s_cmd_resp_buffer == nullptr) {
        s_cmd_resp_buffer = (char *)MEM_CALLOC_EXTRAM(1, MAX_CMD_RESP_BUFFER_SIZE);
        if (s_cmd_resp_buffer == NULL) {
            ESP_LOGE(TAG, "Failed to allocate command response buffer");
            return ESP_ERR_NO_MEM;
        }
        ESP_LOGI(TAG, "Allocated %d bytes for command response buffer", MAX_CMD_RESP_BUFFER_SIZE);
    }
    if (s_cmd_resp_mutex == nullptr) {
        s_cmd_resp_mutex = xSemaphoreCreateMutex();
        if (!s_cmd_resp_mutex) {
            ESP_LOGE(TAG, "Failed to allocate command response mutex");
            return ESP_ERR_NO_MEM;
        }
    }
    s_matter_controller_event_group = xEventGroupCreate();
    if (!s_matter_controller_event_group) {
        ESP_LOGE(TAG, "Failed to allocate controller event group");
        return ESP_ERR_NO_MEM;
    }
    esp_err_t ret = ESP_OK;
    
    ESP_GOTO_ON_ERROR(esp_rmaker_cmd_register(MATTER_CONTROL_CMD_TYPE_INVOKE_CMD,
                                            ESP_RMAKER_USER_ROLE_SUPER_ADMIN |
                                            ESP_RMAKER_USER_ROLE_PRIMARY_USER |
                                            ESP_RMAKER_USER_ROLE_SECONDARY_USER,
                                            esp_rmaker_matter_controller_invoke_cmd_handler,
                                            false, nullptr),
                      clear, TAG, "Failed to register invoke command handler");

    ESP_GOTO_ON_ERROR(esp_rmaker_cmd_register(MATTER_CONTROL_CMD_TYPE_WRITE_ATTR,
                                            ESP_RMAKER_USER_ROLE_SUPER_ADMIN |
                                            ESP_RMAKER_USER_ROLE_PRIMARY_USER |
                                            ESP_RMAKER_USER_ROLE_SECONDARY_USER,
                                            esp_rmaker_matter_controller_write_attr_handler,
                                            false, nullptr),
                      clear, TAG, "Failed to register write attribute handler");

    ESP_GOTO_ON_ERROR(esp_rmaker_cmd_register(MATTER_CONTROL_CMD_TYPE_READ,
                                            ESP_RMAKER_USER_ROLE_SUPER_ADMIN |
                                            ESP_RMAKER_USER_ROLE_PRIMARY_USER |
                                            ESP_RMAKER_USER_ROLE_SECONDARY_USER,
                                            esp_rmaker_matter_controller_read_handler,
                                            false, nullptr),
                      clear, TAG, "Failed to register read handler");
clear:
    if (ret != ESP_OK) {
        if (s_cmd_resp_buffer) {
            free(s_cmd_resp_buffer);
            s_cmd_resp_buffer = nullptr;
        }
        if (s_matter_controller_event_group) {
            vEventGroupDelete(s_matter_controller_event_group);
            s_matter_controller_event_group = NULL;
        }
        if (s_cmd_resp_mutex) {
            vSemaphoreDelete(s_cmd_resp_mutex);
            s_cmd_resp_mutex = nullptr;
        }
    }
    return ret;
}

void app_rmaker_matter_controller_decode_tlv_to_string(chip::TLV::TLVReader *data, char *buf, size_t buf_size)
{
    if (!data || !buf || buf_size == 0) {
        if (buf && buf_size > 0) {
            buf[0] = '\0';
        }
        return;
    }
    chip::TLV::TLVReader copy;
    copy.Init(*data);
    size_t pos = 0;
    decode_tlv_reader_to_string(copy, buf, buf_size, pos, 0);
    if (pos < buf_size) {
        buf[pos] = '\0';
    } else if (buf_size > 0) {
        buf[buf_size - 1] = '\0';
    }
}
