/*
 * Copyright (C) 2024-2026 Siraj Razick
 * SPDX-License-Identifier: AGPL-3.0-only
 */


#include "greeter_ui.h"

#include <math.h>
#include <plexy_canvas.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include "wifi_list.h"

#define SHAKE_DURATION_MS 400.0
#define SHAKE_AMPLITUDE 12.0f
#define FADE_DURATION_MS 300.0
#define CURSOR_BLINK_MS 530.0
#define GREETER_UI_VISIBLE_USERS 3
#define GREETER_UI_POWER_OPTION_COUNT 3
#define GREETER_UI_MEMO_AVATAR_COUNT 35
#define GREETER_UI_USER_SLIDE_MS 180.0
#define GREETER_UI_POWER_SLIDE_MS 180.0
#define GREETER_UI_DIALOG_TRANSITION_MS 220.0

typedef enum {
  GREETER_UI_FACE_USER_SELECT = 0,
  GREETER_UI_FACE_PASSWORD = 1,
} greeter_ui_face_t;

struct greeter_ui_ctx {
  PlexyCanvas *canvas;
  int width;
  int height;
  float scale;
  greeter_ui_face_t face;
  greeter_state_t state;
  greeter_ui_theme_t theme;

  const plexy_dm_user_t *users;
  int user_count;
  int selected_user;
  int user_slide_from;
  int user_slide_to;
  int user_slide_dir;
  double user_slide_elapsed;
  float user_row_pitch;

  char password[PLEXY_DM_MAX_PASSWORD];
  int password_len;
  bool cursor_visible;
  double cursor_timer;

  char error_msg[256];
  double error_timer;

  char username_override[PLEXY_DM_MAX_USERNAME];

  double shake_timer;
  double fade_alpha;
  bool animating;

  uint32_t root_id;
  uint32_t bg_id;
  uint32_t card_id;
  uint32_t title_label_id;
  uint32_t content_id;
  uint32_t avatar_id;
  uint32_t avatar_glyph_id;
  uint32_t username_label_id;
  uint32_t subtitle_label_id;
  uint32_t password_field_id;
  uint32_t password_text_id;
  uint32_t error_label_id;
  uint32_t action_label_id;
  uint32_t user_strip_id;
  uint32_t user_row_ids[GREETER_UI_VISIBLE_USERS];
  uint32_t user_avatar_ids[GREETER_UI_VISIBLE_USERS];
  uint32_t user_avatar_glyph_ids[GREETER_UI_VISIBLE_USERS];
  uint32_t user_text_col_ids[GREETER_UI_VISIBLE_USERS];
  uint32_t user_name_label_ids[GREETER_UI_VISIBLE_USERS];
  uint32_t user_meta_label_ids[GREETER_UI_VISIBLE_USERS];
  uint32_t clock_label_id;
  uint32_t clock_date_label_id;
  uint32_t power_btn_id;
  uint32_t user_btns[PLEXY_DM_MAX_USERS];
  uint32_t caps_lock_label_id;
  uint32_t power_overlay_id;
  uint32_t power_option_strip_id;
  uint32_t power_option_ids[GREETER_UI_POWER_OPTION_COUNT];
  uint32_t power_option_label_ids[GREETER_UI_POWER_OPTION_COUNT];
  int power_selected;
  int power_slide_from;
  int power_slide_to;
  int power_slide_dir;
  double power_slide_elapsed;
  float power_row_pitch;
  bool power_menu_present;
  int power_transition_dir;
  double power_transition_elapsed;
  float power_transition_px;
  greeter_ui_power_result_t power_result;
  bool power_menu_visible;
  bool clock_24h;
  bool caps_lock_on;
  bool authenticating;
  time_t last_clock_min;

  bool wifi_mode;
  const plexy_dm_wifi_ap_t *wifi_aps;
  int wifi_count;
  bool wifi_connect_requested;
  bool wifi_skip_requested;
  int user_click_index;
};

static const char *greeter_ui_power_labels[GREETER_UI_POWER_OPTION_COUNT] = {
    "Shut Down", "Restart", "Suspend"};

static const char *greeter_ui_display_name_at(const greeter_ui_ctx_t *ctx,
                                              int index) {
  if (!ctx || !ctx->users || index < 0 || index >= ctx->user_count)
    return NULL;

  const plexy_dm_user_t *user = &ctx->users[index];
  return user->realname[0] ? user->realname : user->username;
}

static void greeter_ui_sync_username_override(greeter_ui_ctx_t *ctx) {
  if (!ctx || ctx->face != GREETER_UI_FACE_PASSWORD)
    return;

  const char *display_name =
      greeter_ui_display_name_at(ctx, ctx->selected_user);
  if (display_name)
    snprintf(ctx->username_override, sizeof(ctx->username_override), "%s",
             display_name);
  else
    ctx->username_override[0] = '\0';
}

static uint32_t greeter_ui_name_hash(const char *name) {
  uint32_t hash = 2166136261u;
  if (name) {
    for (const unsigned char *p = (const unsigned char *)name; *p; ++p) {
      hash ^= *p;
      hash *= 16777619u;
    }
  }

  return hash;
}

static void greeter_ui_avatar_tint(const char *name, float *r, float *g,
                                   float *b) {
  uint32_t hash = greeter_ui_name_hash(name);

  float t0 = (float)(hash & 0xffu) / 255.0f;
  float t1 = (float)((hash >> 8) & 0xffu) / 255.0f;
  float t2 = (float)((hash >> 16) & 0xffu) / 255.0f;
  if (r)
    *r = 0.20f + t0 * 0.18f;
  if (g)
    *g = 0.26f + t1 * 0.20f;
  if (b)
    *b = 0.34f + t2 * 0.26f;
}

static void greeter_ui_avatar_icon_text(const plexy_dm_user_t *user,
                                        const char *fallback_name, char *buf,
                                        size_t bufsz) {
  if (!buf || bufsz == 0)
    return;

  const char *name = NULL;
  if (user && user->username[0])
    name = user->username;
  else
    name = fallback_name;

  unsigned avatar_index =
      (unsigned)(greeter_ui_name_hash(name) % GREETER_UI_MEMO_AVATAR_COUNT) +
      1u;

  snprintf(buf, bufsz, "[icon=memo_%u]", avatar_index);
}

static float greeter_ui_clampf(float v, float lo, float hi) {
  if (v < lo)
    return lo;
  if (v > hi)
    return hi;
  return v;
}

static float greeter_ui_scale_value(const greeter_ui_ctx_t *ctx, float value) {
  float scale = ctx ? ctx->scale : 1.0f;
  if (!isfinite(scale) || scale <= 0.0f)
    scale = 1.0f;
  return value * scale;
}

static int greeter_ui_visible_user_count(const greeter_ui_ctx_t *ctx) {
  if (!ctx || ctx->user_count <= 0)
    return 0;
  return ctx->user_count < GREETER_UI_VISIBLE_USERS ? ctx->user_count
                                                    : GREETER_UI_VISIBLE_USERS;
}

static int greeter_ui_wrap_user_index(const greeter_ui_ctx_t *ctx, int index) {
  if (!ctx || ctx->user_count <= 0)
    return 0;

  int wrapped = index % ctx->user_count;
  if (wrapped < 0)
    wrapped += ctx->user_count;
  return wrapped;
}

static bool greeter_ui_user_slide_active(const greeter_ui_ctx_t *ctx) {
  return ctx && ctx->face == GREETER_UI_FACE_USER_SELECT &&
         ctx->user_count >= GREETER_UI_VISIBLE_USERS &&
         ctx->user_slide_dir != 0 &&
         ctx->user_slide_elapsed < GREETER_UI_USER_SLIDE_MS;
}

static float greeter_ui_ease_out_cubic(float t) {
  t = greeter_ui_clampf(t, 0.0f, 1.0f);
  float inv = 1.0f - t;
  return 1.0f - inv * inv * inv;
}

static float greeter_ui_user_slide_progress(const greeter_ui_ctx_t *ctx) {
  if (!greeter_ui_user_slide_active(ctx))
    return 1.0f;
  return greeter_ui_ease_out_cubic(
      (float)(ctx->user_slide_elapsed / GREETER_UI_USER_SLIDE_MS));
}

static float greeter_ui_user_slide_offset(const greeter_ui_ctx_t *ctx) {
  if (!greeter_ui_user_slide_active(ctx))
    return 0.0f;
  return -(float)ctx->user_slide_dir * ctx->user_row_pitch *
         greeter_ui_user_slide_progress(ctx);
}

static float greeter_ui_user_item_offset(const greeter_ui_ctx_t *ctx) {
  if (!greeter_ui_user_slide_active(ctx))
    return 0.0f;
  return greeter_ui_user_slide_offset(ctx);
}

static float greeter_ui_user_row_alpha(const greeter_ui_ctx_t *ctx, int slot) {
  if (!greeter_ui_user_slide_active(ctx))
    return 1.0f;

  float progress = greeter_ui_user_slide_progress(ctx);
  if (ctx->user_slide_dir > 0 && slot == 0)
    return 1.0f - progress;
  if (ctx->user_slide_dir < 0 && slot == GREETER_UI_VISIBLE_USERS - 1)
    return 1.0f - progress;
  return 1.0f;
}

static int greeter_ui_user_index_for_slot(const greeter_ui_ctx_t *ctx, int slot,
                                          int visible) {
  if (!ctx || visible <= 0)
    return 0;

  if (visible == 1)
    return ctx->selected_user;

  if (visible == 2)
    return slot == 0 ? greeter_ui_wrap_user_index(ctx, ctx->selected_user - 1)
                     : ctx->selected_user;

  if (greeter_ui_user_slide_active(ctx)) {
    int base =
        ctx->user_slide_dir > 0 ? ctx->user_slide_from - 1 : ctx->user_slide_to;
    return greeter_ui_wrap_user_index(ctx, base + slot);
  }

  return greeter_ui_wrap_user_index(ctx, ctx->selected_user - 1 + slot);
}

static int greeter_ui_center_user_slot(int visible) {
  if (visible <= 1)
    return 0;
  return visible / 2;
}

static void greeter_ui_on_user_row_click(uint32_t widget_id, void *userdata) {
  greeter_ui_ctx_t *ctx = (greeter_ui_ctx_t *)userdata;
  if (!ctx)
    return;

  int slot = -1;
  for (int i = 0; i < GREETER_UI_VISIBLE_USERS; ++i) {
    if (ctx->user_row_ids[i] == widget_id) {
      slot = i;
      break;
    }
  }
  if (slot < 0)
    return;

  int visible = greeter_ui_visible_user_count(ctx);
  if (slot >= visible)
    return;

  int user_index = greeter_ui_user_index_for_slot(ctx, slot, visible);
  ctx->user_click_index = user_index;
}

static int greeter_ui_wrap_power_index(int index) {
  int wrapped = index % GREETER_UI_POWER_OPTION_COUNT;
  if (wrapped < 0)
    wrapped += GREETER_UI_POWER_OPTION_COUNT;
  return wrapped;
}

static bool greeter_ui_power_slide_active(const greeter_ui_ctx_t *ctx) {
  return ctx && ctx->power_menu_present && ctx->power_slide_dir != 0 &&
         ctx->power_slide_elapsed < GREETER_UI_POWER_SLIDE_MS;
}

static bool greeter_ui_power_transition_active(const greeter_ui_ctx_t *ctx) {
  return ctx && ctx->power_transition_dir != 0 &&
         ctx->power_transition_elapsed < GREETER_UI_DIALOG_TRANSITION_MS;
}

static float greeter_ui_power_transition_progress(const greeter_ui_ctx_t *ctx) {
  if (!ctx || !ctx->power_menu_present)
    return 0.0f;

  if (!greeter_ui_power_transition_active(ctx))
    return ctx->power_menu_visible ? 1.0f : 0.0f;

  float t = greeter_ui_ease_out_cubic(
      (float)(ctx->power_transition_elapsed / GREETER_UI_DIALOG_TRANSITION_MS));
  return ctx->power_transition_dir > 0 ? t : 1.0f - t;
}

static float greeter_ui_power_slide_progress(const greeter_ui_ctx_t *ctx) {
  if (!greeter_ui_power_slide_active(ctx))
    return 1.0f;
  return greeter_ui_ease_out_cubic(
      (float)(ctx->power_slide_elapsed / GREETER_UI_POWER_SLIDE_MS));
}

static float greeter_ui_power_item_offset(const greeter_ui_ctx_t *ctx) {
  if (!greeter_ui_power_slide_active(ctx))
    return 0.0f;
  return -(float)ctx->power_slide_dir * ctx->power_row_pitch *
         greeter_ui_power_slide_progress(ctx);
}

static float greeter_ui_power_option_alpha(const greeter_ui_ctx_t *ctx,
                                           int slot) {
  if (!greeter_ui_power_slide_active(ctx))
    return 1.0f;

  float progress = greeter_ui_power_slide_progress(ctx);
  if (ctx->power_slide_dir > 0 && slot == 0)
    return 1.0f - progress;
  if (ctx->power_slide_dir < 0 && slot == GREETER_UI_POWER_OPTION_COUNT - 1)
    return 1.0f - progress;
  return 1.0f;
}

static int greeter_ui_power_index_for_slot(const greeter_ui_ctx_t *ctx,
                                           int slot) {
  if (!ctx)
    return 0;

  if (greeter_ui_power_slide_active(ctx)) {
    int base = ctx->power_slide_dir > 0 ? ctx->power_slide_from - 1
                                        : ctx->power_slide_to;
    return greeter_ui_wrap_power_index(base + slot);
  }

  return greeter_ui_wrap_power_index(ctx->power_selected - 1 + slot);
}

static bool greeter_ui_is_fullscreen_dialog(int width, int height) {
  return abs(width - height) > 4;
}

static void greeter_ui_update_user_rows(greeter_ui_ctx_t *ctx) {
  if (!ctx || ctx->face != GREETER_UI_FACE_USER_SELECT)
    return;

  int visible = greeter_ui_visible_user_count(ctx);
  int center_slot = greeter_ui_center_user_slot(visible);
  float item_offset = greeter_ui_user_item_offset(ctx);
  if (ctx->user_row_ids[0])
    plexy_canvas_set_margin(ctx->canvas, ctx->user_row_ids[0], 0.0f, 0.0f, 0.0f,
                            0.0f);

  for (int slot = 0; slot < GREETER_UI_VISIBLE_USERS; ++slot) {
    uint32_t row_id = ctx->user_row_ids[slot];
    if (!row_id)
      continue;

    if (slot >= visible) {
      if (ctx->user_avatar_ids[slot])
        plexy_canvas_set_translation(ctx->canvas, ctx->user_avatar_ids[slot],
                                     0.0f, 0.0f);
      if (ctx->user_text_col_ids[slot])
        plexy_canvas_set_translation(ctx->canvas, ctx->user_text_col_ids[slot],
                                     0.0f, 0.0f);
      plexy_canvas_set_visible(ctx->canvas, row_id, 0);
      continue;
    }

    int user_index = greeter_ui_user_index_for_slot(ctx, slot, visible);
    bool selected = (user_index == ctx->selected_user);
    bool focus_slot = (slot == center_slot);
    float row_alpha = greeter_ui_user_row_alpha(ctx, slot);
    float ar, ag, ab;
    char avatar_icon[128];

    if (ctx->wifi_mode) {

      const plexy_dm_wifi_ap_t *ap =
          (ctx->wifi_aps && user_index < ctx->wifi_count)
              ? &ctx->wifi_aps[user_index]
              : NULL;

      const char *ssid = ap ? ap->ssid : "";
      int sig = ap ? ap->signal : 0;

      ar = 0.18f;
      ag = 0.42f;
      ab = 0.72f;

      if (sig >= 75)
        snprintf(avatar_icon, sizeof(avatar_icon),
                 "\xe2\x96\x82\xe2\x96\x84\xe2\x96\x86\xe2\x96\x88");
      else if (sig >= 50)
        snprintf(avatar_icon, sizeof(avatar_icon),
                 "\xe2\x96\x82\xe2\x96\x84\xe2\x96\x86\xe2\x96\x91");
      else if (sig >= 25)
        snprintf(avatar_icon, sizeof(avatar_icon),
                 "\xe2\x96\x82\xe2\x96\x84\xe2\x96\x91\xe2\x96\x91");
      else
        snprintf(avatar_icon, sizeof(avatar_icon),
                 "\xe2\x96\x82\xe2\x96\x91\xe2\x96\x91\xe2\x96\x91");

      const char *meta = "";
      if (ap) {
        if (ap->connected)
          meta = "Connected";
        else if (ap->secured)
          meta = "Secured";
        else
          meta = "Open";
      }

      plexy_canvas_set_visible(ctx->canvas, row_id, 1);
      if (ctx->user_avatar_ids[slot])
        plexy_canvas_set_translation(ctx->canvas, ctx->user_avatar_ids[slot],
                                     0.0f, item_offset);
      if (ctx->user_text_col_ids[slot])
        plexy_canvas_set_translation(ctx->canvas, ctx->user_text_col_ids[slot],
                                     0.0f, item_offset);

      plexy_canvas_set_fill_color(ctx->canvas, row_id, 0.96f, 0.98f, 1.00f,
                                  focus_slot ? 0.15f : 0.0f);
      plexy_canvas_set_border(ctx->canvas, row_id, 1.00f, 1.00f, 1.00f,
                              focus_slot ? 0.34f : 0.0f,
                              focus_slot ? 0.8f : 0.0f);

      plexy_canvas_set_fill_color(ctx->canvas, ctx->user_avatar_ids[slot], ar,
                                  ag, ab,
                                  row_alpha * (selected ? 0.22f : 0.10f));
      plexy_canvas_set_border(
          ctx->canvas, ctx->user_avatar_ids[slot], 1.00f, 1.00f, 1.00f,
          row_alpha * (focus_slot ? 0.34f : 0.10f), focus_slot ? 0.8f : 0.6f);
      plexy_canvas_set_text(ctx->canvas, ctx->user_avatar_glyph_ids[slot],
                            avatar_icon);

      plexy_canvas_set_text(ctx->canvas, ctx->user_name_label_ids[slot], ssid);
      plexy_canvas_set_text_color(ctx->canvas, ctx->user_name_label_ids[slot],
                                  1.00f, 1.00f, 1.00f,
                                  row_alpha * (selected ? 0.98f : 0.82f));

      if (ap && meta[0]) {
        plexy_canvas_set_text(ctx->canvas, ctx->user_meta_label_ids[slot],
                              meta);
        plexy_canvas_set_text_color(ctx->canvas, ctx->user_meta_label_ids[slot],
                                    0.92f, 0.94f, 0.98f, row_alpha * 0.48f);
      } else {
        plexy_canvas_set_text(ctx->canvas, ctx->user_meta_label_ids[slot], "");
      }

    } else {

      const plexy_dm_user_t *user = &ctx->users[user_index];
      const char *display_name = greeter_ui_display_name_at(ctx, user_index);

      greeter_ui_avatar_tint(display_name ? display_name : user->username, &ar,
                             &ag, &ab);
      greeter_ui_avatar_icon_text(user, display_name, avatar_icon,
                                  sizeof(avatar_icon));

      plexy_canvas_set_visible(ctx->canvas, row_id, 1);
      if (ctx->user_avatar_ids[slot])
        plexy_canvas_set_translation(ctx->canvas, ctx->user_avatar_ids[slot],
                                     0.0f, item_offset);
      if (ctx->user_text_col_ids[slot])
        plexy_canvas_set_translation(ctx->canvas, ctx->user_text_col_ids[slot],
                                     0.0f, item_offset);

      plexy_canvas_set_fill_color(ctx->canvas, row_id, 0.96f, 0.98f, 1.00f,
                                  focus_slot ? 0.15f : 0.0f);
      plexy_canvas_set_border(ctx->canvas, row_id, 1.00f, 1.00f, 1.00f,
                              focus_slot ? 0.34f : 0.0f,
                              focus_slot ? 0.8f : 0.0f);

      plexy_canvas_set_fill_color(ctx->canvas, ctx->user_avatar_ids[slot], ar,
                                  ag, ab,
                                  row_alpha * (selected ? 0.13f : 0.06f));
      plexy_canvas_set_border(
          ctx->canvas, ctx->user_avatar_ids[slot], 1.00f, 1.00f, 1.00f,
          row_alpha * (focus_slot ? 0.34f : 0.10f), focus_slot ? 0.8f : 0.6f);
      plexy_canvas_set_text(ctx->canvas, ctx->user_avatar_glyph_ids[slot],
                            avatar_icon);

      if (display_name)
        plexy_canvas_set_text(ctx->canvas, ctx->user_name_label_ids[slot],
                              display_name);
      else
        plexy_canvas_set_text(ctx->canvas, ctx->user_name_label_ids[slot], "");

      plexy_canvas_set_text_color(ctx->canvas, ctx->user_name_label_ids[slot],
                                  1.00f, 1.00f, 1.00f,
                                  row_alpha * (selected ? 0.98f : 0.82f));

      if (selected && user->realname[0] && user->username[0] &&
          strcmp(user->realname, user->username) != 0) {
        plexy_canvas_set_text(ctx->canvas, ctx->user_meta_label_ids[slot],
                              user->username);
        plexy_canvas_set_text_color(ctx->canvas, ctx->user_meta_label_ids[slot],
                                    0.92f, 0.94f, 0.98f, row_alpha * 0.48f);
      } else {
        plexy_canvas_set_text(ctx->canvas, ctx->user_meta_label_ids[slot], "");
      }
    }
  }
}

static void greeter_ui_update_power_options(greeter_ui_ctx_t *ctx) {
  if (!ctx || !ctx->power_overlay_id)
    return;

  if (!ctx->power_menu_present) {
    plexy_canvas_set_visible(ctx->canvas, ctx->power_overlay_id, 0);
    plexy_canvas_set_opacity(ctx->canvas, ctx->power_overlay_id, 0.0f);
    plexy_canvas_set_translation(ctx->canvas, ctx->power_overlay_id, 0.0f,
                                 ctx->power_transition_px);
    if (ctx->card_id) {
      plexy_canvas_set_visible(ctx->canvas, ctx->card_id, 1);
      plexy_canvas_set_opacity(ctx->canvas, ctx->card_id, 1.0f);
      plexy_canvas_set_translation(ctx->canvas, ctx->card_id, 0.0f, 0.0f);
    }
    for (int slot = 0; slot < GREETER_UI_POWER_OPTION_COUNT; ++slot) {
      if (ctx->power_option_label_ids[slot])
        plexy_canvas_set_translation(
            ctx->canvas, ctx->power_option_label_ids[slot], 0.0f, 0.0f);
    }
    return;
  }

  int center_slot = GREETER_UI_POWER_OPTION_COUNT / 2;
  float item_offset = greeter_ui_power_item_offset(ctx);
  float dialog_progress = greeter_ui_power_transition_progress(ctx);
  float card_opacity = 1.0f - dialog_progress;
  float power_opacity = dialog_progress;

  plexy_canvas_set_visible(ctx->canvas, ctx->power_overlay_id,
                           power_opacity > 0.01f ? 1 : 0);
  plexy_canvas_set_opacity(ctx->canvas, ctx->power_overlay_id, power_opacity);
  plexy_canvas_set_translation(ctx->canvas, ctx->power_overlay_id, 0.0f,
                               ctx->power_transition_px *
                                   (1.0f - dialog_progress));
  if (ctx->card_id) {
    plexy_canvas_set_visible(ctx->canvas, ctx->card_id,
                             card_opacity > 0.01f ? 1 : 0);
    plexy_canvas_set_opacity(ctx->canvas, ctx->card_id, card_opacity);
    plexy_canvas_set_translation(ctx->canvas, ctx->card_id, 0.0f,
                                 -ctx->power_transition_px * dialog_progress);
  }

  for (int slot = 0; slot < GREETER_UI_POWER_OPTION_COUNT; ++slot) {
    if (!ctx->power_option_ids[slot])
      continue;

    int option_index = greeter_ui_power_index_for_slot(ctx, slot);
    bool selected = option_index == ctx->power_selected;
    bool focus_slot = slot == center_slot;
    float row_alpha = greeter_ui_power_option_alpha(ctx, slot);

    plexy_canvas_set_fill_color(ctx->canvas, ctx->power_option_ids[slot], 0.96f,
                                0.98f, 1.00f, focus_slot ? 0.15f : 0.0f);
    plexy_canvas_set_border(ctx->canvas, ctx->power_option_ids[slot], 1.00f,
                            1.00f, 1.00f, focus_slot ? 0.34f : 0.0f,
                            focus_slot ? 0.8f : 0.0f);

    if (!ctx->power_option_label_ids[slot])
      continue;

    plexy_canvas_set_translation(ctx->canvas, ctx->power_option_label_ids[slot],
                                 0.0f, item_offset);
    plexy_canvas_set_text(ctx->canvas, ctx->power_option_label_ids[slot],
                          greeter_ui_power_labels[option_index]);
    plexy_canvas_set_text_color(
        ctx->canvas, ctx->power_option_label_ids[slot],
        ctx->theme.text_primary[0], ctx->theme.text_primary[1],
        ctx->theme.text_primary[2], row_alpha * (selected ? 0.96f : 0.76f));
  }
}

void greeter_ui_default_theme(greeter_ui_theme_t *theme) {

  float bg[] = {0.00f, 0.00f, 0.00f, 0.00f};
  float card_bg[] = {0.96f, 0.98f, 1.00f, 0.22f};
  float card_border[] = {1.00f, 1.00f, 1.00f, 0.34f};
  float text_pri[] = {1.00f, 1.00f, 1.00f, 0.96f};
  float text_sec[] = {0.92f, 0.94f, 0.98f, 0.64f};
  float input_bg[] = {1.00f, 1.00f, 1.00f, 0.18f};
  float input_border[] = {1.00f, 1.00f, 1.00f, 0.26f};
  float input_focus[] = {1.00f, 1.00f, 1.00f, 0.50f};
  float button_bg[] = {0.40f, 0.58f, 0.90f, 0.94f};
  float button_hover[] = {0.48f, 0.66f, 0.96f, 0.96f};
  float error_text[] = {1.00f, 0.56f, 0.56f, 1.00f};
  float accent[] = {0.92f, 0.95f, 1.00f, 0.58f};

  memcpy(theme->bg, bg, sizeof(bg));
  memcpy(theme->card_bg, card_bg, sizeof(card_bg));
  memcpy(theme->card_border, card_border, sizeof(card_border));
  memcpy(theme->text_primary, text_pri, sizeof(text_pri));
  memcpy(theme->text_secondary, text_sec, sizeof(text_sec));
  memcpy(theme->input_bg, input_bg, sizeof(input_bg));
  memcpy(theme->input_border, input_border, sizeof(input_border));
  memcpy(theme->input_focus, input_focus, sizeof(input_focus));
  memcpy(theme->button_bg, button_bg, sizeof(button_bg));
  memcpy(theme->button_hover, button_hover, sizeof(button_hover));
  memcpy(theme->error_text, error_text, sizeof(error_text));
  memcpy(theme->accent, accent, sizeof(accent));
}

greeter_ui_ctx_t *greeter_ui_create(int width, int height, float scale) {
  greeter_ui_ctx_t *ctx = calloc(1, sizeof(*ctx));
  if (!ctx)
    return NULL;

  ctx->width = width;
  ctx->height = height;
  ctx->scale = scale;
  ctx->face = GREETER_UI_FACE_USER_SELECT;
  ctx->state = GREETER_STATE_USER_SELECT;
  ctx->fade_alpha = 1.0;
  ctx->cursor_visible = true;
  ctx->user_click_index = -1;

  greeter_ui_default_theme(&ctx->theme);

  ctx->canvas = plexy_canvas_create(width, height, PLEXY_CANVAS_TARGET_FBO);
  if (!ctx->canvas) {
    syslog(LOG_ERR, "plexy-dm: failed to create canvas");
    free(ctx);
    return NULL;
  }

  plexy_canvas_init_gl(ctx->canvas);

  static const char *font_candidates[] = {
      "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
      "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
      "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
      "/usr/share/fonts/gnu-free/FreeSans.otf",
      "/usr/share/fonts/Adwaita/AdwaitaSans-Regular.ttf",
      NULL};
  for (const char **fp = font_candidates; *fp; fp++) {
    if (access(*fp, R_OK) == 0) {
      plexy_canvas_set_font(ctx->canvas, *fp);
      syslog(LOG_INFO, "plexy-dm: loaded font %s", *fp);
      break;
    }
  }

  plexy_canvas_set_scale_factor(ctx->canvas, scale);
  plexy_canvas_set_dark_mode(ctx->canvas, 1);

  uint32_t root = plexy_canvas_root(ctx->canvas);
  ctx->root_id = root;
  float logical_size = (float)(width < height ? width : height);
  bool fullscreen_dialog = greeter_ui_is_fullscreen_dialog(width, height);
  float s = greeter_ui_scale_value(ctx, 1.0f);
  float card_w = fullscreen_dialog ? greeter_ui_clampf((float)width * 0.24f,
                                                       400.0f * s, 460.0f * s)
                                   : logical_size * 0.86f;
  float card_h = fullscreen_dialog ? greeter_ui_clampf((float)height * 0.44f,
                                                       420.0f * s, 470.0f * s)
                                   : logical_size * 0.86f;
  if (fullscreen_dialog) {
    card_w = fminf(card_w, (float)width - 48.0f * s);
    card_h = fminf(card_h, (float)height - 48.0f * s);
  }
  float card_pad = fullscreen_dialog ? greeter_ui_clampf(card_w * 0.070f,
                                                         26.0f * s, 32.0f * s)
                                     : logical_size * 0.06f;
  float gap = fullscreen_dialog
                  ? greeter_ui_clampf(card_h * 0.018f, 8.0f * s, 11.0f * s)
                  : logical_size * 0.022f;
  float avatar_sz = fullscreen_dialog ? greeter_ui_clampf(card_w * 0.130f,
                                                          52.0f * s, 58.0f * s)
                                      : logical_size * 0.105f;
  float text_w = card_w - card_pad * 2.0f;
  float row_h = fullscreen_dialog
                    ? greeter_ui_clampf(card_h * 0.150f, 64.0f * s, 72.0f * s)
                    : logical_size * 0.125f;
  float title_font = fullscreen_dialog ? 20.0f : logical_size * 0.054f;
  float title_h = fullscreen_dialog ? 28.0f * s : logical_size * 0.07f;
  float user_strip_h =
      fullscreen_dialog ? row_h * (float)GREETER_UI_VISIBLE_USERS + gap * 2.0f
                        : logical_size * 0.47f;
  float action_font = fullscreen_dialog ? 11.0f : logical_size * 0.021f;
  float error_font = fullscreen_dialog ? 11.0f : logical_size * 0.02f;
  float status_h = fullscreen_dialog ? 24.0f * s : logical_size * 0.055f;
  float small_font = fullscreen_dialog ? 10.0f : logical_size * 0.018f;
  float name_h = fullscreen_dialog ? 22.0f * s : logical_size * 0.04f;
  float meta_h = fullscreen_dialog ? 14.0f * s : logical_size * 0.028f;
  float row_pad_y = fullscreen_dialog ? 5.0f * s : logical_size * 0.014f;
  float row_pad_x = fullscreen_dialog ? 10.0f * s : logical_size * 0.020f;
  float row_gap = fullscreen_dialog ? 13.0f * s : logical_size * 0.022f;
  float strip_gap = fullscreen_dialog ? gap : logical_size * 0.014f;
  ctx->user_row_pitch = row_h + strip_gap;

  plexy_canvas_set_flex_direction(ctx->canvas, root, 0);
  plexy_canvas_set_align(ctx->canvas, root, 2);
  plexy_canvas_set_justify(ctx->canvas, root, 2);
  plexy_canvas_set_fill_color(ctx->canvas, root, 0, 0, 0, 0);

  ctx->card_id =
      plexy_canvas_create_widget(ctx->canvas, PLEXY_WIDGET_PANEL, root);
  plexy_canvas_set_size(ctx->canvas, ctx->card_id, card_w, card_h);
  plexy_canvas_set_corner_radius(ctx->canvas, ctx->card_id,
                                 fullscreen_dialog ? 8.0f * s
                                                   : logical_size * 0.03f);
  plexy_canvas_set_fill_color(ctx->canvas, ctx->card_id, ctx->theme.card_bg[0],
                              ctx->theme.card_bg[1], ctx->theme.card_bg[2],
                              ctx->theme.card_bg[3]);
  plexy_canvas_set_glass_material(ctx->canvas, ctx->card_id,
                                  fullscreen_dialog ? 0.48f : 0.0f);
  plexy_canvas_set_elevation(ctx->canvas, ctx->card_id,
                             fullscreen_dialog ? 3.0f : 0.0f);
  plexy_canvas_set_border(ctx->canvas, ctx->card_id, ctx->theme.card_border[0],
                          ctx->theme.card_border[1], ctx->theme.card_border[2],
                          ctx->theme.card_border[3],
                          fullscreen_dialog ? 0.8f * s : 1.0f);
  plexy_canvas_set_flex_direction(ctx->canvas, ctx->card_id, 0);
  plexy_canvas_set_align(ctx->canvas, ctx->card_id, 2);
  plexy_canvas_set_padding(ctx->canvas, ctx->card_id, card_pad,
                           card_pad * 1.05f, card_pad, card_pad * 1.05f);
  plexy_canvas_set_gap(ctx->canvas, ctx->card_id, gap);

  ctx->title_label_id =
      plexy_canvas_create_widget(ctx->canvas, PLEXY_WIDGET_LABEL, ctx->card_id);
  plexy_canvas_set_font_size(ctx->canvas, ctx->title_label_id, title_font);
  plexy_canvas_set_text_color(
      ctx->canvas, ctx->title_label_id, ctx->theme.text_primary[0],
      ctx->theme.text_primary[1], ctx->theme.text_primary[2],
      ctx->theme.text_primary[3]);
  plexy_canvas_set_text(ctx->canvas, ctx->title_label_id, "Sign in");
  plexy_canvas_set_size(ctx->canvas, ctx->title_label_id, text_w, title_h);
  plexy_canvas_set_justify(ctx->canvas, ctx->title_label_id, 2);
  plexy_canvas_set_margin(ctx->canvas, ctx->title_label_id, 0.0f, 0.0f,
                          logical_size * 0.008f, 0.0f);

  ctx->user_strip_id = plexy_canvas_create_widget(
      ctx->canvas, PLEXY_WIDGET_COLUMN, ctx->card_id);
  plexy_canvas_set_size(ctx->canvas, ctx->user_strip_id, text_w, user_strip_h);
  plexy_canvas_set_gap(ctx->canvas, ctx->user_strip_id, strip_gap);
  plexy_canvas_set_align(ctx->canvas, ctx->user_strip_id, 2);

  for (int i = 0; i < GREETER_UI_VISIBLE_USERS; ++i) {
    ctx->user_row_ids[i] = plexy_canvas_create_widget(
        ctx->canvas, PLEXY_WIDGET_ROW, ctx->user_strip_id);
    plexy_canvas_set_size(ctx->canvas, ctx->user_row_ids[i], text_w, row_h);
    plexy_canvas_set_corner_radius(ctx->canvas, ctx->user_row_ids[i],
                                   fullscreen_dialog ? 8.0f * s
                                                     : logical_size * 0.024f);
    plexy_canvas_set_padding(ctx->canvas, ctx->user_row_ids[i], row_pad_y,
                             row_pad_x, row_pad_y, row_pad_x);
    plexy_canvas_set_gap(ctx->canvas, ctx->user_row_ids[i], row_gap);
    plexy_canvas_set_align(ctx->canvas, ctx->user_row_ids[i], 2);
    plexy_canvas_set_justify(ctx->canvas, ctx->user_row_ids[i], 0);
    plexy_canvas_on_click(ctx->canvas, ctx->user_row_ids[i],
                          greeter_ui_on_user_row_click, ctx);

    ctx->user_avatar_ids[i] = plexy_canvas_create_widget(
        ctx->canvas, PLEXY_WIDGET_PANEL, ctx->user_row_ids[i]);
    plexy_canvas_set_size(ctx->canvas, ctx->user_avatar_ids[i], avatar_sz,
                          avatar_sz);
    plexy_canvas_set_corner_radius(ctx->canvas, ctx->user_avatar_ids[i],
                                   fullscreen_dialog ? 8.0f * s
                                                     : logical_size * 0.028f);

    ctx->user_avatar_glyph_ids[i] = plexy_canvas_create_widget(
        ctx->canvas, PLEXY_WIDGET_LABEL, ctx->user_avatar_ids[i]);
    plexy_canvas_set_size(ctx->canvas, ctx->user_avatar_glyph_ids[i], avatar_sz,
                          avatar_sz);
    plexy_canvas_set_font_size(ctx->canvas, ctx->user_avatar_glyph_ids[i],
                               fullscreen_dialog ? 14.0f
                                                 : logical_size * 0.032f);
    plexy_canvas_set_text_color(ctx->canvas, ctx->user_avatar_glyph_ids[i],
                                1.0f, 1.0f, 1.0f, 0.95f);
    plexy_canvas_set_justify(ctx->canvas, ctx->user_avatar_glyph_ids[i], 2);
    plexy_canvas_set_align(ctx->canvas, ctx->user_avatar_ids[i], 2);
    plexy_canvas_set_justify(ctx->canvas, ctx->user_avatar_ids[i], 2);

    float text_col_w = text_w - avatar_sz -
                       (fullscreen_dialog ? 40.0f * s : logical_size * 0.09f);
    uint32_t text_col = plexy_canvas_create_widget(
        ctx->canvas, PLEXY_WIDGET_COLUMN, ctx->user_row_ids[i]);
    ctx->user_text_col_ids[i] = text_col;
    plexy_canvas_set_size(ctx->canvas, text_col, text_col_w, row_h);
    plexy_canvas_set_align(ctx->canvas, text_col, 0);
    plexy_canvas_set_justify(ctx->canvas, text_col, 2);
    plexy_canvas_set_gap(ctx->canvas, text_col, logical_size * 0.006f);

    ctx->user_name_label_ids[i] =
        plexy_canvas_create_widget(ctx->canvas, PLEXY_WIDGET_LABEL, text_col);
    plexy_canvas_set_size(ctx->canvas, ctx->user_name_label_ids[i], text_col_w,
                          name_h);
    plexy_canvas_set_font_size(ctx->canvas, ctx->user_name_label_ids[i],
                               fullscreen_dialog ? 16.0f
                                                 : logical_size * 0.036f);

    ctx->user_meta_label_ids[i] =
        plexy_canvas_create_widget(ctx->canvas, PLEXY_WIDGET_LABEL, text_col);
    plexy_canvas_set_size(ctx->canvas, ctx->user_meta_label_ids[i], text_col_w,
                          meta_h);
    plexy_canvas_set_font_size(ctx->canvas, ctx->user_meta_label_ids[i],
                               fullscreen_dialog ? 11.0f
                                                 : logical_size * 0.020f);
  }

  ctx->action_label_id =
      plexy_canvas_create_widget(ctx->canvas, PLEXY_WIDGET_LABEL, ctx->card_id);
  plexy_canvas_set_font_size(ctx->canvas, ctx->action_label_id, action_font);
  plexy_canvas_set_text_color(ctx->canvas, ctx->action_label_id,
                              ctx->theme.accent[0], ctx->theme.accent[1],
                              ctx->theme.accent[2], 0.54f);
  plexy_canvas_set_text(ctx->canvas, ctx->action_label_id, "Enter to continue");
  plexy_canvas_set_size(ctx->canvas, ctx->action_label_id, text_w,
                        fullscreen_dialog ? 18.0f * s : logical_size * 0.034f);
  plexy_canvas_set_justify(ctx->canvas, ctx->action_label_id, 2);

  ctx->error_label_id =
      plexy_canvas_create_widget(ctx->canvas, PLEXY_WIDGET_LABEL, ctx->card_id);
  plexy_canvas_set_font_size(ctx->canvas, ctx->error_label_id, error_font);
  plexy_canvas_set_text_color(
      ctx->canvas, ctx->error_label_id, ctx->theme.error_text[0],
      ctx->theme.error_text[1], ctx->theme.error_text[2],
      ctx->theme.error_text[3]);
  plexy_canvas_set_text(ctx->canvas, ctx->error_label_id, "");
  plexy_canvas_set_size(ctx->canvas, ctx->error_label_id, text_w,
                        fullscreen_dialog ? 18.0f * s : logical_size * 0.03f);
  plexy_canvas_set_justify(ctx->canvas, ctx->error_label_id, 2);

  uint32_t status_bar =
      plexy_canvas_create_widget(ctx->canvas, PLEXY_WIDGET_ROW, ctx->card_id);
  plexy_canvas_set_size(ctx->canvas, status_bar, text_w, status_h);
  plexy_canvas_set_align(ctx->canvas, status_bar, 2);
  plexy_canvas_set_justify(ctx->canvas, status_bar, 3);
  plexy_canvas_set_margin(ctx->canvas, status_bar, logical_size * 0.01f, 0.0f,
                          0.0f, 0.0f);

  uint32_t clock_col =
      plexy_canvas_create_widget(ctx->canvas, PLEXY_WIDGET_COLUMN, status_bar);
  plexy_canvas_set_size(ctx->canvas, clock_col, text_w * 0.40f, status_h);
  plexy_canvas_set_align(ctx->canvas, clock_col, 2);
  plexy_canvas_set_justify(ctx->canvas, clock_col, 2);
  plexy_canvas_set_gap(ctx->canvas, clock_col, 0.0f);

  ctx->clock_label_id =
      plexy_canvas_create_widget(ctx->canvas, PLEXY_WIDGET_LABEL, clock_col);
  plexy_canvas_set_font_size(ctx->canvas, ctx->clock_label_id,
                             fullscreen_dialog ? 12.0f : logical_size * 0.022f);
  plexy_canvas_set_text_color(
      ctx->canvas, ctx->clock_label_id, ctx->theme.text_primary[0],
      ctx->theme.text_primary[1], ctx->theme.text_primary[2], 0.78f);
  plexy_canvas_set_text(ctx->canvas, ctx->clock_label_id, "");
  plexy_canvas_set_size(ctx->canvas, ctx->clock_label_id, text_w * 0.40f,
                        fullscreen_dialog ? 18.0f * s : logical_size * 0.028f);
  plexy_canvas_set_justify(ctx->canvas, ctx->clock_label_id, 2);

  ctx->clock_date_label_id =
      plexy_canvas_create_widget(ctx->canvas, PLEXY_WIDGET_LABEL, clock_col);
  plexy_canvas_set_font_size(ctx->canvas, ctx->clock_date_label_id,
                             fullscreen_dialog ? 10.0f : logical_size * 0.015f);
  plexy_canvas_set_text_color(
      ctx->canvas, ctx->clock_date_label_id, ctx->theme.text_secondary[0],
      ctx->theme.text_secondary[1], ctx->theme.text_secondary[2], 0.48f);
  plexy_canvas_set_text(ctx->canvas, ctx->clock_date_label_id, "");
  plexy_canvas_set_size(ctx->canvas, ctx->clock_date_label_id, text_w * 0.40f,
                        fullscreen_dialog ? 0.0f : logical_size * 0.020f);
  plexy_canvas_set_justify(ctx->canvas, ctx->clock_date_label_id, 2);
  if (fullscreen_dialog)
    plexy_canvas_set_visible(ctx->canvas, ctx->clock_date_label_id, 0);

  ctx->power_btn_id =
      plexy_canvas_create_widget(ctx->canvas, PLEXY_WIDGET_LABEL, status_bar);
  plexy_canvas_set_font_size(ctx->canvas, ctx->power_btn_id,
                             fullscreen_dialog ? 10.0f : logical_size * 0.016f);
  plexy_canvas_set_text_color(
      ctx->canvas, ctx->power_btn_id, ctx->theme.text_secondary[0],
      ctx->theme.text_secondary[1], ctx->theme.text_secondary[2], 0.46f);
  plexy_canvas_set_text(ctx->canvas, ctx->power_btn_id, "F1  Power");
  plexy_canvas_set_size(ctx->canvas, ctx->power_btn_id, text_w * 0.26f,
                        fullscreen_dialog ? 16.0f * s : logical_size * 0.03f);
  plexy_canvas_set_justify(ctx->canvas, ctx->power_btn_id, 1);

  float power_w = fullscreen_dialog ? greeter_ui_clampf(card_w * 0.86f,
                                                        340.0f * s, 390.0f * s)
                                    : logical_size * 0.52f;
  float power_h = fullscreen_dialog ? 300.0f * s : logical_size * 0.36f;
  float power_pad = fullscreen_dialog ? 28.0f * s : logical_size * 0.034f;
  float power_gap = fullscreen_dialog ? 10.0f * s : logical_size * 0.018f;
  float power_section_gap =
      fullscreen_dialog ? 14.0f * s : logical_size * 0.022f;
  float power_title_font = fullscreen_dialog ? 20.0f : logical_size * 0.032f;
  float power_title_h = fullscreen_dialog ? 30.0f * s : logical_size * 0.045f;
  float power_opt_font = fullscreen_dialog ? 14.0f : logical_size * 0.024f;
  float power_opt_h = fullscreen_dialog ? 46.0f * s : logical_size * 0.060f;
  float power_hint_font = fullscreen_dialog ? 11.0f : logical_size * 0.016f;
  float power_hint_h = fullscreen_dialog ? 18.0f * s : logical_size * 0.025f;
  float power_content_w = power_w - power_pad * 2.0f;
  float power_strip_h = power_opt_h * (float)GREETER_UI_POWER_OPTION_COUNT +
                        power_gap * (float)(GREETER_UI_POWER_OPTION_COUNT - 1);
  ctx->power_row_pitch = power_opt_h + power_gap;
  ctx->power_transition_px = fullscreen_dialog ? 18.0f : logical_size * 0.032f;

  ctx->power_overlay_id =
      plexy_canvas_create_widget(ctx->canvas, PLEXY_WIDGET_PANEL, root);
  plexy_canvas_set_size(ctx->canvas, ctx->power_overlay_id, power_w, power_h);
  plexy_canvas_set_corner_radius(ctx->canvas, ctx->power_overlay_id,
                                 fullscreen_dialog ? 8.0f * s
                                                   : logical_size * 0.024f);
  plexy_canvas_set_fill_color(ctx->canvas, ctx->power_overlay_id,
                              ctx->theme.card_bg[0], ctx->theme.card_bg[1],
                              ctx->theme.card_bg[2], ctx->theme.card_bg[3]);
  plexy_canvas_set_glass_material(ctx->canvas, ctx->power_overlay_id,
                                  fullscreen_dialog ? 0.48f : 0.0f);
  plexy_canvas_set_elevation(ctx->canvas, ctx->power_overlay_id,
                             fullscreen_dialog ? 3.0f : 0.0f);
  plexy_canvas_set_border(ctx->canvas, ctx->power_overlay_id,
                          ctx->theme.card_border[0], ctx->theme.card_border[1],
                          ctx->theme.card_border[2], ctx->theme.card_border[3],
                          fullscreen_dialog ? 0.8f * s : 1.0f);
  plexy_canvas_set_flex_direction(ctx->canvas, ctx->power_overlay_id, 0);
  plexy_canvas_set_align(ctx->canvas, ctx->power_overlay_id, 2);
  plexy_canvas_set_justify(ctx->canvas, ctx->power_overlay_id, 2);
  plexy_canvas_set_padding(ctx->canvas, ctx->power_overlay_id, power_pad,
                           power_pad, power_pad, power_pad);
  plexy_canvas_set_gap(ctx->canvas, ctx->power_overlay_id, power_section_gap);
  plexy_canvas_set_visible(ctx->canvas, ctx->power_overlay_id, 0);

  uint32_t pm_title = plexy_canvas_create_widget(
      ctx->canvas, PLEXY_WIDGET_LABEL, ctx->power_overlay_id);
  plexy_canvas_set_font_size(ctx->canvas, pm_title, power_title_font);
  plexy_canvas_set_text_color(ctx->canvas, pm_title, ctx->theme.text_primary[0],
                              ctx->theme.text_primary[1],
                              ctx->theme.text_primary[2], 0.95f);
  plexy_canvas_set_text(ctx->canvas, pm_title, "Power");
  plexy_canvas_set_size(ctx->canvas, pm_title, power_content_w, power_title_h);
  plexy_canvas_set_justify(ctx->canvas, pm_title, 2);

  ctx->power_option_strip_id = plexy_canvas_create_widget(
      ctx->canvas, PLEXY_WIDGET_COLUMN, ctx->power_overlay_id);
  plexy_canvas_set_size(ctx->canvas, ctx->power_option_strip_id,
                        power_content_w, power_strip_h);
  plexy_canvas_set_gap(ctx->canvas, ctx->power_option_strip_id, power_gap);
  plexy_canvas_set_align(ctx->canvas, ctx->power_option_strip_id, 2);
  plexy_canvas_set_justify(ctx->canvas, ctx->power_option_strip_id, 2);

  for (int i = 0; i < GREETER_UI_POWER_OPTION_COUNT; i++) {
    ctx->power_option_ids[i] = plexy_canvas_create_widget(
        ctx->canvas, PLEXY_WIDGET_PANEL, ctx->power_option_strip_id);
    plexy_canvas_set_size(ctx->canvas, ctx->power_option_ids[i],
                          power_content_w, power_opt_h);
    plexy_canvas_set_corner_radius(ctx->canvas, ctx->power_option_ids[i],
                                   fullscreen_dialog ? 8.0f * s
                                                     : logical_size * 0.012f);
    plexy_canvas_set_fill_color(ctx->canvas, ctx->power_option_ids[i], 0.96f,
                                0.98f, 1.00f, 0.0f);
    plexy_canvas_set_border(ctx->canvas, ctx->power_option_ids[i], 1.00f, 1.00f,
                            1.00f, 0.0f, 0.0f);
    plexy_canvas_set_flex_direction(ctx->canvas, ctx->power_option_ids[i], 1);
    plexy_canvas_set_align(ctx->canvas, ctx->power_option_ids[i], 2);
    plexy_canvas_set_justify(ctx->canvas, ctx->power_option_ids[i], 2);

    ctx->power_option_label_ids[i] = plexy_canvas_create_widget(
        ctx->canvas, PLEXY_WIDGET_LABEL, ctx->power_option_ids[i]);
    plexy_canvas_set_font_size(ctx->canvas, ctx->power_option_label_ids[i],
                               power_opt_font);
    plexy_canvas_set_text_color(
        ctx->canvas, ctx->power_option_label_ids[i], ctx->theme.text_primary[0],
        ctx->theme.text_primary[1], ctx->theme.text_primary[2], 0.76f);
    plexy_canvas_set_text(ctx->canvas, ctx->power_option_label_ids[i],
                          greeter_ui_power_labels[i]);
    plexy_canvas_set_size(ctx->canvas, ctx->power_option_label_ids[i],
                          power_content_w, power_opt_h);
    plexy_canvas_set_justify(ctx->canvas, ctx->power_option_label_ids[i], 2);
  }

  uint32_t pm_hint = plexy_canvas_create_widget(ctx->canvas, PLEXY_WIDGET_LABEL,
                                                ctx->power_overlay_id);
  plexy_canvas_set_font_size(ctx->canvas, pm_hint, power_hint_font);
  plexy_canvas_set_text_color(
      ctx->canvas, pm_hint, ctx->theme.text_secondary[0],
      ctx->theme.text_secondary[1], ctx->theme.text_secondary[2], 0.50f);
  plexy_canvas_set_text(ctx->canvas, pm_hint, "Esc to cancel");
  plexy_canvas_set_size(ctx->canvas, pm_hint, power_content_w, power_hint_h);
  plexy_canvas_set_justify(ctx->canvas, pm_hint, 2);

  syslog(LOG_INFO,
         "plexy-dm: user-select UI widget tree built (card=%u, list=%u)",
         ctx->card_id, ctx->user_strip_id);

  return ctx;
}

void greeter_ui_destroy(greeter_ui_ctx_t *ctx) {
  if (!ctx)
    return;
  if (ctx->canvas)
    plexy_canvas_destroy(ctx->canvas);
  explicit_bzero(ctx->password, sizeof(ctx->password));
  free(ctx);
}

void greeter_ui_set_theme(greeter_ui_ctx_t *ctx,
                          const greeter_ui_theme_t *theme) {
  if (ctx && theme)
    ctx->theme = *theme;
}

int greeter_ui_load_background(greeter_ui_ctx_t *ctx, const char *path) {

  (void)ctx;
  (void)path;
  return 0;
}

static void greeter_ui_reset_user_slide(greeter_ui_ctx_t *ctx) {
  if (!ctx)
    return;

  ctx->user_slide_from = ctx->selected_user;
  ctx->user_slide_to = ctx->selected_user;
  ctx->user_slide_dir = 0;
  ctx->user_slide_elapsed = GREETER_UI_USER_SLIDE_MS;
}

static void greeter_ui_reset_power_slide(greeter_ui_ctx_t *ctx) {
  if (!ctx)
    return;

  ctx->power_slide_from = ctx->power_selected;
  ctx->power_slide_to = ctx->power_selected;
  ctx->power_slide_dir = 0;
  ctx->power_slide_elapsed = GREETER_UI_POWER_SLIDE_MS;
}

static void greeter_ui_finish_power_transition(greeter_ui_ctx_t *ctx) {
  if (!ctx)
    return;

  if (ctx->power_transition_dir < 0)
    ctx->power_menu_present = false;
  else if (ctx->power_transition_dir > 0)
    ctx->power_menu_present = true;

  ctx->power_transition_dir = 0;
  ctx->power_transition_elapsed = GREETER_UI_DIALOG_TRANSITION_MS;
}

static void greeter_ui_set_power_menu_open(greeter_ui_ctx_t *ctx, bool open) {
  if (!ctx)
    return;

  if (open) {
    ctx->power_menu_visible = true;
    ctx->power_menu_present = true;
    ctx->power_selected = 0;
  } else {
    ctx->power_menu_visible = false;
    if (!ctx->power_menu_present) {
      greeter_ui_finish_power_transition(ctx);
      return;
    }
  }

  greeter_ui_reset_power_slide(ctx);
  ctx->power_transition_dir = open ? 1 : -1;
  ctx->power_transition_elapsed = 0.0;
  ctx->animating = true;
}

static void greeter_ui_set_selected_power(greeter_ui_ctx_t *ctx, int index,
                                          int direction, bool animate) {
  if (!ctx)
    return;

  index = greeter_ui_wrap_power_index(index);
  int old_index = ctx->power_selected;
  if (old_index == index)
    return;

  ctx->power_selected = index;

  if (animate && ctx->power_menu_visible && direction != 0) {
    ctx->power_slide_from = old_index;
    ctx->power_slide_to = index;
    ctx->power_slide_dir = direction > 0 ? 1 : -1;
    ctx->power_slide_elapsed = 0.0;
    ctx->animating = true;
  } else {
    greeter_ui_reset_power_slide(ctx);
  }
}

static void greeter_ui_set_selected_user_index(greeter_ui_ctx_t *ctx, int index,
                                               int direction, bool animate) {
  if (!ctx || index < 0 || index >= ctx->user_count)
    return;

  int old_index = ctx->selected_user;
  if (old_index == index) {
    greeter_ui_sync_username_override(ctx);
    return;
  }

  ctx->selected_user = index;

  if (animate && ctx->face == GREETER_UI_FACE_USER_SELECT &&
      ctx->user_count >= GREETER_UI_VISIBLE_USERS && direction != 0) {
    ctx->user_slide_from = old_index;
    ctx->user_slide_to = index;
    ctx->user_slide_dir = direction > 0 ? 1 : -1;
    ctx->user_slide_elapsed = 0.0;
    ctx->animating = true;
  } else {
    greeter_ui_reset_user_slide(ctx);
  }

  greeter_ui_sync_username_override(ctx);
}

void greeter_ui_set_users(greeter_ui_ctx_t *ctx, const plexy_dm_user_t *users,
                          int count) {
  if (!ctx)
    return;
  ctx->users = users;
  ctx->user_count = count;
  if (ctx->selected_user >= count)
    ctx->selected_user = 0;
  greeter_ui_reset_user_slide(ctx);
  greeter_ui_sync_username_override(ctx);
}

void greeter_ui_select_user(greeter_ui_ctx_t *ctx, int index) {
  if (!ctx || index < 0 || index >= ctx->user_count)
    return;
  greeter_ui_set_selected_user_index(ctx, index, 0, false);
  greeter_ui_clear_password(ctx);
  ctx->error_msg[0] = '\0';
  ctx->error_timer = 0;
}

bool greeter_ui_handle_key(greeter_ui_ctx_t *ctx,
                           const greeter_ui_input_t *input) {
  if (!ctx || !input)
    return false;

  switch (input->type) {
  case UI_KEY_CHAR:
    if (ctx->face != GREETER_UI_FACE_PASSWORD)
      break;
    if (ctx->password_len < PLEXY_DM_MAX_PASSWORD - 1) {

      if (input->codepoint >= 32 && input->codepoint < 127) {
        ctx->password[ctx->password_len++] = (char)input->codepoint;
        ctx->password[ctx->password_len] = '\0';
        ctx->cursor_visible = true;
        ctx->cursor_timer = 0;
        return true;
      }
    }
    break;

  case UI_KEY_BACKSPACE:
    if (ctx->face != GREETER_UI_FACE_PASSWORD)
      break;
    if (ctx->password_len > 0) {
      ctx->password[--ctx->password_len] = '\0';
      return true;
    }
    break;

  case UI_KEY_DELETE:
    if (ctx->face != GREETER_UI_FACE_PASSWORD)
      break;
    if (ctx->password_len > 0) {
      greeter_ui_clear_password(ctx);
      return true;
    }
    break;

  case UI_KEY_ENTER:
    if (ctx->power_menu_visible) {
      switch (ctx->power_selected) {
      case 0:
        ctx->power_result = POWER_RESULT_SHUTDOWN;
        break;
      case 1:
        ctx->power_result = POWER_RESULT_REBOOT;
        break;
      case 2:
        ctx->power_result = POWER_RESULT_SUSPEND;
        break;
      }
      greeter_ui_set_power_menu_open(ctx, false);
      return true;
    }
    if (ctx->wifi_mode) {
      ctx->wifi_connect_requested = true;
      return true;
    }

    return true;

  case UI_KEY_ESCAPE:
    if (ctx->power_menu_visible) {
      greeter_ui_set_power_menu_open(ctx, false);
      return true;
    }
    if (ctx->wifi_mode) {
      ctx->wifi_skip_requested = true;
      return true;
    }
    if (ctx->face == GREETER_UI_FACE_PASSWORD) {
      greeter_ui_clear_password(ctx);
      return true;
    }
    break;

  case UI_KEY_RIGHT:
  case UI_KEY_TAB:
  case UI_KEY_DOWN:
    if (ctx->power_menu_visible) {
      greeter_ui_set_selected_power(ctx, ctx->power_selected + 1, 1, true);
      return true;
    }
    if (ctx->wifi_mode) {
      if (input->type == UI_KEY_TAB) {
        ctx->wifi_skip_requested = true;
        return true;
      }
      if (ctx->user_count > 1) {
        int next = greeter_ui_wrap_user_index(ctx, ctx->selected_user + 1);
        greeter_ui_set_selected_user_index(ctx, next, 1, true);
        return true;
      }
      break;
    }
    if (ctx->face == GREETER_UI_FACE_USER_SELECT && ctx->user_count > 1) {
      int next = greeter_ui_wrap_user_index(ctx, ctx->selected_user + 1);
      greeter_ui_set_selected_user_index(ctx, next, 1, true);
      return true;
    }
    break;

  case UI_KEY_LEFT:
  case UI_KEY_UP:
    if (ctx->power_menu_visible) {
      greeter_ui_set_selected_power(ctx, ctx->power_selected - 1, -1, true);
      return true;
    }
    if (ctx->wifi_mode) {
      if (ctx->user_count > 1) {
        int prev = greeter_ui_wrap_user_index(ctx, ctx->selected_user - 1);
        greeter_ui_set_selected_user_index(ctx, prev, -1, true);
        return true;
      }
      break;
    }
    if (ctx->face == GREETER_UI_FACE_USER_SELECT && ctx->user_count > 1) {
      int prev = greeter_ui_wrap_user_index(ctx, ctx->selected_user - 1);
      greeter_ui_set_selected_user_index(ctx, prev, -1, true);
      return true;
    }
    break;

  case UI_KEY_F1:
    if (ctx->power_overlay_id) {
      greeter_ui_set_power_menu_open(ctx, !ctx->power_menu_visible);
      return true;
    }
    break;

  default:
    break;
  }

  return false;
}

bool greeter_ui_handle_pointer(greeter_ui_ctx_t *ctx, int x, int y,
                               bool pressed) {
  if (!ctx || !ctx->canvas || !pressed)
    return false;

  plexy_canvas_inject_mouse_button(ctx->canvas, 0, pressed ? 1 : 0, (float)x,
                                   (float)y);
  return true;
}

void greeter_ui_set_state(greeter_ui_ctx_t *ctx, greeter_state_t state) {
  if (!ctx)
    return;
  ctx->state = state;

  if (state == GREETER_STATE_AUTH_FAILED) {
    ctx->shake_timer = SHAKE_DURATION_MS;
    ctx->animating = true;
  }
}

void greeter_ui_set_error(greeter_ui_ctx_t *ctx, const char *msg) {
  if (!ctx)
    return;
  if (msg) {
    snprintf(ctx->error_msg, sizeof(ctx->error_msg), "%s", msg);
    ctx->error_timer = 5000.0;
  } else {
    ctx->error_msg[0] = '\0';
    ctx->error_timer = 0;
  }
}

void greeter_ui_set_status(greeter_ui_ctx_t *ctx, const char *msg) {
  if (!ctx)
    return;
  if (ctx->subtitle_label_id) {
    plexy_canvas_set_text(ctx->canvas, ctx->subtitle_label_id, msg ? msg : "");

    plexy_canvas_set_text_color(ctx->canvas, ctx->subtitle_label_id, 0.75f,
                                0.85f, 1.0f, msg ? 0.85f : 0.0f);
    plexy_canvas_set_visible(ctx->canvas, ctx->subtitle_label_id,
                             msg && msg[0] ? 1 : 0);
  }
}

const char *greeter_ui_get_password(const greeter_ui_ctx_t *ctx) {
  return ctx ? ctx->password : "";
}

const char *greeter_ui_get_selected_user(const greeter_ui_ctx_t *ctx) {
  if (!ctx || !ctx->users || ctx->selected_user >= ctx->user_count)
    return NULL;
  return ctx->users[ctx->selected_user].username;
}

void greeter_ui_clear_password(greeter_ui_ctx_t *ctx) {
  if (!ctx)
    return;
  explicit_bzero(ctx->password, sizeof(ctx->password));
  ctx->password_len = 0;
}

static void render_clock(greeter_ui_ctx_t *ctx) {
  if (!ctx->clock_label_id)
    return;

  time_t now = time(NULL);
  time_t now_min = now / 60;
  if (now_min == ctx->last_clock_min)
    return;
  ctx->last_clock_min = now_min;

  struct tm tm;
  localtime_r(&now, &tm);

  char timebuf[32];
  if (ctx->clock_24h)
    strftime(timebuf, sizeof(timebuf), "%H:%M", &tm);
  else
    strftime(timebuf, sizeof(timebuf), "%l:%M %p", &tm);

  plexy_canvas_set_text(ctx->canvas, ctx->clock_label_id, timebuf);

  if (ctx->clock_date_label_id) {
    char datebuf[64];
    strftime(datebuf, sizeof(datebuf), "%A, %B %e", &tm);
    plexy_canvas_set_text(ctx->canvas, ctx->clock_date_label_id, datebuf);
  }
}

void greeter_ui_render(greeter_ui_ctx_t *ctx, double delta_ms) {
  if (!ctx || !ctx->canvas)
    return;

  float dt = (float)(delta_ms / 1000.0);

  if (ctx->shake_timer > 0) {
    ctx->shake_timer -= delta_ms;
    if (ctx->shake_timer <= 0) {
      ctx->shake_timer = 0;
      if (ctx->state == GREETER_STATE_AUTH_FAILED)
        ctx->state = GREETER_STATE_PASSWORD;
    }
  }

  ctx->cursor_timer += delta_ms;
  if (ctx->cursor_timer >= CURSOR_BLINK_MS) {
    ctx->cursor_timer = 0;
    ctx->cursor_visible = !ctx->cursor_visible;
  }

  if (ctx->error_timer > 0)
    ctx->error_timer -= delta_ms;

  if (greeter_ui_user_slide_active(ctx)) {
    double slide_dt = delta_ms > 34.0 ? 16.67 : delta_ms;
    ctx->user_slide_elapsed += slide_dt;
    if (ctx->user_slide_elapsed >= GREETER_UI_USER_SLIDE_MS)
      greeter_ui_reset_user_slide(ctx);
  }

  if (greeter_ui_power_slide_active(ctx)) {
    double slide_dt = delta_ms > 34.0 ? 16.67 : delta_ms;
    ctx->power_slide_elapsed += slide_dt;
    if (ctx->power_slide_elapsed >= GREETER_UI_POWER_SLIDE_MS)
      greeter_ui_reset_power_slide(ctx);
  }

  if (greeter_ui_power_transition_active(ctx)) {
    double dialog_dt = delta_ms > 34.0 ? 16.67 : delta_ms;
    ctx->power_transition_elapsed += dialog_dt;
    if (ctx->power_transition_elapsed >= GREETER_UI_DIALOG_TRANSITION_MS)
      ctx->power_transition_elapsed = GREETER_UI_DIALOG_TRANSITION_MS;
  }

  const plexy_dm_user_t *sel_user = NULL;
  if (ctx->users && ctx->selected_user < ctx->user_count)
    sel_user = &ctx->users[ctx->selected_user];

  const char *display_name = NULL;
  if (ctx->face == GREETER_UI_FACE_PASSWORD && ctx->username_override[0]) {
    display_name = ctx->username_override;
  } else if (sel_user) {
    display_name =
        sel_user->realname[0] ? sel_user->realname : sel_user->username;
  }

  if (ctx->face == GREETER_UI_FACE_USER_SELECT) {
    greeter_ui_update_user_rows(ctx);
  }

  if (ctx->username_label_id && display_name) {
    plexy_canvas_set_text(ctx->canvas, ctx->username_label_id, display_name);
  } else if (ctx->username_label_id) {
    plexy_canvas_set_text(ctx->canvas, ctx->username_label_id,
                          ctx->face == GREETER_UI_FACE_PASSWORD ? ""
                                                                : "Sign in");
  }

  if (ctx->avatar_id) {
    float ar, ag, ab;
    char avatar_icon[PLEXY_DM_MAX_PATH + 16];
    greeter_ui_avatar_tint(display_name ? display_name : "", &ar, &ag, &ab);
    greeter_ui_avatar_icon_text(sel_user, display_name, avatar_icon,
                                sizeof(avatar_icon));
    plexy_canvas_set_fill_color(ctx->canvas, ctx->avatar_id, ar, ag, ab, 0.12f);
    plexy_canvas_set_border(ctx->canvas, ctx->avatar_id, 1.00f, 1.00f, 1.00f,
                            0.34f, 0.8f);
    if (ctx->avatar_glyph_id)
      plexy_canvas_set_text(ctx->canvas, ctx->avatar_glyph_id, avatar_icon);
  }

  if (ctx->password_text_id) {
    if (ctx->password_len > 0) {
      char bullets[33];
      int n = ctx->password_len < 32 ? ctx->password_len : 32;
      for (int i = 0; i < n; i++)
        bullets[i] = '*';
      bullets[n] = '\0';
      plexy_canvas_set_text(ctx->canvas, ctx->password_text_id, bullets);
      plexy_canvas_set_text_color(
          ctx->canvas, ctx->password_text_id, ctx->theme.text_primary[0],
          ctx->theme.text_primary[1], ctx->theme.text_primary[2],
          ctx->theme.text_primary[3]);
      plexy_canvas_set_align(ctx->canvas, ctx->password_text_id, 2);
      plexy_canvas_set_justify(ctx->canvas, ctx->password_text_id, 2);
    } else {
      plexy_canvas_set_text(ctx->canvas, ctx->password_text_id, "Password");
      plexy_canvas_set_text_color(
          ctx->canvas, ctx->password_text_id, ctx->theme.text_secondary[0],
          ctx->theme.text_secondary[1], ctx->theme.text_secondary[2], 0.75f);
      plexy_canvas_set_align(ctx->canvas, ctx->password_text_id, 2);
      plexy_canvas_set_justify(ctx->canvas, ctx->password_text_id, 2);
    }
  }

  if (ctx->password_field_id) {
    if (ctx->state == GREETER_STATE_PASSWORD ||
        ctx->state == GREETER_STATE_LOCKED) {
      plexy_canvas_set_border(
          ctx->canvas, ctx->password_field_id, ctx->theme.input_focus[0],
          ctx->theme.input_focus[1], ctx->theme.input_focus[2],
          ctx->theme.input_focus[3], 1.0f);
    } else {
      plexy_canvas_set_border(
          ctx->canvas, ctx->password_field_id, ctx->theme.input_border[0],
          ctx->theme.input_border[1], ctx->theme.input_border[2],
          ctx->theme.input_border[3], 0.8f);
    }
  }

  if (ctx->error_msg[0] && ctx->error_timer > 0) {
    plexy_canvas_set_text(ctx->canvas, ctx->error_label_id, ctx->error_msg);
    plexy_canvas_set_visible(ctx->canvas, ctx->error_label_id, 1);
  } else {
    plexy_canvas_set_text(ctx->canvas, ctx->error_label_id, "");
    plexy_canvas_set_visible(ctx->canvas, ctx->error_label_id, 0);
  }

  render_clock(ctx);

  if (ctx->caps_lock_label_id) {
    if (ctx->caps_lock_on) {
      plexy_canvas_set_text(ctx->canvas, ctx->caps_lock_label_id,
                            "\xe2\x9a\xa0  Caps Lock is on");
      plexy_canvas_set_visible(ctx->canvas, ctx->caps_lock_label_id, 1);
    } else {
      plexy_canvas_set_text(ctx->canvas, ctx->caps_lock_label_id, "");
      plexy_canvas_set_visible(ctx->canvas, ctx->caps_lock_label_id, 0);
    }
  }

  if (ctx->authenticating && ctx->action_label_id) {
    plexy_canvas_set_text(ctx->canvas, ctx->action_label_id,
                          "Authenticating...");
    plexy_canvas_set_text_color(
        ctx->canvas, ctx->action_label_id, ctx->theme.text_primary[0],
        ctx->theme.text_primary[1], ctx->theme.text_primary[2], 0.80f);
  } else if (ctx->action_label_id && !ctx->authenticating) {
    const char *hint;
    if (ctx->wifi_mode)
      hint = "Enter \xc2\xb7 Connect    Tab \xc2\xb7 Skip";
    else if (ctx->face == GREETER_UI_FACE_PASSWORD)
      hint = "Enter to log in, Esc to go back";
    else
      hint = "Enter to continue";
    plexy_canvas_set_text(ctx->canvas, ctx->action_label_id, hint);
    plexy_canvas_set_text_color(ctx->canvas, ctx->action_label_id,
                                ctx->theme.accent[0], ctx->theme.accent[1],
                                ctx->theme.accent[2], 0.54f);
  }

  greeter_ui_update_power_options(ctx);
  if (ctx->power_transition_dir != 0 &&
      ctx->power_transition_elapsed >= GREETER_UI_DIALOG_TRANSITION_MS)
    greeter_ui_finish_power_transition(ctx);

  plexy_canvas_begin_frame(ctx->canvas, dt);
  plexy_canvas_end_frame(ctx->canvas);

  ctx->animating = (ctx->shake_timer > 0 || ctx->error_timer > 0 ||
                    ctx->authenticating || greeter_ui_user_slide_active(ctx) ||
                    greeter_ui_power_slide_active(ctx) ||
                    greeter_ui_power_transition_active(ctx));
}

bool greeter_ui_animating(const greeter_ui_ctx_t *ctx) {
  return ctx ? ctx->animating : false;
}

PlexyCanvas *greeter_ui_get_canvas(const greeter_ui_ctx_t *ctx) {
  return ctx ? ctx->canvas : NULL;
}

void greeter_ui_set_selected_username(greeter_ui_ctx_t *ctx,
                                      const char *username) {
  if (!ctx || !username)
    return;
  snprintf(ctx->username_override, sizeof(ctx->username_override), "%s",
           username);
  if (ctx->username_label_id)
    plexy_canvas_set_text(ctx->canvas, ctx->username_label_id, username);
}

greeter_ui_ctx_t *greeter_ui_create_password(int width, int height,
                                             float scale) {
  greeter_ui_ctx_t *ctx = calloc(1, sizeof(*ctx));
  if (!ctx)
    return NULL;

  ctx->width = width;
  ctx->height = height;
  ctx->scale = scale;
  ctx->face = GREETER_UI_FACE_PASSWORD;
  ctx->state = GREETER_STATE_PASSWORD;
  ctx->fade_alpha = 1.0;
  ctx->cursor_visible = true;

  greeter_ui_default_theme(&ctx->theme);

  ctx->canvas = plexy_canvas_create(width, height, PLEXY_CANVAS_TARGET_FBO);
  if (!ctx->canvas) {
    syslog(LOG_ERR, "plexy-dm: failed to create password canvas");
    free(ctx);
    return NULL;
  }

  plexy_canvas_init_gl(ctx->canvas);

  static const char *font_candidates[] = {
      "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
      "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
      "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
      "/usr/share/fonts/gnu-free/FreeSans.otf",
      "/usr/share/fonts/Adwaita/AdwaitaSans-Regular.ttf",
      NULL};
  for (const char **fp = font_candidates; *fp; fp++) {
    if (access(*fp, R_OK) == 0) {
      plexy_canvas_set_font(ctx->canvas, *fp);
      break;
    }
  }

  plexy_canvas_set_scale_factor(ctx->canvas, scale);
  plexy_canvas_set_dark_mode(ctx->canvas, 1);

  uint32_t root = plexy_canvas_root(ctx->canvas);
  ctx->root_id = root;
  float logical_size = (float)(width < height ? width : height);
  bool fullscreen_dialog = greeter_ui_is_fullscreen_dialog(width, height);
  float s = greeter_ui_scale_value(ctx, 1.0f);
  float card_w = fullscreen_dialog ? greeter_ui_clampf((float)width * 0.22f,
                                                       340.0f * s, 390.0f * s)
                                   : logical_size * 0.86f;
  float card_h = fullscreen_dialog ? greeter_ui_clampf((float)height * 0.39f,
                                                       390.0f * s, 430.0f * s)
                                   : logical_size * 0.86f;
  if (fullscreen_dialog) {
    card_w = fminf(card_w, (float)width - 48.0f * s);
    card_h = fminf(card_h, (float)height - 48.0f * s);
  }
  float card_pad = fullscreen_dialog ? greeter_ui_clampf(card_w * 0.065f,
                                                         22.0f * s, 28.0f * s)
                                     : logical_size * 0.06f;
  float gap = fullscreen_dialog
                  ? greeter_ui_clampf(card_h * 0.016f, 7.0f * s, 10.0f * s)
                  : logical_size * 0.022f;
  float avatar_sz = fullscreen_dialog ? greeter_ui_clampf(card_w * 0.18f,
                                                          62.0f * s, 72.0f * s)
                                      : logical_size * 0.13f;
  float text_w = card_w - card_pad * 2.0f;
  float field_h = fullscreen_dialog ? 44.0f * s : logical_size * 0.07f;
  float title_font = fullscreen_dialog ? 20.0f : logical_size * 0.054f;
  float title_h = fullscreen_dialog ? 30.0f * s : logical_size * 0.07f;
  float username_font = fullscreen_dialog ? 20.0f : logical_size * 0.046f;
  float body_font = fullscreen_dialog ? 13.0f : logical_size * 0.024f;
  float hint_font = fullscreen_dialog ? 11.0f : logical_size * 0.021f;
  float small_font = fullscreen_dialog ? 10.0f : logical_size * 0.018f;
  float username_h = fullscreen_dialog ? 28.0f * s : logical_size * 0.05f;
  float subtitle_h = fullscreen_dialog ? 0.0f : logical_size * 0.038f;
  float hint_h = fullscreen_dialog ? 18.0f * s : logical_size * 0.034f;
  float line_h = fullscreen_dialog ? 16.0f * s : logical_size * 0.03f;
  plexy_canvas_set_flex_direction(ctx->canvas, root, 0);
  plexy_canvas_set_align(ctx->canvas, root, 2);
  plexy_canvas_set_justify(ctx->canvas, root, 2);
  plexy_canvas_set_fill_color(ctx->canvas, root, 0, 0, 0, 0);

  ctx->card_id =
      plexy_canvas_create_widget(ctx->canvas, PLEXY_WIDGET_PANEL, root);
  plexy_canvas_set_size(ctx->canvas, ctx->card_id, card_w, card_h);
  plexy_canvas_set_corner_radius(ctx->canvas, ctx->card_id,
                                 fullscreen_dialog ? 8.0f * s
                                                   : logical_size * 0.03f);
  plexy_canvas_set_fill_color(ctx->canvas, ctx->card_id, ctx->theme.card_bg[0],
                              ctx->theme.card_bg[1], ctx->theme.card_bg[2],
                              ctx->theme.card_bg[3]);
  plexy_canvas_set_glass_material(ctx->canvas, ctx->card_id,
                                  fullscreen_dialog ? 0.48f : 0.0f);
  plexy_canvas_set_elevation(ctx->canvas, ctx->card_id,
                             fullscreen_dialog ? 3.0f : 0.0f);
  plexy_canvas_set_border(ctx->canvas, ctx->card_id, ctx->theme.card_border[0],
                          ctx->theme.card_border[1], ctx->theme.card_border[2],
                          ctx->theme.card_border[3],
                          fullscreen_dialog ? 0.8f * s : 1.0f);
  plexy_canvas_set_flex_direction(ctx->canvas, ctx->card_id, 0);
  plexy_canvas_set_align(ctx->canvas, ctx->card_id, 2);
  plexy_canvas_set_padding(ctx->canvas, ctx->card_id, card_pad,
                           card_pad * 1.05f, card_pad, card_pad * 1.05f);
  plexy_canvas_set_gap(ctx->canvas, ctx->card_id, gap);

  ctx->title_label_id =
      plexy_canvas_create_widget(ctx->canvas, PLEXY_WIDGET_LABEL, ctx->card_id);
  plexy_canvas_set_font_size(ctx->canvas, ctx->title_label_id, title_font);
  plexy_canvas_set_text_color(
      ctx->canvas, ctx->title_label_id, ctx->theme.text_primary[0],
      ctx->theme.text_primary[1], ctx->theme.text_primary[2],
      ctx->theme.text_primary[3]);
  plexy_canvas_set_text(ctx->canvas, ctx->title_label_id, "Sign in");
  plexy_canvas_set_size(ctx->canvas, ctx->title_label_id, text_w, title_h);
  plexy_canvas_set_justify(ctx->canvas, ctx->title_label_id, 2);
  plexy_canvas_set_margin(ctx->canvas, ctx->title_label_id, 0.0f, 0.0f,
                          logical_size * 0.015f, 0.0f);

  ctx->avatar_id =
      plexy_canvas_create_widget(ctx->canvas, PLEXY_WIDGET_PANEL, ctx->card_id);
  plexy_canvas_set_size(ctx->canvas, ctx->avatar_id, avatar_sz, avatar_sz);
  plexy_canvas_set_corner_radius(ctx->canvas, ctx->avatar_id,
                                 fullscreen_dialog ? 8.0f * s
                                                   : logical_size * 0.030f);
  plexy_canvas_set_fill_color(ctx->canvas, ctx->avatar_id, 1.00f, 1.00f, 1.00f,
                              0.14f);
  plexy_canvas_set_border(ctx->canvas, ctx->avatar_id, 1.00f, 1.00f, 1.00f,
                          0.34f, 0.8f);
  plexy_canvas_set_align(ctx->canvas, ctx->avatar_id, 2);
  plexy_canvas_set_justify(ctx->canvas, ctx->avatar_id, 2);

  ctx->avatar_glyph_id = plexy_canvas_create_widget(
      ctx->canvas, PLEXY_WIDGET_LABEL, ctx->avatar_id);
  plexy_canvas_set_size(ctx->canvas, ctx->avatar_glyph_id, avatar_sz,
                        avatar_sz);
  plexy_canvas_set_font_size(ctx->canvas, ctx->avatar_glyph_id,
                             fullscreen_dialog ? 20.0f : logical_size * 0.038f);
  plexy_canvas_set_text_color(ctx->canvas, ctx->avatar_glyph_id, 1.00f, 1.00f,
                              1.00f, 0.95f);
  plexy_canvas_set_justify(ctx->canvas, ctx->avatar_glyph_id, 2);

  ctx->username_label_id =
      plexy_canvas_create_widget(ctx->canvas, PLEXY_WIDGET_LABEL, ctx->card_id);
  plexy_canvas_set_font_size(ctx->canvas, ctx->username_label_id,
                             username_font);
  plexy_canvas_set_text_color(
      ctx->canvas, ctx->username_label_id, ctx->theme.text_primary[0],
      ctx->theme.text_primary[1], ctx->theme.text_primary[2],
      ctx->theme.text_primary[3]);
  plexy_canvas_set_text(ctx->canvas, ctx->username_label_id, "");
  plexy_canvas_set_size(ctx->canvas, ctx->username_label_id, text_w,
                        username_h);
  plexy_canvas_set_justify(ctx->canvas, ctx->username_label_id, 2);

  ctx->subtitle_label_id =
      plexy_canvas_create_widget(ctx->canvas, PLEXY_WIDGET_LABEL, ctx->card_id);
  plexy_canvas_set_font_size(ctx->canvas, ctx->subtitle_label_id, body_font);
  plexy_canvas_set_text_color(
      ctx->canvas, ctx->subtitle_label_id, ctx->theme.text_secondary[0],
      ctx->theme.text_secondary[1], ctx->theme.text_secondary[2], 0.64f);
  plexy_canvas_set_text(ctx->canvas, ctx->subtitle_label_id, "");
  plexy_canvas_set_size(ctx->canvas, ctx->subtitle_label_id, text_w,
                        subtitle_h);
  plexy_canvas_set_justify(ctx->canvas, ctx->subtitle_label_id, 2);
  if (fullscreen_dialog)
    plexy_canvas_set_visible(ctx->canvas, ctx->subtitle_label_id, 0);

  ctx->password_field_id =
      plexy_canvas_create_widget(ctx->canvas, PLEXY_WIDGET_PANEL, ctx->card_id);
  plexy_canvas_set_size(ctx->canvas, ctx->password_field_id, text_w, field_h);
  plexy_canvas_set_corner_radius(ctx->canvas, ctx->password_field_id,
                                 fullscreen_dialog ? 8.0f * s
                                                   : logical_size * 0.015f);
  plexy_canvas_set_fill_color(ctx->canvas, ctx->password_field_id,
                              ctx->theme.input_bg[0], ctx->theme.input_bg[1],
                              ctx->theme.input_bg[2], ctx->theme.input_bg[3]);
  plexy_canvas_set_border(ctx->canvas, ctx->password_field_id,
                          ctx->theme.input_focus[0], ctx->theme.input_focus[1],
                          ctx->theme.input_focus[2], ctx->theme.input_focus[3],
                          1.0f);
  plexy_canvas_set_flex_direction(ctx->canvas, ctx->password_field_id, 1);
  plexy_canvas_set_align(ctx->canvas, ctx->password_field_id, 0);
  plexy_canvas_set_justify(ctx->canvas, ctx->password_field_id, 0);
  plexy_canvas_set_padding(ctx->canvas, ctx->password_field_id, 14.0f * s, 0.0f,
                           14.0f * s, 0.0f);

  ctx->password_text_id = plexy_canvas_create_widget(
      ctx->canvas, PLEXY_WIDGET_LABEL, ctx->password_field_id);
  plexy_canvas_set_font_size(ctx->canvas, ctx->password_text_id, body_font);
  plexy_canvas_set_text_color(
      ctx->canvas, ctx->password_text_id, ctx->theme.text_secondary[0],
      ctx->theme.text_secondary[1], ctx->theme.text_secondary[2], 0.5f);
  plexy_canvas_set_text(ctx->canvas, ctx->password_text_id, "Password");
  plexy_canvas_set_size(ctx->canvas, ctx->password_text_id, text_w - 28.0f * s,
                        field_h);
  plexy_canvas_set_margin(ctx->canvas, ctx->password_text_id, 0.0f, 6.0f * s,
                          0.0f, 6.0f * s);
  plexy_canvas_set_align(ctx->canvas, ctx->password_text_id, 2);
  plexy_canvas_set_justify(ctx->canvas, ctx->password_text_id, 2);

  ctx->error_label_id =
      plexy_canvas_create_widget(ctx->canvas, PLEXY_WIDGET_LABEL, ctx->card_id);
  plexy_canvas_set_font_size(ctx->canvas, ctx->error_label_id,
                             fullscreen_dialog ? 12.0f : logical_size * 0.02f);
  plexy_canvas_set_text_color(
      ctx->canvas, ctx->error_label_id, ctx->theme.error_text[0],
      ctx->theme.error_text[1], ctx->theme.error_text[2],
      ctx->theme.error_text[3]);
  plexy_canvas_set_text(ctx->canvas, ctx->error_label_id, "");
  plexy_canvas_set_size(ctx->canvas, ctx->error_label_id, text_w, line_h);
  plexy_canvas_set_justify(ctx->canvas, ctx->error_label_id, 2);

  ctx->caps_lock_label_id =
      plexy_canvas_create_widget(ctx->canvas, PLEXY_WIDGET_LABEL, ctx->card_id);
  plexy_canvas_set_font_size(ctx->canvas, ctx->caps_lock_label_id,
                             fullscreen_dialog ? 12.0f : logical_size * 0.019f);
  plexy_canvas_set_text_color(ctx->canvas, ctx->caps_lock_label_id, 1.00f,
                              0.82f, 0.30f, 0.95f);
  plexy_canvas_set_text(ctx->canvas, ctx->caps_lock_label_id, "");
  plexy_canvas_set_size(ctx->canvas, ctx->caps_lock_label_id, text_w,
                        fullscreen_dialog ? 16.0f * s : logical_size * 0.028f);
  plexy_canvas_set_justify(ctx->canvas, ctx->caps_lock_label_id, 2);
  plexy_canvas_set_visible(ctx->canvas, ctx->caps_lock_label_id, 0);

  ctx->clock_label_id =
      plexy_canvas_create_widget(ctx->canvas, PLEXY_WIDGET_LABEL, ctx->card_id);
  plexy_canvas_set_font_size(ctx->canvas, ctx->clock_label_id, small_font);
  plexy_canvas_set_text_color(
      ctx->canvas, ctx->clock_label_id, ctx->theme.text_secondary[0],
      ctx->theme.text_secondary[1], ctx->theme.text_secondary[2], 0.50f);
  plexy_canvas_set_text(ctx->canvas, ctx->clock_label_id, "");
  plexy_canvas_set_size(ctx->canvas, ctx->clock_label_id, text_w,
                        fullscreen_dialog ? 14.0f * s : logical_size * 0.025f);
  plexy_canvas_set_justify(ctx->canvas, ctx->clock_label_id, 2);

  ctx->action_label_id =
      plexy_canvas_create_widget(ctx->canvas, PLEXY_WIDGET_LABEL, ctx->card_id);
  plexy_canvas_set_text(ctx->canvas, ctx->action_label_id,
                        "Enter to log in, Esc to go back");
  plexy_canvas_set_font_size(ctx->canvas, ctx->action_label_id, hint_font);
  plexy_canvas_set_text_color(ctx->canvas, ctx->action_label_id,
                              ctx->theme.accent[0], ctx->theme.accent[1],
                              ctx->theme.accent[2], 0.54f);
  plexy_canvas_set_size(ctx->canvas, ctx->action_label_id, text_w, hint_h);
  plexy_canvas_set_margin(ctx->canvas, ctx->action_label_id,
                          fullscreen_dialog ? 8.0f * s : logical_size * 0.03f,
                          0.0f, 0.0f, 0.0f);
  plexy_canvas_set_justify(ctx->canvas, ctx->action_label_id, 2);

  syslog(LOG_INFO, "plexy-dm: password canvas built (card=%u, user=%u, pw=%u)",
         ctx->card_id, ctx->username_label_id, ctx->password_field_id);
  return ctx;
}

void greeter_ui_set_clock_24h(greeter_ui_ctx_t *ctx, bool use_24h) {
  if (ctx) {
    ctx->clock_24h = use_24h;
    ctx->last_clock_min = 0;
  }
}

void greeter_ui_set_caps_lock(greeter_ui_ctx_t *ctx, bool on) {
  if (ctx)
    ctx->caps_lock_on = on;
}

greeter_ui_power_result_t greeter_ui_get_power_result(greeter_ui_ctx_t *ctx) {
  if (!ctx)
    return POWER_RESULT_NONE;
  greeter_ui_power_result_t r = ctx->power_result;
  ctx->power_result = POWER_RESULT_NONE;
  return r;
}

void greeter_ui_set_authenticating(greeter_ui_ctx_t *ctx, bool active) {
  if (ctx)
    ctx->authenticating = active;
}

void greeter_ui_set_wifi_mode(greeter_ui_ctx_t *ctx, bool wifi_mode) {
  if (!ctx)
    return;
  ctx->wifi_mode = wifi_mode;

  if (ctx->title_label_id)
    plexy_canvas_set_text(ctx->canvas, ctx->title_label_id,
                          wifi_mode ? "Connect to Wi-Fi" : "Sign in");

  if (ctx->action_label_id)
    plexy_canvas_set_text(ctx->canvas, ctx->action_label_id,
                          wifi_mode
                              ? "Enter \xc2\xb7 Connect    Tab \xc2\xb7 Skip"
                              : "Enter \xc2\xb7 Select    F1 \xc2\xb7 Power");
}

void greeter_ui_set_wifi_networks(greeter_ui_ctx_t *ctx,
                                  const plexy_dm_wifi_ap_t *aps, int count) {
  if (!ctx)
    return;
  ctx->wifi_aps = aps;
  ctx->wifi_count = count;
  ctx->user_count = count;
  ctx->selected_user = 0;
  greeter_ui_reset_user_slide(ctx);
  greeter_ui_update_user_rows(ctx);
}

int greeter_ui_get_selected_wifi(const greeter_ui_ctx_t *ctx) {
  return ctx ? ctx->selected_user : -1;
}

bool greeter_ui_wifi_connect_requested(greeter_ui_ctx_t *ctx) {
  if (!ctx)
    return false;
  bool v = ctx->wifi_connect_requested;
  ctx->wifi_connect_requested = false;
  return v;
}

bool greeter_ui_wifi_skip_requested(greeter_ui_ctx_t *ctx) {
  if (!ctx)
    return false;
  bool v = ctx->wifi_skip_requested;
  ctx->wifi_skip_requested = false;
  return v;
}

int greeter_ui_user_click_requested(greeter_ui_ctx_t *ctx) {
  if (!ctx)
    return -1;
  int idx = ctx->user_click_index;
  ctx->user_click_index = -1;
  return idx;
}
