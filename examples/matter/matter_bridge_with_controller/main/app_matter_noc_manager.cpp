/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <esp_check.h>
#include <esp_log.h>
#include <esp_matter_core.h>
#include <esp_matter_controller_utils.h>

#include <app/server/Server.h>
#include <app/server/Dnssd.h>
#include <credentials/CHIPCert.h>
#include <credentials/FabricTable.h>
#include <crypto/CHIPCryptoPAL.h>
#include <lib/support/ScopedBuffer.h>

#include <app_matter_noc_manager.h>
#include <controller_rest_apis.h>

using chip::ByteSpan;
using chip::MutableByteSpan;
using chip::Platform::ScopedMemoryBufferWithSize;

static constexpr const char *TAG = "matter_noc";

/* Resolve the current local FabricIndex/NodeId for the selected Matter fabric id. */
static esp_err_t get_commissioned_fabric(matter_controller_handle_t *handle, chip::FabricIndex &fabric_index,
                                         chip::NodeId &node_id)
{
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_INVALID_ARG, TAG, "Invalid handle");
    ESP_RETURN_ON_FALSE(handle->matter_fabric_id != 0, ESP_ERR_INVALID_STATE, TAG,
                        "Matter fabric id must be set before selecting commissioned fabric");

    auto &fabric_table = chip::Server::GetInstance().GetFabricTable();
    const chip::FabricInfo *matched_fabric = nullptr;
    for (auto it = fabric_table.begin(); it != fabric_table.end(); ++it) {
        if (!it->IsInitialized()) {
            continue;
        }
        if (it->GetFabricId() == handle->matter_fabric_id) {
            matched_fabric = &(*it);
            break;
        }
    }

    ESP_RETURN_ON_FALSE(matched_fabric, ESP_ERR_NOT_FOUND, TAG, "No local fabric matches RainMaker fabric id 0x%016llX",
                        static_cast<unsigned long long>(handle->matter_fabric_id));

    fabric_index = matched_fabric->GetFabricIndex();
    node_id = matched_fabric->GetNodeId();
    handle->matter_node_id = static_cast<uint64_t>(node_id);
    return ESP_OK;
}

/* Sync/recovery path. */
esp_err_t app_matter_noc_manager_sync_commissioned_node(matter_controller_handle_t *handle)
{
    chip::FabricIndex fabric_index = chip::kUndefinedFabricIndex;
    chip::NodeId node_id = chip::kUndefinedNodeId;
    ESP_RETURN_ON_ERROR(get_commissioned_fabric(handle, fabric_index, node_id), TAG, "Commissioned fabric not ready");

    esp_matter::controller::set_fabric_index(fabric_index);
    ESP_LOGI(TAG, "Found commissioned node id 0x%016llX on fabric %u",
             static_cast<unsigned long long>(handle->matter_node_id), fabric_index);
    return ESP_OK;
}

/* Install/update path. */
esp_err_t app_matter_noc_manager_install(matter_controller_handle_t *handle, bool force_install)
{
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_INVALID_ARG, TAG, "Invalid handle");

    chip::FabricIndex fabric_index = chip::kUndefinedFabricIndex;
    chip::NodeId node_id = chip::kUndefinedNodeId;
    ESP_RETURN_ON_ERROR(get_commissioned_fabric(handle, fabric_index, node_id), TAG, "Commissioned fabric not ready");

    if (handle->matter_noc_installed && !force_install) {
        ESP_LOGI(TAG, "Controller NOC already installed");
        return ESP_OK;
    }
    ESP_RETURN_ON_FALSE(handle->base_url && handle->access_token && handle->rmaker_group_id, ESP_ERR_INVALID_STATE, TAG,
                        "Controller service is not ready");

    auto &fabric_table = chip::Server::GetInstance().GetFabricTable();

    /* Allocate working buffers for the RCAC, issued NOC, and Matter-format cert. */
    ScopedMemoryBufferWithSize<unsigned char> rcac_der;
    ScopedMemoryBufferWithSize<unsigned char> noc_der;
    ScopedMemoryBufferWithSize<unsigned char> noc_matter_cert;
    rcac_der.Calloc(chip::Credentials::kMaxDERCertLength);
    noc_der.Calloc(chip::Credentials::kMaxDERCertLength);
    noc_matter_cert.Calloc(chip::Credentials::kMaxCHIPCertLength);
    ESP_RETURN_ON_FALSE(rcac_der.Get() && noc_der.Get() && noc_matter_cert.Get(), ESP_ERR_NO_MEM, TAG, "Alloc failed");

    size_t rcac_der_len = rcac_der.AllocatedSize();
    size_t noc_der_len = noc_der.AllocatedSize();
    uint8_t csr_der_buf[chip::Crypto::kMIN_CSR_Buffer_Size];
    MutableByteSpan csr_span(csr_der_buf);
    MutableByteSpan noc_span(noc_matter_cert.Get(), noc_matter_cert.AllocatedSize());

    /* Generate the CSR on the selected fabric and fetch the RainMaker-issued controller NOC. */
    ESP_RETURN_ON_FALSE(fabric_table.AllocatePendingOperationalKey(chip::MakeOptional(fabric_index), csr_span) == CHIP_NO_ERROR,
                        ESP_FAIL, TAG, "Failed to allocate pending key");
    ESP_RETURN_ON_ERROR(fetch_fabric_rcac_der(handle->base_url, handle->access_token, handle->rmaker_group_id,
                                              rcac_der.Get(), &rcac_der_len),
                        TAG, "Failed to fetch RCAC");
    ESP_LOGI(TAG, "Fetched RCAC DER (%u bytes)", static_cast<unsigned>(rcac_der_len));

    uint64_t node_id_u64 = static_cast<uint64_t>(node_id);
    ESP_RETURN_ON_ERROR(issue_noc_with_csr(handle->base_url, handle->access_token, CSR_TYPE_CONTROLLER, csr_span.data(),
                                           csr_span.size(), handle->rmaker_group_id, &node_id_u64, noc_der.Get(),
                                           &noc_der_len),
                        TAG, "Failed to issue controller NOC");
    ESP_LOGI(TAG, "Issued controller NOC for controller node 0x%016llX", static_cast<unsigned long long>(node_id_u64));

    /* Treat the issued certificate subject DN as the final source of the controller node id. */
    chip::Credentials::ChipDN issued_dn;
    ESP_RETURN_ON_FALSE(chip::Credentials::ExtractSubjectDNFromX509Cert(ByteSpan{noc_der.Get(), noc_der_len}, issued_dn) == CHIP_NO_ERROR,
                        ESP_FAIL, TAG, "Failed to extract subject DN from issued controller NOC");
    chip::NodeId issued_node_id = chip::kUndefinedNodeId;
    ESP_RETURN_ON_FALSE(issued_dn.GetCertChipId(issued_node_id) == CHIP_NO_ERROR, ESP_FAIL, TAG,
                        "Failed to extract node id from issued controller NOC subject DN");
    node_id_u64 = static_cast<uint64_t>(issued_node_id);
    ESP_LOGI(TAG, "Using node id 0x%016llX from issued controller NOC", static_cast<unsigned long long>(node_id_u64));

    /* Convert and commit the newly issued controller NOC onto the selected fabric. */
    ESP_RETURN_ON_FALSE(chip::Credentials::ConvertX509CertToChipCert(ByteSpan{noc_der.Get(), noc_der_len}, noc_span) == CHIP_NO_ERROR,
                        ESP_FAIL, TAG, "Failed to convert controller NOC");
    {
        esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
        ESP_RETURN_ON_FALSE(fabric_table.UpdatePendingFabricWithOperationalKeystore(fabric_index, noc_span, ByteSpan{}) == CHIP_NO_ERROR,
                            ESP_FAIL, TAG, "Failed to update pending fabric");
        ESP_RETURN_ON_FALSE(fabric_table.CommitPendingFabricData() == CHIP_NO_ERROR, ESP_FAIL, TAG,
                            "Failed to commit pending fabric data");
        chip::app::DnssdServer::Instance().StartServer();
        /* Switch the controller back to the fabric that now owns the installed NOC. */
        esp_matter::controller::set_fabric_index(fabric_index);
    }

    /* Publish the final installed state back into the controller handle. */
    handle->matter_node_id = node_id_u64;
    handle->matter_noc_installed = true;
    ESP_LOGI(TAG, "Installed controller NOC for node 0x%016llX", static_cast<unsigned long long>(node_id_u64));
    return ESP_OK;
}
