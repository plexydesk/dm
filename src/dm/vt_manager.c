/*
 * Copyright (C) 2024-2026 Siraj Razick
 * SPDX-License-Identifier: AGPL-3.0-only
 */


#include "vt_manager.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/kd.h>
#include <linux/vt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/signalfd.h>
#include <syslog.h>
#include <unistd.h>

struct plexy_vt_manager {
  int console_fd;
  int greeter_tty_fd;
  int greeter_vt;
  int active_vt;
  int signal_fd;
  int saved_kb_mode;
  bool vt_claimed[64];
  bool lock_vt;
};

static int open_console(void) {
  const char *paths[] = {"/dev/tty0", "/dev/console", NULL};
  for (const char **p = paths; *p; p++) {
    int fd = open(*p, O_RDWR | O_CLOEXEC | O_NOCTTY);
    if (fd >= 0)
      return fd;
  }
  return -1;
}

static int open_tty(int vt_num) {
  char path[32];
  snprintf(path, sizeof(path), "/dev/tty%d", vt_num);
  return open(path, O_RDWR | O_CLOEXEC | O_NOCTTY);
}

plexy_vt_manager_t *vt_manager_create(int greeter_vt) {
  plexy_vt_manager_t *mgr = calloc(1, sizeof(*mgr));
  if (!mgr)
    return NULL;

  mgr->greeter_vt = greeter_vt;
  mgr->console_fd = -1;
  mgr->greeter_tty_fd = -1;
  mgr->signal_fd = -1;
  mgr->active_vt = -1;

  mgr->console_fd = open_console();
  if (mgr->console_fd < 0) {
    syslog(LOG_ERR, "plexy-dm: failed to open console: %s", strerror(errno));
    free(mgr);
    return NULL;
  }

  sigset_t mask;
  sigemptyset(&mask);
  sigaddset(&mask, SIGUSR1);
  sigaddset(&mask, SIGUSR2);
  sigprocmask(SIG_BLOCK, &mask, NULL);

  mgr->signal_fd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
  if (mgr->signal_fd < 0) {
    syslog(LOG_WARNING, "plexy-dm: signalfd failed: %s", strerror(errno));
  }

  syslog(LOG_INFO, "plexy-dm: VT manager created, greeter VT=%d", greeter_vt);
  return mgr;
}

void vt_manager_destroy(plexy_vt_manager_t *mgr) {
  if (!mgr)
    return;

  if (mgr->greeter_tty_fd >= 0) {
    ioctl(mgr->greeter_tty_fd, KDSKBMODE, mgr->saved_kb_mode);
    ioctl(mgr->greeter_tty_fd, KDSETMODE, KD_TEXT);

    struct vt_mode vt_mode = {.mode = VT_AUTO};
    ioctl(mgr->greeter_tty_fd, VT_SETMODE, &vt_mode);

    close(mgr->greeter_tty_fd);
  }

  if (mgr->signal_fd >= 0)
    close(mgr->signal_fd);
  if (mgr->console_fd >= 0)
    close(mgr->console_fd);

  free(mgr);
}

int vt_manager_claim_greeter(plexy_vt_manager_t *mgr) {
  if (!mgr)
    return -1;

  if (ioctl(mgr->console_fd, VT_ACTIVATE, mgr->greeter_vt) < 0) {
    syslog(LOG_ERR, "plexy-dm: VT_ACTIVATE(%d) failed: %s", mgr->greeter_vt,
           strerror(errno));
    return -1;
  }
  ioctl(mgr->console_fd, VT_WAITACTIVE, mgr->greeter_vt);

  mgr->greeter_tty_fd = open_tty(mgr->greeter_vt);
  if (mgr->greeter_tty_fd < 0) {
    syslog(LOG_ERR, "plexy-dm: failed to open tty%d: %s", mgr->greeter_vt,
           strerror(errno));
    return -1;
  }

  ioctl(mgr->greeter_tty_fd, KDGKBMODE, &mgr->saved_kb_mode);
  ioctl(mgr->greeter_tty_fd, KDSKBMODE, K_OFF);

  ioctl(mgr->greeter_tty_fd, KDSETMODE, KD_GRAPHICS);

  struct vt_mode vt_mode = {
      .mode = VT_PROCESS,
      .relsig = SIGUSR1,
      .acqsig = SIGUSR2,
  };
  ioctl(mgr->greeter_tty_fd, VT_SETMODE, &vt_mode);

  mgr->active_vt = mgr->greeter_vt;
  mgr->vt_claimed[mgr->greeter_vt] = true;

  syslog(LOG_INFO, "plexy-dm: claimed greeter VT%d in graphics mode",
         mgr->greeter_vt);
  return mgr->greeter_vt;
}

int vt_manager_alloc_session_vt(plexy_vt_manager_t *mgr) {
  if (!mgr)
    return -1;

  int vt = -1;
  if (ioctl(mgr->console_fd, VT_OPENQRY, &vt) < 0 || vt < 0) {
    syslog(LOG_ERR, "plexy-dm: VT_OPENQRY failed: %s", strerror(errno));
    return -1;
  }

  mgr->vt_claimed[vt] = true;
  syslog(LOG_INFO, "plexy-dm: allocated session VT%d", vt);
  return vt;
}

void vt_manager_release_vt(plexy_vt_manager_t *mgr, int vt) {
  if (!mgr || vt < 0 || vt >= 64)
    return;

  if (vt == mgr->greeter_vt)
    return;

  int tty_fd = open_tty(vt);
  if (tty_fd >= 0) {
    ioctl(tty_fd, KDSETMODE, KD_TEXT);
    ioctl(tty_fd, KDSKBMODE, K_UNICODE);

    struct vt_mode vt_mode = {.mode = VT_AUTO};
    ioctl(tty_fd, VT_SETMODE, &vt_mode);

    ioctl(mgr->console_fd, VT_DISALLOCATE, vt);
    close(tty_fd);
  }

  mgr->vt_claimed[vt] = false;
  syslog(LOG_INFO, "plexy-dm: released VT%d", vt);
}

int vt_manager_switch_to(plexy_vt_manager_t *mgr, int vt) {
  if (!mgr)
    return -1;

  if (mgr->active_vt == vt)
    return 0;

  if (ioctl(mgr->console_fd, VT_ACTIVATE, vt) < 0) {
    syslog(LOG_ERR, "plexy-dm: VT_ACTIVATE(%d) failed: %s", vt,
           strerror(errno));
    return -1;
  }

  mgr->active_vt = vt;
  syslog(LOG_INFO, "plexy-dm: requested switch to VT%d", vt);
  return 0;
}

int vt_manager_switch_to_greeter(plexy_vt_manager_t *mgr) {
  return vt_manager_switch_to(mgr, mgr->greeter_vt);
}

int vt_manager_greeter_vt(const plexy_vt_manager_t *mgr) {
  return mgr ? mgr->greeter_vt : -1;
}

int vt_manager_active_vt(const plexy_vt_manager_t *mgr) {
  if (!mgr || mgr->console_fd < 0)
    return -1;

  struct vt_stat vts;
  if (ioctl(mgr->console_fd, VT_GETSTATE, &vts) < 0)
    return mgr->active_vt;

  return vts.v_active;
}

int vt_manager_get_signal_fd(const plexy_vt_manager_t *mgr) {
  return mgr ? mgr->signal_fd : -1;
}

void vt_manager_handle_switch(plexy_vt_manager_t *mgr) {
  if (!mgr || mgr->signal_fd < 0)
    return;

  struct signalfd_siginfo si;
  while (read(mgr->signal_fd, &si, sizeof(si)) == sizeof(si)) {
    if (si.ssi_signo == SIGUSR1) {
      if (mgr->lock_vt) {

        ioctl(mgr->greeter_tty_fd, VT_RELDISP, 0);
        syslog(LOG_WARNING, "plexy-dm: VT release denied (lock screen active)");
      } else {
        ioctl(mgr->greeter_tty_fd, VT_RELDISP, 1);
        syslog(LOG_DEBUG, "plexy-dm: VT release acknowledged");
      }
    } else if (si.ssi_signo == SIGUSR2) {

      ioctl(mgr->greeter_tty_fd, VT_RELDISP, VT_ACKACQ);
      mgr->active_vt = mgr->greeter_vt;
      syslog(LOG_DEBUG, "plexy-dm: VT acquired (back to greeter)");
    }
  }
}

void vt_manager_set_lock(plexy_vt_manager_t *mgr, bool locked) {
  if (!mgr)
    return;
  mgr->lock_vt = locked;
  syslog(LOG_INFO, "plexy-dm: VT switching %s", locked ? "locked" : "unlocked");
}
