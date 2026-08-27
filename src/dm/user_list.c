/*
 * Copyright (C) 2024-2026 Siraj Razick
 * SPDX-License-Identifier: AGPL-3.0-only
 */


#include "user_list.h"

#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <syslog.h>
#include <utmpx.h>

static const char *nologin_shells[] = {
    "/usr/sbin/nologin", "/sbin/nologin", "/bin/false", "/usr/bin/false", NULL,
};

static bool is_nologin_shell(const char *shell) {
  if (!shell || shell[0] == '\0')
    return true;

  for (const char **s = nologin_shells; *s; s++) {
    if (strcmp(shell, *s) == 0)
      return true;
  }
  return false;
}

static void load_avatar(plexy_dm_user_t *user) {
  user->has_avatar = false;

  char path[PLEXY_DM_MAX_PATH];
  snprintf(path, sizeof(path), "%s/.face", user->homedir);

  struct stat st;
  if (stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0) {
    snprintf(user->avatar_path, sizeof(user->avatar_path), "%s", path);
    user->has_avatar = true;
    return;
  }

  snprintf(path, sizeof(path), "/var/lib/AccountsService/icons/%s",
           user->username);
  if (stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0) {
    snprintf(user->avatar_path, sizeof(user->avatar_path), "%s", path);
    user->has_avatar = true;
    return;
  }

  user->avatar_path[0] = '\0';
}

static time_t get_last_login(const char *username) {
  time_t last = 0;

  setutxent();
  struct utmpx *ut;
  while ((ut = getutxent()) != NULL) {
    if (ut->ut_type == USER_PROCESS &&
        strncmp(ut->ut_user, username, sizeof(ut->ut_user)) == 0) {
      if (ut->ut_tv.tv_sec > last)
        last = ut->ut_tv.tv_sec;
    }
  }
  endutxent();

  return last;
}

static int compare_users(const void *a, const void *b) {
  const plexy_dm_user_t *ua = a;
  const plexy_dm_user_t *ub = b;

  if (ua->last_login != ub->last_login)
    return (ub->last_login > ua->last_login) ? 1 : -1;

  return strcmp(ua->username, ub->username);
}

int user_list_enumerate(plexy_dm_user_t *users, int max_users, uid_t min_uid,
                        uid_t max_uid) {
  int count = 0;

  setpwent();
  struct passwd *pw;
  while ((pw = getpwent()) != NULL && count < max_users) {

    if (pw->pw_uid < min_uid || pw->pw_uid > max_uid)
      continue;

    if (is_nologin_shell(pw->pw_shell))
      continue;

    struct stat st;
    if (stat(pw->pw_dir, &st) != 0 || !S_ISDIR(st.st_mode))
      continue;

    plexy_dm_user_t *u = &users[count];
    u->uid = pw->pw_uid;
    u->gid = pw->pw_gid;
    snprintf(u->username, sizeof(u->username), "%s", pw->pw_name);
    snprintf(u->homedir, sizeof(u->homedir), "%s", pw->pw_dir);
    snprintf(u->shell, sizeof(u->shell), "%s", pw->pw_shell);

    if (pw->pw_gecos && pw->pw_gecos[0]) {
      const char *comma = strchr(pw->pw_gecos, ',');
      size_t len =
          comma ? (size_t)(comma - pw->pw_gecos) : strlen(pw->pw_gecos);
      if (len >= sizeof(u->realname))
        len = sizeof(u->realname) - 1;
      memcpy(u->realname, pw->pw_gecos, len);
      u->realname[len] = '\0';
    } else {
      snprintf(u->realname, sizeof(u->realname), "%s", pw->pw_name);
    }

    load_avatar(u);
    u->last_login = get_last_login(pw->pw_name);

    count++;
  }
  endpwent();

  if (count > 1)
    qsort(users, (size_t)count, sizeof(users[0]), compare_users);

  syslog(LOG_INFO, "plexy-dm: enumerated %d loginable users", count);
  return count;
}

const plexy_dm_user_t *user_list_find(const plexy_dm_user_t *users, int count,
                                      const char *username) {
  for (int i = 0; i < count; i++) {
    if (strcmp(users[i].username, username) == 0)
      return &users[i];
  }
  return NULL;
}

void user_list_refresh_login_time(plexy_dm_user_t *user) {
  user->last_login = get_last_login(user->username);
}
