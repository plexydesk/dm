/*
 * Copyright (C) 2024-2026 Siraj Razick
 * SPDX-License-Identifier: AGPL-3.0-only
 */


#ifndef PLEXY_DM_CONFIG_H
#define PLEXY_DM_CONFIG_H

#include "plexy_dm.h"

typedef struct {

  char default_session[256];
  char autologin_user[PLEXY_DM_MAX_USERNAME];
  int autologin_delay;
  int greeter_vt;
  bool numlock_on;

  uid_t min_uid;
  uid_t max_uid;
  bool allow_root;

  int idle_timeout;
  bool lock_on_suspend;

  char background_path[PLEXY_DM_MAX_PATH];
  char video_path[PLEXY_DM_MAX_PATH];
  bool video_mute;
  char theme[256];
  char shell_config_path[PLEXY_DM_MAX_PATH];
  bool use_3d_theme;
  bool show_user_list;
  bool show_hostname;
  bool clock_24h;

  bool weather_enabled;
  char weather_location[256];
  int weather_refresh_minutes;

  char session_exec[PLEXY_DM_MAX_PATH];
  char runtime_root[PLEXY_DM_MAX_PATH];
} plexy_dm_config_t;

int dm_config_load(plexy_dm_config_t *cfg, const char *path);

void dm_config_defaults(plexy_dm_config_t *cfg);

void dm_config_dump(const plexy_dm_config_t *cfg);

#endif
