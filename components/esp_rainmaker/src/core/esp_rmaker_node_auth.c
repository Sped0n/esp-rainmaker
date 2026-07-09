/*
 * SPDX-FileCopyrightText: 2023-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <esp_log.h>
#include <esp_idf_version.h>
#include "mbedtls/platform.h"
#include "mbedtls/pk.h"
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
#include "mbedtls/md.h"
#include "psa/crypto.h"
#ifdef CONFIG_MBEDTLS_HARDWARE_ECDSA_SIGN
/* Opaque hardware-ECDSA key import, replaces the removed esp_ecdsa_set_pk_context() API */
#include "psa_crypto_driver_esp_ecdsa.h"
#endif
#else
#include "mbedtls/sha256.h"
#include "mbedtls/ecdsa.h"
#include "ecdsa/ecdsa_alt.h"
#endif
/* esp_sha() lives in the mbedtls port on all IDF versions, but its public header
 * was renamed: sha_core.h on v6.0, sha_parallel_engine.h on older IDF. */
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
#include "sha/sha_core.h"
#else
#include "sha/sha_parallel_engine.h"
#endif
#include "soc/soc_caps.h"

/* The TEST_SIGNATURE_VERIFICATION debug aid still uses mbedtls_pk_get_type() and
 * the RSASSA-PSS options type, both removed/made-private in mbedtls 4.0. It has
 * not been ported, so force it off on IDF v6.0+ to avoid a broken build if it is
 * ever enabled there. */
#if defined(TEST_SIGNATURE_VERIFICATION) && TEST_SIGNATURE_VERIFICATION && (ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0))
#undef TEST_SIGNATURE_VERIFICATION
#define TEST_SIGNATURE_VERIFICATION 0
#endif

#include "esp_secure_cert_read.h"
#include "esp_rmaker_utils.h"
#include "esp_rmaker_client_data.h"
#include "mbedtls/x509_crt.h"

static const char *TAG = "esp_rmaker_user_node_auth";

#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(6, 0, 0)
/* RNG callback for the pre-v6.0 mbedtls signing API. The v6.0 path uses
 * mbedtls_pk_sign / mbedtls_pk_sign_ext which take their RNG from PSA
 * internally, so this helper is unused there and would trip
 * -Werror=unused-function. */
static int myrand(void *rng_state, unsigned char *output, size_t len)
{
    esp_fill_random(output, len);
    return 0;
}
#endif

static inline uint8_t to_hex_digit(unsigned val)
{
    return (val < 10) ? ('0' + val) : ('a' + val - 10);
}

static void bytes_to_hex(uint8_t *src, uint8_t *dst, int in_len)
{
    for (int i = 0; i < in_len; i++) {
        dst[2 * i] = to_hex_digit(src[i] >> 4);
        dst[2 * i + 1] = to_hex_digit(src[i] & 0xf);
    }
    dst[2 * in_len] = 0;
}

esp_err_t esp_rmaker_node_auth_sign_msg(const void *challenge, size_t inlen, void **response, size_t *outlen)
{
    esp_err_t err = ESP_OK;
    int ret = 0;
    char *priv_key = NULL;
    size_t priv_key_len = 0;
    uint8_t *signature = NULL;
    char *char_signature = NULL;
    size_t slen = 0;
    mbedtls_pk_context pk_ctx;
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
    /* True when pk_ctx wraps the hardware-ECDSA opaque PSA key, whose type is EC
     * by construction (see the key-type detection block below). */
    bool is_hw_ecdsa = false;
#endif
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0) && defined(CONFIG_ESP_RMAKER_USE_ESP_SECURE_CERT_MGR) && defined(CONFIG_MBEDTLS_HARDWARE_ECDSA_SIGN)
    /* Opaque PSA key wrapping the hardware ECDSA peripheral. mbedtls_pk_wrap_psa()
     * does NOT take ownership, so it is destroyed explicitly in cleanup. */
    psa_key_id_t ecdsa_opaque_key_id = 0;
#endif
#if TEST_SIGNATURE_VERIFICATION
    char *cert = NULL;
    mbedtls_x509_crt crt;
    bool crt_initialized = false;
#endif

    if (!challenge || (inlen == 0)) {
        ESP_LOGE(TAG, "function arguments challenge and inlen cannot be NULL.");
        return ESP_ERR_INVALID_ARG;
    }

    /* Calculate SHA-256 of challenge. esp_sha() is available on all supported
     * IDF versions (mbedtls port), so no version split is needed here. */
    uint8_t hash[32] = {0};
    esp_sha(SHA2_256, (const unsigned char *)challenge, inlen, hash);

    /* Sign the hash using ECDSA or RSA */
    mbedtls_pk_init(&pk_ctx);

#if CONFIG_ESP_RMAKER_USE_ESP_SECURE_CERT_MGR
    esp_secure_cert_key_type_t key_type;
    err = esp_secure_cert_get_priv_key_type(&key_type);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get the type of private key from secure cert partition, err:%d", err);
        goto cleanup;
    }
    if (key_type == ESP_SECURE_CERT_INVALID_KEY) {
        ESP_LOGE(TAG, "Private key type in secure cert partition is invalid");
        err = ESP_FAIL;
        goto cleanup;
    }

    /* This flow is for devices supporting ECDSA peripheral */
    if (key_type == ESP_SECURE_CERT_ECDSA_PERIPHERAL_KEY) {
#if CONFIG_MBEDTLS_HARDWARE_ECDSA_SIGN
        /* Setup ECDSA peripheral */
        uint8_t efuse_block_id;
        err = esp_secure_cert_get_priv_key_efuse_id(&efuse_block_id);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to obtain efuse key id, err:%d", err);
            goto cleanup;
        }

        ESP_LOGD(TAG, "Using key from eFuse block %d for ECDSA key", efuse_block_id);
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
        /* mbedtls v4.0 (IDF v6.0) removed esp_ecdsa_set_pk_context(). Import the
         * hardware-backed key as a PSA opaque key and wrap it into the pk context,
         * per the IDF v6.0 security migration guide. */
        psa_key_attributes_t key_attr = PSA_KEY_ATTRIBUTES_INIT;
        psa_set_key_type(&key_attr, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
        psa_set_key_bits(&key_attr, 256);
        psa_set_key_usage_flags(&key_attr, PSA_KEY_USAGE_SIGN_HASH);
        psa_set_key_algorithm(&key_attr, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
        psa_set_key_lifetime(&key_attr, PSA_KEY_LIFETIME_ESP_ECDSA_VOLATILE);

        esp_ecdsa_opaque_key_t opaque_key = {
            .curve = ESP_ECDSA_CURVE_SECP256R1,
            .efuse_block = efuse_block_id,
#if SOC_KEY_MANAGER_SUPPORTED
            .key_recovery_info = NULL,
#endif
        };
        psa_status_t status = psa_import_key(&key_attr, (const uint8_t *)&opaque_key, sizeof(opaque_key), &ecdsa_opaque_key_id);
        psa_reset_key_attributes(&key_attr);
        if (status != PSA_SUCCESS) {
            ESP_LOGE(TAG, "Failed to import opaque ECDSA key, status:%d", (int)status);
            err = ESP_FAIL;
            goto cleanup;
        }
        ret = mbedtls_pk_wrap_psa(&pk_ctx, ecdsa_opaque_key_id);
        if (ret != 0) {
            ESP_LOGE(TAG, "Failed to wrap opaque ECDSA key, ret:-0x%04x", -ret);
            err = ESP_FAIL;
            goto cleanup;
        }
        is_hw_ecdsa = true;
#else
        esp_ecdsa_pk_conf_t pk_conf = {
            .grp_id = MBEDTLS_ECP_DP_SECP256R1,
            .efuse_block = efuse_block_id,
        };

        ret = esp_ecdsa_set_pk_context(&pk_ctx, &pk_conf);
        if (ret != 0) {
            ESP_LOGE(TAG, "Failed to set ECDSA context, ret:%d", ret);
            err = ESP_FAIL;
            goto cleanup;
        }
#endif
#else /* !CONFIG_MBEDTLS_HARDWARE_ECDSA_SIGN */
        ESP_LOGE(TAG, "ECDSA peripheral support is not available on this SoC");
        err = ESP_ERR_NOT_SUPPORTED;
        goto cleanup;
#endif /* CONFIG_MBEDTLS_HARDWARE_ECDSA_SIGN */
    } else
#endif /* CONFIG_ESP_RMAKER_USE_ESP_SECURE_CERT_MGR */
    {
        /* This flow is for devices which do not support ECDSA peripheral */
        priv_key = esp_rmaker_get_client_key();
        priv_key_len = esp_rmaker_get_client_key_len();

        if (!priv_key) {
            ESP_LOGE(TAG, "Error getting private key");
            err = ESP_FAIL;
            goto cleanup;
        }

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
        ret = mbedtls_pk_parse_key(&pk_ctx, (uint8_t *)priv_key, priv_key_len, NULL, 0);
#elif ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
        ret = mbedtls_pk_parse_key(&pk_ctx, (uint8_t *)priv_key, priv_key_len, NULL, 0, NULL, 0);
#else
        ret = mbedtls_pk_parse_key(&pk_ctx, (uint8_t *)priv_key, priv_key_len, NULL, 0);
#endif

        if (ret != 0) {
            ESP_LOGE(TAG, "Error parsing private key, err:%d", ret);
            err = ESP_FAIL;
            goto cleanup;
        }
    }

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
    /* In mbedtls v4.0 (IDF v6.0+), detect the key type from its PSA attributes.
     * (mbedtls_pk_can_do_psa() matches against the imported key's exact algorithm
     * policy, which a freshly-parsed software key does not satisfy for a specific
     * hash, so it wrongly reports "unsupported" for valid EC/RSA keys.) */
    int is_rsa = 0, is_ec = 0;
    if (is_hw_ecdsa) {
        /* The hardware-ECDSA opaque key was imported as ECC_KEY_PAIR(SECP_R1), so
         * its type is EC by construction. Assert it directly rather than relying
         * on mbedtls_pk_get_psa_attributes() round-tripping a wrapped/opaque key.
         * NOTE: this branch is compile-tested only — the hardware ECDSA sign path
         * has not yet been exercised on a device with an efuse key burned. */
        is_ec = 1;
    } else {
        psa_key_attributes_t key_attr = PSA_KEY_ATTRIBUTES_INIT;
        if (mbedtls_pk_get_psa_attributes(&pk_ctx, PSA_KEY_USAGE_SIGN_HASH, &key_attr) == 0) {
            psa_key_type_t key_type = psa_get_key_type(&key_attr);
            is_ec = PSA_KEY_TYPE_IS_ECC(key_type);
            is_rsa = PSA_KEY_TYPE_IS_RSA(key_type);
        }
        psa_reset_key_attributes(&key_attr);
    }

    /* Allocate signature buffer */
    signature = (uint8_t *)MEM_CALLOC_EXTRAM(1, MBEDTLS_PK_SIGNATURE_MAX_SIZE);
    if (!signature) {
        ESP_LOGE(TAG, "Failed to allocate memory for signature.");
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    if (is_ec) {
        ESP_LOGI(TAG, "ECDSA key found");
        ret = mbedtls_pk_sign(&pk_ctx, MBEDTLS_MD_SHA256, hash, sizeof(hash),
                              signature, MBEDTLS_PK_SIGNATURE_MAX_SIZE, &slen);
        if (ret != 0) {
            ESP_LOGE(TAG, "Error in ECDSA signing. err = -0x%04x", -ret);
            err = ESP_FAIL;
            goto cleanup;
        }
    } else if (is_rsa) {
        ESP_LOGI(TAG, "RSA key found");
        ret = mbedtls_pk_sign_ext(MBEDTLS_PK_SIGALG_RSA_PSS, &pk_ctx, MBEDTLS_MD_SHA256,
                                  hash, sizeof(hash),
                                  signature, MBEDTLS_PK_SIGNATURE_MAX_SIZE, &slen);
        if (ret != 0) {
            ESP_LOGE(TAG, "Error in RSA-PSS signing. err = -0x%04x", -ret);
            err = ESP_FAIL;
            goto cleanup;
        }
        ESP_LOGI(TAG, "Signature length %d", slen);
    } else {
        ESP_LOGE(TAG, "Unsupported key type for signing");
        err = ESP_FAIL;
        goto cleanup;
    }
#else
    /* Allocate signature buffer based on key type */
    if (mbedtls_pk_get_type(&pk_ctx) == MBEDTLS_PK_RSA) {
        ESP_LOGI(TAG, "RSA key found");
        mbedtls_rsa_context *rsa_ctx = mbedtls_pk_rsa(pk_ctx);
        size_t key_size = mbedtls_rsa_get_len(rsa_ctx);
        signature = (uint8_t *)MEM_CALLOC_EXTRAM(1, key_size);
        if (!signature) {
            ESP_LOGE(TAG, "Failed to allocate memory for RSA signature.");
            err = ESP_ERR_NO_MEM;
            goto cleanup;
        }
    } else if (mbedtls_pk_get_type(&pk_ctx) == MBEDTLS_PK_ECKEY ||
               mbedtls_pk_get_type(&pk_ctx) == MBEDTLS_PK_ECDSA) {
        /* MBEDTLS_PK_ECKEY: software EC key (PEM/DER parsed)
         * MBEDTLS_PK_ECDSA: hardware ECDSA peripheral (esp_ecdsa_set_pk_context) */
        ESP_LOGI(TAG, "ECDSA key found");
        signature = (uint8_t *)MEM_CALLOC_EXTRAM(1, MBEDTLS_ECDSA_MAX_LEN);
        if (!signature) {
            ESP_LOGE(TAG, "Failed to allocate memory for ECDSA signature.");
            err = ESP_ERR_NO_MEM;
            goto cleanup;
        }
    } else {
        ESP_LOGE(TAG, "Found unexpected key type: %d", mbedtls_pk_get_type(&pk_ctx));
        err = ESP_FAIL;
        goto cleanup;
    }

    /* Sign using the appropriate method */
    if (mbedtls_pk_get_type(&pk_ctx) == MBEDTLS_PK_ECKEY ||
        mbedtls_pk_get_type(&pk_ctx) == MBEDTLS_PK_ECDSA) {
        ret = mbedtls_pk_sign(&pk_ctx, MBEDTLS_MD_SHA256, hash, sizeof(hash), signature, MBEDTLS_ECDSA_MAX_LEN, &slen, myrand, NULL);
        if (ret != 0) {
            ESP_LOGE(TAG, "Error in writing ECDSA signature. err = %d", ret);
            err = ESP_FAIL;
            goto cleanup;
        }
    } else if (mbedtls_pk_get_type(&pk_ctx) == MBEDTLS_PK_RSA) {
        mbedtls_rsa_context *rsa_ctx = mbedtls_pk_rsa(pk_ctx);
        mbedtls_rsa_set_padding(rsa_ctx, MBEDTLS_RSA_PKCS_V21, MBEDTLS_MD_SHA256);
        ret = mbedtls_rsa_rsassa_pss_sign(rsa_ctx,
                                          myrand,
                                          NULL,
                                          MBEDTLS_MD_SHA256,
                                          sizeof(hash),
                                          hash,
                                          signature);
        if (ret != 0) {
            ESP_LOGE(TAG, "Error in writing RSA signature. err = %d", ret);
            err = ESP_FAIL;
            goto cleanup;
        }
        slen = mbedtls_rsa_get_len(rsa_ctx);
        ESP_LOGI(TAG, "Signature length %d", slen);
    }
#endif

#if TEST_SIGNATURE_VERIFICATION
    cert = esp_rmaker_get_client_cert();
    if (cert == NULL) {
        ESP_LOGE(TAG, "Failed to get client certificate for verification");
        err = ESP_FAIL;
        goto cleanup;
    }

    mbedtls_x509_crt_init(&crt);
    crt_initialized = true;
    size_t cert_len = esp_rmaker_get_client_cert_len();
    ret = mbedtls_x509_crt_parse(&crt, (const unsigned char *)cert, cert_len + 1);
    if (ret != 0) {
        ESP_LOGE(TAG, "Failed to parse certificate: %d", ret);
        err = ESP_FAIL;
        goto cleanup;
    }

    mbedtls_pk_context *pk = &crt.pk;
    if (mbedtls_pk_get_type(&pk_ctx) == MBEDTLS_PK_ECKEY ||
        mbedtls_pk_get_type(&pk_ctx) == MBEDTLS_PK_ECDSA) {
        ret = mbedtls_pk_verify(pk, MBEDTLS_MD_SHA256, hash, sizeof(hash), signature, slen);
    } else if (mbedtls_pk_get_type(&pk_ctx) == MBEDTLS_PK_RSA) {
        mbedtls_pk_rsassa_pss_options opt = {
            .mgf1_hash_id = MBEDTLS_MD_SHA256,
            .expected_salt_len = MBEDTLS_RSA_SALT_LEN_ANY
        };
        ret = mbedtls_pk_verify_ext(MBEDTLS_PK_RSASSA_PSS, (const void *)&opt, pk, MBEDTLS_MD_SHA256, hash, sizeof(hash), signature, slen);
    }

    if (ret == 0) {
        ESP_LOGI(TAG, "Signature is valid");
    } else {
        ESP_LOGE(TAG, "Signature verification failed %d", ret);
    }
#endif /* TEST_SIGNATURE_VERIFICATION */

    /* Convert hex stream to bytes */
#define BYTE_ENCODED_SIGNATURE_LEN ((2 * slen) + 1) /* +1 for null character */
    char_signature = (char *)MEM_ALLOC_EXTRAM(BYTE_ENCODED_SIGNATURE_LEN);
    if (!char_signature) {
        ESP_LOGE(TAG, "Error in allocating memory for challenge response.");
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    bytes_to_hex(signature, (uint8_t *)char_signature, slen);

    /* Set output variables */
    *(char **)response = char_signature;
    *outlen = 2 * slen; /* hex encoding takes 2 bytes per input byte */
    char_signature = NULL; /* Avoid freeing the output buffer in cleanup */

cleanup:
    if (signature) {
        free(signature);
    }
    if (char_signature) {
        free(char_signature);
    }
    mbedtls_pk_free(&pk_ctx);
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0) && defined(CONFIG_ESP_RMAKER_USE_ESP_SECURE_CERT_MGR) && defined(CONFIG_MBEDTLS_HARDWARE_ECDSA_SIGN)
    if (ecdsa_opaque_key_id) {
        psa_destroy_key(ecdsa_opaque_key_id);
    }
#endif
#if TEST_SIGNATURE_VERIFICATION
    if (cert) {
        free(cert);
    }
    if (crt_initialized) {
        mbedtls_x509_crt_free(&crt);
    }
#endif

    return err;
}
