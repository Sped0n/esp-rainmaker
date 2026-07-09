set(ldfragments linker.lf)

idf_component_register(SRCS "app_main.cpp"
                       PRIV_INCLUDE_DIRS "."
                       REQUIRES app_controller esp_matter esp_matter_console esp_rainmaker nvs_flash openthread
                       LDFRAGMENTS "${ldfragments}")

set_property(TARGET ${COMPONENT_LIB} PROPERTY CXX_STANDARD 17)
target_compile_options(${COMPONENT_LIB} PRIVATE "-DCHIP_HAVE_CONFIG_H")
