set(PRIV_REQUIRES_LIST rmaker_app_reset app_end_device)

idf_component_register(SRCS         ./app_main.cpp ./app_matter_switch.cpp ./app_driver.cpp ../../common/app_end_device/led_driver_gpio.c
                      PRIV_INCLUDE_DIRS  "."
                      PRIV_REQUIRES      ${PRIV_REQUIRES_LIST})

set_property(TARGET ${COMPONENT_LIB} PROPERTY CXX_STANDARD 17)
target_compile_options(${COMPONENT_LIB} PRIVATE "-DCHIP_HAVE_CONFIG_H")
