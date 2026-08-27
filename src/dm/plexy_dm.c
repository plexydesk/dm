/*
 * Copyright (C) 2024-2026 Siraj Razick
 * SPDX-License-Identifier: AGPL-3.0-only
 */


#include "plexy_dm.h"
#include "dm_config.h"
#include "greeter.h"
#include "greeter_ui.h"
#include "idle_monitor.h"
#include "lock_screen.h"
#include "logind_dbus.h"
#include "pam_auth.h"
#include "session_manager.h"
#include "user_list.h"
#include "vt_manager.h"

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <syslog.h>
#include <termios.h>
#include <unistd.h>

#ifdef HAVE_SYSTEMD
#include <systemd/sd-daemon.h>
#else
#define sd_notify(unset, state) ((void)0)
#endif

#define MAX_EPOLL_EVENTS 16

typedef struct {
  plexy_dm_config_t cfg;
  plexy_vt_manager_t *vt_mgr;
  plexy_logind_ctx_t *logind;
  plexy_session_mgr_t *sess_mgr;
  plexy_greeter_t *greeter;
  plexy_lock_mgr_t *lock_mgr;
  plexy_idle_monitor_t *idle_mon;

  int epoll_fd;
  int signal_fd;
  int inhibit_fd;
  bool running;
  bool greeter_visible;

  pid_t debug_term_pid;
  int debug_term_vt;
} plexy_dm_t;

static plexy_dm_t dm = {0};

static void release_logind_inhibitor(plexy_dm_t *dm_ctx) {
  if (dm_ctx->inhibit_fd >= 0) {
    close(dm_ctx->inhibit_fd);
    dm_ctx->inhibit_fd = -1;
  }
}

static void reacquire_logind_inhibitor(plexy_dm_t *dm_ctx) {
  if (!dm_ctx->logind || dm_ctx->inhibit_fd >= 0)
    return;

  dm_ctx->inhibit_fd =
      logind_inhibit(dm_ctx->logind, "sleep:shutdown", "plexy-dm",
                     "Display manager is active", "delay");
}

static int setup_signals(void) {
  sigset_t mask;
  sigemptyset(&mask);
  sigaddset(&mask, SIGTERM);
  sigaddset(&mask, SIGINT);
  sigaddset(&mask, SIGCHLD);
  sigaddset(&mask, SIGHUP);
  sigprocmask(SIG_BLOCK, &mask, NULL);

  int sfd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
  if (sfd < 0) {
    syslog(LOG_ERR, "plexy-dm: signalfd failed: %s", strerror(errno));
    return -1;
  }
  return sfd;
}

static void handle_signals(plexy_dm_t *dm_ctx) {
  struct signalfd_siginfo si;
  while (read(dm_ctx->signal_fd, &si, sizeof(si)) == sizeof(si)) {
    switch (si.ssi_signo) {
    case SIGTERM:
    case SIGINT:
      syslog(LOG_INFO, "plexy-dm: received signal %d, shutting down",
             si.ssi_signo);
      dm_ctx->running = false;
      break;

    case SIGCHLD: {

      if (dm_ctx->debug_term_pid > 0) {
        int wstatus;
        pid_t pid = waitpid(dm_ctx->debug_term_pid, &wstatus, WNOHANG);
        if (pid == dm_ctx->debug_term_pid) {
          syslog(LOG_INFO, "plexy-dm: debug terminal exited");
          if (dm_ctx->debug_term_vt > 0) {
            vt_manager_release_vt(dm_ctx->vt_mgr, dm_ctx->debug_term_vt);
            dm_ctx->debug_term_vt = 0;
          }
          dm_ctx->debug_term_pid = 0;
          vt_manager_switch_to_greeter(dm_ctx->vt_mgr);
          greeter_resume(dm_ctx->greeter);
          greeter_request_frame(dm_ctx->greeter);
          dm_ctx->greeter_visible = true;
        }
      }
      if (dm_ctx->sess_mgr) {
        int reaped = session_mgr_reap_children(dm_ctx->sess_mgr);
        if (reaped > 0 && session_mgr_active_count(dm_ctx->sess_mgr) == 0 &&
            dm_ctx->debug_term_pid == 0) {
          syslog(LOG_INFO, "plexy-dm: all sessions ended, showing greeter");
          vt_manager_switch_to_greeter(dm_ctx->vt_mgr);
          greeter_resume(dm_ctx->greeter);
          greeter_set_state(dm_ctx->greeter, GREETER_STATE_USER_SELECT);
          dm_ctx->greeter_visible = true;
        }
      }
      break;
    }

    case SIGHUP:
      syslog(LOG_INFO, "plexy-dm: reloading configuration");
      dm_config_load(&dm_ctx->cfg, PLEXY_DM_CONFIG_PATH);
      break;
    }
  }
}

static int epoll_add(int epoll_fd, int fd, uint32_t events, void *data) {
  if (fd < 0)
    return -1;
  struct epoll_event ev = {
      .events = events,
      .data.ptr = data,
  };
  return epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev);
}

static pid_t launch_debug_term(plexy_dm_t *dm_ctx) {
  int vt = vt_manager_alloc_session_vt(dm_ctx->vt_mgr);
  if (vt < 0) {
    syslog(LOG_WARNING, "plexy-dm: debug term: no free VT");
    return -1;
  }

  greeter_suspend(dm_ctx->greeter);
  dm_ctx->greeter_visible = false;

  vt_manager_switch_to(dm_ctx->vt_mgr, vt);

  pid_t pid = fork();
  if (pid < 0) {
    syslog(LOG_ERR, "plexy-dm: debug term fork: %s", strerror(errno));
    vt_manager_release_vt(dm_ctx->vt_mgr, vt);
    greeter_resume(dm_ctx->greeter);
    dm_ctx->greeter_visible = true;
    return -1;
  }

  if (pid == 0) {

    char vtdev[32];
    snprintf(vtdev, sizeof(vtdev), "/dev/tty%d", vt);

    int fd = open(vtdev, O_RDWR | O_NOCTTY);
    if (fd < 0) {

      fd = open(vtdev, O_RDWR);
    }
    if (fd >= 0) {
      dup2(fd, STDIN_FILENO);
      dup2(fd, STDOUT_FILENO);
      dup2(fd, STDERR_FILENO);
      if (fd > STDERR_FILENO)
        close(fd);
    }

    setsid();
    ioctl(STDIN_FILENO, TIOCSCTTY, 1);

    fcntl(STDIN_FILENO, F_SETFD, 0);
    fcntl(STDOUT_FILENO, F_SETFD, 0);
    fcntl(STDERR_FILENO, F_SETFD, 0);

    execl("/bin/login", "login", NULL);

    execl("/bin/sh", "sh", NULL);
    _exit(127);
  }

  dm_ctx->debug_term_pid = pid;
  dm_ctx->debug_term_vt = vt;
  syslog(LOG_INFO, "plexy-dm: debug terminal pid=%d vt=%d", pid, vt);
  return pid;
}

enum {
  ESRC_SIGNAL = 1,
  ESRC_DRM,
  ESRC_INPUT,
  ESRC_TIMER,
  ESRC_DBUS,
  ESRC_VT_SIGNAL,
  ESRC_IDLE,
};

static void on_idle_timeout(void *data);

static int on_login(const char *username, const char *password, void *data) {
  plexy_dm_t *d = data;

  int existing = session_mgr_find_by_user(d->sess_mgr, username);
  if (existing >= 0) {
    const plexy_dm_session_t *sess = session_mgr_get(d->sess_mgr, existing);
    if (sess && sess->pid > 0) {

      char procpath[64];
      snprintf(procpath, sizeof(procpath), "/proc/%d/task/%d/children",
               sess->pid, sess->pid);
      FILE *f = fopen(procpath, "r");
      int has_children = 0;
      if (f) {
        int ch = fgetc(f);
        has_children = (ch != EOF);
        fclose(f);
      }
      if (has_children) {

        syslog(LOG_INFO, "plexy-dm: switching to existing session for '%s'",
               username);
        vt_manager_set_lock(d->vt_mgr, false);
        session_mgr_activate(d->sess_mgr, existing);
        greeter_suspend(d->greeter);
        d->greeter_visible = false;
        return 0;
      }

      syslog(LOG_INFO, "plexy-dm: cleaning up stale session for '%s' (pid=%d)",
             username, sess->pid);
      session_mgr_terminate(d->sess_mgr, existing);
    }
  }

  plexy_dm_error_t err;

  vt_manager_set_lock(d->vt_mgr, false);

  greeter_suspend(d->greeter);

  int sid = session_mgr_launch(d->sess_mgr, username, password, &err);
  if (sid < 0) {
    syslog(LOG_WARNING, "plexy-dm: login failed for '%s': %s", username,
           plexy_dm_strerror(err));
    greeter_resume(d->greeter);
    return -1;
  }

  d->greeter_visible = false;

  if (d->idle_mon)
    idle_monitor_set_enabled(d->idle_mon, false);

  return 0;
}

static void on_switch_user(void *data) {
  plexy_dm_t *d = data;
  vt_manager_switch_to_greeter(d->vt_mgr);
  greeter_resume(d->greeter);
  greeter_set_state(d->greeter, GREETER_STATE_USER_SELECT);
  d->greeter_visible = true;
}

static void on_power_action(power_action_t action, void *data) {
  plexy_dm_t *d = data;
  int rc;

  syslog(LOG_INFO, "plexy-dm: power action %d requested", action);

  if (!d->logind)
    return;

  if ((action == POWER_ACTION_SUSPEND || action == POWER_ACTION_HIBERNATE) &&
      d->cfg.lock_on_suspend) {
    on_idle_timeout(data);
  }

  release_logind_inhibitor(d);
  rc = logind_power_action(d->logind, action);
  if (rc < 0) {
    syslog(LOG_WARNING, "plexy-dm: power action %d failed", action);
    reacquire_logind_inhibitor(d);
  }
}

static void on_unlock(const char *username, const char *password, void *data) {
  plexy_dm_t *d = data;

  int sid = session_mgr_find_by_user(d->sess_mgr, username);
  if (sid < 0) {
    syslog(LOG_WARNING, "plexy-dm: unlock: no session for '%s'", username);
    return;
  }

  plexy_dm_error_t err = lock_mgr_try_unlock(d->lock_mgr, sid, password);
  if (err != PLEXY_DM_OK) {
    syslog(LOG_WARNING, "plexy-dm: unlock failed for '%s': %s", username,
           plexy_dm_strerror(err));

    if (d->greeter)
      greeter_show_error(d->greeter, "Incorrect password");
  }
}

static void on_session_unlocked(int session_id, void *data) {
  plexy_dm_t *d = data;

  session_mgr_unlock(d->sess_mgr, session_id);
  session_mgr_activate(d->sess_mgr, session_id);

  vt_manager_set_lock(d->vt_mgr, false);

  greeter_suspend(d->greeter);
  d->greeter_visible = false;

  syslog(LOG_INFO, "plexy-dm: session %d unlocked, switching back", session_id);
}

static void on_idle_timeout(void *data) {
  plexy_dm_t *d = data;

  int ids[PLEXY_DM_MAX_SESSIONS];
  int count = session_mgr_list_active(d->sess_mgr, ids, PLEXY_DM_MAX_SESSIONS);
  for (int i = 0; i < count; i++) {
    const plexy_dm_session_t *sess = session_mgr_get(d->sess_mgr, ids[i]);
    if (sess && sess->state == SESSION_STATE_ACTIVE) {
      session_mgr_lock(d->sess_mgr, ids[i]);
      if (d->lock_mgr)
        lock_mgr_lock_session(d->lock_mgr, ids[i], sess->username);
    }
  }

  vt_manager_set_lock(d->vt_mgr, true);

  vt_manager_switch_to_greeter(d->vt_mgr);
  greeter_resume(d->greeter);

  if (count > 0) {
    const plexy_dm_session_t *last = session_mgr_get(d->sess_mgr, ids[0]);
    if (last)
      greeter_enter_lock(d->greeter, last->username);
  }
  d->greeter_visible = true;
}

static void on_logind_lock(const char *session_path, void *data) {
  (void)session_path;
  on_idle_timeout(data);
}

static void on_logind_prepare_sleep(bool preparing, void *data) {
  plexy_dm_t *d = data;
  if (preparing) {
    if (d->cfg.lock_on_suspend) {
      syslog(LOG_INFO, "plexy-dm: locking before suspend");
      on_idle_timeout(data);
    }
    release_logind_inhibitor(d);
  } else {
    reacquire_logind_inhibitor(d);
  }
}

static void on_logind_prepare_shutdown(bool preparing, void *data) {
  plexy_dm_t *d = data;
  if (!preparing)
    return;

  syslog(LOG_INFO, "plexy-dm: shutdown requested by logind");
  release_logind_inhibitor(d);
  d->running = false;
}

static void print_usage(const char *prog) {
  fprintf(stderr,
          "Usage: %s [OPTIONS]\n"
          "\n"
          "PlexyDM Display Manager v%s\n"
          "\n"
          "Options:\n"
          "  -c, --config PATH   Configuration file (default: %s)\n"
          "  -t, --test          Test mode (run in X11 window)\n"
          "  -v, --verbose       Verbose logging\n"
          "  -h, --help          Show this help\n"
          "  -V, --version       Show version\n",
          prog, PLEXY_DM_VERSION, PLEXY_DM_CONFIG_PATH);
}

int main(int argc, char *argv[]) {
  const char *config_path = NULL;
  bool test_mode = false;
  int log_level = LOG_INFO;

  static const struct option long_opts[] = {
      {"config", required_argument, NULL, 'c'},
      {"test", no_argument, NULL, 't'},
      {"verbose", no_argument, NULL, 'v'},
      {"help", no_argument, NULL, 'h'},
      {"version", no_argument, NULL, 'V'},
      {NULL, 0, NULL, 0},
  };

  int opt;
  while ((opt = getopt_long(argc, argv, "c:tvhV", long_opts, NULL)) != -1) {
    switch (opt) {
    case 'c':
      config_path = optarg;
      break;
    case 't':
      test_mode = true;
      break;
    case 'v':
      log_level = LOG_DEBUG;
      break;
    case 'V':
      printf("plexy-dm %s\n", PLEXY_DM_VERSION);
      return 0;
    case 'h':
    default:
      print_usage(argv[0]);
      return (opt == 'h') ? 0 : 1;
    }
  }

  openlog("plexy-dm", LOG_PID | LOG_CONS, LOG_DAEMON);
  setlogmask(LOG_UPTO(log_level));

  syslog(LOG_INFO, "plexy-dm %s starting", PLEXY_DM_VERSION);
  dm.inhibit_fd = -1;

  if (!test_mode && getuid() != 0) {
    fprintf(stderr, "plexy-dm: must run as root\n");
    syslog(LOG_ERR, "plexy-dm: not running as root, aborting");
    return 1;
  }

  if (dm_config_load(&dm.cfg, config_path) < 0) {
    syslog(LOG_ERR, "plexy-dm: fatal config error");
    return 1;
  }
  dm_config_dump(&dm.cfg);

  mkdir(PLEXY_DM_RUN_DIR, 0755);

  dm.signal_fd = setup_signals();
  if (dm.signal_fd < 0)
    return 1;

  dm.vt_mgr = vt_manager_create(dm.cfg.greeter_vt);
  if (!dm.vt_mgr) {
    syslog(LOG_ERR, "plexy-dm: failed to create VT manager");
    return 1;
  }

  if (vt_manager_claim_greeter(dm.vt_mgr) < 0) {
    syslog(LOG_ERR, "plexy-dm: failed to claim greeter VT");
    return 1;
  }

  logind_callbacks_t logind_cbs = {
      .on_lock = on_logind_lock,
      .on_prepare_sleep = on_logind_prepare_sleep,
      .on_prepare_shutdown = on_logind_prepare_shutdown,
      .user_data = &dm,
  };
  dm.logind = logind_connect(&logind_cbs);
  if (!dm.logind) {
    syslog(LOG_WARNING,
           "plexy-dm: logind not available, some features disabled");
  }

  if (dm.logind) {
    reacquire_logind_inhibitor(&dm);
  }

  dm.sess_mgr = session_mgr_create(dm.vt_mgr, dm.logind, &dm.cfg);
  if (!dm.sess_mgr) {
    syslog(LOG_ERR, "plexy-dm: failed to create session manager");
    return 1;
  }

  dm.lock_mgr = lock_mgr_create(&dm.cfg, on_session_unlocked, &dm);

  dm.idle_mon = idle_monitor_create(dm.cfg.idle_timeout, on_idle_timeout, &dm);

  greeter_callbacks_t greeter_cbs = {
      .on_login = on_login,
      .on_switch_user = on_switch_user,
      .on_power_action = on_power_action,
      .on_unlock = on_unlock,
      .user_data = &dm,
  };
  dm.greeter = greeter_create(dm.cfg.greeter_vt, &dm.cfg, &greeter_cbs);
  if (!dm.greeter) {
    syslog(LOG_ERR, "plexy-dm: failed to create greeter");
    return 1;
  }
  dm.greeter_visible = true;

  dm.epoll_fd = epoll_create1(EPOLL_CLOEXEC);
  if (dm.epoll_fd < 0) {
    syslog(LOG_ERR, "plexy-dm: epoll_create1 failed");
    return 1;
  }

  intptr_t tag;

  tag = ESRC_SIGNAL;
  epoll_add(dm.epoll_fd, dm.signal_fd, EPOLLIN, (void *)tag);

  tag = ESRC_DRM;
  epoll_add(dm.epoll_fd, greeter_get_drm_fd(dm.greeter), EPOLLIN, (void *)tag);

  tag = ESRC_INPUT;
  epoll_add(dm.epoll_fd, greeter_get_input_fd(dm.greeter), EPOLLIN,
            (void *)tag);

  tag = ESRC_TIMER;
  epoll_add(dm.epoll_fd, greeter_get_timer_fd(dm.greeter), EPOLLIN,
            (void *)tag);

  if (dm.logind) {
    tag = ESRC_DBUS;
    epoll_add(dm.epoll_fd, logind_get_fd(dm.logind), EPOLLIN, (void *)tag);
  }

  int vt_sig_fd = vt_manager_get_signal_fd(dm.vt_mgr);
  if (vt_sig_fd >= 0) {
    tag = ESRC_VT_SIGNAL;
    epoll_add(dm.epoll_fd, vt_sig_fd, EPOLLIN, (void *)tag);
  }

  if (dm.idle_mon) {
    tag = ESRC_IDLE;
    epoll_add(dm.epoll_fd, idle_monitor_get_fd(dm.idle_mon), EPOLLIN,
              (void *)tag);
  }

  greeter_render_frame(dm.greeter);

  if (dm.cfg.autologin_user[0]) {
    syslog(LOG_INFO, "plexy-dm: autologin for '%s'", dm.cfg.autologin_user);

    if (dm.cfg.autologin_delay > 0)
      sleep((unsigned)dm.cfg.autologin_delay);

    plexy_dm_error_t err;
    int sid = session_mgr_autologin(dm.sess_mgr, dm.cfg.autologin_user, &err);
    if (sid >= 0) {
      greeter_suspend(dm.greeter);
      dm.greeter_visible = false;
    } else {
      syslog(LOG_WARNING, "plexy-dm: autologin failed: %s",
             plexy_dm_strerror(err));
    }
  }

  sd_notify(0, "READY=1\nSTATUS=PlexyDM greeter active");

  syslog(LOG_INFO, "plexy-dm: entering main loop");
  dm.running = true;

  struct epoll_event events[MAX_EPOLL_EVENTS];

  while (dm.running) {
    int nfds = epoll_wait(dm.epoll_fd, events, MAX_EPOLL_EVENTS, -1);
    if (nfds < 0) {
      if (errno == EINTR)
        continue;
      syslog(LOG_ERR, "plexy-dm: epoll_wait: %s", strerror(errno));
      break;
    }

    for (int i = 0; i < nfds; i++) {
      intptr_t src = (intptr_t)events[i].data.ptr;

      switch (src) {
      case ESRC_SIGNAL:
        handle_signals(&dm);
        break;

      case ESRC_DRM:

        greeter_handle_drm_event(dm.greeter);
        break;

      case ESRC_INPUT:

        if (dm.greeter_visible) {
          greeter_handle_input(dm.greeter);
          if (dm.idle_mon)
            idle_monitor_reset(dm.idle_mon);

          if (dm.debug_term_pid == 0 &&
              greeter_debug_term_requested(dm.greeter)) {
            launch_debug_term(&dm);
          }
        } else {
          greeter_drain_input(dm.greeter);
        }
        break;

      case ESRC_TIMER:

        greeter_handle_timer(dm.greeter);
        break;

      case ESRC_DBUS:
        if (dm.logind)
          logind_dispatch(dm.logind);
        break;

      case ESRC_VT_SIGNAL:
        vt_manager_handle_switch(dm.vt_mgr);

        if (vt_manager_active_vt(dm.vt_mgr) ==
            vt_manager_greeter_vt(dm.vt_mgr)) {
          greeter_resume(dm.greeter);
          dm.greeter_visible = true;
        }
        break;

      case ESRC_IDLE:
        if (dm.idle_mon)
          idle_monitor_handle(dm.idle_mon);
        break;
      }
    }
  }

  syslog(LOG_INFO, "plexy-dm: shutting down");
  sd_notify(0, "STOPPING=1");

  release_logind_inhibitor(&dm);

  idle_monitor_destroy(dm.idle_mon);
  lock_mgr_destroy(dm.lock_mgr);
  greeter_destroy(dm.greeter);
  session_mgr_destroy(dm.sess_mgr);
  logind_disconnect(dm.logind);
  vt_manager_destroy(dm.vt_mgr);

  close(dm.epoll_fd);
  close(dm.signal_fd);

  syslog(LOG_INFO, "plexy-dm: exit");
  closelog();

  return 0;
}
