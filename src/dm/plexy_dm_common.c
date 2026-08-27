/*
 * Copyright (C) 2024-2026 Siraj Razick
 * SPDX-License-Identifier: AGPL-3.0-only
 */


#include "plexy_dm.h"

const char *plexy_dm_strerror(plexy_dm_error_t err) {
  switch (err) {
  case PLEXY_DM_OK:
    return "success";
  case PLEXY_DM_ERR_PAM_AUTH:
    return "authentication failed";
  case PLEXY_DM_ERR_PAM_ACCOUNT:
    return "account expired or locked";
  case PLEXY_DM_ERR_PAM_SESSION:
    return "session setup failed";
  case PLEXY_DM_ERR_PAM_PASSWORD:
    return "password change required";
  case PLEXY_DM_ERR_VT_ALLOC:
    return "no free virtual terminal";
  case PLEXY_DM_ERR_VT_SWITCH:
    return "VT switch failed";
  case PLEXY_DM_ERR_FORK:
    return "process fork failed";
  case PLEXY_DM_ERR_SETUID:
    return "privilege drop failed";
  case PLEXY_DM_ERR_EXEC:
    return "session exec failed";
  case PLEXY_DM_ERR_LOGIND:
    return "logind communication error";
  case PLEXY_DM_ERR_DRM:
    return "display initialization error";
  case PLEXY_DM_ERR_CONFIG:
    return "configuration error";
  case PLEXY_DM_ERR_MAX_SESSIONS:
    return "maximum sessions reached";
  }
  return "unknown error";
}
