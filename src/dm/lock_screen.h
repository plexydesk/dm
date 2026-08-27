/*
 * Copyright (C) 2024-2026 Siraj Razick
 * SPDX-License-Identifier: AGPL-3.0-only
 */


#ifndef PLEXY_DM_LOCK_SCREEN_H
#define PLEXY_DM_LOCK_SCREEN_H

#include "dm_config.h"
#include "plexy_dm.h"

typedef struct plexy_lock_mgr plexy_lock_mgr_t;

typedef void (*lock_unlock_cb)(int session_id, void *data);

plexy_lock_mgr_t *lock_mgr_create(const plexy_dm_config_t *cfg,
                                  lock_unlock_cb on_unlock, void *user_data);

void lock_mgr_destroy(plexy_lock_mgr_t *mgr);

int lock_mgr_lock_session(plexy_lock_mgr_t *mgr, int session_id,
                          const char *username);

plexy_dm_error_t lock_mgr_try_unlock(plexy_lock_mgr_t *mgr, int session_id,
                                     const char *password);

bool lock_mgr_is_locked(const plexy_lock_mgr_t *mgr, int session_id);

#endif
