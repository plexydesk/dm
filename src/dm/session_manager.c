/*
 * Copyright (C) 2024-2026 Siraj Razick
 * SPDX-License-Identifier: AGPL-3.0-only
 */


#include "session_manager.h"

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <linux/vt.h>
#include <pwd.h>
#include <security/pam_appl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

struct plexy_session_mgr {
  plexy_dm_session_t sessions[PLEXY_DM_MAX_SESSIONS];
  int session_count;
  plexy_vt_manager_t *vt_mgr;
  plexy_logind_ctx_t *logind;
  const plexy_dm_config_t *cfg;
};

static plexy_dm_session_t *find_free_slot(plexy_session_mgr_t *mgr) {
  for (int i = 0; i < PLEXY_DM_MAX_SESSIONS; i++) {
    if (mgr->sessions[i].state == SESSION_STATE_DEAD &&
        mgr->sessions[i].pid == 0) {
      return &mgr->sessions[i];
    }
  }
  return NULL;
}

static void setup_session_env(const plexy_dm_session_t *sess, uid_t uid,
                              const char *pw_dir, const char *pw_name,
                              const char *pw_shell, int vt, char **pam_env,
                              const plexy_dm_config_t *cfg) {

  clearenv();

  setenv("HOME", pw_dir, 1);
  setenv("USER", pw_name, 1);
  setenv("LOGNAME", pw_name, 1);
  setenv("SHELL", pw_shell, 1);
  setenv("TERM", "linux", 1);

  setenv("PATH",
         "/usr/local/sbin:/usr/local/bin:"
         "/usr/sbin:/usr/bin:/sbin:/bin",
         1);

  char buf[512];
  snprintf(buf, sizeof(buf), "/run/user/%u", uid);
  setenv("XDG_RUNTIME_DIR", buf, 1);
  setenv("XDG_SESSION_TYPE", "wayland", 1);
  setenv("XDG_SEAT", "seat0", 1);

  snprintf(buf, sizeof(buf), "%d", vt);
  setenv("XDG_VTNR", buf, 1);

  if (sess->logind_session_id[0])
    setenv("XDG_SESSION_ID", sess->logind_session_id, 1);

  snprintf(buf, sizeof(buf), "unix:path=/run/user/%u/bus", uid);
  setenv("DBUS_SESSION_BUS_ADDRESS", buf, 1);

  {
    FILE *loc = fopen("/etc/default/locale", "r");
    int lang_set = 0;
    if (loc) {
      char line[256];
      while (fgets(line, sizeof(line), loc)) {
        if (strncmp(line, "LANG=", 5) == 0) {
          line[strcspn(line, "\n")] = '\0';
          setenv("LANG", line + 5, 1);
          lang_set = 1;
        }
      }
      fclose(loc);
    }
    if (!lang_set)
      setenv("LANG", "C.UTF-8", 1);
  }

  setenv("LIBSEAT_BACKEND", "logind", 1);
  setenv("PLEXY_NONINTERACTIVE", "1", 1);
  setenv("PLEXY_ALLOW_DM_SESSION", "1", 1);
  if (cfg->runtime_root[0]) {
    setenv("PLEXY_RUNTIME_ROOT", cfg->runtime_root, 1);

    char plugin_dir[PLEXY_DM_MAX_PATH];
    struct stat st;
    snprintf(plugin_dir, sizeof(plugin_dir), "%s/lib/libdecor/plugins-1",
             cfg->runtime_root);
    if (stat(plugin_dir, &st) == 0 && S_ISDIR(st.st_mode))
      setenv("LIBDECOR_PLUGIN_DIR", plugin_dir, 1);
  }

  if (pam_env) {
    for (char **env = pam_env; *env; env++)
      putenv(*env);
  }
}

static int drop_privileges(uid_t uid, gid_t gid, const char *username) {
  if (setgid(gid) < 0) {
    syslog(LOG_ERR, "plexy-dm: setgid(%u) failed: %s", gid, strerror(errno));
    return -1;
  }

  if (initgroups(username, gid) < 0) {
    syslog(LOG_ERR, "plexy-dm: initgroups('%s') failed: %s", username,
           strerror(errno));
    return -1;
  }

  if (setuid(uid) < 0) {
    syslog(LOG_ERR, "plexy-dm: setuid(%u) failed: %s", uid, strerror(errno));
    return -1;
  }

  if (getuid() != uid || geteuid() != uid) {
    syslog(LOG_ERR, "plexy-dm: privilege drop verification failed");
    return -1;
  }

  return 0;
}

static void ensure_runtime_dir(uid_t uid, gid_t gid) {
  char path[256];
  snprintf(path, sizeof(path), "/run/user/%u", uid);

  struct stat st;
  if (stat(path, &st) == 0)
    return;

  if (mkdir(path, 0700) < 0 && errno != EEXIST) {
    syslog(LOG_WARNING, "plexy-dm: mkdir(%s) failed: %s", path,
           strerror(errno));
    return;
  }

  chown(path, uid, gid);
  chmod(path, 0700);
}

plexy_session_mgr_t *session_mgr_create(plexy_vt_manager_t *vt_mgr,
                                        plexy_logind_ctx_t *logind,
                                        const plexy_dm_config_t *cfg) {
  plexy_session_mgr_t *mgr = calloc(1, sizeof(*mgr));
  if (!mgr)
    return NULL;

  mgr->vt_mgr = vt_mgr;
  mgr->logind = logind;
  mgr->cfg = cfg;

  for (int i = 0; i < PLEXY_DM_MAX_SESSIONS; i++) {
    mgr->sessions[i].id = i;
    mgr->sessions[i].state = SESSION_STATE_DEAD;
  }

  return mgr;
}

void session_mgr_destroy(plexy_session_mgr_t *mgr) {
  if (!mgr)
    return;

  for (int i = 0; i < PLEXY_DM_MAX_SESSIONS; i++) {
    if (mgr->sessions[i].pid > 0 &&
        mgr->sessions[i].state != SESSION_STATE_DEAD) {
      session_mgr_terminate(mgr, i);
    }
  }

  free(mgr);
}

static int launch_session_common(plexy_session_mgr_t *mgr, const char *username,
                                 const char *password, bool autologin,
                                 plexy_dm_error_t *err) {

  plexy_dm_session_t *sess = find_free_slot(mgr);
  if (!sess) {
    *err = PLEXY_DM_ERR_MAX_SESSIONS;
    return -1;
  }

  struct passwd *pw = getpwnam(username);
  if (!pw) {
    syslog(LOG_ERR, "plexy-dm: user '%s' not found", username);
    *err = PLEXY_DM_ERR_PAM_AUTH;
    return -1;
  }

  uid_t user_uid = pw->pw_uid;
  gid_t user_gid = pw->pw_gid;
  char user_dir[256], user_name[256], user_shell[256];
  snprintf(user_dir, sizeof(user_dir), "%s", pw->pw_dir);
  snprintf(user_name, sizeof(user_name), "%s", pw->pw_name);
  snprintf(user_shell, sizeof(user_shell), "%s", pw->pw_shell);

  plexy_pam_ctx_t *pam = plexy_pam_start(PLEXY_DM_PAM_SERVICE, username);
  if (!pam) {
    *err = PLEXY_DM_ERR_PAM_AUTH;
    return -1;
  }

  if (!autologin) {
    *err = plexy_pam_authenticate(pam, password);
    if (*err != PLEXY_DM_OK) {
      plexy_pam_end(pam);
      return -1;
    }
  }

  *err = plexy_pam_acct_mgmt(pam);
  if (*err != PLEXY_DM_OK) {
    plexy_pam_end(pam);
    return -1;
  }

  int vt = vt_manager_alloc_session_vt(mgr->vt_mgr);
  if (vt < 0) {
    plexy_pam_end(pam);
    *err = PLEXY_DM_ERR_VT_ALLOC;
    return -1;
  }

  ensure_runtime_dir(user_uid, user_gid);

  pid_t pid = fork();
  if (pid < 0) {
    syslog(LOG_ERR, "plexy-dm: fork failed: %s", strerror(errno));
    vt_manager_release_vt(mgr->vt_mgr, vt);
    plexy_pam_end(pam);
    *err = PLEXY_DM_ERR_FORK;
    return -1;
  }

  if (pid == 0) {

    setsid();

    {
      char logpath[256];
      snprintf(logpath, sizeof(logpath), "/tmp/plexy-session-%u.log", user_uid);
      int logfd = open(logpath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
      if (logfd >= 0) {
        dup2(logfd, STDOUT_FILENO);
        dup2(logfd, STDERR_FILENO);
        close(logfd);
      }
    }

    {
      plexy_pam_ctx_t *child_pam =
          plexy_pam_start(PLEXY_DM_PAM_SERVICE, username);
      if (child_pam) {
        pam_handle_t *pamh = plexy_pam_get_handle(child_pam);
        char tty_name[32];
        snprintf(tty_name, sizeof(tty_name), "/dev/tty%d", vt);
        pam_set_item(pamh, PAM_TTY, tty_name);

        char env_vtnr[32];
        snprintf(env_vtnr, sizeof(env_vtnr), "XDG_VTNR=%d", vt);
        pam_putenv(pamh, env_vtnr);
        pam_putenv(pamh, "XDG_SESSION_TYPE=wayland");
        pam_putenv(pamh, "XDG_SESSION_CLASS=user");
        pam_putenv(pamh, "XDG_SEAT=seat0");

        plexy_pam_open_session(child_pam);

        char **pam_env = plexy_pam_get_envlist(child_pam);

        setup_session_env(sess, user_uid, user_dir, user_name, user_shell, vt,
                          pam_env, mgr->cfg);

      } else {

        setup_session_env(sess, user_uid, user_dir, user_name, user_shell, vt,
                          NULL, mgr->cfg);
      }
    }

    {
      char ttypath[32];
      snprintf(ttypath, sizeof(ttypath), "/dev/tty%d", vt);
      int ttyfd = open(ttypath, O_RDWR);
      if (ttyfd >= 0) {
        if (ioctl(ttyfd, TIOCSCTTY, 0) < 0)
          syslog(LOG_WARNING, "plexy-dm: TIOCSCTTY(%s): %s", ttypath,
                 strerror(errno));

        dup2(ttyfd, STDIN_FILENO);

        int safe_fd = dup(ttyfd);
        if (safe_fd >= 0) {
          close(ttyfd);
          char fd_str[16];
          snprintf(fd_str, sizeof(fd_str), "%d", safe_fd);
          setenv("PLEXY_TTY_FD", fd_str, 1);
          syslog(LOG_INFO,
                 "plexy-dm: VT tty %s on fd %d "
                 "(PLEXY_TTY_FD=%s)",
                 ttypath, safe_fd, fd_str);
        } else {
          close(ttyfd);
        }
      } else {
        syslog(LOG_ERR, "plexy-dm: open(%s): %s", ttypath, strerror(errno));
      }
    }

    if (drop_privileges(user_uid, user_gid, user_name) < 0)
      _exit(127);

    if (chdir(user_dir) < 0)
      chdir("/");

    const char *session_exec = mgr->cfg->session_exec;
    char exec_path[PLEXY_DM_MAX_PATH];
    struct stat st;
    int found = 0;

    if (mgr->cfg->runtime_root[0]) {
      const char *search_dirs[] = {"libexec", "lib/plexydesk", "bin"};
      for (int d = 0; d < 3 && !found; d++) {
        snprintf(exec_path, sizeof(exec_path), "%s/%s/%s",
                 mgr->cfg->runtime_root, search_dirs[d], session_exec);
        if (stat(exec_path, &st) == 0 && (st.st_mode & S_IXUSR))
          found = 1;
      }
    }

    if (!found) {
      const char *fallback_roots[] = {"/opt/plexydesk/current", "/usr", NULL};
      const char *fallback_dirs[] = {"libexec", "lib/plexydesk", "bin", NULL};
      for (int r = 0; fallback_roots[r] && !found; r++) {
        for (int d = 0; fallback_dirs[d] && !found; d++) {
          snprintf(exec_path, sizeof(exec_path), "%s/%s/%s", fallback_roots[r],
                   fallback_dirs[d], session_exec);
          if (stat(exec_path, &st) == 0 && (st.st_mode & S_IXUSR))
            found = 1;
        }
      }
    }
    if (!found)
      snprintf(exec_path, sizeof(exec_path), "%s", session_exec);

    syslog(LOG_INFO, "plexy-dm: launching session for '%s' on VT%d: %s",
           username, vt, exec_path);

    execl(exec_path, exec_path, (char *)NULL);

    execlp(session_exec, session_exec, (char *)NULL);

    dprintf(STDERR_FILENO, "plexy-dm: exec '%s' failed: %s\n", exec_path,
            strerror(errno));
    syslog(LOG_ERR, "plexy-dm: exec '%s' failed: %s", exec_path,
           strerror(errno));
    _exit(127);
  }

  memset(sess, 0, sizeof(*sess));
  sess->id = (int)(sess - mgr->sessions);
  sess->state = SESSION_STATE_STARTING;
  sess->uid = user_uid;
  sess->pid = pid;
  sess->vt = vt;
  sess->pam_handle = pam;
  sess->started_at = time(NULL);
  snprintf(sess->username, sizeof(sess->username), "%s", username);

  mgr->session_count++;

  if (mgr->logind) {
    for (int retry = 0; retry < 20; retry++) {
      if (logind_create_session(mgr->logind, user_uid, pid, vt, "seat0",
                                "wayland", sess->logind_session_id,
                                sizeof(sess->logind_session_id)) == 0 &&
          sess->logind_session_id[0]) {
        syslog(LOG_INFO, "plexy-dm: logind session %s for pid %d",
               sess->logind_session_id, pid);
        break;
      }
      usleep(50000);
    }
    if (!sess->logind_session_id[0]) {
      syslog(LOG_WARNING,
             "plexy-dm: could not retrieve logind session id for pid %d", pid);
    }
  }

  vt_manager_switch_to(mgr->vt_mgr, vt);

  if (mgr->logind && sess->logind_session_id[0]) {
    if (logind_activate_session(mgr->logind, sess->logind_session_id) < 0) {
      syslog(LOG_WARNING, "plexy-dm: failed to activate logind session %s",
             sess->logind_session_id);
    } else {
      syslog(LOG_INFO, "plexy-dm: activated logind session %s",
             sess->logind_session_id);
    }
  }

  sess->state = SESSION_STATE_ACTIVE;
  syslog(LOG_INFO, "plexy-dm: session %d started for '%s' (pid=%d, VT%d)",
         sess->id, username, pid, vt);

  *err = PLEXY_DM_OK;
  return sess->id;
}

int session_mgr_launch(plexy_session_mgr_t *mgr, const char *username,
                       const char *password, plexy_dm_error_t *err) {
  return launch_session_common(mgr, username, password, false, err);
}

int session_mgr_autologin(plexy_session_mgr_t *mgr, const char *username,
                          plexy_dm_error_t *err) {
  return launch_session_common(mgr, username, NULL, true, err);
}

void session_mgr_terminate(plexy_session_mgr_t *mgr, int session_id) {
  if (!mgr || session_id < 0 || session_id >= PLEXY_DM_MAX_SESSIONS)
    return;

  plexy_dm_session_t *sess = &mgr->sessions[session_id];
  if (sess->pid <= 0 || sess->state == SESSION_STATE_DEAD)
    return;

  sess->state = SESSION_STATE_CLOSING;

  syslog(LOG_INFO, "plexy-dm: terminating session %d (pid=%d)", session_id,
         sess->pid);
  kill(-sess->pid, SIGTERM);

  for (int i = 0; i < 50; i++) {
    int status;
    pid_t w = waitpid(sess->pid, &status, WNOHANG);
    if (w > 0)
      goto cleanup;
    usleep(100000);
  }

  syslog(LOG_WARNING, "plexy-dm: force-killing session %d", session_id);
  kill(-sess->pid, SIGKILL);
  waitpid(sess->pid, NULL, 0);

cleanup:

  if (sess->pam_handle) {
    plexy_pam_end(sess->pam_handle);
    sess->pam_handle = NULL;
  }

  if (mgr->logind && sess->logind_session_id[0]) {
    logind_release_session(mgr->logind, sess->logind_session_id);
  }

  vt_manager_release_vt(mgr->vt_mgr, sess->vt);

  sess->state = SESSION_STATE_DEAD;
  sess->pid = 0;
  mgr->session_count--;
}

int session_mgr_reap_children(plexy_session_mgr_t *mgr) {
  if (!mgr)
    return 0;

  int reaped = 0;
  int status;
  pid_t pid;

  while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {

    for (int i = 0; i < PLEXY_DM_MAX_SESSIONS; i++) {
      plexy_dm_session_t *sess = &mgr->sessions[i];
      if (sess->pid != pid)
        continue;

      syslog(LOG_INFO,
             "plexy-dm: session %d exited (pid=%d, "
             "status=%d)",
             i, pid,
             WIFEXITED(status)     ? WEXITSTATUS(status)
             : WIFSIGNALED(status) ? -WTERMSIG(status)
                                   : -999);

      if (sess->pam_handle) {
        plexy_pam_end(sess->pam_handle);
        sess->pam_handle = NULL;
      }

      if (mgr->logind && sess->logind_session_id[0]) {
        logind_release_session(mgr->logind, sess->logind_session_id);
        sess->logind_session_id[0] = '\0';
      }

      vt_manager_release_vt(mgr->vt_mgr, sess->vt);

      sess->state = SESSION_STATE_DEAD;
      sess->pid = 0;
      mgr->session_count--;
      reaped++;
      break;
    }
  }

  return reaped;
}

const plexy_dm_session_t *session_mgr_get(const plexy_session_mgr_t *mgr,
                                          int session_id) {
  if (!mgr || session_id < 0 || session_id >= PLEXY_DM_MAX_SESSIONS)
    return NULL;
  if (mgr->sessions[session_id].state == SESSION_STATE_DEAD)
    return NULL;
  return &mgr->sessions[session_id];
}

int session_mgr_find_by_user(const plexy_session_mgr_t *mgr,
                             const char *username) {
  if (!mgr)
    return -1;
  for (int i = 0; i < PLEXY_DM_MAX_SESSIONS; i++) {
    if (mgr->sessions[i].state != SESSION_STATE_DEAD &&
        strcmp(mgr->sessions[i].username, username) == 0)
      return i;
  }
  return -1;
}

int session_mgr_active_count(const plexy_session_mgr_t *mgr) {
  return mgr ? mgr->session_count : 0;
}

int session_mgr_list_active(const plexy_session_mgr_t *mgr, int *ids_out,
                            int max_ids) {
  if (!mgr)
    return 0;
  int count = 0;
  for (int i = 0; i < PLEXY_DM_MAX_SESSIONS && count < max_ids; i++) {
    if (mgr->sessions[i].state != SESSION_STATE_DEAD &&
        mgr->sessions[i].pid > 0) {
      ids_out[count++] = i;
    }
  }
  return count;
}

void session_mgr_lock(plexy_session_mgr_t *mgr, int session_id) {
  if (!mgr || session_id < 0 || session_id >= PLEXY_DM_MAX_SESSIONS)
    return;
  plexy_dm_session_t *sess = &mgr->sessions[session_id];
  if (sess->state == SESSION_STATE_ACTIVE) {
    sess->state = SESSION_STATE_LOCKED;
    sess->locked_at = time(NULL);

    if (mgr->logind && sess->logind_session_id[0])
      logind_lock_session(mgr->logind, sess->logind_session_id);

    syslog(LOG_INFO, "plexy-dm: session %d locked", session_id);
  }
}

void session_mgr_unlock(plexy_session_mgr_t *mgr, int session_id) {
  if (!mgr || session_id < 0 || session_id >= PLEXY_DM_MAX_SESSIONS)
    return;
  plexy_dm_session_t *sess = &mgr->sessions[session_id];
  if (sess->state == SESSION_STATE_LOCKED) {
    sess->state = SESSION_STATE_ACTIVE;
    sess->locked_at = 0;

    if (mgr->logind && sess->logind_session_id[0])
      logind_unlock_session(mgr->logind, sess->logind_session_id);

    syslog(LOG_INFO, "plexy-dm: session %d unlocked", session_id);
  }
}

int session_mgr_activate(plexy_session_mgr_t *mgr, int session_id) {
  if (!mgr || session_id < 0 || session_id >= PLEXY_DM_MAX_SESSIONS)
    return -1;

  plexy_dm_session_t *sess = &mgr->sessions[session_id];
  if (sess->state == SESSION_STATE_DEAD)
    return -1;

  return vt_manager_switch_to(mgr->vt_mgr, sess->vt);
}
