/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <agent_ng.h>

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

const agent_ng_tool_t *app_agent_tools_get(size_t *count);

#ifdef __cplusplus
}
#endif
