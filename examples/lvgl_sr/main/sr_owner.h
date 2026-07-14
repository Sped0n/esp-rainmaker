#pragma once

#include <stdint.h>

#include "esp_err.h"

typedef enum {
    SR_STATE_STOPPED,
    SR_STATE_STARTING,
    SR_STATE_RUNNING,
    SR_STATE_PAUSED,
    SR_STATE_STOPPING,
} sr_state_t;

typedef enum {
    SR_INTENT_START,
    SR_INTENT_PAUSE,
    SR_INTENT_RESUME,
    SR_INTENT_STOP,
} sr_intent_t;

typedef struct {
    sr_state_t state;
    uint32_t wake_count;
} sr_snapshot_t;

esp_err_t sr_owner_init(void);
esp_err_t sr_owner_submit(sr_intent_t intent);
void sr_owner_get_snapshot(sr_snapshot_t *snapshot);
