set(ldfragments linker.lf)
idf_component_register(SRCS
                       ./app_main.cpp
                       ./app_matter_bridge.cpp
                       ./app_matter_controller.cpp
                       ./app_matter_noc_manager.cpp
                       ./rmaker_controller_service/app_matter_controller.cpp
                       ./rmaker_controller_service/app_matter_controller_callback.cpp
                       ./rmaker_controller_service/app_matter_device_manager.cpp
                      ./rmaker_controller_service/matter_controller_std.cpp
                      PRIV_INCLUDE_DIRS
                      "."
                      "./rmaker_controller_service"
                      "../../../common/rmaker_app_network"
                      LDFRAGMENTS "${ldfragments}")

set_property(TARGET ${COMPONENT_LIB} PROPERTY CXX_STANDARD 17)
target_compile_options(${COMPONENT_LIB} PRIVATE "-DCHIP_HAVE_CONFIG_H")
