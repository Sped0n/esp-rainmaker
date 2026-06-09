set(ldfragments linker.lf)

set(SRCS_LIST "app_main.cpp"
              "app_matter_ctrl.cpp"
              "app_matter_device_list.cpp"
              "app_matter_onoff.cpp"
              "box/box_main.c"
              "box/box_platform.c"
              "box/gui/ui_about_us.c"
              "box/gui/ui_boot_animate.c"
              "box/gui/ui_main.c"
              "box/gui/ui_matter_ctrl.cpp"
              "box/gui/image/QR_matter.c"
              "box/gui/image/esp_logo.c"
              "box/gui/image/esp_logo_tiny.c"
              "box/gui/image/esp_text.c"
              "box/gui/image/icon_about_us.c"
              "box/gui/image/icon_air_off.c"
              "box/gui/image/icon_air_on.c"
              "box/gui/image/icon_box.c"
              "box/gui/image/icon_light_off.c"
              "box/gui/image/icon_light_on.c"
              "box/gui/image/icon_matter_ctrl.c"
              "box/gui/image/icon_plug_off.c"
              "box/gui/image/icon_plug_on.c"
              "box/gui/image/icon_switch_off.c"
              "box/gui/image/icon_switch_on.c")

set(PRIV_INCLUDE_DIRS_LIST "."
                           "./box"
                           "./box/gui")

idf_component_register(
    SRCS ${SRCS_LIST}
    PRIV_INCLUDE_DIRS ${PRIV_INCLUDE_DIRS_LIST}
    REQUIRES app_controller esp_event freertos nvs_flash
    LDFRAGMENTS "${ldfragments}")

target_compile_options(${COMPONENT_LIB} PRIVATE "-Wno-format")

set_property(TARGET ${COMPONENT_LIB} PROPERTY CXX_STANDARD 17)

target_compile_options(${COMPONENT_LIB} PRIVATE "-DCHIP_HAVE_CONFIG_H")
