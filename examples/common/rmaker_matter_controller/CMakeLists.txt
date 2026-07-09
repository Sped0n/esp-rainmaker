idf_component_register(SRC_DIRS "."
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "priv_include"
                       PRIV_REQUIRES chip esp_matter esp_matter_controller esp_timer freertos mbedtls nvs_flash
                       REQUIRES esp_rainmaker rmaker_user_api)
