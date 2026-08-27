/*
 * Copyright (C) 2024-2026 Siraj Razick
 * SPDX-License-Identifier: AGPL-3.0-only
 */


#include "pam_auth.h"

#include <security/pam_appl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

struct plexy_pam_ctx {
  pam_handle_t *pamh;
  char username[PLEXY_DM_MAX_USERNAME];
  char password[PLEXY_DM_MAX_PASSWORD];
  int last_status;
  bool session_open;
  bool cred_set;
};

static int pam_conversation(int num_msg, const struct pam_message **msg,
                            struct pam_response **resp, void *appdata_ptr) {
  struct plexy_pam_ctx *ctx = appdata_ptr;
  struct pam_response *reply;

  if (num_msg <= 0 || num_msg > PAM_MAX_NUM_MSG)
    return PAM_CONV_ERR;

  reply = calloc((size_t)num_msg, sizeof(*reply));
  if (!reply)
    return PAM_BUF_ERR;

  for (int i = 0; i < num_msg; i++) {
    switch (msg[i]->msg_style) {
    case PAM_PROMPT_ECHO_OFF:
    case PAM_PROMPT_ECHO_ON:
      reply[i].resp = strdup(ctx->password);
      if (!reply[i].resp) {
        for (int j = 0; j < i; j++)
          free(reply[j].resp);
        free(reply);
        return PAM_BUF_ERR;
      }
      break;
    case PAM_ERROR_MSG:
      syslog(LOG_ERR, "plexy-dm: PAM error: %s", msg[i]->msg);
      break;
    case PAM_TEXT_INFO:
      syslog(LOG_INFO, "plexy-dm: PAM info: %s", msg[i]->msg);
      break;
    default:
      break;
    }
  }

  *resp = reply;
  return PAM_SUCCESS;
}

plexy_pam_ctx_t *plexy_pam_start(const char *service, const char *username) {
  struct plexy_pam_ctx *ctx = calloc(1, sizeof(*ctx));
  if (!ctx)
    return NULL;

  snprintf(ctx->username, sizeof(ctx->username), "%s", username);

  struct pam_conv conv = {
      .conv = pam_conversation,
      .appdata_ptr = ctx,
  };

  ctx->last_status = pam_start(service, username, &conv, &ctx->pamh);
  if (ctx->last_status != PAM_SUCCESS) {
    syslog(LOG_ERR, "plexy-dm: pam_start failed: %s",
           pam_strerror(ctx->pamh, ctx->last_status));
    free(ctx);
    return NULL;
  }

  pam_set_item(ctx->pamh, PAM_TTY, "tty1");
  pam_set_item(ctx->pamh, PAM_RHOST, "localhost");

  return ctx;
}

plexy_dm_error_t plexy_pam_authenticate(plexy_pam_ctx_t *ctx,
                                        const char *password) {
  if (!ctx || !ctx->pamh)
    return PLEXY_DM_ERR_PAM_AUTH;

  snprintf(ctx->password, sizeof(ctx->password), "%s", password);

  syslog(LOG_DEBUG, "plexy-dm: attempting auth for '%s' (pw_len=%zu)",
         ctx->username, password ? strlen(password) : 0);

  ctx->last_status = pam_authenticate(ctx->pamh, 0);

  explicit_bzero(ctx->password, sizeof(ctx->password));

  if (ctx->last_status != PAM_SUCCESS) {
    syslog(LOG_WARNING, "plexy-dm: auth failed for '%s': %s", ctx->username,
           pam_strerror(ctx->pamh, ctx->last_status));
    return PLEXY_DM_ERR_PAM_AUTH;
  }

  return PLEXY_DM_OK;
}

plexy_dm_error_t plexy_pam_acct_mgmt(plexy_pam_ctx_t *ctx) {
  if (!ctx || !ctx->pamh)
    return PLEXY_DM_ERR_PAM_ACCOUNT;

  ctx->last_status = pam_acct_mgmt(ctx->pamh, 0);

  switch (ctx->last_status) {
  case PAM_SUCCESS:
    return PLEXY_DM_OK;
  case PAM_NEW_AUTHTOK_REQD:
    return PLEXY_DM_ERR_PAM_PASSWORD;
  default:
    syslog(LOG_WARNING, "plexy-dm: acct_mgmt failed for '%s': %s",
           ctx->username, pam_strerror(ctx->pamh, ctx->last_status));
    return PLEXY_DM_ERR_PAM_ACCOUNT;
  }
}

plexy_dm_error_t plexy_pam_open_session(plexy_pam_ctx_t *ctx) {
  if (!ctx || !ctx->pamh)
    return PLEXY_DM_ERR_PAM_SESSION;

  ctx->last_status = pam_setcred(ctx->pamh, PAM_ESTABLISH_CRED);
  if (ctx->last_status != PAM_SUCCESS) {
    syslog(LOG_ERR, "plexy-dm: pam_setcred failed: %s",
           pam_strerror(ctx->pamh, ctx->last_status));
    return PLEXY_DM_ERR_PAM_SESSION;
  }
  ctx->cred_set = true;

  ctx->last_status = pam_open_session(ctx->pamh, 0);
  if (ctx->last_status != PAM_SUCCESS) {
    syslog(LOG_ERR, "plexy-dm: pam_open_session failed: %s",
           pam_strerror(ctx->pamh, ctx->last_status));
    pam_setcred(ctx->pamh, PAM_DELETE_CRED);
    ctx->cred_set = false;
    return PLEXY_DM_ERR_PAM_SESSION;
  }
  ctx->session_open = true;

  syslog(LOG_INFO, "plexy-dm: session opened for '%s'", ctx->username);
  return PLEXY_DM_OK;
}

void plexy_pam_close_session(plexy_pam_ctx_t *ctx) {
  if (!ctx || !ctx->pamh)
    return;

  if (ctx->session_open) {
    pam_close_session(ctx->pamh, 0);
    ctx->session_open = false;
    syslog(LOG_INFO, "plexy-dm: session closed for '%s'", ctx->username);
  }

  if (ctx->cred_set) {
    pam_setcred(ctx->pamh, PAM_DELETE_CRED);
    ctx->cred_set = false;
  }
}

void plexy_pam_end(plexy_pam_ctx_t *ctx) {
  if (!ctx)
    return;

  plexy_pam_close_session(ctx);

  if (ctx->pamh) {
    pam_end(ctx->pamh, ctx->last_status);
    ctx->pamh = NULL;
  }

  explicit_bzero(ctx->password, sizeof(ctx->password));
  free(ctx);
}

char **plexy_pam_get_envlist(plexy_pam_ctx_t *ctx) {
  if (!ctx || !ctx->pamh)
    return NULL;
  return pam_getenvlist(ctx->pamh);
}

void *plexy_pam_get_handle(plexy_pam_ctx_t *ctx) {
  return ctx ? ctx->pamh : NULL;
}

const char *plexy_pam_last_error(plexy_pam_ctx_t *ctx) {
  if (!ctx || !ctx->pamh)
    return "PAM context not initialized";
  return pam_strerror(ctx->pamh, ctx->last_status);
}
