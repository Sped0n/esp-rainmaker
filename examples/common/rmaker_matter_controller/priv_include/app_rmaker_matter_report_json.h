/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cJSON.h>
#include <esp_rmaker_core.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Set the RainMaker MTDevices parameter used for attribute reports
 *
 * @param[in] matter_devices_param The RainMaker parameter handle for MTDevices
 */
void app_rmaker_matter_report_set_param(esp_rmaker_param_t *matter_devices_param);

/**
 * @brief Update a Matter attribute JSON tree from a JSON string value
 *
 * @param[in,out] root The per-node Matter attribute JSON root
 * @param[in] endpoint_id The Matter endpoint ID
 * @param[in] cluster_id The Matter cluster ID
 * @param[in] attribute_id The Matter attribute ID
 * @param[in] value The attribute value encoded as JSON text
 *
 * @return true if the tree was updated
 * @return false if the input was invalid or allocation failed
 */
bool app_rmaker_matter_report_json_update_tree(cJSON *root, uint16_t endpoint_id, uint32_t cluster_id,
                                              uint32_t attribute_id, const char *value);

/**
 * @brief Update a Matter attribute JSON tree from a cJSON value
 *
 * @param[in,out] root The per-node Matter attribute JSON root
 * @param[in] endpoint_id The Matter endpoint ID
 * @param[in] cluster_id The Matter cluster ID
 * @param[in] attribute_id The Matter attribute ID
 * @param[in] value The read-only cJSON attribute value
 *
 * @return true if the tree was updated
 * @return false if the input was invalid or allocation failed
 */
bool app_rmaker_matter_report_json_update_tree_item(cJSON *root, uint16_t endpoint_id, uint32_t cluster_id,
                                                  uint32_t attribute_id, const cJSON *value);

/**
 * @brief Publish a Matter devices delta to RainMaker
 *
 * @param[in] matter_devices_obj The MTDevices delta object; ownership is consumed by this function
 */
void app_rmaker_matter_report_publish_matter_devices_delta(cJSON *matter_devices_obj);

/**
 * @brief Publish a node online-state delta to RainMaker and dispatch the online-state event
 *
 * @param[in] node_id The Matter node ID
 * @param[in] rainmaker_node_id The RainMaker node ID for the Matter node
 * @param[in] online The node online state to publish
 */
void app_rmaker_matter_report_publish_online(uint64_t node_id, const char *rainmaker_node_id, bool online);

/**
 * @brief Check whether a pending attribute delta tree is empty
 *
 * @param[in] pending_root The pending delta root
 *
 * @return true if the tree is NULL or has no pending nodes
 * @return false if the tree contains pending data
 */
bool app_rmaker_matter_report_json_pending_is_empty(cJSON *pending_root);

/**
 * @brief Merge an attribute JSON string into a pending MTDevices delta
 *
 * @param[in,out] pending_root The pending delta root pointer
 * @param[in] node_id The Matter node ID
 * @param[in] rainmaker_node_id The RainMaker node ID for the Matter node
 * @param[in] endpoint_id The Matter endpoint ID
 * @param[in] cluster_id The Matter cluster ID
 * @param[in] attribute_id The Matter attribute ID
 * @param[in] value_json The attribute value encoded as JSON text
 *
 * @return true if the pending delta was updated
 * @return false if the input was invalid or allocation failed
 */
bool app_rmaker_matter_report_json_merge_pending_attr(cJSON **pending_root, uint64_t node_id,
                                                    const char *rainmaker_node_id, uint16_t endpoint_id,
                                                    uint32_t cluster_id, uint32_t attribute_id,
                                                    const char *value_json);

/**
 * @brief Merge a cJSON attribute value into a pending MTDevices delta
 *
 * @param[in,out] pending_root The pending delta root pointer
 * @param[in] node_id The Matter node ID
 * @param[in] rainmaker_node_id The RainMaker node ID for the Matter node
 * @param[in] endpoint_id The Matter endpoint ID
 * @param[in] cluster_id The Matter cluster ID
 * @param[in] attribute_id The Matter attribute ID
 * @param[in] value The read-only cJSON attribute value
 *
 * @return true if the pending delta was updated
 * @return false if the input was invalid or allocation failed
 */
bool app_rmaker_matter_report_json_merge_pending_attr_item(cJSON **pending_root, uint64_t node_id,
                                                         const char *rainmaker_node_id, uint16_t endpoint_id,
                                                         uint32_t cluster_id, uint32_t attribute_id,
                                                         const cJSON *value);

/**
 * @brief Detach and return all pending MTDevices deltas
 *
 * @param[in,out] pending_root The pending delta root pointer
 *
 * @return Detached pending delta object on success
 * @return NULL if no pending delta is available
 */
cJSON *app_rmaker_matter_report_json_detach_pending_all(cJSON **pending_root);

/**
 * @brief Detach and return the pending MTDevices delta for one Matter node
 *
 * @param[in,out] pending_root The pending delta root pointer
 * @param[in] node_id The Matter node ID
 *
 * @return Detached pending node delta object on success
 * @return NULL if no pending delta is available for the node
 */
cJSON *app_rmaker_matter_report_json_detach_pending_node(cJSON **pending_root, uint64_t node_id);

/**
 * @brief Create a canonicalized duplicate of a cJSON value
 *
 * @param[in] item The read-only cJSON value to canonicalize
 *
 * @return Canonicalized cJSON value on success
 * @return NULL if allocation failed or item is invalid
 */
cJSON *app_rmaker_matter_report_json_canonicalize(const cJSON *item);

#ifdef __cplusplus
}
#endif
