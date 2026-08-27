/*
 * Copyright (C) 2024-2026 Siraj Razick
 * SPDX-License-Identifier: AGPL-3.0-only
 */


#ifndef PLEXY_DM_H
#define PLEXY_DM_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

#define PLEXY_DM_VERSION "1.5.4"
#define PLEXY_DM_SERVICE_NAME "plexy-dm"

#define PLEXY_DM_CONFIG_PATH "/etc/plexy-dm/plexy-dm.conf"
#define PLEXY_DM_PAM_SERVICE "plexy-dm"
#define PLEXY_DM_RUN_DIR "/run/plexy-dm"
#define PLEXY_DM_LOG_DIR "/var/log/plexy-dm"
#define PLEXY_DM_SESSION_EXEC "plexydesk-session"

#define PLEXY_DM_MAX_SESSIONS 8
#define PLEXY_DM_MAX_USERS 64
#define PLEXY_DM_MAX_USERNAME 256
#define PLEXY_DM_MAX_PASSWORD 1024
#define PLEXY_DM_MAX_PATH 4096
#define PLEXY_DM_MAX_REALNAME 256

#define PLEXY_DM_DEFAULT_MIN_UID 1000
#define PLEXY_DM_DEFAULT_MAX_UID 60000
#define PLEXY_DM_DEFAULT_GREETER_VT 1
#define PLEXY_DM_DEFAULT_IDLE_TIMEOUT 300
#define PLEXY_DM_DEFAULT_AUTOLOGIN_DELAY 0
#define PLEXY_DM_DEFAULT_BACKGROUND                                            \
  "/opt/plexydesk/current/share/plexydesk/background/wallpaper.jpeg"

typedef enum {
  GREETER_STATE_WIFI,
  GREETER_STATE_WIFI_PASSWORD,
  GREETER_STATE_USER_SELECT,
  GREETER_STATE_PASSWORD,
  GREETER_STATE_AUTHENTICATING,
  GREETER_STATE_AUTH_FAILED,
  GREETER_STATE_SWITCHING,
  GREETER_STATE_LOCKED,
  GREETER_STATE_HIDDEN,
  GREETER_STATE_POWER_MENU,
} greeter_state_t;

typedef enum {
  SESSION_STATE_STARTING,
  SESSION_STATE_ACTIVE,
  SESSION_STATE_LOCKED,
  SESSION_STATE_CLOSING,
  SESSION_STATE_DEAD,
} session_state_t;

typedef enum {
  POWER_ACTION_SHUTDOWN,
  POWER_ACTION_REBOOT,
  POWER_ACTION_SUSPEND,
  POWER_ACTION_HIBERNATE,
} power_action_t;

typedef struct {
  uid_t uid;
  gid_t gid;
  char username[PLEXY_DM_MAX_USERNAME];
  char realname[PLEXY_DM_MAX_REALNAME];
  char homedir[PLEXY_DM_MAX_PATH];
  char shell[PLEXY_DM_MAX_PATH];
  char avatar_path[PLEXY_DM_MAX_PATH];
  bool has_avatar;
  time_t last_login;
} plexy_dm_user_t;

typedef struct {
  int id;
  session_state_t state;
  uid_t uid;
  char username[PLEXY_DM_MAX_USERNAME];
  pid_t pid;
  int vt;
  char logind_session_id[64];
  void *pam_handle;
  time_t started_at;
  time_t locked_at;
} plexy_dm_session_t;

typedef enum {
  PLEXY_DM_OK = 0,
  PLEXY_DM_ERR_PAM_AUTH,
  PLEXY_DM_ERR_PAM_ACCOUNT,
  PLEXY_DM_ERR_PAM_SESSION,
  PLEXY_DM_ERR_PAM_PASSWORD,
  PLEXY_DM_ERR_VT_ALLOC,
  PLEXY_DM_ERR_VT_SWITCH,
  PLEXY_DM_ERR_FORK,
  PLEXY_DM_ERR_SETUID,
  PLEXY_DM_ERR_EXEC,
  PLEXY_DM_ERR_LOGIND,
  PLEXY_DM_ERR_DRM,
  PLEXY_DM_ERR_CONFIG,
  PLEXY_DM_ERR_MAX_SESSIONS,
} plexy_dm_error_t;

const char *plexy_dm_strerror(plexy_dm_error_t err);

#endif
