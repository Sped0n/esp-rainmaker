set(ldfragments linker.lf)

set(SRCS_DIRS_LIST "."
                   "./devices"
                   "./box"
                   "./box/gui"
                   "./box/gui/image")

set(PRIV_INCLUDE_DIRS_LIST "."
                           "./box"
                           "./box/gui")

idf_component_register(SRC_DIRS ${SRCS_DIRS_LIST}
                       PRIV_INCLUDE_DIRS ${PRIV_INCLUDE_DIRS_LIST}
                       REQUIRES agent_ng app_controller audio_ng chip esp_matter esp_matter_console esp_matter_controller esp_rainmaker nvs_flash openthread
                       LDFRAGMENTS "${ldfragments}")


set_property(TARGET ${COMPONENT_LIB} PROPERTY CXX_STANDARD 17)
target_compile_options(${COMPONENT_LIB} PRIVATE "-DCHIP_HAVE_CONFIG_H")
