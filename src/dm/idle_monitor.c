/*
 * Copyright (C) 2024-2026 Siraj Razick
 * SPDX-License-Identifier: AGPL-3.0-only
 */


#include "idle_monitor.h"

#include <stdint.h>
#include <stdlib.h>
#include <sys/timerfd.h>
#include <syslog.h>
#include <unistd.h>

struct plexy_idle_monitor {
  int timer_fd;
  int timeout_secs;
  bool enabled;
  idle_timeout_cb callback;
  void *user_data;
};

static void arm_timer(plexy_idle_monitor_t *mon) {
  if (!mon || mon->timer_fd < 0 || !mon->enabled || mon->timeout_secs <= 0)
    return;

  struct itimerspec its = {
      .it_value =
          {
              .tv_sec = mon->timeout_secs,
              .tv_nsec = 0,
          },
      .it_interval = {0, 0},
  };
  timerfd_settime(mon->timer_fd, 0, &its, NULL);
}

static void disarm_timer(plexy_idle_monitor_t *mon) {
  if (!mon || mon->timer_fd < 0)
    return;

  struct itimerspec its = {0};
  timerfd_settime(mon->timer_fd, 0, &its, NULL);
}

plexy_idle_monitor_t *idle_monitor_create(int timeout_secs, idle_timeout_cb cb,
                                          void *data) {
  plexy_idle_monitor_t *mon = calloc(1, sizeof(*mon));
  if (!mon)
    return NULL;

  mon->timeout_secs = timeout_secs;
  mon->callback = cb;
  mon->user_data = data;
  mon->enabled = (timeout_secs > 0);

  mon->timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
  if (mon->timer_fd < 0) {
    syslog(LOG_ERR, "plexy-dm: idle_monitor timerfd_create failed");
    free(mon);
    return NULL;
  }

  if (mon->enabled)
    arm_timer(mon);

  return mon;
}

void idle_monitor_destroy(plexy_idle_monitor_t *mon) {
  if (!mon)
    return;
  if (mon->timer_fd >= 0)
    close(mon->timer_fd);
  free(mon);
}

int idle_monitor_get_fd(const plexy_idle_monitor_t *mon) {
  return mon ? mon->timer_fd : -1;
}

void idle_monitor_handle(plexy_idle_monitor_t *mon) {
  if (!mon || mon->timer_fd < 0)
    return;

  uint64_t expirations;
  if (read(mon->timer_fd, &expirations, sizeof(expirations)) ==
      sizeof(expirations)) {
    if (mon->enabled && mon->callback) {
      syslog(LOG_INFO, "plexy-dm: idle timeout reached (%ds)",
             mon->timeout_secs);
      mon->callback(mon->user_data);
    }
  }
}

void idle_monitor_reset(plexy_idle_monitor_t *mon) {
  if (!mon || !mon->enabled)
    return;
  arm_timer(mon);
}

void idle_monitor_set_enabled(plexy_idle_monitor_t *mon, bool enabled) {
  if (!mon)
    return;

  mon->enabled = enabled;
  if (enabled)
    arm_timer(mon);
  else
    disarm_timer(mon);
}
