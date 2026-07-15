/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <string.h>

#include <cJSON.h>
#include <esp_heap_caps.h>

#include "agent_ng_internal.h"

static esp_err_t serialize(cJSON *root, char **out_json)
{
    if (root == NULL || out_json == NULL) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    char *internal_json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (internal_json == NULL) {
        return ESP_ERR_NO_MEM;
    }
    *out_json = agent_ng_preferred_strdup(internal_json);
    cJSON_free(internal_json);
    return *out_json == NULL ? ESP_ERR_NO_MEM : ESP_OK;
}

static esp_err_t create_audio_message(const char *type, char **out_json)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }
    cJSON *metadata = cJSON_AddObjectToObject(root, "metadata");
    cJSON *content = cJSON_AddObjectToObject(root, "content");
    if (metadata == NULL || content == NULL ||
            cJSON_AddStringToObject(root, "type", type) == NULL ||
            cJSON_AddStringToObject(root, "content_type", "json") == NULL ||
            cJSON_AddStringToObject(metadata, "role", "user") == NULL) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    return serialize(root, out_json);
}

esp_err_t agent_ng_protocol_handshake(char **out_json)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }

    cJSON *content = cJSON_AddObjectToObject(root, "content");
    cJSON *audio = content == NULL ? NULL :
                   cJSON_AddObjectToObject(content, "audioConfiguration");
    cJSON *input = audio == NULL ? NULL : cJSON_AddObjectToObject(audio, "input");
    cJSON *output = audio == NULL ? NULL : cJSON_AddObjectToObject(audio, "output");
    if (content == NULL || audio == NULL || input == NULL || output == NULL ||
            cJSON_AddStringToObject(root, "type", "handshake") == NULL ||
            cJSON_AddStringToObject(root, "content_type", "json") == NULL ||
            cJSON_AddStringToObject(content, "conversationType", "audio") == NULL ||
            cJSON_AddStringToObject(input, "format", "audio/opus") == NULL ||
            cJSON_AddNumberToObject(input, "sampleRate", 8000) == NULL ||
            cJSON_AddNumberToObject(input, "frameDurationMs", 60) == NULL ||
            cJSON_AddStringToObject(output, "format", "audio/opus") == NULL ||
            cJSON_AddNumberToObject(output, "sampleRate", 16000) == NULL ||
            cJSON_AddNumberToObject(output, "frameDurationMs", 60) == NULL) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    return serialize(root, out_json);
}

esp_err_t agent_ng_protocol_audio_start(char **out_json)
{
    return create_audio_message("audio_stream_start", out_json);
}

esp_err_t agent_ng_protocol_audio_end(char **out_json)
{
    return create_audio_message("audio_stream_end", out_json);
}

esp_err_t agent_ng_protocol_tool_result(const char *request_id, esp_err_t result,
                                         cJSON *result_json, char **out_json)
{
    if (request_id == NULL) {
        cJSON_Delete(result_json);
        return ESP_ERR_INVALID_ARG;
    }

    char *result_text = result_json == NULL ? NULL : cJSON_PrintUnformatted(result_json);
    cJSON_Delete(result_json);
    if (result_json != NULL && result_text == NULL) {
        return ESP_ERR_NO_MEM;
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        cJSON_free(result_text);
        return ESP_ERR_NO_MEM;
    }
    cJSON *content_type = cJSON_AddObjectToObject(root, "content_type");
    cJSON *content = cJSON_AddObjectToObject(root, "content");
    cJSON *reply = content == NULL ? NULL : cJSON_AddObjectToObject(content, "result");
    if (content_type == NULL || content == NULL || reply == NULL ||
            cJSON_AddStringToObject(root, "type", "tool_response") == NULL ||
            cJSON_AddStringToObject(content_type, "type", "json") == NULL ||
            cJSON_AddStringToObject(content, "request_id", request_id) == NULL ||
            cJSON_AddStringToObject(reply, "status", result == ESP_OK ? "success" : "error") == NULL) {
        cJSON_free(result_text);
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    if (result_text != NULL) {
        if (cJSON_AddStringToObject(reply, "result", result_text) == NULL) {
            cJSON_free(result_text);
            cJSON_Delete(root);
            return ESP_ERR_NO_MEM;
        }
    }
    cJSON_free(result_text);
    return serialize(root, out_json);
}

static esp_err_t parse_tool_request(cJSON *content, agent_ng_intent_t *intent)
{
    const char *request_id = cJSON_GetStringValue(cJSON_GetObjectItem(content, "request_id"));
    const char *tool_name = cJSON_GetStringValue(cJSON_GetObjectItem(content, "tool_name"));
    cJSON *input = cJSON_GetObjectItem(content, "input");
    if (request_id == NULL || tool_name == NULL || input == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    intent->request_id = agent_ng_preferred_strdup(request_id);
    intent->tool_name = agent_ng_preferred_strdup(tool_name);
    intent->arguments = cJSON_DetachItemFromObjectCaseSensitive(content, "input");
    if (intent->request_id == NULL || intent->tool_name == NULL || intent->arguments == NULL) {
        agent_ng_intent_free(intent);
        return ESP_ERR_NO_MEM;
    }
    intent->id = AGENT_NG_INT_TOOL;
    return ESP_OK;
}

static esp_err_t parse_transcript(const char *type, cJSON *content, cJSON *metadata,
                                  agent_ng_intent_t *intent)
{
    const char *text = cJSON_GetStringValue(content);
    if (text == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    intent->text = agent_ng_preferred_strdup(text);
    if (intent->text == NULL) {
        return ESP_ERR_NO_MEM;
    }
    intent->id = AGENT_NG_INT_TEXT;
    intent->role = strcmp(type, "assistant") == 0 ? AGENT_NG_ROLE_ASSISTANT :
                                                    AGENT_NG_ROLE_USER;
    const char *stage = metadata == NULL ? NULL :
                        cJSON_GetStringValue(cJSON_GetObjectItem(metadata, "generation_stage"));
    intent->final = stage == NULL || strcmp(stage, "final") == 0;
    return ESP_OK;
}

esp_err_t agent_ng_protocol_parse(const char *json, agent_ng_intent_t *out_intent)
{
    if (json == NULL || out_intent == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out_intent, 0, sizeof(*out_intent));

    cJSON *root = cJSON_Parse(json);
    if (root == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(root, "type"));
    cJSON *content = cJSON_GetObjectItem(root, "content");
    cJSON *metadata = cJSON_GetObjectItem(root, "metadata");
    esp_err_t err = ESP_OK;

    if (type == NULL) {
        err = ESP_ERR_INVALID_RESPONSE;
    } else if (strcmp(type, "handshake_ack") == 0) {
        const char *conversation_id = content == NULL ? NULL :
                                      cJSON_GetStringValue(cJSON_GetObjectItem(content,
                                                                               "conversationId"));
        err = conversation_id == NULL ? ESP_ERR_INVALID_RESPONSE : ESP_OK;
        out_intent->id = AGENT_NG_INT_HANDSHAKE_ACK;
    } else if (strcmp(type, "audio_stream_start") == 0) {
        out_intent->id = AGENT_NG_INT_SPEECH_START;
    } else if (strcmp(type, "audio_stream_end") == 0) {
        out_intent->id = AGENT_NG_INT_SPEECH_END;
    } else if (strcmp(type, "tool_request") == 0) {
        err = parse_tool_request(content, out_intent);
    } else if (strcmp(type, "user") == 0 || strcmp(type, "assistant") == 0) {
        err = parse_transcript(type, content, metadata, out_intent);
    } else {
        err = ESP_ERR_NOT_SUPPORTED;
    }

    cJSON_Delete(root);
    if (err != ESP_OK) {
        agent_ng_intent_free(out_intent);
    }
    return err;
}
