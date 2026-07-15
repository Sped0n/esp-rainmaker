/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <app_controller_op_creds_issuer.h>

#include <app_rmaker_matter_controller.h>
#include <esp_check.h>
#include <esp_err.h>
#include <esp_log.h>
#include <esp_matter_core.h>
#include <esp_matter_controller_client.h>
#include <esp_matter_controller_credentials_issuer.h>

#include <app/server/Dnssd.h>
#include <controller/CHIPDeviceControllerFactory.h>
#include <credentials/CHIPCert.h>
#include <credentials/FabricTable.h>
#include <crypto/CHIPCryptoPAL.h>
#include <lib/core/DataModelTypes.h>
#include <lib/core/NodeId.h>
#include <lib/support/ScopedMemoryBuffer.h>
#include <lib/support/Span.h>

#define TAG "MatterController"

using chip::ByteSpan;
using chip::Callback::Callback;
using chip::Controller::OnNOCChainGeneration;
using chip::Platform::ScopedMemoryBufferWithSize;

namespace {
class app_controller_op_creds_delegate : public chip::Controller::OperationalCredentialsDelegate {
public:
    CHIP_ERROR GenerateNOCChain(const ByteSpan &csrElements, const ByteSpan &csrNonce, const ByteSpan &attestationSignature,
                                const ByteSpan &attestationChallenge, const ByteSpan &DAC, const ByteSpan &PAI,
                                Callback<OnNOCChainGeneration> *onCompletion) override
    {
        return CHIP_ERROR_BAD_REQUEST;
    }
};

class app_controller_op_creds_issuer : public esp_matter::controller::credentials_issuer {
public:
    esp_err_t initialize_credentials_issuer(chip::PersistentStorageDelegate &storage) override
    {
        (void)storage;
        return ESP_OK;
    }

    chip::Controller::OperationalCredentialsDelegate *get_delegate() override
    {
        return &m_delegate;
    }

    esp_err_t generate_controller_noc_chain(chip::NodeId node_id, chip::FabricId fabric,
                                            chip::Crypto::P256Keypair &keypair, chip::MutableByteSpan &rcac,
                                            chip::MutableByteSpan &icac, chip::MutableByteSpan &noc) override;
    esp_err_t generate_controller_noc_chain_with_csr(chip::NodeId node_id, chip::FabricId fabric,
                                                     chip::MutableByteSpan &csr, chip::MutableByteSpan &rcac,
                                                     chip::MutableByteSpan &icac, chip::MutableByteSpan &noc) override;

    esp_err_t update_controller_noc();

private:
    app_controller_op_creds_delegate m_delegate;
};

app_controller_op_creds_issuer s_op_creds_issuer;

static esp_err_t app_controller_generate_csr(chip::MutableByteSpan &csr_span, chip::Crypto::P256Keypair &keypair)
{
    ESP_RETURN_ON_FALSE(keypair.Initialize(chip::Crypto::ECPKeyTarget::ECDSA) == CHIP_NO_ERROR, ESP_FAIL, TAG,
                        "Failed to initialize keypair");
    size_t csr_len = csr_span.size();
    ESP_RETURN_ON_FALSE(keypair.NewCertificateSigningRequest(csr_span.data(), csr_len) == CHIP_NO_ERROR, ESP_FAIL, TAG,
                        "Failed to generate CSR");
    csr_span.reduce_size(csr_len);
    return ESP_OK;
}

static esp_err_t app_controller_fetch_matter_rcac(chip::MutableByteSpan &rcac)
{
    size_t rcac_der_len = rcac.size();
    ESP_RETURN_ON_ERROR(app_rmaker_matter_controller_fetch_rcac(rcac.data(), &rcac_der_len), TAG,
                        "Failed to fetch fabric RCAC der file");
    rcac.reduce_size(rcac_der_len);
    return ESP_OK;
}

} // namespace

void app_controller_register_op_creds_issuer(void)
{
    esp_matter::controller::set_custom_credentials_issuer(&s_op_creds_issuer);
}

esp_err_t app_controller_client_setup(uint8_t *ipk, size_t ipk_len, uint64_t fabric_id)
{
    esp_err_t ret = ESP_OK;
    chip::MutableByteSpan ipk_span(ipk, ipk_len);
    {
        esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
        ret = esp_matter::controller::matter_controller_client::get_instance().init(0, 0, 5580);
        ESP_GOTO_ON_ERROR(ret, exit, TAG, "matter_controller_client init failed");
        chip::FabricIndex stored_fabric_index = chip::kUndefinedFabricIndex;
        if (ipk_len == 0) {
            auto *system_state = chip::Controller::DeviceControllerFactory::GetInstance().GetSystemState();
            ESP_GOTO_ON_FALSE(system_state && system_state->Fabrics(), ESP_ERR_INVALID_STATE, exit, TAG,
                              "Controller system state or fabric table is unavailable");
            for (const chip::FabricInfo &fabric : *system_state->Fabrics()) {
                if (fabric.GetFabricId() == static_cast<chip::FabricId>(fabric_id)) {
                    stored_fabric_index = fabric.GetFabricIndex();
                    break;
                }
            }
            ESP_GOTO_ON_FALSE(stored_fabric_index != chip::kUndefinedFabricIndex, ESP_ERR_NOT_FOUND, exit, TAG,
                              "No stored fabric found for fabric_id=0x%016llx", (unsigned long long)fabric_id);
        }
        ret = esp_matter::controller::matter_controller_client::get_instance().setup_controller(ipk_span,
                                                                                                stored_fabric_index);
    }
exit:
    return ret;
}

esp_err_t app_controller_update_noc(uint64_t fabric_id)
{
    (void)fabric_id;
    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
    esp_err_t err = s_op_creds_issuer.update_controller_noc();
    return err;
}

esp_err_t app_controller_op_creds_issuer::generate_controller_noc_chain([[maybe_unused]] chip::NodeId node_id,
                                                                        [[maybe_unused]] chip::FabricId fabric,
                                                                        [[maybe_unused]] chip::Crypto::P256Keypair &keypair,
                                                                        [[maybe_unused]] chip::MutableByteSpan &rcac,
                                                                        [[maybe_unused]] chip::MutableByteSpan &icac,
                                                                        [[maybe_unused]] chip::MutableByteSpan &noc)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t app_controller_op_creds_issuer::generate_controller_noc_chain_with_csr(chip::NodeId node_id,
                                                                                 chip::FabricId fabric,
                                                                                 chip::MutableByteSpan &csr,
                                                                                 chip::MutableByteSpan &rcac,
                                                                                 chip::MutableByteSpan &icac,
                                                                                 chip::MutableByteSpan &noc)
{
    size_t rcac_len = rcac.size();
    esp_err_t err = app_rmaker_matter_controller_get_stored_rcac(rcac.data(), &rcac_len);
    if (err == ESP_OK) {
        rcac.reduce_size(rcac_len);
    } else {
        ESP_RETURN_ON_ERROR(app_controller_fetch_matter_rcac(rcac), TAG, "Failed on fetching RCAC");
    }

    icac.reduce_size(0);

    size_t noc_der_len = noc.size();
    ESP_RETURN_ON_ERROR(app_rmaker_matter_controller_issue_controller_noc(csr.data(), csr.size(), noc.data(),
                                                                          &noc_der_len, node_id),
                        TAG, "Failed to issue controller NOC from CSR");
    noc.reduce_size(noc_der_len);
    return ESP_OK;
}

esp_err_t app_controller_op_creds_issuer::update_controller_noc()
{
    ScopedMemoryBufferWithSize<uint8_t> noc;
    noc.Calloc(chip::Controller::kMaxCHIPDERCertLength);
    ESP_RETURN_ON_FALSE(noc.Get(), ESP_ERR_NO_MEM, TAG, "Failed allocate memory for noc");
    ScopedMemoryBufferWithSize<uint8_t> noc_chip;
    noc_chip.Calloc(chip::Controller::kMaxCHIPDERCertLength);
    ESP_RETURN_ON_FALSE(noc_chip.Get(), ESP_ERR_NO_MEM, TAG, "Failed allocate memory for noc_chip");

    chip::MutableByteSpan noc_span(noc.Get(), chip::Controller::kMaxCHIPDERCertLength);
    chip::MutableByteSpan noc_chip_span(noc_chip.Get(), chip::Controller::kMaxCHIPDERCertLength);
    chip::MutableByteSpan icac_span;
    uint8_t csr_der_buf[chip::Crypto::kMIN_CSR_Buffer_Size];
    chip::MutableByteSpan csr_span(csr_der_buf);
    chip::Crypto::P256Keypair keypair;

    ESP_RETURN_ON_ERROR(app_controller_generate_csr(csr_span, keypair), TAG, "Failed on generating CSR");

    size_t noc_der_len = noc_span.size();
    ESP_RETURN_ON_ERROR(app_rmaker_matter_controller_issue_controller_noc(
                            csr_span.data(), csr_span.size(), noc_span.data(), &noc_der_len, 0),
                        TAG, "Failed to issue user NOC");
    noc_span.reduce_size(noc_der_len);
    ESP_LOGI(TAG, "Update NOC: csr_len=%u noc_der_len=%u", (unsigned)csr_span.size(), (unsigned)noc_span.size());

    ESP_RETURN_ON_FALSE(chip::Credentials::ConvertX509CertToChipCert(noc_span, noc_chip_span) == CHIP_NO_ERROR,
                        ESP_FAIL, TAG, "Failed on converting NOC to Chip Cert");
    ESP_LOGI(TAG, "Update NOC: noc_chip_len=%u", (unsigned)noc_chip_span.size());

    auto *controller = esp_matter::controller::matter_controller_client::get_instance().get_controller();
    ESP_RETURN_ON_FALSE(controller, ESP_FAIL, TAG, "Failed to get controller instance for NOC update");

    CHIP_ERROR chip_err = controller->UpdateControllerNOCChain(noc_chip_span, icac_span, &keypair, false);
    if (chip_err != CHIP_NO_ERROR) {
        ESP_LOGE(TAG, "UpdateControllerNOCChain failed: %" CHIP_ERROR_FORMAT, chip_err.Format());
        return ESP_FAIL;
    }
    chip::app::DnssdServer::Instance().StartServer();
    return ESP_OK;
}
