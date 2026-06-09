set(srcs "app_end_device.cpp")
set(requires button driver)

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "include"
                       REQUIRES ${requires})
