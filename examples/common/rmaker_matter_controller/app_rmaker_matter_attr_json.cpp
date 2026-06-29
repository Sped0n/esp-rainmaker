/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <app_rmaker_matter_attr_json.h>

#include <esp_check.h>
#include <esp_log.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#define TAG "rmaker_matter_attr_json"

#define MATTER_DEVICES_SCHEMA_REVISION 0U

static esp_rmaker_param_t *s_attributes_param = NULL;
static uint32_t s_last_report_hash = 0;
static bool s_have_last_report_hash = false;

static uint32_t hash_string(const char *str)
{
    uint32_t h = 5381;
    if (!str) {
        return 0;
    }
    while (*str) {
        h = ((h << 5) + h) + (uint8_t)*str++;
    }
    return h;
}

static int cmp_cjson_object_entry(const void *a, const void *b)
{
    const cJSON *const *ja = (const cJSON *const *)a;
    const cJSON *const *jb = (const cJSON *const *)b;
    const char *sa = (*ja)->string;
    const char *sb = (*jb)->string;
    if (!sa && !sb) {
        return 0;
    }
    if (!sa) {
        return -1;
    }
    if (!sb) {
        return 1;
    }
    return strcmp(sa, sb);
}

static cJSON *cjson_canonicalize(const cJSON *item)
{
    if (!item) {
        return NULL;
    }
    if (cJSON_IsObject(item)) {
        int count = 0;
        for (cJSON *c = item->child; c; c = c->next) {
            count++;
        }
        if (count == 0) {
            return cJSON_CreateObject();
        }
        cJSON **entries = (cJSON **)calloc((size_t)count, sizeof(cJSON *));
        if (!entries) {
            return NULL;
        }
        int i = 0;
        for (cJSON *c = item->child; c; c = c->next) {
            entries[i++] = c;
        }
        qsort(entries, (size_t)count, sizeof(cJSON *), cmp_cjson_object_entry);
        cJSON *out = cJSON_CreateObject();
        if (!out) {
            free(entries);
            return NULL;
        }
        for (i = 0; i < count; i++) {
            cJSON *child = cjson_canonicalize(entries[i]);
            if (!child) {
                cJSON_Delete(out);
                free(entries);
                return NULL;
            }
            cJSON_AddItemToObject(out, entries[i]->string, child);
        }
        free(entries);
        return out;
    }
    if (cJSON_IsArray(item)) {
        cJSON *out = cJSON_CreateArray();
        if (!out) {
            return NULL;
        }
        cJSON *c;
        cJSON_ArrayForEach(c, item)
        {
            cJSON *child = cjson_canonicalize(c);
            if (!child) {
                cJSON_Delete(out);
                return NULL;
            }
            cJSON_AddItemToArray(out, child);
        }
        return out;
    }
    return cJSON_Duplicate(item, 1);
}

static bool is_legacy_decimal_key(const char *key)
{
    if (!key || !*key) {
        return false;
    }
    if (key[0] == '0' && (key[1] == 'x' || key[1] == 'X')) {
        return false;
    }
    for (const char *p = key; *p; p++) {
        if (*p < '0' || *p > '9') {
            return false;
        }
    }
    return true;
}

static void strip_legacy_attr_root(cJSON *root)
{
    if (!root || !cJSON_IsObject(root)) {
        return;
    }
    for (cJSON *c = root->child; c != NULL; c = c->next) {
        if (c->string && is_legacy_decimal_key(c->string)) {
            while (root->child) {
                cJSON_Delete(cJSON_DetachItemViaPointer(root, root->child));
            }
            return;
        }
    }
}

static void cjson_hexify_matter_keys(cJSON *item)
{
    if (!item) {
        return;
    }
    if (cJSON_IsObject(item)) {
        cJSON *child = item->child;
        while (child) {
            cJSON *next = child->next;
            const char *key = child->string;
            if (key && key[0] != '\0') {
                bool need_hex = false;
                if (strncmp(key, "0x", 2) != 0 && strncmp(key, "0X", 2) != 0) {
                    char *end = NULL;
                    (void)strtoul(key, &end, 10);
                    if (end && end > key && *end == '\0') {
                        need_hex = true;
                    }
                }
                if (need_hex) {
                    char new_key[32];
                    unsigned long v = strtoul(key, NULL, 10);
                    snprintf(new_key, sizeof(new_key), "0x%lX", v);
                    cJSON *detached = cJSON_DetachItemViaPointer(item, child);
                    cjson_hexify_matter_keys(detached);
                    cJSON_AddItemToObject(item, new_key, detached);
                } else {
                    cjson_hexify_matter_keys(child);
                }
            }
            child = next;
        }
    } else if (cJSON_IsArray(item)) {
        cJSON *el = NULL;
        cJSON_ArrayForEach(el, item)
        {
            cjson_hexify_matter_keys(el);
        }
    }
}

static bool cjson_items_equal_canonical(const cJSON *a, const cJSON *b)
{
    if (!a || !b) {
        return false;
    }
    cJSON *ca = cjson_canonicalize(a);
    cJSON *cb = cjson_canonicalize(b);
    if (!ca || !cb) {
        cJSON_Delete(ca);
        cJSON_Delete(cb);
        return false;
    }
    char *sa = cJSON_PrintUnformatted(ca);
    char *sb = cJSON_PrintUnformatted(cb);
    cJSON_Delete(ca);
    cJSON_Delete(cb);
    bool eq = (sa && sb && strcmp(sa, sb) == 0);
    cJSON_free(sa);
    cJSON_free(sb);
    return eq;
}

void app_rmaker_matter_attr_json_set_param(esp_rmaker_param_t *matter_devices_param)
{
    s_attributes_param = matter_devices_param;
}

bool app_rmaker_matter_attr_json_update_tree(cJSON *root, uint16_t endpoint_id, uint32_t cluster_id,
                                             uint32_t attribute_id, const char *value)
{
    strip_legacy_attr_root(root);

    char ep_key[24];
    char cluster_key[24];
    char attr_key[24];
    snprintf(ep_key, sizeof(ep_key), "0x%X", (unsigned)endpoint_id);
    snprintf(cluster_key, sizeof(cluster_key), "0x%" PRIX32, cluster_id);
    snprintf(attr_key, sizeof(attr_key), "0x%" PRIX32, attribute_id);

    cJSON *ep_obj = cJSON_GetObjectItem(root, ep_key);
    if (!ep_obj) {
        ep_obj = cJSON_CreateObject();
        if (!ep_obj) {
            return false;
        }
        cJSON_AddItemToObject(root, ep_key, ep_obj);
    }
    cJSON *clusters_obj = cJSON_GetObjectItem(ep_obj, "clusters");
    if (!clusters_obj) {
        clusters_obj = cJSON_CreateObject();
        if (!clusters_obj) {
            return false;
        }
        cJSON_AddItemToObject(ep_obj, "clusters", clusters_obj);
    }
    cJSON *servers_obj = cJSON_GetObjectItem(clusters_obj, "servers");
    if (!servers_obj) {
        servers_obj = cJSON_CreateObject();
        if (!servers_obj) {
            return false;
        }
        cJSON_AddItemToObject(clusters_obj, "servers", servers_obj);
    }
    cJSON *cluster_wrap = cJSON_GetObjectItem(servers_obj, cluster_key);
    if (!cluster_wrap) {
        cluster_wrap = cJSON_CreateObject();
        if (!cluster_wrap) {
            return false;
        }
        cJSON_AddItemToObject(servers_obj, cluster_key, cluster_wrap);
    }
    cJSON *attr_obj = cJSON_GetObjectItem(cluster_wrap, "attributes");
    if (!attr_obj) {
        attr_obj = cJSON_CreateObject();
        if (!attr_obj) {
            return false;
        }
        cJSON_AddItemToObject(cluster_wrap, "attributes", attr_obj);
    }

    cJSON *old = cJSON_DetachItemFromObject(attr_obj, attr_key);
    cJSON *new_item = NULL;
    if (value && value[0] != '\0') {
        new_item = cJSON_Parse(value);
    }
    if (new_item) {
        cjson_hexify_matter_keys(new_item);
    } else {
        new_item = cJSON_CreateString(value ? value : "");
    }
    if (!new_item) {
        if (old) {
            cJSON_AddItemToObject(attr_obj, attr_key, old);
        }
        return false;
    }

    bool changed = !old || !cjson_items_equal_canonical(old, new_item);
    if (!changed) {
        cJSON_Delete(new_item);
        cJSON_AddItemToObject(attr_obj, attr_key, old);
        return false;
    }

    cJSON_Delete(old);
    cJSON_AddItemToObject(attr_obj, attr_key, new_item);
    return true;
}

void app_rmaker_matter_attr_json_publish_matter_devices_delta(cJSON *matter_devices_obj)
{
    if (!s_attributes_param) {
        cJSON_Delete(matter_devices_obj);
        return;
    }
    if (!matter_devices_obj) {
        return;
    }
    cJSON_DeleteItemFromObject(matter_devices_obj, "revision");
    if (!cJSON_AddNumberToObject(matter_devices_obj, "revision", MATTER_DEVICES_SCHEMA_REVISION)) {
        cJSON_Delete(matter_devices_obj);
        return;
    }

    cJSON *canonical = cjson_canonicalize(matter_devices_obj);
    cJSON_Delete(matter_devices_obj);
    if (!canonical) {
        return;
    }
    char *payload = cJSON_PrintUnformatted(canonical);
    cJSON_Delete(canonical);
    if (!payload) {
        return;
    }

    uint32_t h = hash_string(payload);
    if (s_have_last_report_hash && h == s_last_report_hash) {
        cJSON_free(payload);
        return;
    }
    s_last_report_hash = h;
    s_have_last_report_hash = true;

    esp_err_t err = esp_rmaker_param_update_and_report(s_attributes_param, esp_rmaker_obj(payload));
    cJSON_free(payload);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to report matter attributes param: %s", esp_err_to_name(err));
    }
}

void app_rmaker_matter_attr_json_publish_online_delta(uint64_t node_id, const char *rainmaker_node_id, bool online)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *wrapper = cJSON_CreateObject();
    if (!root || !wrapper) {
        cJSON_Delete(root);
        cJSON_Delete(wrapper);
        return;
    }
    char node_key[32];
    snprintf(node_key, sizeof(node_key), "%016llx", (unsigned long long)node_id);
    cJSON_AddItemToObject(wrapper, "rainmaker_node_id", cJSON_CreateString(rainmaker_node_id ? rainmaker_node_id : ""));
    cJSON_AddItemToObject(wrapper, "online", cJSON_CreateBool(online));
    cJSON_AddItemToObject(root, node_key, wrapper);
    app_rmaker_matter_attr_json_publish_matter_devices_delta(root);
}
