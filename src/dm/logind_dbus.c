/*
 * Copyright (C) 2024-2026 Siraj Razick
 * SPDX-License-Identifier: AGPL-3.0-only
 */


#include "logind_dbus.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#define LOGIND_BUS "org.freedesktop.login1"
#define LOGIND_PATH "/org/freedesktop/login1"
#define LOGIND_MGR "org.freedesktop.login1.Manager"
#define LOGIND_SESS "org.freedesktop.login1.Session"

struct plexy_logind_ctx {
  DBusConnection *conn;
  logind_callbacks_t cbs;
};

static int call_void_method(DBusConnection *conn, const char *dest,
                            const char *path, const char *iface,
                            const char *method, int first_arg_type, ...) {
  DBusMessage *msg = dbus_message_new_method_call(dest, path, iface, method);
  if (!msg)
    return -1;

  if (first_arg_type != DBUS_TYPE_INVALID) {
    va_list ap;
    va_start(ap, first_arg_type);
    dbus_message_append_args_valist(msg, first_arg_type, ap);
    va_end(ap);
  }

  DBusError err;
  dbus_error_init(&err);
  DBusMessage *reply =
      dbus_connection_send_with_reply_and_block(conn, msg, 5000, &err);
  dbus_message_unref(msg);

  if (dbus_error_is_set(&err)) {
    syslog(LOG_ERR, "plexy-dm: D-Bus %s.%s failed: %s", iface, method,
           err.message);
    dbus_error_free(&err);
    return -1;
  }

  if (reply)
    dbus_message_unref(reply);
  return 0;
}

static DBusHandlerResult signal_filter(DBusConnection *conn, DBusMessage *msg,
                                       void *data) {
  (void)conn;
  struct plexy_logind_ctx *ctx = data;

  if (dbus_message_is_signal(msg, LOGIND_MGR, "SessionNew")) {
    const char *session_id = NULL;
    const char *path = NULL;
    if (dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &session_id,
                              DBUS_TYPE_OBJECT_PATH, &path,
                              DBUS_TYPE_INVALID) &&
        ctx->cbs.on_session_new) {
      ctx->cbs.on_session_new(session_id, path, ctx->cbs.user_data);
    }
    return DBUS_HANDLER_RESULT_HANDLED;
  }

  if (dbus_message_is_signal(msg, LOGIND_MGR, "SessionRemoved")) {
    const char *session_id = NULL;
    const char *path = NULL;
    if (dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &session_id,
                              DBUS_TYPE_OBJECT_PATH, &path,
                              DBUS_TYPE_INVALID) &&
        ctx->cbs.on_session_removed) {
      ctx->cbs.on_session_removed(session_id, path, ctx->cbs.user_data);
    }
    return DBUS_HANDLER_RESULT_HANDLED;
  }

  if (dbus_message_is_signal(msg, LOGIND_SESS, "Lock")) {
    const char *path = dbus_message_get_path(msg);
    if (path && ctx->cbs.on_lock)
      ctx->cbs.on_lock(path, ctx->cbs.user_data);
    return DBUS_HANDLER_RESULT_HANDLED;
  }

  if (dbus_message_is_signal(msg, LOGIND_SESS, "Unlock")) {
    const char *path = dbus_message_get_path(msg);
    if (path && ctx->cbs.on_unlock)
      ctx->cbs.on_unlock(path, ctx->cbs.user_data);
    return DBUS_HANDLER_RESULT_HANDLED;
  }

  if (dbus_message_is_signal(msg, LOGIND_MGR, "PrepareForSleep")) {
    dbus_bool_t preparing = FALSE;
    if (dbus_message_get_args(msg, NULL, DBUS_TYPE_BOOLEAN, &preparing,
                              DBUS_TYPE_INVALID) &&
        ctx->cbs.on_prepare_sleep) {
      ctx->cbs.on_prepare_sleep(preparing, ctx->cbs.user_data);
    }
    return DBUS_HANDLER_RESULT_HANDLED;
  }

  if (dbus_message_is_signal(msg, LOGIND_MGR, "PrepareForShutdown")) {
    dbus_bool_t preparing = FALSE;
    if (dbus_message_get_args(msg, NULL, DBUS_TYPE_BOOLEAN, &preparing,
                              DBUS_TYPE_INVALID) &&
        ctx->cbs.on_prepare_shutdown) {
      ctx->cbs.on_prepare_shutdown(preparing, ctx->cbs.user_data);
    }
    return DBUS_HANDLER_RESULT_HANDLED;
  }

  return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

plexy_logind_ctx_t *logind_connect(const logind_callbacks_t *cbs) {
  struct plexy_logind_ctx *ctx = calloc(1, sizeof(*ctx));
  if (!ctx)
    return NULL;

  if (cbs)
    ctx->cbs = *cbs;

  DBusError err;
  dbus_error_init(&err);

  ctx->conn = dbus_bus_get(DBUS_BUS_SYSTEM, &err);
  if (!ctx->conn || dbus_error_is_set(&err)) {
    syslog(LOG_ERR, "plexy-dm: failed to connect to system D-Bus: %s",
           err.message);
    dbus_error_free(&err);
    free(ctx);
    return NULL;
  }

  dbus_bus_add_match(ctx->conn,
                     "type='signal',sender='" LOGIND_BUS "',"
                     "interface='" LOGIND_MGR "',"
                     "member='SessionNew'",
                     NULL);
  dbus_bus_add_match(ctx->conn,
                     "type='signal',sender='" LOGIND_BUS "',"
                     "interface='" LOGIND_MGR "',"
                     "member='SessionRemoved'",
                     NULL);
  dbus_bus_add_match(ctx->conn,
                     "type='signal',sender='" LOGIND_BUS "',"
                     "interface='" LOGIND_MGR "',"
                     "member='PrepareForSleep'",
                     NULL);
  dbus_bus_add_match(ctx->conn,
                     "type='signal',sender='" LOGIND_BUS "',"
                     "interface='" LOGIND_MGR "',"
                     "member='PrepareForShutdown'",
                     NULL);

  dbus_bus_add_match(ctx->conn,
                     "type='signal',sender='" LOGIND_BUS "',"
                     "interface='" LOGIND_SESS "',"
                     "member='Lock'",
                     NULL);
  dbus_bus_add_match(ctx->conn,
                     "type='signal',sender='" LOGIND_BUS "',"
                     "interface='" LOGIND_SESS "',"
                     "member='Unlock'",
                     NULL);

  dbus_connection_add_filter(ctx->conn, signal_filter, ctx, NULL);
  dbus_connection_flush(ctx->conn);

  syslog(LOG_INFO, "plexy-dm: connected to logind via D-Bus");
  return ctx;
}

void logind_disconnect(plexy_logind_ctx_t *ctx) {
  if (!ctx)
    return;

  if (ctx->conn) {
    dbus_connection_remove_filter(ctx->conn, signal_filter, ctx);
    dbus_connection_unref(ctx->conn);
  }
  free(ctx);
}

int logind_get_fd(plexy_logind_ctx_t *ctx) {
  if (!ctx || !ctx->conn)
    return -1;

  int fd = -1;
  if (!dbus_connection_get_unix_fd(ctx->conn, &fd))
    return -1;
  return fd;
}

void logind_dispatch(plexy_logind_ctx_t *ctx) {
  if (!ctx || !ctx->conn)
    return;

  while (dbus_connection_dispatch(ctx->conn) == DBUS_DISPATCH_DATA_REMAINS)
    ;

  dbus_connection_read_write(ctx->conn, 0);

  while (dbus_connection_dispatch(ctx->conn) == DBUS_DISPATCH_DATA_REMAINS)
    ;
}

int logind_create_session(plexy_logind_ctx_t *ctx, uid_t uid, pid_t leader_pid,
                          int vt, const char *seat, const char *session_type,
                          char *session_id_out, size_t id_size) {

  if (!ctx || !ctx->conn)
    return -1;

  DBusMessage *msg = dbus_message_new_method_call(
      LOGIND_BUS, LOGIND_PATH, LOGIND_MGR, "GetSessionByPID");
  if (!msg)
    return -1;

  dbus_uint32_t pid_val = (dbus_uint32_t)leader_pid;
  dbus_message_append_args(msg, DBUS_TYPE_UINT32, &pid_val, DBUS_TYPE_INVALID);

  DBusError err;
  dbus_error_init(&err);
  DBusMessage *reply =
      dbus_connection_send_with_reply_and_block(ctx->conn, msg, 5000, &err);
  dbus_message_unref(msg);

  if (!reply || dbus_error_is_set(&err)) {
    syslog(LOG_WARNING, "plexy-dm: GetSessionByPID failed: %s",
           dbus_error_is_set(&err) ? err.message : "no reply");
    dbus_error_free(&err);

    return -1;
  }

  const char *session_path = NULL;
  if (!dbus_message_get_args(reply, NULL, DBUS_TYPE_OBJECT_PATH, &session_path,
                             DBUS_TYPE_INVALID) ||
      !session_path) {
    dbus_message_unref(reply);
    return -1;
  }

  DBusMessage *prop_msg = dbus_message_new_method_call(
      LOGIND_BUS, session_path, "org.freedesktop.DBus.Properties", "Get");
  if (prop_msg) {
    const char *iface = LOGIND_SESS;
    const char *prop_name = "Id";
    dbus_message_append_args(prop_msg, DBUS_TYPE_STRING, &iface,
                             DBUS_TYPE_STRING, &prop_name, DBUS_TYPE_INVALID);

    dbus_error_init(&err);
    DBusMessage *prop_reply = dbus_connection_send_with_reply_and_block(
        ctx->conn, prop_msg, 5000, &err);
    dbus_message_unref(prop_msg);

    if (prop_reply && !dbus_error_is_set(&err)) {
      DBusMessageIter iter;
      if (dbus_message_iter_init(prop_reply, &iter) &&
          dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_VARIANT) {
        DBusMessageIter variant;
        dbus_message_iter_recurse(&iter, &variant);
        const char *id = NULL;
        if (dbus_message_iter_get_arg_type(&variant) == DBUS_TYPE_STRING)
          dbus_message_iter_get_basic(&variant, &id);
        if (id && id[0]) {
          snprintf(session_id_out, id_size, "%s", id);
          dbus_message_unref(prop_reply);
          dbus_message_unref(reply);
          return 0;
        }
      }
    } else {
      dbus_error_free(&err);
    }
    if (prop_reply)
      dbus_message_unref(prop_reply);
  }

  const char *last_slash = strrchr(session_path, '/');
  if (last_slash)
    snprintf(session_id_out, id_size, "%s", last_slash + 1);
  else
    snprintf(session_id_out, id_size, "%s", session_path);

  dbus_message_unref(reply);
  (void)uid;
  (void)vt;
  (void)seat;
  (void)session_type;
  return 0;
}

int logind_release_session(plexy_logind_ctx_t *ctx, const char *session_id) {
  if (!ctx || !session_id)
    return -1;

  return call_void_method(ctx->conn, LOGIND_BUS, LOGIND_PATH, LOGIND_MGR,
                          "ReleaseSession", DBUS_TYPE_STRING, &session_id,
                          DBUS_TYPE_INVALID);
}

int logind_activate_session(plexy_logind_ctx_t *ctx, const char *session_id) {
  if (!ctx || !session_id)
    return -1;

  return call_void_method(ctx->conn, LOGIND_BUS, LOGIND_PATH, LOGIND_MGR,
                          "ActivateSession", DBUS_TYPE_STRING, &session_id,
                          DBUS_TYPE_INVALID);
}

int logind_lock_session(plexy_logind_ctx_t *ctx, const char *session_id) {
  if (!ctx || !session_id)
    return -1;

  return call_void_method(ctx->conn, LOGIND_BUS, LOGIND_PATH, LOGIND_MGR,
                          "LockSession", DBUS_TYPE_STRING, &session_id,
                          DBUS_TYPE_INVALID);
}

int logind_unlock_session(plexy_logind_ctx_t *ctx, const char *session_id) {
  if (!ctx || !session_id)
    return -1;

  return call_void_method(ctx->conn, LOGIND_BUS, LOGIND_PATH, LOGIND_MGR,
                          "UnlockSession", DBUS_TYPE_STRING, &session_id,
                          DBUS_TYPE_INVALID);
}

int logind_inhibit(plexy_logind_ctx_t *ctx, const char *what, const char *who,
                   const char *why, const char *mode) {
  if (!ctx || !ctx->conn)
    return -1;

  DBusMessage *msg = dbus_message_new_method_call(LOGIND_BUS, LOGIND_PATH,
                                                  LOGIND_MGR, "Inhibit");
  if (!msg)
    return -1;

  dbus_message_append_args(msg, DBUS_TYPE_STRING, &what, DBUS_TYPE_STRING, &who,
                           DBUS_TYPE_STRING, &why, DBUS_TYPE_STRING, &mode,
                           DBUS_TYPE_INVALID);

  DBusError err;
  dbus_error_init(&err);
  DBusMessage *reply =
      dbus_connection_send_with_reply_and_block(ctx->conn, msg, 5000, &err);
  dbus_message_unref(msg);

  if (!reply || dbus_error_is_set(&err)) {
    syslog(LOG_ERR, "plexy-dm: Inhibit failed: %s",
           dbus_error_is_set(&err) ? err.message : "no reply");
    dbus_error_free(&err);
    return -1;
  }

  int fd = -1;
  dbus_message_get_args(reply, NULL, DBUS_TYPE_UNIX_FD, &fd, DBUS_TYPE_INVALID);
  dbus_message_unref(reply);

  return fd;
}

int logind_power_action(plexy_logind_ctx_t *ctx, power_action_t action) {
  if (!ctx)
    return -1;

  const char *method = NULL;
  switch (action) {
  case POWER_ACTION_SHUTDOWN:
    method = "PowerOff";
    break;
  case POWER_ACTION_REBOOT:
    method = "Reboot";
    break;
  case POWER_ACTION_SUSPEND:
    method = "Suspend";
    break;
  case POWER_ACTION_HIBERNATE:
    method = "Hibernate";
    break;
  }

  dbus_bool_t interactive = FALSE;
  return call_void_method(ctx->conn, LOGIND_BUS, LOGIND_PATH, LOGIND_MGR,
                          method, DBUS_TYPE_BOOLEAN, &interactive,
                          DBUS_TYPE_INVALID);
}
