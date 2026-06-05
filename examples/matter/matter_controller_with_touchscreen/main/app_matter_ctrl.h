#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

void matter_factory_reset(void);
void matter_ctrl_change_state(intptr_t arg);
void matter_ctrl_on_device_list_update(esp_err_t err);
esp_err_t matter_ctrl_refresh_ui_init(void);
const char *matter_ctrl_get_qr_payload(void);
void matter_ctrl_set_qr_payload(const char *payload);
void matter_ctrl_set_provisioned(bool provisioned);
bool matter_ctrl_is_provisioned(void);

#ifdef __cplusplus
}
#endif
