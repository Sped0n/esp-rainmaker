#pragma once

#include <esp_err.h>

#include "rmaker_controller_service/app_matter_controller.h"

esp_err_t app_matter_noc_manager_sync_commissioned_node(matter_controller_handle_t *handle);
esp_err_t app_matter_noc_manager_install(matter_controller_handle_t *handle, bool force_install);
