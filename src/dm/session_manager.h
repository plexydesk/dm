/*
 * Copyright (C) 2024-2026 Siraj Razick
 * SPDX-License-Identifier: AGPL-3.0-only
 */


#ifndef PLEXY_DM_SESSION_MANAGER_H
#define PLEXY_DM_SESSION_MANAGER_H

#include "dm_config.h"
#include "logind_dbus.h"
#include "pam_auth.h"
#include "plexy_dm.h"
#include "vt_manager.h"

typedef struct plexy_session_mgr plexy_session_mgr_t;

plexy_session_mgr_t *session_mgr_create(plexy_vt_manager_t *vt_mgr,
                                        plexy_logind_ctx_t *logind,
                                        const plexy_dm_config_t *cfg);

void session_mgr_destroy(plexy_session_mgr_t *mgr);

int session_mgr_launch(plexy_session_mgr_t *mgr, const char *username,
                       const char *password, plexy_dm_error_t *err);

int session_mgr_autologin(plexy_session_mgr_t *mgr, const char *username,
                          plexy_dm_error_t *err);

void session_mgr_terminate(plexy_session_mgr_t *mgr, int session_id);

int session_mgr_reap_children(plexy_session_mgr_t *mgr);

const plexy_dm_session_t *session_mgr_get(const plexy_session_mgr_t *mgr,
                                          int session_id);

int session_mgr_find_by_user(const plexy_session_mgr_t *mgr,
                             const char *username);

int session_mgr_active_count(const plexy_session_mgr_t *mgr);

int session_mgr_list_active(const plexy_session_mgr_t *mgr, int *ids_out,
                            int max_ids);

void session_mgr_lock(plexy_session_mgr_t *mgr, int session_id);

void session_mgr_unlock(plexy_session_mgr_t *mgr, int session_id);

int session_mgr_activate(plexy_session_mgr_t *mgr, int session_id);

#endif
