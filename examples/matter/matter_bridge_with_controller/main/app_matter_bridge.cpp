/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <esp_log.h>
#include <esp_matter_console_bridge.h>

#include <app_matter_bridge.h>

using namespace esp_matter;
using namespace esp_matter::endpoint;

/*
 * Bridge demo flow:
 * 1. Initialize the native backend transport and callback registration.
 * 2. Create the bridge aggregator endpoint that will own bridged endpoints.
 * 3. Discover native devices and map each one to a Matter device type.
 * 4. Create or resume bridged endpoints under the aggregator endpoint.
 * 5. Forward Matter control writes to the native backend.
 * 6. Update Matter attributes when the native backend reports state changes.
 */

static const char *TAG = "matter_bridge";
static uint16_t s_bridge_aggregator_endpoint_id = 0;

esp_err_t app_matter_bridge_backend_init(void)
{
    return ESP_OK;
}

esp_err_t app_matter_bridge_create(void)
{
    node_t *node = node::get();
    if (!node) {
        ESP_LOGE(TAG, "Matter node not found");
        return ESP_FAIL;
    }

    aggregator::config_t aggregator_config;
    endpoint_t *endpoint = aggregator::create(node, &aggregator_config, ENDPOINT_FLAG_NONE, NULL);
    if (!endpoint) {
        ESP_LOGE(TAG, "Failed to create bridge aggregator endpoint");
        return ESP_FAIL;
    }

    /*
     * NOTE: in this demo, s_bridge_aggregator_endpoint_id should be 1
     *       and bridged device should have endpoint id > 1
     */
    s_bridge_aggregator_endpoint_id = endpoint::get_id(endpoint);
    ESP_LOGI(TAG, "Aggregator endpoint id: %d", s_bridge_aggregator_endpoint_id);
    return ESP_OK;
}

esp_err_t app_matter_bridge_init_demo_hooks(void)
{
    if (s_bridge_aggregator_endpoint_id == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * Step 3/6 and preparation of Step 4/6.
     *
     * In a real bridge implementation, this is where you would:
     * - discover native devices from the backend
     * - map them to Matter device types
     * - create or resume bridged endpoints under `s_bridge_aggregator_endpoint_id`
     */
    return ESP_OK;
}

esp_err_t app_matter_bridge_attribute_update(uint16_t endpoint_id, uint32_t cluster_id, uint32_t attribute_id,
                                             esp_matter_attr_val_t *val)
{
    (void)endpoint_id;
    (void)cluster_id;
    (void)attribute_id;
    (void)val;
    /*
     * Step 5/6: Forward Matter control writes to the native backend.
     * Please refer to:
     * - https://github.com/espressif/esp-matter/blob/da3910cec5a2277681d8c1edaf73bc82dd759df1/examples/bridge_apps/blemesh_bridge/main/blemesh_bridge.cpp#L71-L87
     *
     * WARN: If this hook matches endpoints too broadly, it can intercept PRE_UPDATE writes
     * that were meant for normal controller/device behavior.
     */
    return ESP_OK;
}

/*
 * Step 6/6: Update Matter attributes when the native backend reports state changes.
 *
 * A typical native-to-Matter flow looks like:
 *
 *   uint16_t endpoint_id = app_bridge_get_matter_endpointid_by_blemesh_addr(blemesh_addr);
 *   attribute_t *attribute = attribute::get(
 *       endpoint_id,
 *       OnOff::Id,
 *       OnOff::Attributes::OnOff::Id
 *   );
 *   esp_matter_attr_val_t val = esp_matter_bool(new_onoff_state);
 *   attribute::update(endpoint_id, OnOff::Id, OnOff::Attributes::OnOff::Id, &val);
 */

void app_matter_bridge_on_commissioning_complete(void)
{
    (void)s_bridge_aggregator_endpoint_id;
    /*
     * Step 4/6.
     *
     * Commissioning completion is the lifecycle point where a real bridge may start
     * discovery/sync and then populate bridged endpoints.
     */
}
