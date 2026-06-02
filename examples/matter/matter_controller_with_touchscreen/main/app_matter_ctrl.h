#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CONTROL_LIGHT_DEVICE = 0,
    CONTROL_PLUG_DEVICE,
    CONTROL_SWITCH_DEVICE,
    CONTROL_UNKNOWN_DEVICE,
} control_device_type_t;

typedef struct node_endpoint_id_list {
    uint64_t node_id;
    bool OnOff;
    bool onoff_known;
    bool is_online;
    bool is_rainmaker_device;
    uint16_t endpoint_id;
    size_t device_type;
    char name[33];
    lv_obj_t *lv_obj;
    struct node_endpoint_id_list *next;
} node_endpoint_id_list_t;

typedef struct {
    size_t device_num;
    size_t online_num;
    node_endpoint_id_list_t *dev_list;
} device_to_control_t;

void matter_factory_reset(void);
void matter_ctrl_lv_obj_clear(void);
void matter_ctrl_change_state(intptr_t arg);
void matter_device_list_lock(void);
void matter_device_list_unlock(void);
void matter_ctrl_refresh_device_list(void);
bool matter_ctrl_can_refresh_device_list(void);
void matter_ctrl_request_device_list_update(void);
const char *matter_ctrl_get_qr_payload(void);
void matter_ctrl_set_qr_payload(const char *payload);
void matter_ctrl_set_provisioned(bool provisioned);

extern device_to_control_t device_to_control;

#ifdef __cplusplus
}
#endif
