/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    matter_device_list_type_light = 0,
    matter_device_list_type_plug,
    matter_device_list_type_switch,
    matter_device_list_type_unknown,
} matter_device_list_type_t;

typedef struct matter_device_list_node {
    uint64_t node_id;
    bool onoff;
    bool is_online;
    uint16_t endpoint_id;
    size_t device_type;
    char name[33];
    struct matter_device_list_node *next;
} matter_device_list_node_t;

typedef struct {
    size_t device_num;
    size_t online_num;
    matter_device_list_node_t *dev_list;
} matter_device_list_state_t;

void matter_device_list_lock(void);
void matter_device_list_unlock(void);
void matter_device_list_rebuild(void);
bool matter_device_list_is_fetchable(void);
void matter_device_list_fetch(void);

extern matter_device_list_state_t matter_device_list;

#ifdef __cplusplus
}
#endif
