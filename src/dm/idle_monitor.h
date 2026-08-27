/*
 * Copyright (C) 2024-2026 Siraj Razick
 * SPDX-License-Identifier: AGPL-3.0-only
 */


#ifndef PLEXY_DM_IDLE_MONITOR_H
#define PLEXY_DM_IDLE_MONITOR_H

#include <stdbool.h>

typedef struct plexy_idle_monitor plexy_idle_monitor_t;

typedef void (*idle_timeout_cb)(void *data);

plexy_idle_monitor_t *idle_monitor_create(int timeout_secs, idle_timeout_cb cb,
                                          void *data);

void idle_monitor_destroy(plexy_idle_monitor_t *mon);

int idle_monitor_get_fd(const plexy_idle_monitor_t *mon);

void idle_monitor_handle(plexy_idle_monitor_t *mon);

void idle_monitor_reset(plexy_idle_monitor_t *mon);

void idle_monitor_set_enabled(plexy_idle_monitor_t *mon, bool enabled);

#endif
