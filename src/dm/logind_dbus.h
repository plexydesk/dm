/*
 * Copyright (C) 2024-2026 Siraj Razick
 * SPDX-License-Identifier: AGPL-3.0-only
 */


#ifndef PLEXY_DM_LOGIND_DBUS_H
#define PLEXY_DM_LOGIND_DBUS_H

#include "plexy_dm.h"
#include <dbus/dbus.h>

typedef struct plexy_logind_ctx plexy_logind_ctx_t;

typedef void (*logind_lock_cb)(const char *session_id, void *data);
typedef void (*logind_session_cb)(const char *session_id, const char *path,
                                  void *data);
typedef void (*logind_sleep_cb)(bool preparing, void *data);

typedef struct {
  logind_lock_cb on_lock;
  logind_lock_cb on_unlock;
  logind_session_cb on_session_new;
  logind_session_cb on_session_removed;
  logind_sleep_cb on_prepare_sleep;
  logind_sleep_cb on_prepare_shutdown;
  void *user_data;
} logind_callbacks_t;

plexy_logind_ctx_t *logind_connect(const logind_callbacks_t *cbs);

void logind_disconnect(plexy_logind_ctx_t *ctx);

int logind_get_fd(plexy_logind_ctx_t *ctx);

void logind_dispatch(plexy_logind_ctx_t *ctx);

int logind_create_session(plexy_logind_ctx_t *ctx, uid_t uid, pid_t leader_pid,
                          int vt, const char *seat, const char *session_type,
                          char *session_id_out, size_t id_size);

int logind_release_session(plexy_logind_ctx_t *ctx, const char *session_id);

int logind_activate_session(plexy_logind_ctx_t *ctx, const char *session_id);

int logind_lock_session(plexy_logind_ctx_t *ctx, const char *session_id);

int logind_unlock_session(plexy_logind_ctx_t *ctx, const char *session_id);

int logind_inhibit(plexy_logind_ctx_t *ctx, const char *what, const char *who,
                   const char *why, const char *mode);

int logind_power_action(plexy_logind_ctx_t *ctx, power_action_t action);

#endif
