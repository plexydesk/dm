/*
 * Copyright (C) 2024-2026 Siraj Razick
 * SPDX-License-Identifier: AGPL-3.0-only
 */


#ifndef PLEXY_DM_PAM_AUTH_H
#define PLEXY_DM_PAM_AUTH_H

#include "plexy_dm.h"

typedef struct plexy_pam_ctx plexy_pam_ctx_t;

plexy_pam_ctx_t *plexy_pam_start(const char *service, const char *username);

plexy_dm_error_t plexy_pam_authenticate(plexy_pam_ctx_t *ctx,
                                        const char *password);

plexy_dm_error_t plexy_pam_acct_mgmt(plexy_pam_ctx_t *ctx);

plexy_dm_error_t plexy_pam_open_session(plexy_pam_ctx_t *ctx);

void plexy_pam_close_session(plexy_pam_ctx_t *ctx);

void plexy_pam_end(plexy_pam_ctx_t *ctx);

char **plexy_pam_get_envlist(plexy_pam_ctx_t *ctx);

void *plexy_pam_get_handle(plexy_pam_ctx_t *ctx);

const char *plexy_pam_last_error(plexy_pam_ctx_t *ctx);

#endif
