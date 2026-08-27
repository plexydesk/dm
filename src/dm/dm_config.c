/*
 * Copyright (C) 2024-2026 Siraj Razick
 * SPDX-License-Identifier: AGPL-3.0-only
 */


#include "dm_config.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <syslog.h>
#include <unistd.h>

void dm_config_defaults(plexy_dm_config_t *cfg) {
  memset(cfg, 0, sizeof(*cfg));

  snprintf(cfg->default_session, sizeof(cfg->default_session), "plexydesk");
  cfg->autologin_user[0] = '\0';
  cfg->autologin_delay = PLEXY_DM_DEFAULT_AUTOLOGIN_DELAY;
  cfg->greeter_vt = PLEXY_DM_DEFAULT_GREETER_VT;
  cfg->numlock_on = true;

  cfg->min_uid = PLEXY_DM_DEFAULT_MIN_UID;
  cfg->max_uid = PLEXY_DM_DEFAULT_MAX_UID;
  cfg->allow_root = false;

  cfg->idle_timeout = PLEXY_DM_DEFAULT_IDLE_TIMEOUT;
  cfg->lock_on_suspend = true;

  snprintf(cfg->background_path, sizeof(cfg->background_path), "%s",
           PLEXY_DM_DEFAULT_BACKGROUND);
  cfg->video_path[0] = '\0';
  cfg->video_mute = true;
  snprintf(cfg->theme, sizeof(cfg->theme), "2d");
  cfg->shell_config_path[0] = '\0';
  cfg->use_3d_theme = false;
  cfg->show_user_list = true;
  cfg->show_hostname = true;
  cfg->clock_24h = false;

  cfg->weather_enabled = true;
  cfg->weather_location[0] = '\0';
  cfg->weather_refresh_minutes = 30;

  snprintf(cfg->session_exec, sizeof(cfg->session_exec), "%s",
           PLEXY_DM_SESSION_EXEC);
  cfg->runtime_root[0] = '\0';
}

static char *strip(char *s) {
  while (*s && isspace((unsigned char)*s))
    s++;
  size_t len = strlen(s);
  if (len == 0)
    return s;
  char *end = s + len - 1;
  while (end > s && isspace((unsigned char)*end))
    *end-- = '\0';
  return s;
}

static void strip_inline_comment(char *s) {
  bool quoted = false;
  char quote = '\0';

  for (char *p = s; *p; ++p) {
    if ((*p == '"' || *p == '\'') && (!quoted || *p == quote)) {
      quoted = !quoted;
      quote = quoted ? *p : '\0';
      continue;
    }
    if (!quoted && (*p == '#' || *p == ';')) {
      *p = '\0';
      return;
    }
  }
}

static char *strip_quotes(char *s) {
  size_t len = strlen(s);
  if (len >= 2 && ((s[0] == '"' && s[len - 1] == '"') ||
                   (s[0] == '\'' && s[len - 1] == '\''))) {
    s[len - 1] = '\0';
    s++;
  }
  return s;
}

static bool str_to_bool(const char *val) {
  return (strcasecmp(val, "true") == 0 || strcasecmp(val, "yes") == 0 ||
          strcasecmp(val, "1") == 0 || strcasecmp(val, "on") == 0);
}

typedef enum {
  SECTION_NONE,
  SECTION_GENERAL,
  SECTION_SECURITY,
  SECTION_LOCK,
  SECTION_APPEARANCE,
  SECTION_WEATHER,
  SECTION_SESSION,
} config_section_t;

static config_section_t parse_section(const char *name) {
  if (strcasecmp(name, "General") == 0)
    return SECTION_GENERAL;
  if (strcasecmp(name, "Security") == 0)
    return SECTION_SECURITY;
  if (strcasecmp(name, "Lock") == 0)
    return SECTION_LOCK;
  if (strcasecmp(name, "Appearance") == 0)
    return SECTION_APPEARANCE;
  if (strcasecmp(name, "Weather") == 0)
    return SECTION_WEATHER;
  if (strcasecmp(name, "Session") == 0)
    return SECTION_SESSION;
  return SECTION_NONE;
}

static void apply_value(plexy_dm_config_t *cfg, config_section_t section,
                        const char *key, const char *val) {
  switch (section) {
  case SECTION_GENERAL:
    if (strcasecmp(key, "DefaultSession") == 0)
      snprintf(cfg->default_session, sizeof(cfg->default_session), "%s", val);
    else if (strcasecmp(key, "AutoLoginUser") == 0)
      snprintf(cfg->autologin_user, sizeof(cfg->autologin_user), "%s", val);
    else if (strcasecmp(key, "AutoLoginDelay") == 0)
      cfg->autologin_delay = atoi(val);
    else if (strcasecmp(key, "GreeterVT") == 0)
      cfg->greeter_vt = atoi(val);
    else if (strcasecmp(key, "NumLock") == 0)
      cfg->numlock_on = str_to_bool(val);
    break;

  case SECTION_SECURITY:
    if (strcasecmp(key, "MinUID") == 0)
      cfg->min_uid = (uid_t)strtoul(val, NULL, 10);
    else if (strcasecmp(key, "MaxUID") == 0)
      cfg->max_uid = (uid_t)strtoul(val, NULL, 10);
    else if (strcasecmp(key, "AllowRoot") == 0)
      cfg->allow_root = str_to_bool(val);
    break;

  case SECTION_LOCK:
    if (strcasecmp(key, "IdleTimeout") == 0)
      cfg->idle_timeout = atoi(val);
    else if (strcasecmp(key, "LockOnSuspend") == 0)
      cfg->lock_on_suspend = str_to_bool(val);
    break;

  case SECTION_APPEARANCE:
    if (strcasecmp(key, "Background") == 0)
      snprintf(cfg->background_path, sizeof(cfg->background_path), "%s", val);
    else if (strcasecmp(key, "Theme") == 0)
      snprintf(cfg->theme, sizeof(cfg->theme), "%s", val);
    else if (strcasecmp(key, "Enable3DLoginTheme") == 0 ||
             strcasecmp(key, "Use3DTheme") == 0)
      cfg->use_3d_theme = str_to_bool(val);
    else if (strcasecmp(key, "ShowUserList") == 0)
      cfg->show_user_list = str_to_bool(val);
    else if (strcasecmp(key, "ShowHostname") == 0)
      cfg->show_hostname = str_to_bool(val);
    else if (strcasecmp(key, "Clock24h") == 0)
      cfg->clock_24h = str_to_bool(val);
    break;

  case SECTION_WEATHER:
    if (strcasecmp(key, "Enabled") == 0)
      cfg->weather_enabled = str_to_bool(val);
    else if (strcasecmp(key, "Location") == 0)
      snprintf(cfg->weather_location, sizeof(cfg->weather_location), "%s", val);
    else if (strcasecmp(key, "RefreshMinutes") == 0)
      cfg->weather_refresh_minutes = atoi(val);
    break;

  case SECTION_SESSION:
    if (strcasecmp(key, "SessionExec") == 0)
      snprintf(cfg->session_exec, sizeof(cfg->session_exec), "%s", val);
    else if (strcasecmp(key, "RuntimeRoot") == 0)
      snprintf(cfg->runtime_root, sizeof(cfg->runtime_root), "%s", val);
    break;

  case SECTION_NONE:
    break;
  }
}

static bool file_readable(const char *path) {
  return path && path[0] && access(path, R_OK) == 0;
}

static void copy_parent_dir(const char *path, char *out, size_t out_sz) {
  if (!out || out_sz == 0)
    return;

  out[0] = '\0';
  if (!path || !path[0])
    return;

  snprintf(out, out_sz, "%s", path);
  char *slash = strrchr(out, '/');
  if (!slash) {
    snprintf(out, out_sz, ".");
  } else if (slash == out) {
    slash[1] = '\0';
  } else {
    *slash = '\0';
  }
}

static void resolve_shell_path(const plexy_dm_config_t *cfg,
                               const char *config_path, const char *value,
                               char *out, size_t out_sz) {
  if (!out || out_sz == 0 || !value || !value[0])
    return;

  if (value[0] == '/') {
    snprintf(out, out_sz, "%s", value);
    return;
  }

  char dir[PLEXY_DM_MAX_PATH];
  char candidate[PLEXY_DM_MAX_PATH];

  copy_parent_dir(config_path, dir, sizeof(dir));
  if (dir[0]) {
    snprintf(candidate, sizeof(candidate), "%s/%s", dir, value);
    if (file_readable(candidate)) {
      snprintf(out, out_sz, "%s", candidate);
      return;
    }
  }

  const char *roots[] = {cfg && cfg->runtime_root[0] ? cfg->runtime_root : "",
                         "/opt/plexydesk/current", "/usr", NULL};

  for (const char **r = roots; *r; ++r) {
    if (!(*r)[0])
      continue;
    snprintf(candidate, sizeof(candidate), "%s/share/plexydesk/%s", *r, value);
    if (file_readable(candidate)) {
      snprintf(out, out_sz, "%s", candidate);
      return;
    }
  }

  snprintf(out, out_sz, "%s", value);
}

static const char *find_shell_config(const plexy_dm_config_t *cfg, char *out,
                                     size_t out_sz) {
  const char *env = getenv("PLEXY_CONFIG_FILE");
  if (file_readable(env)) {
    snprintf(out, out_sz, "%s", env);
    return out;
  }

  char candidate[PLEXY_DM_MAX_PATH];

  if (cfg && cfg->runtime_root[0]) {
    snprintf(candidate, sizeof(candidate), "%s/plexyshell.conf",
             cfg->runtime_root);
    if (file_readable(candidate)) {
      snprintf(out, out_sz, "%s", candidate);
      return out;
    }
  }

  const char *paths[] = {"./plexyshell.conf",
                         "/opt/plexydesk/current/plexyshell.conf",
                         "/etc/plexyshell/plexyshell.conf", NULL};

  for (const char **p = paths; *p; ++p) {
    if (file_readable(*p)) {
      snprintf(out, out_sz, "%s", *p);
      return out;
    }
  }

  const char *xdg_config = getenv("XDG_CONFIG_HOME");
  if (xdg_config && xdg_config[0]) {
    snprintf(candidate, sizeof(candidate), "%s/plexyshell/plexyshell.conf",
             xdg_config);
    if (file_readable(candidate)) {
      snprintf(out, out_sz, "%s", candidate);
      return out;
    }
  }

  const char *home = getenv("HOME");
  if (home && home[0]) {
    snprintf(candidate, sizeof(candidate),
             "%s/.config/plexyshell/plexyshell.conf", home);
    if (file_readable(candidate)) {
      snprintf(out, out_sz, "%s", candidate);
      return out;
    }
  }

  return NULL;
}

static void apply_shell_value(plexy_dm_config_t *cfg, const char *section,
                              const char *key, const char *val) {
  if (strcasecmp(section, "background") == 0) {
    if (strcasecmp(key, "wallpaper_path") == 0) {
      resolve_shell_path(cfg, cfg->shell_config_path, val, cfg->background_path,
                         sizeof(cfg->background_path));
    } else if (strcasecmp(key, "wallpaper_video_path") == 0) {
      resolve_shell_path(cfg, cfg->shell_config_path, val, cfg->video_path,
                         sizeof(cfg->video_path));
    } else if (strcasecmp(key, "wallpaper_video_mute") == 0) {
      cfg->video_mute = str_to_bool(val);
    }
    return;
  }

  if (strcasecmp(section, "weather") == 0) {
    if (strcasecmp(key, "enabled") == 0) {
      cfg->weather_enabled = str_to_bool(val);
    } else if (strcasecmp(key, "location") == 0) {
      snprintf(cfg->weather_location, sizeof(cfg->weather_location), "%s", val);
    } else if (strcasecmp(key, "refresh_minutes") == 0) {
      cfg->weather_refresh_minutes = atoi(val);
    }
    return;
  }

  if (strcasecmp(section, "display_manager") != 0 &&
      strcasecmp(section, "plexy-dm") != 0 &&
      strcasecmp(section, "greeter") != 0) {
    return;
  }

  if (strcasecmp(key, "enable_3d_login_theme") == 0 ||
      strcasecmp(key, "login_theme_3d") == 0 ||
      strcasecmp(key, "use_3d_theme") == 0) {
    cfg->use_3d_theme = str_to_bool(val);
    snprintf(cfg->theme, sizeof(cfg->theme), "%s",
             cfg->use_3d_theme ? "3d" : "2d");
  } else if (strcasecmp(key, "login_theme") == 0 ||
             strcasecmp(key, "theme") == 0) {
    if (strcasecmp(val, "3d") == 0 || strcasecmp(val, "cube") == 0) {
      cfg->use_3d_theme = true;
      snprintf(cfg->theme, sizeof(cfg->theme), "3d");
    } else if (strcasecmp(val, "2d") == 0 || strcasecmp(val, "default") == 0) {
      cfg->use_3d_theme = false;
      snprintf(cfg->theme, sizeof(cfg->theme), "2d");
    }
  }
}

static void load_shell_config(plexy_dm_config_t *cfg) {
  char path[PLEXY_DM_MAX_PATH];
  if (!find_shell_config(cfg, path, sizeof(path)))
    return;

  FILE *f = fopen(path, "r");
  if (!f)
    return;

  snprintf(cfg->shell_config_path, sizeof(cfg->shell_config_path), "%s", path);

  char section[128] = "";
  char line[1024];
  while (fgets(line, sizeof(line), f)) {
    char *s = strip(line);
    if (s[0] == '\0' || s[0] == '#' || s[0] == ';')
      continue;

    if (s[0] == '[') {
      char *end = strchr(s, ']');
      if (!end)
        continue;
      *end = '\0';
      snprintf(section, sizeof(section), "%s", strip(s + 1));
      continue;
    }

    char *eq = strchr(s, '=');
    if (!eq)
      continue;

    *eq = '\0';
    char *key = strip(s);
    char *val = strip(eq + 1);
    strip_inline_comment(val);
    val = strip(strip_quotes(strip(val)));
    apply_shell_value(cfg, section, key, val);
  }

  fclose(f);
}

int dm_config_load(plexy_dm_config_t *cfg, const char *path) {
  dm_config_defaults(cfg);

  if (!path || path[0] == '\0')
    path = PLEXY_DM_CONFIG_PATH;

  FILE *f = fopen(path, "r");
  if (!f) {
    syslog(LOG_WARNING, "plexy-dm: config not found at '%s', using defaults",
           path);
    load_shell_config(cfg);
    return 0;
  }

  config_section_t current_section = SECTION_NONE;
  char line[1024];
  int lineno = 0;

  while (fgets(line, sizeof(line), f)) {
    lineno++;
    char *s = strip(line);

    if (s[0] == '\0' || s[0] == '#' || s[0] == ';')
      continue;

    if (s[0] == '[') {
      char *end = strchr(s, ']');
      if (!end) {
        syslog(LOG_WARNING, "plexy-dm: config:%d: malformed section", lineno);
        continue;
      }
      *end = '\0';
      current_section = parse_section(s + 1);
      if (current_section == SECTION_NONE) {
        syslog(LOG_WARNING, "plexy-dm: config:%d: unknown section '%s'", lineno,
               s + 1);
      }
      continue;
    }

    char *eq = strchr(s, '=');
    if (!eq) {
      syslog(LOG_WARNING, "plexy-dm: config:%d: missing '='", lineno);
      continue;
    }

    *eq = '\0';
    char *key = strip(s);
    char *val = strip(eq + 1);
    strip_inline_comment(val);
    val = strip(strip_quotes(strip(val)));

    apply_value(cfg, current_section, key, val);
  }

  fclose(f);
  if (strcasecmp(cfg->theme, "3d") == 0 ||
      strcasecmp(cfg->theme, "cube") == 0) {
    cfg->use_3d_theme = true;
  } else if (strcasecmp(cfg->theme, "2d") == 0 ||
             strcasecmp(cfg->theme, "default") == 0) {
    cfg->use_3d_theme = false;
  }
  load_shell_config(cfg);
  syslog(LOG_INFO, "plexy-dm: loaded config from '%s'", path);
  return 0;
}

void dm_config_dump(const plexy_dm_config_t *cfg) {
  syslog(LOG_DEBUG,
         "plexy-dm config: session=%s autologin='%s' "
         "greeter_vt=%d uid=%u-%u idle=%ds bg=%s video=%s",
         cfg->default_session, cfg->autologin_user, cfg->greeter_vt,
         cfg->min_uid, cfg->max_uid, cfg->idle_timeout, cfg->background_path,
         cfg->video_path[0] ? cfg->video_path : "(none)");
}
