/*
 * Copyright (C) 2024-2026 Siraj Razick
 * SPDX-License-Identifier: AGPL-3.0-only
 */


#ifndef PLEXY_DM_GREETER_UI_H
#define PLEXY_DM_GREETER_UI_H

#include "plexy_dm.h"
#include <stdbool.h>

typedef struct greeter_ui_ctx greeter_ui_ctx_t;

typedef enum {
  UI_KEY_NONE = 0,
  UI_KEY_CHAR,
  UI_KEY_BACKSPACE,
  UI_KEY_ENTER,
  UI_KEY_ESCAPE,
  UI_KEY_TAB,
  UI_KEY_LEFT,
  UI_KEY_RIGHT,
  UI_KEY_UP,
  UI_KEY_DOWN,
  UI_KEY_DELETE,
  UI_KEY_F1,
} greeter_ui_key_t;

typedef struct {
  greeter_ui_key_t type;
  uint32_t codepoint;
  bool shift;
  bool ctrl;
} greeter_ui_input_t;

typedef struct {
  float bg[4];
  float card_bg[4];
  float card_border[4];
  float text_primary[4];
  float text_secondary[4];
  float input_bg[4];
  float input_border[4];
  float input_focus[4];
  float button_bg[4];
  float button_hover[4];
  float error_text[4];
  float accent[4];
} greeter_ui_theme_t;

greeter_ui_ctx_t *greeter_ui_create(int width, int height, float scale);

void greeter_ui_destroy(greeter_ui_ctx_t *ctx);

void greeter_ui_set_theme(greeter_ui_ctx_t *ctx,
                          const greeter_ui_theme_t *theme);

int greeter_ui_load_background(greeter_ui_ctx_t *ctx, const char *path);

void greeter_ui_set_users(greeter_ui_ctx_t *ctx, const plexy_dm_user_t *users,
                          int count);

void greeter_ui_select_user(greeter_ui_ctx_t *ctx, int index);

int greeter_ui_user_click_requested(greeter_ui_ctx_t *ctx);

bool greeter_ui_handle_key(greeter_ui_ctx_t *ctx,
                           const greeter_ui_input_t *input);

bool greeter_ui_handle_pointer(greeter_ui_ctx_t *ctx, int x, int y,
                               bool pressed);

void greeter_ui_set_state(greeter_ui_ctx_t *ctx, greeter_state_t state);

void greeter_ui_set_error(greeter_ui_ctx_t *ctx, const char *msg);

void greeter_ui_set_status(greeter_ui_ctx_t *ctx, const char *msg);

const char *greeter_ui_get_password(const greeter_ui_ctx_t *ctx);

const char *greeter_ui_get_selected_user(const greeter_ui_ctx_t *ctx);

void greeter_ui_clear_password(greeter_ui_ctx_t *ctx);

void greeter_ui_render(greeter_ui_ctx_t *ctx, double delta_ms);

bool greeter_ui_animating(const greeter_ui_ctx_t *ctx);

void greeter_ui_default_theme(greeter_ui_theme_t *theme);

struct PlexyCanvas;
struct PlexyCanvas *greeter_ui_get_canvas(const greeter_ui_ctx_t *ctx);

greeter_ui_ctx_t *greeter_ui_create_password(int width, int height,
                                             float scale);

void greeter_ui_set_selected_username(greeter_ui_ctx_t *ctx,
                                      const char *username);

void greeter_ui_set_clock_24h(greeter_ui_ctx_t *ctx, bool use_24h);

void greeter_ui_set_caps_lock(greeter_ui_ctx_t *ctx, bool on);

#include "wifi_list.h"

void greeter_ui_set_wifi_mode(greeter_ui_ctx_t *ctx, bool wifi_mode);

void greeter_ui_set_wifi_networks(greeter_ui_ctx_t *ctx,
                                  const plexy_dm_wifi_ap_t *aps, int count);

int greeter_ui_get_selected_wifi(const greeter_ui_ctx_t *ctx);

bool greeter_ui_wifi_connect_requested(greeter_ui_ctx_t *ctx);

bool greeter_ui_wifi_skip_requested(greeter_ui_ctx_t *ctx);

typedef enum {
  POWER_RESULT_NONE = 0,
  POWER_RESULT_SHUTDOWN,
  POWER_RESULT_REBOOT,
  POWER_RESULT_SUSPEND,
} greeter_ui_power_result_t;

greeter_ui_power_result_t greeter_ui_get_power_result(greeter_ui_ctx_t *ctx);

void greeter_ui_set_authenticating(greeter_ui_ctx_t *ctx, bool active);

#endif
