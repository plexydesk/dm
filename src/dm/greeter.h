/*
 * Copyright (C) 2024-2026 Siraj Razick
 * SPDX-License-Identifier: AGPL-3.0-only
 */


#ifndef PLEXY_DM_GREETER_H
#define PLEXY_DM_GREETER_H

#include "dm_config.h"
#include "plexy_dm.h"

typedef struct plexy_greeter plexy_greeter_t;

typedef struct {

  int (*on_login)(const char *username, const char *password, void *data);

  void (*on_switch_session)(int session_id, void *data);

  void (*on_switch_user)(void *data);

  void (*on_power_action)(power_action_t action, void *data);

  void (*on_unlock)(const char *username, const char *password, void *data);

  void *user_data;
} greeter_callbacks_t;

plexy_greeter_t *greeter_create(int vt, const plexy_dm_config_t *cfg,
                                const greeter_callbacks_t *cbs);

void greeter_destroy(plexy_greeter_t *greeter);

void greeter_set_users(plexy_greeter_t *greeter, const plexy_dm_user_t *users,
                       int count);

void greeter_set_sessions(plexy_greeter_t *greeter,
                          const plexy_dm_session_t *sessions, int count);

void greeter_set_state(plexy_greeter_t *greeter, greeter_state_t state);

void greeter_enter_lock(plexy_greeter_t *greeter, const char *username);

void greeter_show_error(plexy_greeter_t *greeter, const char *message);

int greeter_get_drm_fd(const plexy_greeter_t *greeter);

int greeter_get_input_fd(const plexy_greeter_t *greeter);

int greeter_get_timer_fd(const plexy_greeter_t *greeter);

void greeter_handle_drm_event(plexy_greeter_t *greeter);

void greeter_handle_input(plexy_greeter_t *greeter);

void greeter_drain_input(plexy_greeter_t *greeter);

void greeter_handle_timer(plexy_greeter_t *greeter);

void greeter_request_frame(plexy_greeter_t *greeter);

void greeter_render_frame(plexy_greeter_t *greeter);

void greeter_suspend(plexy_greeter_t *greeter);

void greeter_resume(plexy_greeter_t *greeter);

bool greeter_debug_term_requested(plexy_greeter_t *greeter);

#endif
