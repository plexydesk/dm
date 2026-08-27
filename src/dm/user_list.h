/*
 * Copyright (C) 2024-2026 Siraj Razick
 * SPDX-License-Identifier: AGPL-3.0-only
 */


#ifndef PLEXY_DM_USER_LIST_H
#define PLEXY_DM_USER_LIST_H

#include "plexy_dm.h"

int user_list_enumerate(plexy_dm_user_t *users, int max_users, uid_t min_uid,
                        uid_t max_uid);

const plexy_dm_user_t *user_list_find(const plexy_dm_user_t *users, int count,
                                      const char *username);

void user_list_refresh_login_time(plexy_dm_user_t *user);

#endif
