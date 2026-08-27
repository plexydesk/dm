/*
 * Copyright (C) 2024-2026 Siraj Razick
 * SPDX-License-Identifier: AGPL-3.0-only
 */


#include "lock_screen.h"
#include "pam_auth.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

struct plexy_lock_mgr {
  const plexy_dm_config_t *cfg;
  lock_unlock_cb on_unlock;
  void *user_data;

  struct {
    bool locked;
    char username[PLEXY_DM_MAX_USERNAME];
  } sessions[PLEXY_DM_MAX_SESSIONS];
};

plexy_lock_mgr_t *lock_mgr_create(const plexy_dm_config_t *cfg,
                                  lock_unlock_cb on_unlock, void *user_data) {
  plexy_lock_mgr_t *mgr = calloc(1, sizeof(*mgr));
  if (!mgr)
    return NULL;

  mgr->cfg = cfg;
  mgr->on_unlock = on_unlock;
  mgr->user_data = user_data;

  return mgr;
}

void lock_mgr_destroy(plexy_lock_mgr_t *mgr) { free(mgr); }

int lock_mgr_lock_session(plexy_lock_mgr_t *mgr, int session_id,
                          const char *username) {
  if (!mgr || session_id < 0 || session_id >= PLEXY_DM_MAX_SESSIONS)
    return -1;

  mgr->sessions[session_id].locked = true;
  snprintf(mgr->sessions[session_id].username,
           sizeof(mgr->sessions[session_id].username), "%s", username);

  syslog(LOG_INFO, "plexy-dm: lock_mgr: session %d locked for '%s'", session_id,
         username);
  return 0;
}

plexy_dm_error_t lock_mgr_try_unlock(plexy_lock_mgr_t *mgr, int session_id,
                                     const char *password) {
  if (!mgr || session_id < 0 || session_id >= PLEXY_DM_MAX_SESSIONS)
    return PLEXY_DM_ERR_PAM_AUTH;

  if (!mgr->sessions[session_id].locked)
    return PLEXY_DM_OK;

  const char *username = mgr->sessions[session_id].username;

  plexy_pam_ctx_t *pam = plexy_pam_start(PLEXY_DM_PAM_SERVICE, username);
  if (!pam)
    return PLEXY_DM_ERR_PAM_AUTH;

  plexy_dm_error_t err = plexy_pam_authenticate(pam, password);
  plexy_pam_end(pam);

  if (err != PLEXY_DM_OK) {
    syslog(LOG_WARNING, "plexy-dm: lock_mgr: unlock auth failed for '%s'",
           username);
    return err;
  }

  mgr->sessions[session_id].locked = false;
  syslog(LOG_INFO, "plexy-dm: lock_mgr: session %d unlocked for '%s'",
         session_id, username);

  if (mgr->on_unlock)
    mgr->on_unlock(session_id, mgr->user_data);

  return PLEXY_DM_OK;
}

bool lock_mgr_is_locked(const plexy_lock_mgr_t *mgr, int session_id) {
  if (!mgr || session_id < 0 || session_id >= PLEXY_DM_MAX_SESSIONS)
    return false;
  return mgr->sessions[session_id].locked;
}
