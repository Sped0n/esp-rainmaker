/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <app_rmaker_matter_report_json.h>

#if CONFIG_RMAKER_MTCTL_MEMORY_ALLOCATION_PREFER_SPIRAM
#include <esp_heap_caps.h>
#endif
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

static int cmp_cjson_object_entry(const void *a, const void *b)
{
    const cJSON *const *ja = (const cJSON * const *)a;
    const cJSON *const *jb = (const cJSON * const *)b;
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

cJSON *app_rmaker_matter_report_json_canonicalize(const cJSON *item)
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
#if CONFIG_RMAKER_MTCTL_MEMORY_ALLOCATION_PREFER_SPIRAM
        cJSON **entries = (cJSON **)heap_caps_calloc_prefer((size_t)count, sizeof(cJSON *), 2,
                                                            MALLOC_CAP_DEFAULT | MALLOC_CAP_SPIRAM,
                                                            MALLOC_CAP_DEFAULT | MALLOC_CAP_INTERNAL);
#else
        cJSON **entries = (cJSON **)calloc((size_t)count, sizeof(cJSON *));
#endif
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
            cJSON *child = app_rmaker_matter_report_json_canonicalize(entries[i]);
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
        cJSON_ArrayForEach(c, item) {
            cJSON *child = app_rmaker_matter_report_json_canonicalize(c);
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
        cJSON_ArrayForEach(el, item) {
            cjson_hexify_matter_keys(el);
        }
    }
}

static bool cjson_items_equal_canonical(const cJSON *a, const cJSON *b)
{
    if (!a || !b) {
        return false;
    }
    cJSON *ca = app_rmaker_matter_report_json_canonicalize(a);
    cJSON *cb = app_rmaker_matter_report_json_canonicalize(b);
    if (!ca || !cb) {
        cJSON_Delete(ca);
        cJSON_Delete(cb);
        return false;
    }
    bool eq = cJSON_Compare(ca, cb, true);
    cJSON_Delete(ca);
    cJSON_Delete(cb);
    return eq;
}

bool app_rmaker_matter_report_json_update_tree_item(cJSON *root, uint16_t endpoint_id, uint32_t cluster_id,
                                                  uint32_t attribute_id, const cJSON *value)
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

    cJSON *old_item = cJSON_DetachItemFromObject(attr_obj, attr_key);
    cJSON *new_item = value ? cJSON_Duplicate(value, true) : cJSON_CreateNull();
    if (!new_item) {
        if (old_item) {
            cJSON_AddItemToObject(attr_obj, attr_key, old_item);
        }
        return false;
    }
    cjson_hexify_matter_keys(new_item);

    bool changed = !old_item || !cjson_items_equal_canonical(old_item, new_item);
    if (!changed) {
        cJSON_Delete(new_item);
        cJSON_AddItemToObject(attr_obj, attr_key, old_item);
        return false;
    }

    cJSON_Delete(old_item);
    cJSON_AddItemToObject(attr_obj, attr_key, new_item);
    return true;
}

bool app_rmaker_matter_report_json_update_tree(cJSON *root, uint16_t endpoint_id, uint32_t cluster_id,
                                              uint32_t attribute_id, const char *value)
{
    cJSON *value_item = NULL;
    if (value && value[0] != '\0') {
        value_item = cJSON_Parse(value);
    }
    if (!value_item) {
        value_item = cJSON_CreateString(value ? value : "");
    }
    if (!value_item) {
        return false;
    }
    bool changed = app_rmaker_matter_report_json_update_tree_item(root, endpoint_id, cluster_id, attribute_id,
                                                                value_item);
    cJSON_Delete(value_item);
    return changed;
}

bool app_rmaker_matter_report_json_pending_is_empty(cJSON *pending_root)
{
    return !pending_root || !pending_root->child;
}

bool app_rmaker_matter_report_json_merge_pending_attr(cJSON **pending_root, uint64_t node_id,
                                                    const char *rainmaker_node_id, uint16_t endpoint_id,
                                                    uint32_t cluster_id, uint32_t attribute_id,
                                                    const char *value_json)
{
    cJSON *value = NULL;
    if (value_json && value_json[0] != '\0') {
        value = cJSON_Parse(value_json);
    }
    if (!value) {
        value = cJSON_CreateString(value_json ? value_json : "");
    }
    if (!value) {
        return false;
    }
    bool merged = app_rmaker_matter_report_json_merge_pending_attr_item(pending_root, node_id, rainmaker_node_id,
                                                                      endpoint_id, cluster_id, attribute_id, value);
    cJSON_Delete(value);
    return merged;
}

bool app_rmaker_matter_report_json_merge_pending_attr_item(cJSON **pending_root, uint64_t node_id,
                                                         const char *rainmaker_node_id, uint16_t endpoint_id,
                                                         uint32_t cluster_id, uint32_t attribute_id,
                                                         const cJSON *value)
{
    if (!pending_root) {
        return false;
    }
    if (!*pending_root) {
        *pending_root = cJSON_CreateObject();
        if (!*pending_root) {
            return false;
        }
    }

    char node_key[32];
    snprintf(node_key, sizeof(node_key), "%016llx", (unsigned long long)node_id);
    cJSON *wrapper = cJSON_GetObjectItem(*pending_root, node_key);
    if (!wrapper) {
        wrapper = cJSON_CreateObject();
        if (!wrapper) {
            return false;
        }
        cJSON_AddItemToObject(*pending_root, node_key, wrapper);
        cJSON_AddItemToObject(wrapper, "rainmaker_node_id", cJSON_CreateString(rainmaker_node_id ? rainmaker_node_id : ""));
    }
    cJSON *endpoints = cJSON_GetObjectItem(wrapper, "endpoints");
    if (!endpoints) {
        endpoints = cJSON_CreateObject();
        if (!endpoints) {
            return false;
        }
        cJSON_AddItemToObject(wrapper, "endpoints", endpoints);
    }
    return app_rmaker_matter_report_json_update_tree_item(endpoints, endpoint_id, cluster_id, attribute_id, value);
}

cJSON *app_rmaker_matter_report_json_detach_pending_all(cJSON **pending_root)
{
    if (!pending_root || app_rmaker_matter_report_json_pending_is_empty(*pending_root)) {
        return NULL;
    }
    cJSON *payload = *pending_root;
    *pending_root = NULL;
    return payload;
}

cJSON *app_rmaker_matter_report_json_detach_pending_node(cJSON **pending_root, uint64_t node_id)
{
    if (!pending_root || app_rmaker_matter_report_json_pending_is_empty(*pending_root)) {
        return NULL;
    }
    char node_key[32];
    snprintf(node_key, sizeof(node_key), "%016llx", (unsigned long long)node_id);
    cJSON *node = cJSON_DetachItemFromObject(*pending_root, node_key);
    if (!node) {
        return NULL;
    }
    cJSON *payload = cJSON_CreateObject();
    if (!payload) {
        cJSON_Delete(node);
        return NULL;
    }
    cJSON_AddItemToObject(payload, node_key, node);
    if (app_rmaker_matter_report_json_pending_is_empty(*pending_root)) {
        cJSON_Delete(*pending_root);
        *pending_root = NULL;
    }
    return payload;
}
