/*
 * Copyright (C) 2024-2026 Siraj Razick
 * SPDX-License-Identifier: AGPL-3.0-only
 */


#include "greeter.h"
#include "greeter_ui.h"
#include "user_list.h"
#include "wifi_list.h"

#include <plexy_canvas.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#include <stb/stb_image.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <GL/glew.h>
#include <X11/Xcursor/Xcursor.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <gbm.h>
#include <libinput.h>
#include <libudev.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <xkbcommon/xkbcommon.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vaapi.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>

#include "plexy_zerocopy_video.h"

#define GREETER_2D_DIALOG_TRANSITION_MS 240.0
#define GREETER_2D_DIALOG_TRANSITION_PX 18.0f

typedef float mat4[16];

static inline void mat4_identity(mat4 m) {
  for (int i = 0; i < 16; i++)
    m[i] = 0.0f;
  m[0] = m[5] = m[10] = m[15] = 1.0f;
}

static inline void mat4_mul(mat4 out, const mat4 a, const mat4 b) {
  mat4 tmp;
  for (int col = 0; col < 4; col++)
    for (int row = 0; row < 4; row++) {
      float s = 0.0f;
      for (int k = 0; k < 4; k++)
        s += a[k * 4 + row] * b[col * 4 + k];
      tmp[col * 4 + row] = s;
    }
  for (int i = 0; i < 16; i++)
    out[i] = tmp[i];
}

static inline void mat4_perspective(mat4 m, float fovy, float aspect,
                                    float near, float far) {
  mat4_identity(m);
  float f = 1.0f / tanf(fovy * 0.5f);
  m[0] = f / aspect;
  m[5] = f;
  m[10] = (far + near) / (near - far);
  m[11] = -1.0f;
  m[14] = (2.0f * far * near) / (near - far);
  m[15] = 0.0f;
}

static inline void mat4_rotate_y(mat4 m, float rad) {
  mat4_identity(m);
  float c = cosf(rad), s = sinf(rad);
  m[0] = c;
  m[2] = -s;
  m[8] = s;
  m[10] = c;
}

static inline void mat4_rotate_x(mat4 m, float rad) {
  mat4_identity(m);
  float c = cosf(rad), s = sinf(rad);
  m[5] = c;
  m[6] = s;
  m[9] = -s;
  m[10] = c;
}

static inline void mat4_translate(mat4 m, float x, float y, float z) {
  mat4_identity(m);
  m[12] = x;
  m[13] = y;
  m[14] = z;
}

static inline float greeter_clampf(float v, float lo, float hi) {
  if (v < lo)
    return lo;
  if (v > hi)
    return hi;
  return v;
}

static inline float greeter_smoothstep01(float t) {
  t = greeter_clampf(t, 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

static inline void mat4_scale_uniform(mat4 m, float s) {
  mat4_identity(m);
  m[0] = m[5] = m[10] = s;
}

static bool project_point_ndc(const mat4 mvp, float x, float y, float z,
                              float *out_x, float *out_y) {
  const float clip_x = mvp[0] * x + mvp[4] * y + mvp[8] * z + mvp[12];
  const float clip_y = mvp[1] * x + mvp[5] * y + mvp[9] * z + mvp[13];
  const float clip_w = mvp[3] * x + mvp[7] * y + mvp[11] * z + mvp[15];
  if (fabsf(clip_w) < 1.0e-5f)
    return false;

  if (out_x)
    *out_x = clip_x / clip_w;
  if (out_y)
    *out_y = clip_y / clip_w;
  return true;
}

typedef struct {
  int drm_fd;
  struct gbm_device *gbm_dev;
  struct gbm_surface *gbm_surface;
  EGLDisplay egl_display;
  EGLContext egl_context;
  EGLSurface egl_surface;
  EGLConfig egl_config;

  uint32_t connector_id;
  uint32_t crtc_id;
  drmModeModeInfo mode;
  uint32_t prev_fb;
  struct gbm_bo *prev_bo;
  bool flip_pending;

#define DRM_MAX_MIRRORS 8
  struct {
    uint32_t connector_id;
    uint32_t crtc_id;
    drmModeModeInfo mode;
  } mirrors[DRM_MAX_MIRRORS];
  int mirror_count;

  struct gbm_bo *hw_cursor_bo;
  uint32_t hw_cursor_width;
  uint32_t hw_cursor_height;
  int hw_cursor_hotspot_x;
  int hw_cursor_hotspot_y;
  bool hw_cursor_supported;
} drm_state_t;

typedef struct {
  struct libinput *li;
  struct xkb_context *xkb_ctx;
  struct xkb_keymap *xkb_keymap;
  struct xkb_state *xkb_state;
  double mouse_x;
  double mouse_y;
} input_state_t;

typedef enum {
  CUBE_IDLE = 0,
  CUBE_TRANSITION = 1,
  CUBE_SPINOUT = 2,
  CUBE_SHAKE = 3,
  CUBE_DONE = 4,
} cube_anim_t;

typedef enum {
  WEATHER_CLEAR = 0,
  WEATHER_CLOUDS = 1,
  WEATHER_RAIN = 2,
  WEATHER_SNOW = 3,
  WEATHER_STORM = 4,
  WEATHER_FOG = 5,
} weather_effect_t;

struct plexy_greeter {
  int vt;
  const plexy_dm_config_t *cfg;
  greeter_callbacks_t cbs;
  greeter_state_t state;
  bool suspended;
  bool use_3d_theme;

  drm_state_t drm;
  input_state_t input;

  greeter_ui_ctx_t *ui_userselect;
  greeter_ui_ctx_t *ui_password;
  greeter_ui_ctx_t *ui;
  greeter_ui_ctx_t *ui_transition_from;
  greeter_ui_ctx_t *ui_transition_to;
  double ui_transition_timer;
  int ui_transition_dir;

  int timer_fd;
  bool frame_pending;

  plexy_dm_user_t users[PLEXY_DM_MAX_USERS];
  int user_count;

  char lock_username[PLEXY_DM_MAX_USERNAME];

  GLuint wp_texture;
  GLuint wp_program;
  GLuint wp_vbo;
  GLuint wp_vao;
  int wp_width;
  int wp_height;

  bool video_active;
  pthread_t video_thread;
  bool video_thread_started;
  char video_path[512];
  bool video_mute;
  pthread_mutex_t video_mutex;
  GLuint video_texture;
  int video_tex_w;
  int video_tex_h;
  bool video_new_frame;
  uint8_t *video_frame_buf;
  int video_frame_w;
  int video_frame_h;
  bool video_frame_valid;

  GLuint video_pbos[2];
  int video_pbo_index;
  bool video_pbo_allocated;
  int video_pbo_w;
  int video_pbo_h;

  AVFrame *video_hw_frame;
  VADRMPRIMESurfaceDescriptor video_hw_desc;
  int video_hw_cs;
  bool video_hw_valid;
  volatile bool video_zerocopy_failed;
  EGLDisplay video_egl_display;
  plexy_zc_slot video_slot, video_prev_slot;
  GLuint video_tex_uv;
  bool video_yuv_mode;
  GLuint scene_fbo;
  GLuint scene_texture;
  int scene_width;
  int scene_height;

  pthread_t weather_thread;
  pthread_mutex_t weather_lock;
  bool weather_thread_started;
  bool weather_stop;
  weather_effect_t weather_effect;
  float weather_intensity;
  char weather_condition[128];

  GLuint ui_blit_program;
  GLuint ui_blit_vao;
  GLuint ui_blit_vbo;
  GLuint floor_fx_program;

  GLuint cube_program;
  GLuint cube_glow_program;
  GLuint cube_vao;
  GLuint cube_vbo;
  GLuint cube_ebo;

  GLuint msaa_fbo;
  GLuint msaa_color_rbo;
  GLuint msaa_depth_rbo;
  int msaa_samples;

  cube_anim_t cube_anim;
  float cube_angle_y;
  float cube_anim_start_y;
  double cube_anim_timer;
  float cube_scale;
  float cube_shake_x;
  int cube_active_face;
  float cube_fade_alpha;

  struct timespec last_frame_time;
  float scene_time_s;

  bool deferred_login;
  char deferred_username[PLEXY_DM_MAX_USERNAME];
  char deferred_password[PLEXY_DM_MAX_PASSWORD];

  plexy_dm_wifi_ap_t wifi_aps[PLEXY_DM_MAX_WIFI_APS];
  int wifi_count;
  char wifi_pending_ssid[PLEXY_DM_MAX_SSID];
  bool wifi_scanning;

  bool debug_term_requested;
};

static const char *weather_effect_name(weather_effect_t effect) {
  switch (effect) {
  case WEATHER_CLEAR:
    return "clear";
  case WEATHER_CLOUDS:
    return "clouds";
  case WEATHER_RAIN:
    return "rain";
  case WEATHER_SNOW:
    return "snow";
  case WEATHER_STORM:
    return "storm";
  case WEATHER_FOG:
    return "fog";
  }
  return "clear";
}

static bool weather_condition_contains(const char *haystack,
                                       const char *needle) {
  return haystack && needle && strstr(haystack, needle) != NULL;
}

static weather_effect_t map_weather_condition(const char *condition,
                                              float *out_intensity) {
  char lower[128];
  size_t len = condition ? strlen(condition) : 0;
  if (len >= sizeof(lower))
    len = sizeof(lower) - 1;
  for (size_t i = 0; i < len; ++i)
    lower[i] = (char)tolower((unsigned char)condition[i]);
  lower[len] = '\0';

  float intensity = 0.72f;
  if (weather_condition_contains(lower, "heavy") ||
      weather_condition_contains(lower, "thunder") ||
      weather_condition_contains(lower, "storm") ||
      weather_condition_contains(lower, "blizzard")) {
    intensity = 1.0f;
  } else if (weather_condition_contains(lower, "light") ||
             weather_condition_contains(lower, "patchy")) {
    intensity = 0.52f;
  }

  if (out_intensity)
    *out_intensity = intensity;

  if (weather_condition_contains(lower, "thunder") ||
      weather_condition_contains(lower, "storm")) {
    return WEATHER_STORM;
  }
  if (weather_condition_contains(lower, "rain") ||
      weather_condition_contains(lower, "drizzle") ||
      weather_condition_contains(lower, "shower")) {
    return WEATHER_RAIN;
  }
  if (weather_condition_contains(lower, "snow") ||
      weather_condition_contains(lower, "sleet") ||
      weather_condition_contains(lower, "blizzard") ||
      weather_condition_contains(lower, "ice pellet")) {
    return WEATHER_SNOW;
  }
  if (weather_condition_contains(lower, "fog") ||
      weather_condition_contains(lower, "mist") ||
      weather_condition_contains(lower, "haze")) {
    return WEATHER_FOG;
  }
  if (weather_condition_contains(lower, "cloud") ||
      weather_condition_contains(lower, "overcast")) {
    return WEATHER_CLOUDS;
  }
  if (weather_condition_contains(lower, "clear") ||
      weather_condition_contains(lower, "sunny")) {
    return WEATHER_CLEAR;
  }

  return WEATHER_CLOUDS;
}

static void weather_url_encode_location(const char *in, char *out,
                                        size_t out_sz) {
  static const char hex[] = "0123456789ABCDEF";
  size_t pos = 0;

  if (!out || out_sz == 0)
    return;
  out[0] = '\0';
  if (!in)
    return;

  for (const unsigned char *p = (const unsigned char *)in;
       *p && pos + 1 < out_sz; ++p) {
    if (isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == ',') {
      out[pos++] = (char)*p;
    } else if (*p == ' ') {
      if (pos + 3 >= out_sz)
        break;
      out[pos++] = '%';
      out[pos++] = '2';
      out[pos++] = '0';
    } else {
      if (pos + 3 >= out_sz)
        break;
      out[pos++] = '%';
      out[pos++] = hex[*p >> 4];
      out[pos++] = hex[*p & 0x0f];
    }
  }
  out[pos] = '\0';
}

static bool weather_fetch_condition(const plexy_dm_config_t *cfg,
                                    char *condition, size_t condition_sz) {
  char loc[768];
  char url[1024];
  char cmd[1400];

  if (!condition || condition_sz == 0)
    return false;
  condition[0] = '\0';

  weather_url_encode_location(cfg ? cfg->weather_location : "", loc,
                              sizeof(loc));
  if (loc[0]) {
    snprintf(url, sizeof(url), "https://wttr.in/%s?format=%%C", loc);
  } else {
    snprintf(url, sizeof(url), "https://wttr.in/?format=%%C");
  }

  snprintf(cmd, sizeof(cmd),
           "if command -v curl >/dev/null 2>&1; then "
           "timeout 6 curl -fsSL --max-time 4 '%s' 2>/dev/null; "
           "elif command -v wget >/dev/null 2>&1; then "
           "timeout 6 wget -q -T 4 -O - '%s' 2>/dev/null; "
           "fi",
           url, url);

  FILE *pipe = popen(cmd, "r");
  if (!pipe)
    return false;

  bool ok = false;
  if (fgets(condition, condition_sz, pipe)) {
    condition[strcspn(condition, "\r\n")] = '\0';
    ok = condition[0] != '\0';
  }

  int status = pclose(pipe);
  return ok && status == 0;
}

static bool weather_should_stop(plexy_greeter_t *g) {
  bool stop;
  pthread_mutex_lock(&g->weather_lock);
  stop = g->weather_stop;
  pthread_mutex_unlock(&g->weather_lock);
  return stop;
}

static void *weather_thread_main(void *data) {
  plexy_greeter_t *g = data;
  int refresh_minutes = g->cfg ? g->cfg->weather_refresh_minutes : 30;
  if (refresh_minutes < 5)
    refresh_minutes = 5;

  while (!weather_should_stop(g)) {
    char condition[128];
    if (weather_fetch_condition(g->cfg, condition, sizeof(condition))) {
      float intensity = 0.72f;
      weather_effect_t effect = map_weather_condition(condition, &intensity);

      pthread_mutex_lock(&g->weather_lock);
      g->weather_effect = effect;
      g->weather_intensity = intensity;
      snprintf(g->weather_condition, sizeof(g->weather_condition), "%s",
               condition);
      pthread_mutex_unlock(&g->weather_lock);

      syslog(LOG_INFO, "plexy-dm: weather '%s' -> %s effect", condition,
             weather_effect_name(effect));
    } else {
      syslog(LOG_WARNING,
             "plexy-dm: weather lookup failed; keeping fallback effect");
    }

    for (int i = 0; i < refresh_minutes * 60; ++i) {
      if (weather_should_stop(g))
        break;
      sleep(1);
    }
  }

  return NULL;
}

static void init_weather(plexy_greeter_t *g) {
  pthread_mutex_init(&g->weather_lock, NULL);
  g->weather_effect = WEATHER_CLOUDS;
  g->weather_intensity = 0.65f;
  snprintf(g->weather_condition, sizeof(g->weather_condition), "fallback");

  if (!g->cfg || !g->cfg->weather_enabled)
    return;

  int rc = pthread_create(&g->weather_thread, NULL, weather_thread_main, g);
  if (rc == 0) {
    g->weather_thread_started = true;
  } else {
    syslog(LOG_WARNING, "plexy-dm: failed to start weather worker: %s",
           strerror(rc));
  }
}

static void shutdown_weather(plexy_greeter_t *g) {
  if (!g)
    return;

  pthread_mutex_lock(&g->weather_lock);
  g->weather_stop = true;
  pthread_mutex_unlock(&g->weather_lock);

  if (g->weather_thread_started)
    pthread_join(g->weather_thread, NULL);

  pthread_mutex_destroy(&g->weather_lock);
}

static int find_drm_device(void) {
  const char *dir = "/dev/dri";
  DIR *d = opendir(dir);
  if (!d)
    return -1;

  struct dirent *ent;
  while ((ent = readdir(d)) != NULL) {
    if (strncmp(ent->d_name, "card", 4) != 0)
      continue;

    char path[256];
    snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0)
      continue;

    drmModeRes *res = drmModeGetResources(fd);
    if (!res) {
      close(fd);
      continue;
    }

    bool has_connected = false;
    for (int i = 0; i < res->count_connectors; i++) {
      drmModeConnector *conn = drmModeGetConnector(fd, res->connectors[i]);
      if (conn) {
        if (conn->connection == DRM_MODE_CONNECTED && conn->count_modes > 0)
          has_connected = true;
        drmModeFreeConnector(conn);
      }
      if (has_connected)
        break;
    }
    drmModeFreeResources(res);

    if (has_connected) {
      closedir(d);
      syslog(LOG_INFO, "plexy-dm: using DRM device %s", path);
      return fd;
    }
    close(fd);
  }

  closedir(d);
  return -1;
}

static uint32_t find_crtc_for_connector(int drm_fd, drmModeRes *res,
                                        drmModeConnector *conn,
                                        const uint32_t *used_crtcs,
                                        int used_count) {

  if (conn->encoder_id) {
    drmModeEncoder *enc = drmModeGetEncoder(drm_fd, conn->encoder_id);
    if (enc) {
      uint32_t cid = enc->crtc_id;
      drmModeFreeEncoder(enc);
      if (cid) {
        bool taken = false;
        for (int i = 0; i < used_count; i++)
          if (used_crtcs[i] == cid) {
            taken = true;
            break;
          }
        if (!taken)
          return cid;
      }
    }
  }

  for (int e = 0; e < conn->count_encoders; e++) {
    drmModeEncoder *enc = drmModeGetEncoder(drm_fd, conn->encoders[e]);
    if (!enc)
      continue;
    for (int c = 0; c < res->count_crtcs; c++) {
      if (!(enc->possible_crtcs & (1u << c)))
        continue;
      uint32_t cid = res->crtcs[c];
      bool taken = false;
      for (int i = 0; i < used_count; i++)
        if (used_crtcs[i] == cid) {
          taken = true;
          break;
        }
      if (!taken) {
        drmModeFreeEncoder(enc);
        return cid;
      }
    }
    drmModeFreeEncoder(enc);
  }
  return 0;
}

static bool init_drm(drm_state_t *drm) {
  drm->drm_fd = find_drm_device();
  if (drm->drm_fd < 0) {
    syslog(LOG_ERR, "plexy-dm: no DRM device found");
    return false;
  }

  drmModeRes *res = drmModeGetResources(drm->drm_fd);
  if (!res) {
    syslog(LOG_ERR, "plexy-dm: drmModeGetResources failed");
    return false;
  }

  uint32_t conn_ids[DRM_MAX_MIRRORS + 1];
  drmModeModeInfo conn_modes[DRM_MAX_MIRRORS + 1];
  int conn_count = 0;

  for (int i = 0; i < res->count_connectors && conn_count < DRM_MAX_MIRRORS + 1;
       i++) {
    drmModeConnector *conn =
        drmModeGetConnector(drm->drm_fd, res->connectors[i]);
    if (!conn)
      continue;
    if (conn->connection == DRM_MODE_CONNECTED && conn->count_modes > 0) {
      conn_ids[conn_count] = conn->connector_id;
      conn_modes[conn_count] = conn->modes[0];
      conn_count++;
    }
    drmModeFreeConnector(conn);
  }

  if (conn_count == 0) {
    syslog(LOG_ERR, "plexy-dm: no connected display found");
    drmModeFreeResources(res);
    return false;
  }

  uint32_t used_crtcs[DRM_MAX_MIRRORS + 1];
  int used_count = 0;
  drm->mirror_count = 0;

  for (int i = 0; i < conn_count; i++) {
    drmModeConnector *conn = drmModeGetConnector(drm->drm_fd, conn_ids[i]);
    if (!conn)
      continue;
    uint32_t crtc =
        find_crtc_for_connector(drm->drm_fd, res, conn, used_crtcs, used_count);
    drmModeFreeConnector(conn);
    if (!crtc)
      continue;

    if (i == 0) {
      drm->connector_id = conn_ids[i];
      drm->crtc_id = crtc;
      drm->mode = conn_modes[i];
    } else {
      int m = drm->mirror_count;
      drm->mirrors[m].connector_id = conn_ids[i];
      drm->mirrors[m].crtc_id = crtc;
      drm->mirrors[m].mode = conn_modes[i];
      drm->mirror_count++;
    }
    used_crtcs[used_count++] = crtc;
  }

  drmModeFreeResources(res);

  syslog(LOG_INFO, "plexy-dm: primary display %dx%d @ %dHz, %d mirror(s)",
         drm->mode.hdisplay, drm->mode.vdisplay, drm->mode.vrefresh,
         drm->mirror_count);
  return true;
}

static float detect_drm_ui_scale(const drm_state_t *drm) {
  if (!drm || drm->drm_fd < 0 || drm->connector_id == 0)
    return 1.0f;

  drmModeConnector *conn = drmModeGetConnector(drm->drm_fd, drm->connector_id);
  if (!conn)
    return 1.0f;

  float scale = 1.0f;
  float dpi = 96.0f;

  if (conn->mmWidth > 0 && conn->mmHeight > 0) {
    const float width = (float)drm->mode.hdisplay;
    const float height = (float)drm->mode.vdisplay;
    const float diag_px = sqrtf(width * width + height * height);
    const float diag_mm = sqrtf((float)conn->mmWidth * (float)conn->mmWidth +
                                (float)conn->mmHeight * (float)conn->mmHeight);
    const float diag_in = diag_mm / 25.4f;

    if (diag_in > 0.0f) {
      dpi = diag_px / diag_in;
      if (dpi >= 200.0f)
        scale = 2.0f;
      else if (dpi >= 140.0f)
        scale = 1.5f;
    }
  } else {
    syslog(
        LOG_WARNING,
        "plexy-dm: primary display has no physical size, using 1.0x UI scale");
  }

  syslog(LOG_INFO, "plexy-dm: primary display DPI %.1f, UI scale %.1fx", dpi,
         scale);

  drmModeFreeConnector(conn);
  return scale;
}

static void refresh_drm_outputs(drm_state_t *drm) {
  drmModeRes *res = drmModeGetResources(drm->drm_fd);
  if (!res)
    return;

  uint32_t conn_ids[DRM_MAX_MIRRORS + 1];
  int conn_count = 0;

  for (int i = 0; i < res->count_connectors && conn_count <= DRM_MAX_MIRRORS;
       i++) {
    drmModeConnector *conn =
        drmModeGetConnector(drm->drm_fd, res->connectors[i]);
    if (!conn)
      continue;
    if (conn->connection == DRM_MODE_CONNECTED && conn->count_modes > 0)
      conn_ids[conn_count++] = conn->connector_id;
    drmModeFreeConnector(conn);
  }

  if (conn_count == 0) {
    drmModeFreeResources(res);
    return;
  }

  struct {
    uint32_t conn_id;
    uint32_t crtc_id;
    bool active;
  } cands[DRM_MAX_MIRRORS + 1];
  int cand_count = 0;
  uint32_t used[DRM_MAX_MIRRORS + 1];
  int used_n = 0;

  for (int i = 0; i < conn_count; i++) {
    drmModeConnector *conn = drmModeGetConnector(drm->drm_fd, conn_ids[i]);
    if (!conn)
      continue;
    uint32_t crtc =
        find_crtc_for_connector(drm->drm_fd, res, conn, used, used_n);
    drmModeFreeConnector(conn);
    if (!crtc)
      continue;

    drmModeCrtc *ci = drmModeGetCrtc(drm->drm_fd, crtc);
    bool active = ci && ci->buffer_id != 0;
    if (ci)
      drmModeFreeCrtc(ci);

    cands[cand_count].conn_id = conn_ids[i];
    cands[cand_count].crtc_id = crtc;
    cands[cand_count].active = active;
    cand_count++;
    used[used_n++] = crtc;
  }

  drmModeFreeResources(res);

  int active_n = 0;
  for (int i = 0; i < cand_count; i++)
    if (cands[i].active)
      active_n++;

  bool use_all = (active_n == 0);

  bool primary_set = false;
  drm->mirror_count = 0;

  for (int i = 0; i < cand_count; i++) {
    if (!use_all && !cands[i].active)
      continue;

    if (!primary_set) {
      drm->connector_id = cands[i].conn_id;
      drm->crtc_id = cands[i].crtc_id;

      primary_set = true;
    } else if (drm->mirror_count < DRM_MAX_MIRRORS) {
      int m = drm->mirror_count;
      drm->mirrors[m].connector_id = cands[i].conn_id;
      drm->mirrors[m].crtc_id = cands[i].crtc_id;
      drm->mirror_count++;
    }
  }

  syslog(LOG_INFO,
         "plexy-dm: output refresh: %d active, primary crtc %u, %d mirror(s)%s",
         active_n, drm->crtc_id, drm->mirror_count,
         use_all ? " (fallback: all)" : "");
}

static bool init_gbm_egl(drm_state_t *drm) {
  drm->gbm_dev = gbm_create_device(drm->drm_fd);
  if (!drm->gbm_dev) {
    syslog(LOG_ERR, "plexy-dm: gbm_create_device failed");
    return false;
  }

  drm->gbm_surface = gbm_surface_create(
      drm->gbm_dev, drm->mode.hdisplay, drm->mode.vdisplay, GBM_FORMAT_ARGB8888,
      GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
  if (!drm->gbm_surface) {
    syslog(LOG_ERR, "plexy-dm: gbm_surface_create failed");
    return false;
  }

  PFNEGLGETPLATFORMDISPLAYEXTPROC eglGetPlatformDisplayEXT =
      (void *)eglGetProcAddress("eglGetPlatformDisplayEXT");

  if (eglGetPlatformDisplayEXT)
    drm->egl_display =
        eglGetPlatformDisplayEXT(EGL_PLATFORM_GBM_KHR, drm->gbm_dev, NULL);
  else
    drm->egl_display = eglGetDisplay((EGLNativeDisplayType)drm->gbm_dev);

  if (drm->egl_display == EGL_NO_DISPLAY) {
    syslog(LOG_ERR, "plexy-dm: eglGetDisplay failed");
    return false;
  }

  EGLint major, minor;
  if (!eglInitialize(drm->egl_display, &major, &minor)) {
    syslog(LOG_ERR, "plexy-dm: eglInitialize failed");
    return false;
  }

  syslog(LOG_INFO, "plexy-dm: EGL %d.%d initialized", major, minor);

  eglBindAPI(EGL_OPENGL_API);

  static const EGLint config_attribs[] = {
      EGL_SURFACE_TYPE,
      EGL_WINDOW_BIT,
      EGL_RED_SIZE,
      8,
      EGL_GREEN_SIZE,
      8,
      EGL_BLUE_SIZE,
      8,
      EGL_ALPHA_SIZE,
      8,
      EGL_DEPTH_SIZE,
      24,
      EGL_RENDERABLE_TYPE,
      EGL_OPENGL_BIT,
      EGL_NONE,
  };

  EGLint num_configs = 0;
  eglChooseConfig(drm->egl_display, config_attribs, NULL, 0, &num_configs);
  if (num_configs == 0) {
    syslog(LOG_ERR, "plexy-dm: eglChooseConfig found 0 configs");
    return false;
  }

  EGLConfig *configs = calloc(num_configs, sizeof(EGLConfig));
  eglChooseConfig(drm->egl_display, config_attribs, configs, num_configs,
                  &num_configs);

  drm->egl_config = NULL;
  for (int i = 0; i < num_configs; i++) {
    EGLint visual_id;
    eglGetConfigAttrib(drm->egl_display, configs[i], EGL_NATIVE_VISUAL_ID,
                       &visual_id);
    if (visual_id == (EGLint)GBM_FORMAT_ARGB8888) {
      drm->egl_config = configs[i];
      syslog(LOG_INFO, "plexy-dm: EGL config %d matches ARGB8888", i);
      break;
    }
  }
  if (!drm->egl_config) {
    drm->egl_config = configs[0];
    syslog(LOG_WARNING, "plexy-dm: no ARGB8888 config, using first of %d",
           num_configs);
  }
  free(configs);

  static const EGLint context_attribs[] = {
      EGL_CONTEXT_MAJOR_VERSION,
      3,
      EGL_CONTEXT_MINOR_VERSION,
      3,
      EGL_CONTEXT_OPENGL_PROFILE_MASK,
      EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
      EGL_NONE,
  };

  drm->egl_context = eglCreateContext(drm->egl_display, drm->egl_config,
                                      EGL_NO_CONTEXT, context_attribs);
  if (drm->egl_context == EGL_NO_CONTEXT) {
    syslog(LOG_ERR, "plexy-dm: eglCreateContext failed");
    return false;
  }

  PFNEGLCREATEPLATFORMWINDOWSURFACEEXTPROC eglCreatePlatformWindowSurfaceEXT =
      (void *)eglGetProcAddress("eglCreatePlatformWindowSurfaceEXT");

  if (eglCreatePlatformWindowSurfaceEXT) {
    drm->egl_surface = eglCreatePlatformWindowSurfaceEXT(
        drm->egl_display, drm->egl_config, drm->gbm_surface, NULL);
  }
  if (drm->egl_surface == EGL_NO_SURFACE) {
    drm->egl_surface = eglCreatePlatformWindowSurface(
        drm->egl_display, drm->egl_config, drm->gbm_surface, NULL);
  }
  if (drm->egl_surface == EGL_NO_SURFACE) {
    drm->egl_surface =
        eglCreateWindowSurface(drm->egl_display, drm->egl_config,
                               (EGLNativeWindowType)drm->gbm_surface, NULL);
  }
  if (drm->egl_surface == EGL_NO_SURFACE) {
    syslog(LOG_ERR, "plexy-dm: eglCreateWindowSurface failed (0x%x)",
           eglGetError());
    return false;
  }

  eglMakeCurrent(drm->egl_display, drm->egl_surface, drm->egl_surface,
                 drm->egl_context);

  return true;
}

static uint32_t get_fb_for_bo(int drm_fd, struct gbm_bo *bo) {
  uint32_t fb_id = (uint32_t)(uintptr_t)gbm_bo_get_user_data(bo);
  if (fb_id)
    return fb_id;

  uint32_t width = gbm_bo_get_width(bo);
  uint32_t height = gbm_bo_get_height(bo);
  uint32_t stride = gbm_bo_get_stride(bo);
  uint32_t handle = gbm_bo_get_handle(bo).u32;

  drmModeAddFB(drm_fd, width, height, 24, 32, stride, handle, &fb_id);
  gbm_bo_set_user_data(bo, (void *)(uintptr_t)fb_id, NULL);
  return fb_id;
}

static void page_flip_handler(int fd, unsigned int sequence,
                              unsigned int tv_sec, unsigned int tv_usec,
                              void *data) {
  (void)fd;
  (void)sequence;
  (void)tv_sec;
  (void)tv_usec;
  struct plexy_greeter *greeter = data;
  greeter->drm.flip_pending = false;
}

static int open_restricted(const char *path, int flags, void *user_data) {
  (void)user_data;
  return open(path, flags | O_CLOEXEC);
}

static void close_restricted(int fd, void *user_data) {
  (void)user_data;
  close(fd);
}

static bool init_input(input_state_t *input) {
  static const struct libinput_interface li_iface = {
      .open_restricted = open_restricted,
      .close_restricted = close_restricted,
  };

  struct udev *udev = udev_new();
  if (!udev) {
    syslog(LOG_ERR, "plexy-dm: udev_new failed");
    return false;
  }

  input->li = libinput_udev_create_context(&li_iface, NULL, udev);
  udev_unref(udev);

  if (!input->li) {
    syslog(LOG_ERR, "plexy-dm: libinput_udev_create_context failed");
    return false;
  }

  if (libinput_udev_assign_seat(input->li, "seat0") != 0) {
    syslog(LOG_ERR, "plexy-dm: libinput seat assignment failed");
    return false;
  }

  input->xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
  if (input->xkb_ctx) {
    struct xkb_rule_names names = {0};
    input->xkb_keymap = xkb_keymap_new_from_names(input->xkb_ctx, &names,
                                                  XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (input->xkb_keymap) {
      input->xkb_state = xkb_state_new(input->xkb_keymap);
    }
  }

  libinput_dispatch(input->li);
  syslog(LOG_INFO, "plexy-dm: input initialized");
  return true;
}

static void shutdown_input(input_state_t *input) {
  if (input->xkb_state)
    xkb_state_unref(input->xkb_state);
  if (input->xkb_keymap)
    xkb_keymap_unref(input->xkb_keymap);
  if (input->xkb_ctx)
    xkb_context_unref(input->xkb_ctx);
  if (input->li)
    libinput_unref(input->li);
}

static const char *wp_vert_src = "#version 330 core\n"
                                 "in vec2 a_pos;\n"
                                 "in vec2 a_uv;\n"
                                 "out vec2 v_uv;\n"
                                 "void main() {\n"
                                 "  gl_Position = vec4(a_pos, 0.0, 1.0);\n"
                                 "  v_uv = a_uv;\n"
                                 "}\n";

static const char *wp_frag_src =
    "#version 330 core\n"
    "in vec2 v_uv;\n"
    "out vec4 fragColor;\n"
    "uniform sampler2D u_wallpaper;\n"
    "uniform bool u_has_wallpaper;\n"
    "uniform vec2 u_wallpaper_size;\n"
    "uniform vec2 u_screen_size;\n"
    "uniform float u_blur_radius;\n"
    "uniform float u_darken;\n"
    "uniform float u_time;\n"
    "uniform float u_weather_kind;\n"
    "uniform float u_weather_intensity;\n"
    "uniform bool u_flip_y;\n"

    "uniform bool u_yuv_nv12;\n"
    "uniform sampler2D u_wallpaper_uv;\n"
    "uniform int u_yuv_cs;\n" PLEXY_NV12_GLSL "vec3 sample_wp(vec2 uv) {\n"
    "  if (u_yuv_nv12) return plexy_nv12_to_rgb(u_wallpaper, u_wallpaper_uv, "
    "uv, u_yuv_cs);\n"
    "  return texture(u_wallpaper, uv).rgb;\n"
    "}\n"
    "vec3 sample_wp_lod(vec2 uv, float lod) {\n"
    "  if (u_yuv_nv12) return plexy_nv12_to_rgb(u_wallpaper, u_wallpaper_uv, "
    "uv, u_yuv_cs);\n"
    "  return textureLod(u_wallpaper, uv, lod).rgb;\n"
    "}\n"
    "float soft_ellipse(vec2 p, vec2 center, vec2 radius) {\n"
    "  vec2 d = (p - center) / max(radius, vec2(0.001));\n"
    "  return exp(-dot(d, d));\n"
    "}\n"
    "vec2 cover_uv(vec2 uv) {\n"
    "  if (u_flip_y) { uv.y = 1.0 - uv.y; }\n"
    "  float screen_aspect = u_screen_size.x / max(u_screen_size.y, 1.0);\n"
    "  float tex_aspect = u_wallpaper_size.x / max(u_wallpaper_size.y, 1.0);\n"
    "  vec2 scale = vec2(1.0);\n"
    "  if (screen_aspect > tex_aspect) {\n"
    "    scale.y = tex_aspect / screen_aspect;\n"
    "  } else {\n"
    "    scale.x = screen_aspect / tex_aspect;\n"
    "  }\n"
    "  return (uv - 0.5) * scale + 0.5;\n"
    "}\n"
    "vec3 sample_blur(vec2 uv) {\n"
    "  vec2 texel = (u_blur_radius * 0.42) / max(u_wallpaper_size, "
    "vec2(1.0));\n"
    "  vec3 c = vec3(0.0);\n"
    "  float total = 0.0;\n"
    "  float w[7] = float[7](0.2161, 0.1907, 0.1311, 0.0702, 0.0293, 0.0095, "
    "0.0024);\n"
    "  for (int i = -6; i <= 6; i++) {\n"
    "    float wi = w[abs(i)];\n"
    "    vec2 ox = texel * vec2(float(i), 0.0);\n"
    "    c += sample_wp(clamp(uv + ox, vec2(0.001), vec2(0.999))) * wi;\n"
    "    total += wi;\n"
    "  }\n"
    "  vec3 h = c / max(total, 0.001);\n"
    "  c = vec3(0.0);\n"
    "  total = 0.0;\n"
    "  for (int j = -6; j <= 6; j++) {\n"
    "    float wj = w[abs(j)];\n"
    "    vec2 oy = texel * vec2(0.0, float(j));\n"
    "    c += mix(sample_wp(clamp(uv + oy, vec2(0.001), vec2(0.999))), h, "
    "0.55) * wj;\n"
    "    total += wj;\n"
    "  }\n"
    "  vec3 cross = c / max(total, 0.001);\n"
    "  vec3 diag = vec3(0.0);\n"
    "  diag += sample_wp(clamp(uv + texel * vec2(-4.0, -4.0), vec2(0.001), "
    "vec2(0.999)));\n"
    "  diag += sample_wp(clamp(uv + texel * vec2( 4.0, -4.0), vec2(0.001), "
    "vec2(0.999)));\n"
    "  diag += sample_wp(clamp(uv + texel * vec2(-4.0,  4.0), vec2(0.001), "
    "vec2(0.999)));\n"
    "  diag += sample_wp(clamp(uv + texel * vec2( 4.0,  4.0), vec2(0.001), "
    "vec2(0.999)));\n"
    "  diag *= 0.25;\n"
    "  return mix(cross, diag, 0.18);\n"
    "}\n"
    "float saw_curve(float b, float t) {\n"
    "  return smoothstep(0.0, b, t) * smoothstep(1.0, b, t);\n"
    "}\n"
    "float hash11(float p) {\n"
    "  return fract(sin(p * 12345.564) * 7658.76);\n"
    "}\n"
    "float snowflakes(vec2 uv, float t) {\n"
    "  float flakes = 0.0;\n"
    "  for (int i = 0; i < 3; i++) {\n"
    "    float layer = float(i);\n"
    "    float scale = mix(18.0, 46.0, layer / 2.0);\n"
    "    vec2 suv = uv * scale + vec2(sin(t * 0.25 + layer) * 1.8, t * (0.55 + "
    "layer * 0.18));\n"
    "    vec2 id = floor(suv);\n"
    "    vec2 cell = fract(suv) - 0.5;\n"
    "    float rnd = hash11(id.x * 73.2 + id.y * 271.9 + layer * 41.7);\n"
    "    vec2 p = vec2(rnd - 0.5, hash11(rnd * 91.3) - 0.5) * 0.74;\n"
    "    float d = length(cell - p);\n"
    "    flakes += smoothstep(0.060, 0.0, d) * smoothstep(0.58, 1.0, rnd) * "
    "(0.42 + layer * 0.28);\n"
    "  }\n"
    "  return clamp(flakes, 0.0, 1.0);\n"
    "}\n"
    "vec3 hash31(float p) {\n"
    "  vec3 p3 = fract(vec3(p) * vec3(0.1031, 0.11369, 0.13787));\n"
    "  p3 += dot(p3, p3.yzx + 19.19);\n"
    "  return fract(vec3((p3.x + p3.y) * p3.z, (p3.x + p3.z) * p3.y, (p3.y + "
    "p3.z) * p3.x));\n"
    "}\n"
    "float static_drops(vec2 uv, float t) {\n"
    "  uv *= 40.0;\n"
    "  vec2 id = floor(uv);\n"
    "  vec2 cell = fract(uv) - 0.5;\n"
    "  vec3 n = hash31(id.x * 107.45 + id.y * 3543.654);\n"
    "  vec2 p = (n.xy - 0.5) * 0.7;\n"
    "  float d = length(cell - p);\n"
    "  float fade = saw_curve(0.025, fract(t + n.z));\n"
    "  return smoothstep(0.30, 0.0, d) * fract(n.z * 10.0) * fade;\n"
    "}\n"
    "vec2 drop_layer(vec2 uv, float t) {\n"
    "  vec2 base_uv = uv;\n"
    "  uv.y += t * 0.75;\n"
    "  vec2 aspect = vec2(6.0, 1.0);\n"
    "  vec2 grid = aspect * 2.0;\n"
    "  vec2 id = floor(uv * grid);\n"
    "  uv.y += hash11(id.x);\n"
    "  id = floor(uv * grid);\n"
    "  vec3 n = hash31(id.x * 35.2 + id.y * 2376.1);\n"
    "  vec2 st = fract(uv * grid) - vec2(0.5, 0.0);\n"
    "  float x = n.x - 0.5;\n"
    "  float y_motion = base_uv.y * 20.0;\n"
    "  float wiggle = sin(y_motion + sin(y_motion));\n"
    "  x += wiggle * (0.5 - abs(x)) * (n.z - 0.5);\n"
    "  x *= 0.7;\n"
    "  float ti = fract(t + n.z);\n"
    "  float y = (saw_curve(0.85, ti) - 0.5) * 0.9 + 0.5;\n"
    "  vec2 p = vec2(x, y);\n"
    "  float d = length((st - p) * aspect.yx);\n"
    "  float main_drop = smoothstep(0.40, 0.0, d);\n"
    "  float r = sqrt(smoothstep(1.0, y, st.y));\n"
    "  float cd = abs(st.x - x);\n"
    "  float trail = smoothstep(0.23 * r, 0.15 * r * r, cd);\n"
    "  float trail_front = smoothstep(-0.02, 0.02, st.y - y);\n"
    "  trail *= trail_front * r * r;\n"
    "  float trail2 = smoothstep(0.20 * r, 0.0, cd);\n"
    "  float droplets = max(0.0, (sin(base_uv.y * (1.0 - base_uv.y) * 120.0) - "
    "st.y)) * trail2 * trail_front * n.z;\n"
    "  float yd = fract(base_uv.y * 10.0) + (st.y - 0.5);\n"
    "  float dd = length(st - vec2(x, yd));\n"
    "  droplets *= smoothstep(0.30, 0.0, dd);\n"
    "  float mask = main_drop + droplets * r * trail_front;\n"
    "  return vec2(mask, trail);\n"
    "}\n"
    "vec2 drops(vec2 uv, float t, float l0, float l1, float l2) {\n"
    "  float s = static_drops(uv, t) * l0;\n"
    "  vec2 m1 = drop_layer(uv, t) * l1;\n"
    "  vec2 m2 = drop_layer(uv * 1.85, t) * l2;\n"
    "  float c = smoothstep(0.30, 1.0, s + m1.x + m2.x);\n"
    "  return vec2(c, max(m1.y * l0, m2.y * l1));\n"
    "}\n"
    "vec3 sample_focus(vec2 uv, float focus) {\n"
    "  float lod = clamp(focus, 0.0, 6.0);\n"
    "  vec3 mip = sample_wp_lod(clamp(uv, vec2(0.001), vec2(0.999)), lod);\n"
    "  vec3 soft = sample_blur(uv);\n"
    "  return mix(mip, soft, smoothstep(4.5, 6.0, lod) * 0.35);\n"
    "}\n"
    "vec3 shade_water(vec3 base, float drop_mask, float trail_mask, vec2 grad, "
    "vec2 uv) {\n"
    "  vec3 nrm = normalize(vec3(-grad.x * 42.0, grad.y * 42.0, 1.0));\n"
    "  vec3 light_dir = normalize(vec3(-0.45, -0.62, 0.78));\n"
    "  vec3 view_dir = vec3(0.0, 0.0, 1.0);\n"
    "  float ndl = max(dot(nrm, light_dir), 0.0);\n"
    "  float fresnel = pow(1.0 - max(dot(nrm, view_dir), 0.0), 3.0);\n"
    "  float edge = smoothstep(0.10, 0.38, drop_mask) * (1.0 - "
    "smoothstep(0.62, 0.98, drop_mask));\n"
    "  float body = smoothstep(0.26, 0.92, drop_mask);\n"
    "  float spec = pow(max(dot(reflect(-light_dir, nrm), view_dir), 0.0), "
    "90.0) * body;\n"
    "  float glint = pow(max(dot(reflect(normalize(vec3(0.35, -0.25, 0.90)), "
    "nrm), view_dir), 0.0), 180.0) * body;\n"
    "  float lower_shadow = smoothstep(0.0, 0.8, body) * smoothstep(0.35, "
    "-0.15, grad.y);\n"
    "  float contact = edge * smoothstep(0.0, 0.8, -grad.y + 0.10);\n"
    "  vec3 water_tint = vec3(0.72, 0.86, 1.0);\n"
    "  base = mix(base, base * vec3(0.68, 0.76, 0.88), contact * 0.34 + "
    "trail_mask * 0.18);\n"
    "  base += water_tint * (ndl * 0.06 + fresnel * 0.18) * (body + edge * "
    "0.8);\n"
    "  base += vec3(1.0, 0.98, 0.92) * spec * 0.85;\n"
    "  base += vec3(0.72, 0.86, 1.0) * glint * 0.45;\n"
    "  base -= vec3(0.05, 0.07, 0.10) * lower_shadow * 0.28;\n"
    "  base += vec3(0.45, 0.65, 0.92) * edge * 0.10;\n"
    "  return base;\n"
    "}\n"
    "void main() {\n"
    "  if (u_has_wallpaper) {\n"
    "    vec2 p = v_uv * 2.0 - 1.0;\n"
    "    float weather = floor(u_weather_kind + 0.5);\n"
    "    float intensity = clamp(u_weather_intensity, 0.0, 1.0);\n"
    "    if (weather != 2.0 && weather != 4.0) {\n"
    "      vec2 uv = cover_uv(v_uv);\n"
    "      vec3 col = sample_wp_lod(clamp(uv, vec2(0.001), vec2(0.999)), "
    "0.0);\n"
    "      float vignette = smoothstep(1.30, 0.16, length(p * vec2(0.86, "
    "1.05)));\n"
    "      if (weather == 3.0) {\n"
    "        vec3 soft = sample_focus(uv, 2.8);\n"
    "        col = mix(col, soft * vec3(0.82, 0.90, 1.0), 0.28 + intensity * "
    "0.18);\n"
    "        float snow = snowflakes(v_uv, u_time * 0.55);\n"
    "        col = mix(col, vec3(0.92, 0.96, 1.0), snow * (0.45 + intensity * "
    "0.35));\n"
    "      } else if (weather == 5.0) {\n"
    "        vec3 soft = sample_focus(uv, 5.0);\n"
    "        col = mix(col, soft + vec3(0.10, 0.12, 0.13), 0.46 + intensity * "
    "0.18);\n"
    "        col = mix(col, vec3(dot(col, vec3(0.299, 0.587, 0.114))), 0.18);\n"
    "      } else if (weather == 1.0) {\n"
    "        vec3 soft = sample_focus(uv, 3.3);\n"
    "        col = mix(col, soft * vec3(0.82, 0.87, 0.94), 0.34 + intensity * "
    "0.12);\n"
    "      } else {\n"
    "        col = mix(col, col * vec3(1.04, 1.03, 1.00), 0.18);\n"
    "      }\n"
    "      col *= 0.72 + vignette * 0.22;\n"
    "      col *= (1.0 - u_darken * 0.65);\n"
    "      fragColor = vec4(col, 1.0);\n"
    "      return;\n"
    "    }\n"
    "    vec2 sim_st = vec2(v_uv.x, 1.0 - v_uv.y);\n"
    "    vec2 sim_uv = (sim_st * u_screen_size - 0.5 * u_screen_size) / "
    "max(u_screen_size.y, 1.0);\n"
    "    float t = u_time * 0.2;\n"
    "    float rain_amount = sin(u_time * 0.05) * 0.3 + 0.7;\n"
    "    float max_blur = mix(3.0, 6.0, rain_amount);\n"
    "    float min_blur = 2.0;\n"
    "    float static_layer = smoothstep(-0.5, 1.0, rain_amount) * 2.0;\n"
    "    float layer1 = smoothstep(0.25, 0.75, rain_amount);\n"
    "    float layer2 = smoothstep(0.0, 0.5, rain_amount);\n"
    "    vec2 c = drops(sim_uv, t, static_layer, layer1, layer2);\n"
    "    vec2 e = vec2(0.001, 0.0);\n"
    "    float cx = drops(sim_uv + e, t, static_layer, layer1, layer2).x;\n"
    "    float cy = drops(sim_uv + e.yx, t, static_layer, layer1, layer2).x;\n"
    "    vec2 grad = vec2(cx - c.x, cy - c.x);\n"
    "    vec2 n = grad * 0.16;\n"
    "    vec2 refract_n = vec2(n.x, -n.y);\n"
    "    float focus = mix(max_blur - c.y, min_blur, smoothstep(0.10, 0.20, "
    "c.x));\n"
    "    vec2 uv = cover_uv(v_uv + refract_n);\n"
    "    vec3 col = sample_focus(uv, focus);\n"
    "    float drop_mask = smoothstep(0.08, 0.82, c.x);\n"
    "    float trail_mask = smoothstep(0.05, 0.55, c.y);\n"
    "    vec3 sharp = sample_wp_lod(clamp(uv + refract_n * 2.0, vec2(0.001), "
    "vec2(0.999)), 0.0);\n"
    "    col = mix(col, sharp, drop_mask * 0.86 + trail_mask * 0.34);\n"
    "    col = mix(col, vec3(dot(col, vec3(0.299, 0.587, 0.114))), 0.05 + (1.0 "
    "- drop_mask) * 0.08);\n"
    "    col = shade_water(col, drop_mask, trail_mask, grad, uv);\n"
    "    col = mix(col, col * vec3(0.78, 0.84, 0.94), trail_mask * 0.22);\n"
    "    col = mix(col, col * vec3(0.82, 0.86, 0.92), 0.20);\n"
    "    float vignette = smoothstep(1.28, 0.18, length(p * vec2(0.86, "
    "1.05)));\n"
    "    col *= 0.66 + vignette * 0.24;\n"
    "    float lightning = sin(t * sin(t * 10.0));\n"
    "    lightning *= pow(max(0.0, sin(t + sin(t))), 10.0);\n"
    "    col *= 1.0 + lightning * (weather == 4.0 ? 0.24 : 0.10);\n"
    "    float rim = smoothstep(0.06, 0.22, c.x) * (1.0 - smoothstep(0.45, "
    "0.92, c.x));\n"
    "    col += vec3(0.95, 0.98, 1.0) * rim * 0.18;\n"
    "    col -= vec3(0.08, 0.10, 0.13) * trail_mask * 0.10;\n"
    "    col *= (1.0 - u_darken);\n"
    "    fragColor = vec4(col, 1.0);\n"
    "    return;\n"
    "  }\n"
    "  float drift = u_time * 0.015;\n"
    "  vec2 p = v_uv * 2.0 - 1.0\n"
    "         + vec2(sin(drift * 1.1) * 0.06, cos(drift * 0.7) * 0.04);\n"
    "  float radial = length(p * vec2(0.82, 1.06));\n"
    "  float vertical = clamp(1.0 - pow(abs(p.y * 0.68 + 0.02), 1.25), 0.0, "
    "1.0);\n"
    "  float depth_falloff = exp(-radial * 1.24);\n"
    "  vec3 col = mix(vec3(0.0015, 0.0025, 0.0050), vec3(0.040, 0.064, 0.118), "
    "vertical * 0.58 + depth_falloff * 0.10);\n"
    "  col = mix(col, vec3(0.002, 0.004, 0.009), smoothstep(0.05, 1.18, "
    "radial));\n"
    "  vec2 focus_center = vec2(0.0, -0.02);\n"
    "  float depth_haze = soft_ellipse(p, focus_center, vec2(1.24, 0.96));\n"
    "  float back_glow = soft_ellipse(p, vec2(0.0, -0.02), vec2(0.78, 0.60));\n"
    "  float core_glow = soft_ellipse(p, vec2(0.0, 0.00), vec2(0.30, 0.22));\n"
    "  float inner_halo = soft_ellipse(p, vec2(0.0, 0.03), vec2(0.46, 0.32));\n"
    "  float cool_fill = soft_ellipse(p, vec2(-0.36, 0.04), vec2(0.38, "
    "0.30));\n"
    "  float upper_volume = soft_ellipse(p, vec2(0.0, 0.42), vec2(0.92, "
    "0.30));\n"
    "  float side_volume_l = soft_ellipse(p, vec2(-0.84, 0.02), vec2(0.26, "
    "0.96));\n"
    "  float side_volume_r = soft_ellipse(p, vec2(0.84, 0.02), vec2(0.26, "
    "0.96));\n"
    "  float rear_volume = soft_ellipse(p, vec2(0.0, -0.10), vec2(1.14, "
    "0.88));\n"
    "  float aperture = soft_ellipse(p, vec2(0.0, -0.02), vec2(0.56, 0.44));\n"
    "  col += vec3(0.04, 0.07, 0.14) * back_glow * 0.72;\n"
    "  col += vec3(0.92, 0.97, 1.00) * core_glow * 0.30;\n"
    "  col += vec3(0.40, 0.60, 1.00) * inner_halo * 0.18;\n"
    "  col += vec3(0.46, 0.64, 1.00) * cool_fill * 0.24;\n"
    "  col += vec3(0.10, 0.14, 0.22) * upper_volume * 0.14;\n"
    "  col += vec3(0.04, 0.06, 0.11) * side_volume_l * 0.16;\n"
    "  col += vec3(0.04, 0.06, 0.11) * side_volume_r * 0.16;\n"
    "  col += vec3(0.05, 0.08, 0.14) * rear_volume * 0.20;\n"
    "  col = mix(col, col + vec3(0.030, 0.040, 0.060), depth_haze * 0.10 + "
    "aperture * 0.08);\n"
    "  float vignette = 1.0 - smoothstep(0.30, 1.22, dot(p * vec2(0.92, 1.08), "
    "p * vec2(0.92, 1.08)));\n"
    "  col *= (0.72 + vignette * 0.18) * (1.0 - u_darken * 0.10);\n"
    "  col = col / (1.0 + col * 0.55);\n"
    "  col = pow(col, vec3(0.93));\n"
    "  fragColor = vec4(col, 1.0);\n"
    "}\n";

static GLuint compile_shader(GLenum type, const char *src) {
  GLuint s = glCreateShader(type);
  glShaderSource(s, 1, &src, NULL);
  glCompileShader(s);
  GLint ok = 0;
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[256];
    glGetShaderInfoLog(s, sizeof(log), NULL, log);
    syslog(LOG_ERR, "plexy-dm: shader compile: %s", log);
    glDeleteShader(s);
    return 0;
  }
  return s;
}

static unsigned char *resize_rgba_bilinear(const unsigned char *src, int sw,
                                           int sh, int dw, int dh) {
  if (!src || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0)
    return NULL;

  unsigned char *dst = malloc((size_t)dw * (size_t)dh * 4u);
  if (!dst)
    return NULL;

  float sx = (float)sw / (float)dw;
  float sy = (float)sh / (float)dh;

  for (int y = 0; y < dh; ++y) {
    float fy = (y + 0.5f) * sy - 0.5f;
    int y0 = (int)fy;
    int y1 = y0 + 1;
    float ty = fy - (float)y0;
    y0 = y0 < 0 ? 0 : (y0 >= sh ? sh - 1 : y0);
    y1 = y1 < 0 ? 0 : (y1 >= sh ? sh - 1 : y1);
    for (int x = 0; x < dw; ++x) {
      float fx = (x + 0.5f) * sx - 0.5f;
      int x0 = (int)fx;
      int x1 = x0 + 1;
      float tx = fx - (float)x0;
      x0 = x0 < 0 ? 0 : (x0 >= sw ? sw - 1 : x0);
      x1 = x1 < 0 ? 0 : (x1 >= sw ? sw - 1 : x1);
      unsigned char *dp = dst + ((size_t)y * (size_t)dw + (size_t)x) * 4u;
      for (int c = 0; c < 4; ++c) {
        float v00 = src[((size_t)y0 * sw + x0) * 4 + c];
        float v10 = src[((size_t)y0 * sw + x1) * 4 + c];
        float v01 = src[((size_t)y1 * sw + x0) * 4 + c];
        float v11 = src[((size_t)y1 * sw + x1) * 4 + c];
        float v = v00 * (1.0f - tx) * (1.0f - ty) + v10 * tx * (1.0f - ty) +
                  v01 * (1.0f - tx) * ty + v11 * tx * ty;
        dp[c] = (unsigned char)(v + 0.5f);
      }
    }
  }

  return dst;
}

static GLuint load_wallpaper_texture(const char *path, int max_w, int max_h,
                                     int *out_w, int *out_h) {
  if (!path || !path[0])
    return 0;

  int sw = 0, sh = 0, ch = 0;
  stbi_set_flip_vertically_on_load(0);
  unsigned char *src = stbi_load(path, &sw, &sh, &ch, 4);
  if (!src)
    return 0;

  int dw = sw;
  int dh = sh;
  if (max_w > 0 && max_h > 0 && (sw > max_w || sh > max_h)) {
    float scale_x = (float)max_w / (float)sw;
    float scale_y = (float)max_h / (float)sh;
    float scale = scale_x < scale_y ? scale_x : scale_y;
    if (scale <= 0.0f)
      scale = 1.0f;
    dw = (int)fmaxf(1.0f, floorf((float)sw * scale));
    dh = (int)fmaxf(1.0f, floorf((float)sh * scale));
  }

  unsigned char *pixels = src;
  if (dw != sw || dh != sh) {
    pixels = resize_rgba_bilinear(src, sw, sh, dw, dh);
    stbi_image_free(src);
    src = NULL;
    if (!pixels)
      return 0;
  }

  GLuint tex = 0;
  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_2D, tex);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, dw, dh, 0, GL_RGBA, GL_UNSIGNED_BYTE,
               pixels);
  glGenerateMipmap(GL_TEXTURE_2D);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                  GL_LINEAR_MIPMAP_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glBindTexture(GL_TEXTURE_2D, 0);

  if (src)
    stbi_image_free(src);
  else
    free(pixels);

  if (out_w)
    *out_w = dw;
  if (out_h)
    *out_h = dh;
  return tex;
}

static void zc_greeter_log(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsyslog(LOG_INFO, fmt, ap);
  va_end(ap);
}

static void video_free_hw_frame(plexy_greeter_t *g) {
  if (g->video_hw_valid) {
    plexy_zc_close_desc_fds(&g->video_hw_desc);
    g->video_hw_valid = false;
  }
  if (g->video_hw_frame) {
    av_frame_free(&g->video_hw_frame);
  }
}

static void *video_decode_thread(void *arg) {
  plexy_greeter_t *g = (plexy_greeter_t *)arg;
  AVBufferRef *hw_device_ctx = NULL;
  bool use_hw = false;
  AVFormatContext *fmt = NULL;
  AVCodecContext *vctx = NULL;
  SwsContext *sws = NULL;
  AVPacket *pkt = NULL;
  AVFrame *frame = NULL, *rgba = NULL, *hw_frame = NULL;
  int video_stream = -1;
  int sws_w = 0, sws_h = 0;

  int ret = avformat_open_input(&fmt, g->video_path, NULL, NULL);
  if (ret < 0) {
    syslog(LOG_ERR, "plexy-dm: video: failed to open %s", g->video_path);
    return NULL;
  }
  ret = avformat_find_stream_info(fmt, NULL);
  if (ret < 0) {
    syslog(LOG_ERR, "plexy-dm: video: no stream info");
    avformat_close_input(&fmt);
    return NULL;
  }
  for (unsigned i = 0; i < fmt->nb_streams; i++) {
    if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
      video_stream = (int)i;
      break;
    }
  }
  if (video_stream < 0) {
    syslog(LOG_ERR, "plexy-dm: video: no video stream");
    avformat_close_input(&fmt);
    return NULL;
  }
  const AVCodec *dec =
      avcodec_find_decoder(fmt->streams[video_stream]->codecpar->codec_id);
  if (!dec) {
    syslog(LOG_ERR, "plexy-dm: video: decoder not found");
    avformat_close_input(&fmt);
    return NULL;
  }
  vctx = avcodec_alloc_context3(dec);
  avcodec_parameters_to_context(vctx, fmt->streams[video_stream]->codecpar);

  ret = av_hwdevice_ctx_create(&hw_device_ctx, AV_HWDEVICE_TYPE_VAAPI, NULL,
                               NULL, 0);
  if (ret >= 0 && hw_device_ctx) {
    vctx->hw_device_ctx = av_buffer_ref(hw_device_ctx);
    if (vctx->hw_device_ctx) {
      use_hw = true;
    } else {
      av_buffer_unref(&hw_device_ctx);
      hw_device_ctx = NULL;
    }
  }

  ret = avcodec_open2(vctx, dec, NULL);
  if (ret < 0) {

    if (hw_device_ctx) {
      av_buffer_unref(&hw_device_ctx);
      hw_device_ctx = NULL;
      use_hw = false;
      vctx->hw_device_ctx = NULL;
      ret = avcodec_open2(vctx, dec, NULL);
    }
    if (ret < 0) {
      syslog(LOG_ERR, "plexy-dm: video: failed to open decoder");
      avcodec_free_context(&vctx);
      avformat_close_input(&fmt);
      return NULL;
    }
    syslog(LOG_INFO, "plexy-dm: video: fell back to software decode");
  }

  if (use_hw)
    syslog(LOG_INFO, "plexy-dm: video: VAAPI hardware decode enabled");

  pkt = av_packet_alloc();
  frame = av_frame_alloc();
  rgba = av_frame_alloc();
  hw_frame = av_frame_alloc();

  syslog(LOG_INFO, "plexy-dm: video wallpaper started: %dx%d, hw=%s",
         vctx->width, vctx->height, use_hw ? "vaapi" : "software");

  while (g->video_active) {

    ret = avformat_seek_file(fmt, video_stream, 0, 0, 0, AVSEEK_FLAG_BACKWARD);
    if (ret < 0) {

      break;
    }
    avcodec_flush_buffers(vctx);

    bool eof = false;
    while (!eof && g->video_active) {
      ret = av_read_frame(fmt, pkt);
      if (ret < 0) {
        eof = true;
        break;
      }
      if (pkt->stream_index != video_stream) {
        av_packet_unref(pkt);
        continue;
      }
      ret = avcodec_send_packet(vctx, pkt);
      av_packet_unref(pkt);
      if (ret < 0 && ret != AVERROR(EAGAIN))
        continue;

      while (ret >= 0 || ret == AVERROR(EAGAIN)) {
        ret = avcodec_receive_frame(vctx, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
          break;
        if (ret < 0)
          break;

        bool pushed_hw = false;
        if (use_hw && frame->format == AV_PIX_FMT_VAAPI &&
            !g->video_zerocopy_failed) {
          VADRMPRIMESurfaceDescriptor desc;
          if (plexy_zc_export_surface(hw_device_ctx, frame, &desc)) {
            AVFrame *ref = av_frame_clone(frame);
            if (ref) {
              pthread_mutex_lock(&g->video_mutex);
              video_free_hw_frame(g);
              g->video_hw_frame = ref;
              g->video_hw_desc = desc;
              g->video_hw_cs = plexy_zc_colorspace(frame);
              g->video_hw_valid = true;
              g->video_new_frame = true;
              pthread_mutex_unlock(&g->video_mutex);
              pushed_hw = true;
            } else {
              plexy_zc_close_desc_fds(&desc);
            }
          }
          if (!pushed_hw)
            g->video_zerocopy_failed = true;
        }

        AVFrame *decode_frame = frame;
        if (!pushed_hw) {

          if (use_hw && frame->format == AV_PIX_FMT_VAAPI) {
            ret = av_hwframe_transfer_data(hw_frame, frame, 0);
            if (ret < 0) {
              syslog(LOG_ERR, "plexy-dm: video: hwframe transfer failed");
              break;
            }
            decode_frame = hw_frame;
          }

          int w = decode_frame->width;
          int h = decode_frame->height;
          if (!sws || sws_w != w || sws_h != h) {
            if (sws)
              sws_freeContext(sws);
            sws =
                sws_getContext(w, h, decode_frame->format, w, h,
                               AV_PIX_FMT_RGBA, SWS_BILINEAR, NULL, NULL, NULL);
            sws_w = w;
            sws_h = h;
            av_frame_unref(rgba);
            rgba->width = w;
            rgba->height = h;
            rgba->format = AV_PIX_FMT_RGBA;
            av_image_alloc(rgba->data, rgba->linesize, w, h, AV_PIX_FMT_RGBA,
                           1);
          }

          sws_scale(sws, (const uint8_t *const *)decode_frame->data,
                    decode_frame->linesize, 0, h, rgba->data, rgba->linesize);

          pthread_mutex_lock(&g->video_mutex);
          size_t buf_sz = (size_t)w * h * 4;
          uint8_t *buf = (uint8_t *)malloc(buf_sz);
          if (buf) {
            int stride = w * 4;
            for (int y = 0; y < h; y++) {
              memcpy(buf + (size_t)y * stride,
                     rgba->data[0] + (size_t)(h - 1 - y) * stride,
                     (size_t)stride);
            }
            if (g->video_frame_buf)
              free(g->video_frame_buf);
            g->video_frame_buf = buf;
            g->video_frame_w = w;
            g->video_frame_h = h;
            g->video_frame_valid = true;
            g->video_new_frame = true;
          }
          pthread_mutex_unlock(&g->video_mutex);
        }

        while (g->video_new_frame && g->video_active)
          usleep(8000);

        if (decode_frame == hw_frame)
          av_frame_unref(hw_frame);
        break;
      }
    }
    if (g->video_active && eof)
      usleep(50000);
  }

  if (sws)
    sws_freeContext(sws);
  if (rgba->data[0])
    av_freep(&rgba->data[0]);
  av_frame_free(&frame);
  av_frame_free(&rgba);
  av_packet_free(&pkt);
  avcodec_free_context(&vctx);
  avformat_close_input(&fmt);

  syslog(LOG_INFO, "plexy-dm: video decode thread exited");
  return NULL;
}

static bool is_video_file(const char *path) {
  if (!path || !path[0])
    return false;
  const char *dot = strrchr(path, '.');
  if (!dot)
    return false;
  const char *exts[] = {".mp4", ".mkv",  ".webm", ".avi", ".mov", ".m4v",
                        ".mpg", ".mpeg", ".ts",   ".flv", ".wmv", NULL};
  for (int i = 0; exts[i]; i++) {
    if (strcasecmp(dot, exts[i]) == 0)
      return true;
  }
  return false;
}

static void video_wallpaper_start(plexy_greeter_t *g, const char *path) {
  g->video_active = true;
  g->video_mute = true;
  snprintf(g->video_path, sizeof(g->video_path), "%s", path);
  g->video_texture = 0;
  g->video_tex_w = 0;
  g->video_tex_h = 0;
  g->video_new_frame = false;
  g->video_frame_buf = NULL;
  g->video_frame_valid = false;

  plexy_zc_set_log_fn(zc_greeter_log);
  g->video_hw_frame = NULL;
  memset(&g->video_hw_desc, 0, sizeof(g->video_hw_desc));
  g->video_hw_cs = 1;
  g->video_hw_valid = false;
  g->video_zerocopy_failed = false;
  g->video_egl_display = g->drm.egl_display;
  memset(&g->video_slot, 0, sizeof(g->video_slot));
  memset(&g->video_prev_slot, 0, sizeof(g->video_prev_slot));
  g->video_slot.img_y = EGL_NO_IMAGE;
  g->video_slot.img_uv = EGL_NO_IMAGE;
  g->video_prev_slot.img_y = EGL_NO_IMAGE;
  g->video_prev_slot.img_uv = EGL_NO_IMAGE;
  g->video_tex_uv = 0;
  g->video_yuv_mode = false;
  pthread_mutex_init(&g->video_mutex, NULL);
  g->video_thread_started = true;
  pthread_create(&g->video_thread, NULL, video_decode_thread, g);
  syslog(LOG_INFO, "plexy-dm: video wallpaper thread started for %s", path);
}

static void video_wallpaper_stop(plexy_greeter_t *g) {
  if (!g->video_thread_started)
    return;

  g->video_active = false;
  pthread_join(g->video_thread, NULL);
  g->video_thread_started = false;

  pthread_mutex_lock(&g->video_mutex);
  if (g->video_frame_buf) {
    free(g->video_frame_buf);
    g->video_frame_buf = NULL;
  }
  g->video_frame_valid = false;
  g->video_new_frame = false;
  video_free_hw_frame(g);
  pthread_mutex_unlock(&g->video_mutex);
  pthread_mutex_destroy(&g->video_mutex);

  if (g->video_slot.frame || g->video_slot.tex_y || g->video_prev_slot.frame ||
      g->video_prev_slot.tex_y) {
    plexy_zc_release_slot(g->video_egl_display, &g->video_slot);
    plexy_zc_release_slot(g->video_egl_display, &g->video_prev_slot);
    g->video_texture = 0;
    g->video_tex_uv = 0;
  } else if (g->video_texture) {
    glDeleteTextures(1, &g->video_texture);
    g->video_texture = 0;
  }
  g->video_tex_w = 0;
  g->video_tex_h = 0;
  g->video_yuv_mode = false;
}

static bool video_wallpaper_update(plexy_greeter_t *g) {
  if (!g->video_thread_started)
    return false;

  AVFrame *hw_frame = NULL;
  VADRMPRIMESurfaceDescriptor hw_desc;
  int hw_cs = 1;
  bool have_hw = false;

  pthread_mutex_lock(&g->video_mutex);
  if (g->video_hw_valid && g->video_hw_frame) {
    hw_frame = g->video_hw_frame;
    hw_desc = g->video_hw_desc;
    hw_cs = g->video_hw_cs;
    g->video_hw_frame = NULL;
    g->video_hw_desc.num_objects = 0;
    g->video_hw_valid = false;
    g->video_new_frame = false;
    have_hw = true;
  }
  pthread_mutex_unlock(&g->video_mutex);

  if (have_hw) {
    if (g->video_zerocopy_failed) {

      plexy_zc_close_desc_fds(&hw_desc);
      av_frame_free(&hw_frame);
      return false;
    }
    plexy_zc_slot slot;
    if (!plexy_zc_import(g->video_egl_display, hw_frame, &hw_desc, &slot)) {
      plexy_zc_close_desc_fds(&hw_desc);
      av_frame_free(&hw_frame);
      g->video_zerocopy_failed = true;
      syslog(LOG_INFO, "plexy-dm: zero-copy import failed — permanently using "
                       "software path");
      return false;
    }

    plexy_zc_release_slot(g->video_egl_display, &g->video_prev_slot);
    g->video_prev_slot = g->video_slot;
    g->video_slot = slot;

    g->video_texture = slot.tex_y;
    g->video_tex_uv = slot.tex_uv;
    g->video_tex_w = slot.frame->width;
    g->video_tex_h = slot.frame->height;
    g->video_yuv_mode = true;

    static bool logged_once = false;
    if (!logged_once) {
      logged_once = true;
      syslog(LOG_INFO,
             "plexy-dm: zero-copy VAAPI→DMA-BUF path active "
             "(NV12, %dx%d, cs=%s)",
             g->video_tex_w, g->video_tex_h, hw_cs == 1 ? "bt709" : "bt601");
    }

    g->wp_texture = g->video_texture;
    g->wp_width = g->video_tex_w;
    g->wp_height = g->video_tex_h;
    return true;
  }

  bool got_frame = false;
  uint8_t *buf = NULL;
  int w = 0, h = 0;

  pthread_mutex_lock(&g->video_mutex);
  if (g->video_new_frame && g->video_frame_valid && g->video_frame_buf) {
    w = g->video_frame_w;
    h = g->video_frame_h;
    buf = (uint8_t *)malloc((size_t)w * h * 4);
    if (buf) {
      memcpy(buf, g->video_frame_buf, (size_t)w * h * 4);
      g->video_new_frame = false;
      got_frame = true;
    }
  }
  pthread_mutex_unlock(&g->video_mutex);

  if (!got_frame || !buf)
    return false;

  if (g->video_texture == 0 || w != g->video_tex_w || h != g->video_tex_h) {
    if (g->video_texture)
      glDeleteTextures(1, &g->video_texture);
    glGenTextures(1, &g->video_texture);
    glBindTexture(GL_TEXTURE_2D, g->video_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 buf);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    g->video_tex_w = w;
    g->video_tex_h = h;
  } else {
    glBindTexture(GL_TEXTURE_2D, g->video_texture);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE,
                    buf);
  }

  free(buf);

  g->video_yuv_mode = false;
  g->wp_texture = g->video_texture;
  g->wp_width = g->video_tex_w;
  g->wp_height = g->video_tex_h;
  return true;
}

static bool init_wallpaper(plexy_greeter_t *g, const char *path) {
  g->wp_texture = 0;
  g->wp_width = 0;
  g->wp_height = 0;
  g->video_thread_started = false;

  if (path && path[0] && is_video_file(path)) {

    video_wallpaper_start(g, path);
  } else if (path && path[0]) {
    g->wp_texture =
        load_wallpaper_texture(path, g->drm.mode.hdisplay, g->drm.mode.vdisplay,
                               &g->wp_width, &g->wp_height);
  }

  GLuint vs = compile_shader(GL_VERTEX_SHADER, wp_vert_src);
  GLuint fs = compile_shader(GL_FRAGMENT_SHADER, wp_frag_src);
  if (!vs || !fs) {
    if (vs)
      glDeleteShader(vs);
    if (fs)
      glDeleteShader(fs);
    return false;
  }

  g->wp_program = glCreateProgram();
  glAttachShader(g->wp_program, vs);
  glAttachShader(g->wp_program, fs);
  glBindAttribLocation(g->wp_program, 0, "a_pos");
  glBindAttribLocation(g->wp_program, 1, "a_uv");
  glLinkProgram(g->wp_program);
  glDeleteShader(vs);
  glDeleteShader(fs);

  GLint linked = 0;
  glGetProgramiv(g->wp_program, GL_LINK_STATUS, &linked);
  if (!linked) {
    glDeleteProgram(g->wp_program);
    g->wp_program = 0;
    return false;
  }

  float verts[] = {
      -1.0f, -1.0f, 0.0f, 1.0f, 1.0f, -1.0f, 1.0f, 1.0f,
      -1.0f, 1.0f,  0.0f, 0.0f, 1.0f, 1.0f,  1.0f, 0.0f,
  };

  glGenVertexArrays(1, &g->wp_vao);
  glBindVertexArray(g->wp_vao);
  glGenBuffers(1, &g->wp_vbo);
  glBindBuffer(GL_ARRAY_BUFFER, g->wp_vbo);
  glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
  glEnableVertexAttribArray(0);
  glEnableVertexAttribArray(1);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, (void *)0);
  glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, (void *)8);
  glBindVertexArray(0);
  glBindBuffer(GL_ARRAY_BUFFER, 0);

  return true;
}

static void render_wallpaper(plexy_greeter_t *g) {
  if (!g->wp_program)
    return;

  glUseProgram(g->wp_program);

  GLint loc_wallpaper = glGetUniformLocation(g->wp_program, "u_wallpaper");
  GLint loc_has = glGetUniformLocation(g->wp_program, "u_has_wallpaper");
  GLint loc_wp_size = glGetUniformLocation(g->wp_program, "u_wallpaper_size");
  GLint loc_screen_size = glGetUniformLocation(g->wp_program, "u_screen_size");
  GLint loc_blur = glGetUniformLocation(g->wp_program, "u_blur_radius");
  GLint loc_dark = glGetUniformLocation(g->wp_program, "u_darken");
  GLint loc_time = glGetUniformLocation(g->wp_program, "u_time");
  GLint loc_weather = glGetUniformLocation(g->wp_program, "u_weather_kind");
  GLint loc_weather_intensity =
      glGetUniformLocation(g->wp_program, "u_weather_intensity");
  GLint loc_flip_y = glGetUniformLocation(g->wp_program, "u_flip_y");
  GLint loc_yuv = glGetUniformLocation(g->wp_program, "u_yuv_nv12");
  GLint loc_wp_uv = glGetUniformLocation(g->wp_program, "u_wallpaper_uv");
  GLint loc_yuv_cs = glGetUniformLocation(g->wp_program, "u_yuv_cs");

  weather_effect_t weather = WEATHER_CLOUDS;
  float weather_intensity = 0.65f;
  pthread_mutex_lock(&g->weather_lock);
  weather = g->weather_effect;
  weather_intensity = g->weather_intensity;
  pthread_mutex_unlock(&g->weather_lock);

  glUniform1i(loc_has, g->wp_texture != 0 ? 1 : 0);
  glUniform2f(loc_wp_size, g->wp_width > 0 ? (float)g->wp_width : 1.0f,
              g->wp_height > 0 ? (float)g->wp_height : 1.0f);
  glUniform2f(loc_screen_size, (float)g->drm.mode.hdisplay,
              (float)g->drm.mode.vdisplay);
  glUniform1f(loc_blur, g->wp_texture ? 14.0f : 0.0f);
  glUniform1f(loc_dark, g->wp_texture ? 0.16f : 0.18f);
  glUniform1f(loc_time, g->scene_time_s);
  glUniform1f(loc_weather, (float)weather);
  glUniform1f(loc_weather_intensity, weather_intensity);

  int flip_y = (g->video_thread_started && !g->video_yuv_mode) ? 1 : 0;
  glUniform1i(loc_flip_y, flip_y);
  glUniform1i(loc_yuv, g->video_yuv_mode ? 1 : 0);
  glUniform1i(loc_yuv_cs, g->video_hw_cs);

  if (g->wp_texture) {
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g->wp_texture);
    glUniform1i(loc_wallpaper, 0);
  }

  if (g->video_yuv_mode && g->video_tex_uv) {
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, g->video_tex_uv);
    glUniform1i(loc_wp_uv, 1);
    glActiveTexture(GL_TEXTURE0);
  }

  glBindVertexArray(g->wp_vao);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  glBindVertexArray(0);
  if (g->wp_texture)
    glBindTexture(GL_TEXTURE_2D, 0);
  glUseProgram(0);
}

static void cleanup_wallpaper(plexy_greeter_t *g) {

  video_wallpaper_stop(g);

  if (g->wp_program)
    glDeleteProgram(g->wp_program);

  if (g->video_texture == 0 && g->wp_texture)
    glDeleteTextures(1, &g->wp_texture);
  if (g->wp_vbo)
    glDeleteBuffers(1, &g->wp_vbo);
  if (g->wp_vao)
    glDeleteVertexArrays(1, &g->wp_vao);
  g->wp_texture = 0;
  g->wp_program = 0;
  g->wp_vbo = 0;
  g->wp_vao = 0;
  g->wp_width = 0;
  g->wp_height = 0;
}

static bool init_scene_buffer(plexy_greeter_t *g) {
  if (!g)
    return false;

  g->scene_width = g->drm.mode.hdisplay;
  g->scene_height = g->drm.mode.vdisplay;

  glGenTextures(1, &g->scene_texture);
  glBindTexture(GL_TEXTURE_2D, g->scene_texture);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, g->scene_width, g->scene_height, 0,
               GL_RGBA, GL_UNSIGNED_BYTE, NULL);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  glGenFramebuffers(1, &g->scene_fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, g->scene_fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                         g->scene_texture, 0);

  GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glBindTexture(GL_TEXTURE_2D, 0);
  if (status != GL_FRAMEBUFFER_COMPLETE) {
    syslog(LOG_WARNING, "plexy-dm: scene framebuffer incomplete (status=0x%x)",
           status);
    if (g->scene_fbo)
      glDeleteFramebuffers(1, &g->scene_fbo);
    if (g->scene_texture)
      glDeleteTextures(1, &g->scene_texture);
    g->scene_fbo = 0;
    g->scene_texture = 0;
    g->scene_width = 0;
    g->scene_height = 0;
    return false;
  }

  return true;
}

static void cleanup_scene_buffer(plexy_greeter_t *g) {
  if (!g)
    return;
  if (g->scene_fbo)
    glDeleteFramebuffers(1, &g->scene_fbo);
  if (g->scene_texture)
    glDeleteTextures(1, &g->scene_texture);
  g->scene_fbo = 0;
  g->scene_texture = 0;
  g->scene_width = 0;
  g->scene_height = 0;
}

static void init_msaa_fbo(plexy_greeter_t *g) {

  GLint max_samples = 0;
  glGetIntegerv(GL_MAX_SAMPLES, &max_samples);
  g->msaa_samples = (max_samples >= 8) ? 8 : (max_samples >= 4) ? 4 : 0;
  if (g->msaa_samples < 2) {
    syslog(LOG_INFO, "plexy-dm: MSAA not supported (max_samples=%d)",
           max_samples);
    return;
  }

  int w = g->drm.mode.hdisplay;
  int h = g->drm.mode.vdisplay;

  glGenRenderbuffers(1, &g->msaa_color_rbo);
  glBindRenderbuffer(GL_RENDERBUFFER, g->msaa_color_rbo);
  glRenderbufferStorageMultisample(GL_RENDERBUFFER, g->msaa_samples, GL_RGBA8,
                                   w, h);

  glGenRenderbuffers(1, &g->msaa_depth_rbo);
  glBindRenderbuffer(GL_RENDERBUFFER, g->msaa_depth_rbo);
  glRenderbufferStorageMultisample(GL_RENDERBUFFER, g->msaa_samples,
                                   GL_DEPTH_COMPONENT24, w, h);

  glGenFramebuffers(1, &g->msaa_fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, g->msaa_fbo);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                            GL_RENDERBUFFER, g->msaa_color_rbo);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                            GL_RENDERBUFFER, g->msaa_depth_rbo);

  GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);

  if (status != GL_FRAMEBUFFER_COMPLETE) {
    syslog(LOG_WARNING,
           "plexy-dm: MSAA FBO incomplete (status=0x%x), disabling AA", status);
    glDeleteFramebuffers(1, &g->msaa_fbo);
    glDeleteRenderbuffers(1, &g->msaa_color_rbo);
    glDeleteRenderbuffers(1, &g->msaa_depth_rbo);
    g->msaa_fbo = g->msaa_color_rbo = g->msaa_depth_rbo = 0;
    g->msaa_samples = 0;
    return;
  }

  syslog(LOG_INFO, "plexy-dm: MSAA %dx enabled for cube anti-aliasing",
         g->msaa_samples);
}

static void cleanup_msaa_fbo(plexy_greeter_t *g) {
  if (g->msaa_fbo)
    glDeleteFramebuffers(1, &g->msaa_fbo);
  if (g->msaa_color_rbo)
    glDeleteRenderbuffers(1, &g->msaa_color_rbo);
  if (g->msaa_depth_rbo)
    glDeleteRenderbuffers(1, &g->msaa_depth_rbo);
  g->msaa_fbo = g->msaa_color_rbo = g->msaa_depth_rbo = 0;
  g->msaa_samples = 0;
}

static const char *ui_blit_vert_src =
    "#version 330 core\n"
    "in vec2 a_pos;\n"
    "in vec2 a_uv;\n"
    "out vec2 v_uv;\n"
    "uniform vec2 u_offset;\n"
    "void main() {\n"
    "  gl_Position = vec4(a_pos + u_offset, 0.0, 1.0);\n"
    "  v_uv = a_uv;\n"
    "}\n";

static const char *ui_blit_frag_src =
    "#version 330 core\n"
    "in vec2 v_uv;\n"
    "out vec4 fragColor;\n"
    "uniform sampler2D u_tex;\n"
    "uniform float u_alpha;\n"
    "void main() {\n"
    "  vec4 c = texture(u_tex, v_uv);\n"
    "  fragColor = vec4(c.rgb, c.a * u_alpha);\n"
    "}\n";

static void init_ui_blit(plexy_greeter_t *g) {
  GLuint vs = compile_shader(GL_VERTEX_SHADER, ui_blit_vert_src);
  GLuint fs = compile_shader(GL_FRAGMENT_SHADER, ui_blit_frag_src);

  g->ui_blit_program = glCreateProgram();
  glAttachShader(g->ui_blit_program, vs);
  glAttachShader(g->ui_blit_program, fs);
  glBindAttribLocation(g->ui_blit_program, 0, "a_pos");
  glBindAttribLocation(g->ui_blit_program, 1, "a_uv");
  glLinkProgram(g->ui_blit_program);
  glDeleteShader(vs);
  glDeleteShader(fs);

  float verts[] = {
      -1.0f, -1.0f, 0.0f, 0.0f, 1.0f, -1.0f, 1.0f, 0.0f,
      -1.0f, 1.0f,  0.0f, 1.0f, 1.0f, 1.0f,  1.0f, 1.0f,
  };

  glGenVertexArrays(1, &g->ui_blit_vao);
  glBindVertexArray(g->ui_blit_vao);
  glGenBuffers(1, &g->ui_blit_vbo);
  glBindBuffer(GL_ARRAY_BUFFER, g->ui_blit_vbo);
  glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
  glEnableVertexAttribArray(0);
  glEnableVertexAttribArray(1);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, (void *)0);
  glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, (void *)8);
  glBindVertexArray(0);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
}

static void blit_texture_fullscreen_ex(plexy_greeter_t *g, GLuint tex,
                                       float alpha, float offset_x,
                                       float offset_y) {
  if (!g || !g->ui_blit_program || !g->ui_blit_vao || !tex || alpha <= 0.001f)
    return;

  alpha = greeter_clampf(alpha, 0.0f, 1.0f);

  glDisable(GL_DEPTH_TEST);
  glDisable(GL_CULL_FACE);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

  glUseProgram(g->ui_blit_program);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, tex);
  glUniform1i(glGetUniformLocation(g->ui_blit_program, "u_tex"), 0);
  glUniform1f(glGetUniformLocation(g->ui_blit_program, "u_alpha"), alpha);
  glUniform2f(glGetUniformLocation(g->ui_blit_program, "u_offset"), offset_x,
              offset_y);

  glBindVertexArray(g->ui_blit_vao);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  glBindVertexArray(0);

  glUseProgram(0);
  glDisable(GL_BLEND);
}

static void blit_texture_fullscreen(plexy_greeter_t *g, GLuint tex) {
  blit_texture_fullscreen_ex(g, tex, 1.0f, 0.0f, 0.0f);
}

static void blit_ui_overlay(plexy_greeter_t *g) {

  PlexyCanvas *canvas = greeter_ui_get_canvas(g->ui ? g->ui : g->ui_userselect);
  if (!canvas)
    return;

  GLuint ui_tex = plexy_canvas_get_texture(canvas);
  if (!ui_tex) {
    static bool warned = false;
    if (!warned) {
      syslog(LOG_WARNING,
             "plexy-dm: UI FBO texture is 0, canvas may have failed init");
      warned = true;
    }
    return;
  }

  blit_texture_fullscreen(g, ui_tex);
}

static void cleanup_ui_blit(plexy_greeter_t *g) {
  if (g->ui_blit_program)
    glDeleteProgram(g->ui_blit_program);
  if (g->ui_blit_vbo)
    glDeleteBuffers(1, &g->ui_blit_vbo);
  if (g->ui_blit_vao)
    glDeleteVertexArrays(1, &g->ui_blit_vao);
  g->ui_blit_program = 0;
  g->ui_blit_vbo = 0;
  g->ui_blit_vao = 0;
}

static const char *floor_fx_frag_src =
    "#version 330 core\n"
    "in vec2 v_uv;\n"
    "out vec4 fragColor;\n"
    "uniform vec2 u_shadow_center;\n"
    "uniform vec2 u_shadow_size;\n"
    "uniform vec2 u_contact_center;\n"
    "uniform vec2 u_contact_size;\n"
    "uniform vec2 u_reflect_center;\n"
    "uniform vec2 u_reflect_size;\n"
    "uniform float u_alpha;\n"
    "float soft_ellipse(vec2 p, vec2 center, vec2 radius) {\n"
    "  vec2 d = (p - center) / max(radius, vec2(0.001));\n"
    "  return exp(-dot(d, d));\n"
    "}\n"
    "void main() {\n"
    "  vec2 p = v_uv * 2.0 - 1.0;\n"
    "  float floor_mask = 1.0 - smoothstep(-0.22, 0.05, p.y);\n"
    "  float shadow = soft_ellipse(p, u_shadow_center + vec2(0.08, -0.02), "
    "u_shadow_size);\n"
    "  float shadow_soft = soft_ellipse(p, u_shadow_center + vec2(0.10, "
    "-0.03), u_shadow_size * vec2(1.42, 1.06));\n"
    "  float shadow_tail = soft_ellipse(p, u_shadow_center + vec2(0.22, "
    "-0.06), u_shadow_size * vec2(1.26, 0.82));\n"
    "  float contact = soft_ellipse(p, u_contact_center, u_contact_size);\n"
    "  float reflection = soft_ellipse(p, u_reflect_center, u_reflect_size);\n"
    "  float reflection_core = soft_ellipse(p, u_reflect_center + vec2(0.0, "
    "0.02), u_reflect_size * vec2(0.62, 0.42));\n"
    "  float reflection_wide = soft_ellipse(p, u_reflect_center + vec2(0.0, "
    "0.07), u_reflect_size * vec2(1.62, 0.76));\n"
    "  float reflection_streak = soft_ellipse(p, u_reflect_center + vec2(0.0, "
    "0.12), u_reflect_size * vec2(2.10, 0.26));\n"
    "  vec3 col = vec3(0.0);\n"
    "  float alpha = 0.0;\n"
    "  col += vec3(0.00, 0.01, 0.02) * shadow * 1.08;\n"
    "  alpha += shadow * 0.30;\n"
    "  col += vec3(0.01, 0.02, 0.03) * shadow_soft * 0.70;\n"
    "  alpha += shadow_soft * 0.11;\n"
    "  col += vec3(0.01, 0.015, 0.03) * shadow_tail * 0.84;\n"
    "  alpha += shadow_tail * 0.16;\n"
    "  col += vec3(0.00, 0.00, 0.01) * contact * 1.08;\n"
    "  alpha += contact * 0.52;\n"
    "  col += vec3(0.08, 0.10, 0.15) * reflection_wide * 0.12;\n"
    "  alpha += reflection_wide * 0.05;\n"
    "  col += vec3(0.20, 0.25, 0.36) * reflection * 0.26;\n"
    "  alpha += reflection * 0.14;\n"
    "  col += vec3(0.56, 0.66, 0.84) * reflection_core * 0.14;\n"
    "  alpha += reflection_core * 0.11;\n"
    "  col += vec3(0.48, 0.56, 0.74) * reflection_streak * 0.08;\n"
    "  alpha += reflection_streak * 0.05;\n"
    "  fragColor = vec4(col * floor_mask * u_alpha, min(alpha * floor_mask * "
    "u_alpha, 0.72));\n"
    "}\n";

static bool init_floor_fx(plexy_greeter_t *g) {
  GLuint vs = compile_shader(GL_VERTEX_SHADER, wp_vert_src);
  GLuint fs = compile_shader(GL_FRAGMENT_SHADER, floor_fx_frag_src);
  if (!vs || !fs) {
    glDeleteShader(vs);
    glDeleteShader(fs);
    return false;
  }

  g->floor_fx_program = glCreateProgram();
  glAttachShader(g->floor_fx_program, vs);
  glAttachShader(g->floor_fx_program, fs);
  glBindAttribLocation(g->floor_fx_program, 0, "a_pos");
  glBindAttribLocation(g->floor_fx_program, 1, "a_uv");
  glLinkProgram(g->floor_fx_program);
  glDeleteShader(vs);
  glDeleteShader(fs);

  GLint ok = 0;
  glGetProgramiv(g->floor_fx_program, GL_LINK_STATUS, &ok);
  if (!ok) {
    char log[256];
    glGetProgramInfoLog(g->floor_fx_program, sizeof(log), NULL, log);
    syslog(LOG_WARNING, "plexy-dm: floor fx shader link: %s", log);
    glDeleteProgram(g->floor_fx_program);
    g->floor_fx_program = 0;
    return false;
  }
  return true;
}

static void cleanup_floor_fx(plexy_greeter_t *g) {
  if (g->floor_fx_program)
    glDeleteProgram(g->floor_fx_program);
  g->floor_fx_program = 0;
}

static void render_floor_fx(plexy_greeter_t *g, const mat4 mvp, float alpha) {
  if (!g->floor_fx_program || !g->ui_blit_vao || alpha <= 0.0f)
    return;

  static const float cube_corners[8][3] = {
      {-1.0f, -1.0f, -1.0f}, {1.0f, -1.0f, -1.0f}, {-1.0f, 1.0f, -1.0f},
      {1.0f, 1.0f, -1.0f},   {-1.0f, -1.0f, 1.0f}, {1.0f, -1.0f, 1.0f},
      {-1.0f, 1.0f, 1.0f},   {1.0f, 1.0f, 1.0f},
  };

  float min_x = 0.0f, max_x = 0.0f;
  float min_y = 0.0f, max_y = 0.0f;
  for (size_t i = 0; i < 8; ++i) {
    float x = 0.0f, y = 0.0f;
    if (!project_point_ndc(mvp, cube_corners[i][0], cube_corners[i][1],
                           cube_corners[i][2], &x, &y))
      return;

    if (i == 0) {
      min_x = max_x = x;
      min_y = max_y = y;
      continue;
    }

    min_x = fminf(min_x, x);
    max_x = fmaxf(max_x, x);
    min_y = fminf(min_y, y);
    max_y = fmaxf(max_y, y);
  }

  const float cube_width = fmaxf(max_x - min_x, 0.08f);
  const float cube_height = fmaxf(max_y - min_y, 0.08f);
  const float cube_center_x = (min_x + max_x) * 0.5f;
  const float cube_bottom_y = min_y;

  const float shadow_center_x = cube_center_x;
  const float shadow_center_y = cube_bottom_y - cube_height * 0.12f;
  const float contact_center_y = cube_bottom_y - cube_height * 0.05f;
  const float reflect_center_y = cube_bottom_y - cube_height * 0.20f;

  glDisable(GL_DEPTH_TEST);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

  glUseProgram(g->floor_fx_program);
  glUniform2f(glGetUniformLocation(g->floor_fx_program, "u_shadow_center"),
              shadow_center_x, shadow_center_y);
  glUniform2f(glGetUniformLocation(g->floor_fx_program, "u_shadow_size"),
              cube_width * 0.66f, cube_height * 0.22f);
  glUniform2f(glGetUniformLocation(g->floor_fx_program, "u_contact_center"),
              shadow_center_x, contact_center_y);
  glUniform2f(glGetUniformLocation(g->floor_fx_program, "u_contact_size"),
              cube_width * 0.42f, cube_height * 0.07f);
  glUniform2f(glGetUniformLocation(g->floor_fx_program, "u_reflect_center"),
              shadow_center_x, reflect_center_y);
  glUniform2f(glGetUniformLocation(g->floor_fx_program, "u_reflect_size"),
              cube_width * 0.40f, cube_height * 0.18f);
  glUniform1f(glGetUniformLocation(g->floor_fx_program, "u_alpha"), alpha);

  glBindVertexArray(g->ui_blit_vao);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  glBindVertexArray(0);

  glUseProgram(0);
  glDisable(GL_BLEND);
}

static const char *cube_vert_src =
    "#version 330 core\n"
    "in vec3 a_pos;\n"
    "in vec2 a_uv;\n"
    "in float a_face_id;\n"
    "in vec3 a_normal;\n"
    "out vec2 v_uv;\n"
    "out float v_face_id;\n"
    "out vec3 v_normal;\n"
    "out vec3 v_world_pos;\n"
    "out vec3 v_local_pos;\n"
    "out vec4 v_clip_pos;\n"
    "uniform mat4 u_mvp;\n"
    "uniform mat4 u_model;\n"
    "uniform mat4 u_proj;\n"
    "uniform float u_expand;\n"
    "void main() {\n"
    "    vec3 pos = a_pos + a_normal * u_expand;\n"
    "    vec4 world = u_model * vec4(pos, 1.0);\n"
    "    vec4 clip = u_proj * world;\n"
    "    gl_Position = clip;\n"
    "    v_uv = a_uv;\n"
    "    v_face_id = a_face_id;\n"
    "    v_normal = normalize(mat3(u_model) * a_normal);\n"
    "    v_world_pos = world.xyz;\n"
    "    v_local_pos = a_pos;\n"
    "    v_clip_pos = clip;\n"
    "}\n";

static const char *cube_frag_src =
    "#version 330 core\n"
    "in vec2 v_uv;\n"
    "in float v_face_id;\n"
    "in vec3 v_normal;\n"
    "in vec3 v_world_pos;\n"
    "in vec3 v_local_pos;\n"
    "in vec4 v_clip_pos;\n"
    "out vec4 fragColor;\n"
    "uniform sampler2D u_tex_front;\n"
    "uniform sampler2D u_tex_right;\n"
    "uniform sampler2D u_scene_tex;\n"
    "uniform float u_alpha;\n"
    "uniform float u_time;\n"
    "uniform vec3 u_light_dir;\n"
    "uniform mat4 u_proj;\n"
    "uniform mat4 u_model;\n"
    "uniform vec2 u_scene_texel;\n"
    "float saturate(float x) {\n"
    "    return clamp(x, 0.0, 1.0);\n"
    "}\n"
    "vec2 saturate2(vec2 v) {\n"
    "    return clamp(v, vec2(0.0), vec2(1.0));\n"
    "}\n"
    "float ggx_d(float ndh, float rough) {\n"
    "    float a = rough * rough;\n"
    "    float a2 = a * a;\n"
    "    float d = ndh * ndh * (a2 - 1.0) + 1.0;\n"
    "    return a2 / max(3.14159265 * d * d, 1.0e-4);\n"
    "}\n"
    "float schlick_ggx(float ndx, float rough) {\n"
    "    float r = rough + 1.0;\n"
    "    float k = (r * r) / 8.0;\n"
    "    return ndx / max(ndx * (1.0 - k) + k, 1.0e-4);\n"
    "}\n"
    "vec3 fresnel_schlick(float cos_theta, vec3 f0) {\n"
    "    return f0 + (1.0 - f0) * pow(1.0 - cos_theta, 5.0);\n"
    "}\n"
    "float hash13(vec3 p) {\n"
    "    return fract(sin(dot(p, vec3(127.1, 311.7, 191.9))) * "
    "43758.5453123);\n"
    "}\n"
    "vec3 orthogonal(vec3 n) {\n"
    "    return normalize(abs(n.z) < 0.999 ? cross(n, vec3(0.0, 0.0, 1.0))\n"
    "                                       : cross(n, vec3(0.0, 1.0, 0.0)));\n"
    "}\n"
    "vec3 studio_env(vec3 d) {\n"
    "    float up = saturate(d.y * 0.5 + 0.5);\n"
    "    float forward = saturate(d.z * 0.5 + 0.5);\n"
    "    vec3 env = mix(vec3(0.003, 0.005, 0.012), vec3(0.070, 0.104, 0.19), "
    "up * 0.56 + forward * 0.22);\n"
    "    vec2 key_xy = (d.xy - vec2(0.15, 0.26)) / vec2(0.12, 0.07);\n"
    "    vec2 fill_xy = (d.xy - vec2(-0.32, 0.08)) / vec2(0.28, 0.18);\n"
    "    vec2 strip_xy = (d.xy - vec2(-0.08, 0.38)) / vec2(0.46, 0.09);\n"
    "    vec2 rim_xy = (d.xy - vec2(0.74, 0.04)) / vec2(0.12, 0.28);\n"
    "    vec2 rear_xy = (d.xy - vec2(0.0, -0.10)) / vec2(0.74, 0.52);\n"
    "    vec2 dome_xy = (d.xy - vec2(0.0, 0.18)) / vec2(0.90, 0.42);\n"
    "    float key = exp(-dot(key_xy, key_xy));\n"
    "    float fill = exp(-dot(fill_xy, fill_xy));\n"
    "    float strip = exp(-dot(strip_xy, strip_xy));\n"
    "    float rim = exp(-dot(rim_xy, rim_xy));\n"
    "    float rear = exp(-dot(rear_xy, rear_xy));\n"
    "    float dome = exp(-dot(dome_xy, dome_xy));\n"
    "    env += vec3(1.00, 1.00, 1.00) * key * 0.74;\n"
    "    env += vec3(0.42, 0.62, 1.00) * fill * 0.46;\n"
    "    env += vec3(0.86, 0.94, 1.00) * strip * 0.36;\n"
    "    env += vec3(0.74, 0.82, 0.96) * rim * 0.20;\n"
    "    env += vec3(0.22, 0.30, 0.48) * rear * 0.28;\n"
    "    env += vec3(0.08, 0.12, 0.20) * dome * 0.26;\n"
    "    return env;\n"
    "}\n"
    "vec2 project_scene_uv(vec3 world_pos, vec2 fallback_uv) {\n"
    "    vec4 clip = u_proj * vec4(world_pos, 1.0);\n"
    "    if (clip.w <= 0.0001)\n"
    "        return fallback_uv;\n"
    "    return clip.xy / clip.w * 0.5 + 0.5;\n"
    "}\n"
    "vec3 sample_scene_color(vec2 uv, float radius) {\n"
    "    uv = clamp(uv, vec2(0.002), vec2(0.998));\n"
    "    vec2 r = u_scene_texel * radius;\n"
    "    vec3 c = texture(u_scene_tex, uv).rgb * 0.34;\n"
    "    c += texture(u_scene_tex, clamp(uv + vec2( r.x, 0.0), vec2(0.002), "
    "vec2(0.998))).rgb * 0.15;\n"
    "    c += texture(u_scene_tex, clamp(uv + vec2(-r.x, 0.0), vec2(0.002), "
    "vec2(0.998))).rgb * 0.15;\n"
    "    c += texture(u_scene_tex, clamp(uv + vec2(0.0,  r.y), vec2(0.002), "
    "vec2(0.998))).rgb * 0.15;\n"
    "    c += texture(u_scene_tex, clamp(uv + vec2(0.0, -r.y), vec2(0.002), "
    "vec2(0.998))).rgb * 0.15;\n"
    "    c += texture(u_scene_tex, clamp(uv + r * vec2( 0.7,  0.7), "
    "vec2(0.002), vec2(0.998))).rgb * 0.03;\n"
    "    c += texture(u_scene_tex, clamp(uv + r * vec2(-0.7,  0.7), "
    "vec2(0.002), vec2(0.998))).rgb * 0.03;\n"
    "    return c;\n"
    "}\n"
    "vec3 harmonic_volume_color(vec3 p) {\n"
    "    p = abs(p);\n"
    "    p *= 1.25;\n"
    "    p = 0.5 * p / max(dot(p, p), 0.18);\n"
    "    float t = 0.13 * length(p) + u_time * 0.025;\n"
    "    vec3 col = vec3(0.30, 0.40, 0.50);\n"
    "    col += 0.12 * cos(6.28318 * t * 1.0   + vec3(0.0, 0.8, 1.1));\n"
    "    col += 0.11 * cos(6.28318 * t * 3.1   + vec3(0.3, 0.4, 0.1));\n"
    "    col += 0.10 * cos(6.28318 * t * 5.1   + vec3(0.1, 0.7, 1.1));\n"
    "    col += 0.10 * cos(6.28318 * t * 17.1  + vec3(0.2, 0.6, 0.7));\n"
    "    col += 0.10 * cos(6.28318 * t * 31.1  + vec3(0.1, 0.6, 0.7));\n"
    "    col += 0.10 * cos(6.28318 * t * 65.1  + vec3(0.0, 0.5, 0.8));\n"
    "    col += 0.10 * cos(6.28318 * t * 115.1 + vec3(0.1, 0.4, 0.7));\n"
    "    col += 0.10 * cos(6.28318 * t * 265.1 + vec3(1.1, 1.4, 2.7));\n"
    "    return clamp(col, vec3(0.0), vec3(1.0));\n"
    "}\n"
    "mat3 bp_rotx(float a) {\n"
    "    float s = sin(a), c = cos(a);\n"
    "    return mat3(1.0, 0.0, 0.0, 0.0, c, s, 0.0, -s, c);\n"
    "}\n"
    "mat3 bp_roty(float a) {\n"
    "    float s = sin(a), c = cos(a);\n"
    "    return mat3(c, 0.0, s, 0.0, 1.0, 0.0, -s, 0.0, c);\n"
    "}\n"
    "mat3 bp_rotz(float a) {\n"
    "    float s = sin(a), c = cos(a);\n"
    "    return mat3(c, s, 0.0, -s, c, 0.0, 0.0, 0.0, 1.0);\n"
    "}\n"
    "bool bp_intersect(vec3 ro, vec3 rd, vec4 ps, vec4 ph, float sz,\n"
    "    out float t, out vec3 norm,\n"
    "    out bool si, out float tsi, out vec3 normsi, out float fade) {\n"
    "    vec3 va = vec3(0.0, 0.0, ph.x + ph.w - ph.y - ph.z);\n"
    "    vec3 vb = vec3(0.0, ps.w - ps.y, ph.z - ph.x);\n"
    "    vec3 vc = vec3(ps.z - ps.x, 0.0, ph.y - ph.x);\n"
    "    vec3 vd = vec3(ps.xy, ph.x);\n"
    "    t = -1.0; tsi = -1.0; si = false; fade = 1.0;\n"
    "    norm = vec3(0.0, 1.0, 0.0);\n"
    "    normsi = vec3(0.0, 1.0, 0.0);\n"
    "    float inv = 1.0 / (vb.y * vc.x);\n"
    "    float ca = 0.0, cb = 0.0, cc = 0.0;\n"
    "    float cd = va.z * inv;\n"
    "    float ce = 0.0, cf = 0.0;\n"
    "    float cg = (vc.z * vb.y - vd.y * va.z) * inv;\n"
    "    float ch = (vb.z * vc.x - va.z * vd.x) * inv;\n"
    "    float ci = -1.0;\n"
    "    float cj = (vd.x * vd.y * va.z + vd.z * vb.y * vc.x) * inv\n"
    "            - (vd.y * vb.z * vc.x + vd.x * vc.z * vb.y) * inv;\n"
    "    float p = dot(vec3(ca, cb, cc), rd.xzy * rd.xzy)\n"
    "           + dot(vec3(cd, ce, cf), rd.xzy * rd.zyx);\n"
    "    float q = dot(vec3(2.0) * ro.xzy * rd.xyz, vec3(ca, cb, cc))\n"
    "           + dot(ro.xzz * rd.zxy, vec3(cd, cd, ce))\n"
    "           + dot(ro.yyx * rd.zxy, vec3(ce, cf, cf))\n"
    "           + dot(vec3(cg, ch, ci), rd.xzy);\n"
    "    float r = dot(vec3(ca, cb, cc), ro.xzy * ro.xzy)\n"
    "           + dot(vec3(cd, ce, cf), ro.xzy * ro.zyx)\n"
    "           + dot(vec3(cg, ch, ci), ro.xzy) + cj;\n"
    "    if (abs(p) < 0.000001) {\n"
    "        float tt = -r / q;\n"
    "        if (tt <= 0.0) return false;\n"
    "        t = tt;\n"
    "        vec3 pos = ro + t * rd;\n"
    "        if (length(pos) > sz) return false;\n"
    "        vec3 grad = vec3(2.0) * pos.xzy * vec3(ca, cb, cc)\n"
    "                  + pos.zxz * vec3(cd, cd, ce)\n"
    "                  + pos.yyx * vec3(cf, ce, cf)\n"
    "                  + vec3(cg, ch, ci);\n"
    "        norm = -normalize(grad);\n"
    "        return true;\n"
    "    }\n"
    "    float sq = q * q - 4.0 * p * r;\n"
    "    if (sq < 0.0) return false;\n"
    "    float s = sqrt(sq);\n"
    "    float t0 = (-q + s) / (2.0 * p);\n"
    "    float t1 = (-q - s) / (2.0 * p);\n"
    "    float tt1 = min(t0 < 0.0 ? t1 : t0, t1 < 0.0 ? t0 : t1);\n"
    "    float tt2 = max(t0 > 0.0 ? t1 : t0, t1 > 0.0 ? t0 : t1);\n"
    "    float tt0 = tt1;\n"
    "    if (tt0 <= 0.0) return false;\n"
    "    vec3 pos = ro + tt0 * rd;\n"
    "    bool ru = step(sz, length(pos)) > 0.5;\n"
    "    if (ru) { tt0 = tt2; pos = ro + tt0 * rd; }\n"
    "    if (tt0 <= 0.0) return false;\n"
    "    if (step(sz, length(pos)) > 0.5) return false;\n"
    "    if (tt2 > 0.0 && !ru && !(step(sz, length(ro + tt2 * rd)) > 0.5)) {\n"
    "        si = true; tsi = tt2;\n"
    "        vec3 tpos = ro + tsi * rd;\n"
    "        vec3 tgrad = vec3(2.0) * tpos.xzy * vec3(ca, cb, cc)\n"
    "                   + tpos.zxz * vec3(cd, cd, ce)\n"
    "                   + tpos.yyx * vec3(cf, ce, cf)\n"
    "                   + vec3(cg, ch, ci);\n"
    "        normsi = -normalize(tgrad);\n"
    "    }\n"
    "    fade = s;\n"
    "    t = tt0;\n"
    "    vec3 grad = vec3(2.0) * pos.xzy * vec3(ca, cb, cc)\n"
    "              + pos.zxz * vec3(cd, cd, ce)\n"
    "              + pos.yyx * vec3(cf, ce, cf)\n"
    "              + vec3(cg, ch, ci);\n"
    "    norm = -normalize(grad);\n"
    "    return true;\n"
    "}\n"
    "vec4 insides_bp(vec3 ro, vec3 rd, vec3 nor_c, vec3 l_dir) {\n"
    "    float pi = 3.14159265;\n"
    "    if (abs(nor_c.x) > 0.5) {\n"
    "        rd = rd.xzy * nor_c.x;\n"
    "        ro = ro.xzy * nor_c.x;\n"
    "    } else if (abs(nor_c.z) > 0.5) {\n"
    "        l_dir *= bp_roty(pi);\n"
    "        rd = rd.yxz * nor_c.z;\n"
    "        ro = ro.yxz * nor_c.z;\n"
    "    } else if (abs(nor_c.y) > 0.5) {\n"
    "        l_dir *= bp_rotz(-pi * 0.5);\n"
    "        rd = rd * nor_c.y;\n"
    "        ro = ro * nor_c.y;\n"
    "    }\n"
    "    float bp_ang = u_time * 0.12;\n"
    "    ro *= bp_roty(bp_ang);\n"
    "    rd *= bp_roty(bp_ang);\n"
    "    l_dir *= bp_roty(bp_ang);\n"
    "    float t_anim = mod(u_time * 0.18, 20.0);\n"
    "    float shape_mix = smoothstep(0.0, 8.5, t_anim)\n"
    "                    * (1.0 - smoothstep(10.0, 18.5, t_anim));\n"
    "    float curvature = 0.15 + 0.65 * (1.0 - shape_mix);\n"
    "    float bil_size = 0.75;\n"
    "    vec4 ps = vec4(-bil_size, -bil_size, bil_size, bil_size) * "
    "curvature;\n"
    "    vec4 ph = vec4(-bil_size, bil_size, bil_size, -bil_size) * "
    "curvature;\n"
    "    vec4 colx[3] = vec4[3](vec4(0.0), vec4(0.0), vec4(0.0));\n"
    "    vec3 dx[3] = vec3[3](vec3(-1.0), vec3(-1.0), vec3(-1.0));\n"
    "    vec4 colxsi[3] = vec4[3](vec4(0.0), vec4(0.0), vec4(0.0));\n"
    "    int order[3] = int[3](0, 1, 2);\n"
    "    for (int i = 0; i < 3; i++) {\n"
    "        if (abs(nor_c.x) > 0.5) {\n"
    "            ro *= bp_rotz(-pi / 3.0);\n"
    "            rd *= bp_rotz(-pi / 3.0);\n"
    "        } else if (abs(nor_c.z) > 0.5) {\n"
    "            ro *= bp_rotz(pi / 3.0);\n"
    "            rd *= bp_rotz(pi / 3.0);\n"
    "        } else if (abs(nor_c.y) > 0.5) {\n"
    "            ro *= bp_rotx(pi / 3.0);\n"
    "            rd *= bp_rotx(pi / 3.0);\n"
    "        }\n"
    "        vec3 normnew; float tnew;\n"
    "        bool bsi; float btsi; vec3 bnormsi; float bfade;\n"
    "        if (bp_intersect(ro, rd, ps, ph, bil_size,\n"
    "            tnew, normnew, bsi, btsi, bnormsi, bfade)) {\n"
    "            if (tnew > 0.0) {\n"
    "                vec3 pos = ro + rd * tnew;\n"
    "                float ea = 1.0 - smoothstep(bil_size - 0.075,\n"
    "                    bil_size + 0.00001, length(pos));\n"
    "                vec3 rc = harmonic_volume_color(pos);\n"
    "                if (ea > 0.0) {\n"
    "                    dx[i] = vec3(tnew, float(bsi), btsi);\n"
    "                    float dif = clamp(dot(normnew, l_dir), 0.0, 1.0);\n"
    "                    float amb = clamp(0.5 + 0.5 * dot(normnew, l_dir),\n"
    "                        0.0, 1.0);\n"
    "                    vec3 shad = vec3(0.32, 0.43, 0.54) * amb\n"
    "                              + vec3(1.0, 0.9, 0.7) * dif;\n"
    "                    vec3 tcr = vec3(1.0, 0.21, 0.11);\n"
    "                    float ta = clamp(length(rc), 0.0, 1.0);\n"
    "                    rc = clamp(rc * rc * 2.0, 0.0, 1.0);\n"
    "                    vec3 lit = rc * shad * 1.4\n"
    "                        + 3.0 * tcr * rc\n"
    "                        * clamp(1.0 - (amb + dif), 0.0, 1.0);\n"
    "                    lit = clamp(2.0 * lit * lit, 0.0, 1.0);\n"
    "                    lit *= min(bfade * 5.0, 1.0);\n"
    "                    colx[i] = vec4(lit, min(ea, ta));\n"
    "                    if (bsi) {\n"
    "                        pos = ro + rd * btsi;\n"
    "                        float ea2 = 1.0 - smoothstep(\n"
    "                            bil_size - 0.075,\n"
    "                            bil_size + 0.00001, length(pos));\n"
    "                        vec3 rc2 = harmonic_volume_color(pos);\n"
    "                        dif = clamp(dot(bnormsi, l_dir), 0.0, 1.0);\n"
    "                        amb = clamp(0.5 + 0.5\n"
    "                            * dot(bnormsi, l_dir), 0.0, 1.0);\n"
    "                        shad = vec3(0.32, 0.43, 0.54) * amb\n"
    "                             + vec3(1.0, 0.9, 0.7) * dif;\n"
    "                        float ta2 = clamp(length(rc2), 0.0, 1.0);\n"
    "                        rc2 = clamp(rc2 * rc2 * 2.0, 0.0, 1.0);\n"
    "                        vec3 lit2 = rc2 * shad\n"
    "                            + 3.0 * tcr * rc2\n"
    "                            * clamp(1.0 - (amb + dif), 0.0, 1.0);\n"
    "                        lit2 = clamp(2.0 * lit2 * lit2, 0.0, 1.0);\n"
    "                        colxsi[i] = vec4(lit2, min(ea2, ta2));\n"
    "                    }\n"
    "                }\n"
    "            }\n"
    "        }\n"
    "    }\n"
    "    if (dx[0].x < dx[1].x) {\n"
    "        vec3 tv = dx[0]; dx[0] = dx[1]; dx[1] = tv;\n"
    "        int ti = order[0]; order[0] = order[1]; order[1] = ti;\n"
    "    }\n"
    "    if (dx[1].x < dx[2].x) {\n"
    "        vec3 tv = dx[1]; dx[1] = dx[2]; dx[2] = tv;\n"
    "        int ti = order[1]; order[1] = order[2]; order[2] = ti;\n"
    "    }\n"
    "    if (dx[0].x < dx[1].x) {\n"
    "        vec3 tv = dx[0]; dx[0] = dx[1]; dx[1] = tv;\n"
    "        int ti = order[0]; order[0] = order[1]; order[1] = ti;\n"
    "    }\n"
    "    float a = 1.0;\n"
    "    if (dx[0].y < 0.5) a = colx[order[0]].a;\n"
    "    bool rul0 = (dx[0].y > 0.5) && (dx[1].x <= 0.0);\n"
    "    bool rul1 = (dx[1].y > 0.5) && (dx[0].x > dx[1].z);\n"
    "    bool rul2 = (dx[2].y > 0.5) && (dx[1].x > dx[2].z);\n"
    "    if (rul0) {\n"
    "        vec4 mx = mix(colxsi[order[0]], colx[order[0]],\n"
    "            colx[order[0]].a);\n"
    "        colx[order[0]] = mix(vec4(0.0), mx,\n"
    "            max(colx[order[0]].a, colxsi[order[0]].a));\n"
    "    }\n"
    "    if (rul1) {\n"
    "        vec4 mx = mix(colxsi[order[1]], colx[order[1]],\n"
    "            colx[order[1]].a);\n"
    "        colx[order[1]] = mix(vec4(0.0), mx,\n"
    "            max(colx[order[1]].a, colxsi[order[1]].a));\n"
    "    }\n"
    "    if (rul2) {\n"
    "        vec4 mx = mix(colxsi[order[2]], colx[order[2]],\n"
    "            colx[order[2]].a);\n"
    "        colx[order[2]] = mix(vec4(0.0), mx,\n"
    "            max(colx[order[2]].a, colxsi[order[2]].a));\n"
    "    }\n"
    "    float a1 = (dx[1].y < 0.5) ? colx[order[1]].a\n"
    "        : ((dx[1].z > dx[0].x) ? colx[order[1]].a : 1.0);\n"
    "    float a2 = (dx[2].y < 0.5) ? colx[order[2]].a\n"
    "        : ((dx[2].z > dx[1].x) ? colx[order[2]].a : 1.0);\n"
    "    vec3 col = mix(mix(colx[order[0]].rgb,\n"
    "        colx[order[1]].rgb, a1), colx[order[2]].rgb, a2);\n"
    "    a = max(max(a, a1), a2);\n"
    "    return vec4(col, a);\n"
    "}\n"
    "void main() {\n"
    "    vec3 n0 = normalize(v_normal);\n"
    "    vec3 view_dir = normalize(-v_world_pos);\n"
    "    vec3 tangent = orthogonal(n0);\n"
    "    vec3 bitangent = normalize(cross(n0, tangent));\n"
    "    float micro_a = hash13(v_world_pos * 14.0 + vec3(v_uv, v_face_id * "
    "0.37));\n"
    "    float micro_b = hash13(v_world_pos.zxy * 17.0 + vec3(v_uv.yx, 2.17 + "
    "v_face_id));\n"
    "    vec2 micro = (vec2(micro_a, micro_b) - 0.5) * 2.0;\n"
    "    vec3 n = normalize(n0 + (tangent * micro.x + bitangent * micro.y) * "
    "0.020);\n"
    "    vec3 refl_dir = reflect(-view_dir, n);\n"
    "    vec3 key_dir = normalize(u_light_dir);\n"
    "    vec3 fill_dir = normalize(vec3(-0.58, 0.32, 0.74));\n"
    "    vec3 rim_dir = normalize(vec3(0.74, 0.10, -0.66));\n"
    "    float ndv = max(dot(n, view_dir), 0.001);\n"
    "    vec2 surface_uv = saturate2(v_clip_pos.xy / max(v_clip_pos.w, 0.0001) "
    "* 0.5 + 0.5);\n"
    "    float edge_dist = min(min(v_uv.x, 1.0 - v_uv.x), min(v_uv.y, 1.0 - "
    "v_uv.y));\n"
    "    float edge_bevel = 1.0 - smoothstep(0.0, 0.055, edge_dist);\n"
    "    float face_frame = smoothstep(0.02, 0.22, edge_dist);\n"
    "    float thickness = mix(0.66, 1.46, pow(1.0 - ndv, 0.52)) + edge_bevel "
    "* 0.30;\n"
    "    float rough = mix(0.038, 0.088, micro_a * 0.6 + edge_bevel * 0.4);\n"
    "    float coat_rough = 0.025;\n"
    "    vec4 tex = vec4(0.0);\n"
    "    vec2 content_shift = (n.xy * 0.010 + refl_dir.xy * 0.006) * (0.55 + "
    "thickness * 0.18);\n"
    "    vec2 content_uv = clamp(v_uv + content_shift, vec2(0.0), vec2(1.0));\n"
    "    if (v_face_id > -0.5 && v_face_id < 0.5) {\n"
    "        tex = texture(u_tex_front, content_uv);\n"
    "    } else if (v_face_id >= 0.5) {\n"
    "        tex = texture(u_tex_right, content_uv);\n"
    "    }\n"
    "    vec3 f0 = vec3(0.040);\n"
    "    vec3 env = studio_env(refl_dir);\n"
    "    vec3 reflect_scene = sample_scene_color(surface_uv + refl_dir.xy * "
    "0.026, 3.2 + rough * 20.0);\n"
    "    vec3 reflect_env = mix(env, reflect_scene, 0.28);\n"
    "    vec3 refr_r = refract(-view_dir, n, 1.0 / 1.436);\n"
    "    vec3 refr_g = refract(-view_dir, n, 1.0 / 1.444);\n"
    "    vec3 refr_b = refract(-view_dir, n, 1.0 / 1.452);\n"
    "    if (length(refr_r) < 0.001) refr_r = reflect(-view_dir, n);\n"
    "    if (length(refr_g) < 0.001) refr_g = reflect(-view_dir, n);\n"
    "    if (length(refr_b) < 0.001) refr_b = reflect(-view_dir, n);\n"
    "    vec2 refr_uv_r = project_scene_uv(v_world_pos + refr_r * thickness, "
    "surface_uv);\n"
    "    vec2 refr_uv_g = project_scene_uv(v_world_pos + refr_g * thickness * "
    "0.98, surface_uv);\n"
    "    vec2 refr_uv_b = project_scene_uv(v_world_pos + refr_b * thickness * "
    "1.02, surface_uv);\n"
    "    float refr_radius = 1.1 + rough * 18.0;\n"
    "    vec3 scene_refract;\n"
    "    scene_refract.r = sample_scene_color(refr_uv_r + refr_r.xy * 0.010, "
    "refr_radius).r;\n"
    "    scene_refract.g = sample_scene_color(refr_uv_g, refr_radius * "
    "0.92).g;\n"
    "    scene_refract.b = sample_scene_color(refr_uv_b - refr_b.xy * 0.010, "
    "refr_radius).b;\n"
    "    vec3 deep_refr = normalize(refr_g + n * 0.08);\n"
    "    vec2 deep_uv = project_scene_uv(v_world_pos + deep_refr * thickness * "
    "1.62, surface_uv);\n"
    "    vec3 back_refract = sample_scene_color(deep_uv, refr_radius * 1.20);\n"
    "    vec2 internal_uv = project_scene_uv(v_world_pos + reflect(refr_g, -n) "
    "* thickness * 0.78, surface_uv);\n"
    "    vec3 internal_reflection = sample_scene_color(internal_uv, 4.0 + "
    "rough * 22.0);\n"
    "    vec3 transmission = mix(scene_refract, back_refract, 0.30);\n"
    "    vec3 absorption = exp(-vec3(0.44, 0.30, 0.16) * (thickness * 1.72));\n"
    "    vec3 smoke_tint = vec3(0.56, 0.62, 0.74);\n"
    "    vec3 abs_lp = abs(v_local_pos);\n"
    "    vec3 face_n = vec3(0.0, 0.0, sign(v_local_pos.z));\n"
    "    if (abs_lp.x >= abs_lp.y && abs_lp.x >= abs_lp.z)\n"
    "        face_n = vec3(sign(v_local_pos.x), 0.0, 0.0);\n"
    "    else if (abs_lp.y >= abs_lp.z)\n"
    "        face_n = vec3(0.0, sign(v_local_pos.y), 0.0);\n"
    "    vec3 local_rd = normalize(transpose(mat3(u_model)) * refr_g);\n"
    "    vec3 local_light = normalize(transpose(mat3(u_model)) * key_dir);\n"
    "    vec4 bp_result = insides_bp(v_local_pos, local_rd,\n"
    "        face_n, local_light);\n"
    "    vec3 volume_mix = bp_result.rgb;\n"
    "    float volume_mask = bp_result.a;\n"
    "    vec3 body = mix(transmission * 0.16, transmission * absorption, "
    "0.30);\n"
    "    body *= smoke_tint * 0.35;\n"
    "    body += vec3(0.004, 0.006, 0.010) * (0.40 + thickness * 0.18);\n"
    "    body += volume_mix * (0.8 + volume_mask * 1.6);\n"
    "    body += internal_reflection * vec3(0.18, 0.22, 0.30) * 0.02;\n"
    "    body += reflect_env * 0.008;\n"
    "    vec3 col = body;\n"
    "    vec3 light_colors[3] = vec3[3](\n"
    "        vec3(1.00, 0.98, 0.96),\n"
    "        vec3(0.44, 0.58, 0.96),\n"
    "        vec3(0.92, 0.82, 0.88)\n"
    "    );\n"
    "    vec3 light_dirs[3] = vec3[3](key_dir, fill_dir, rim_dir);\n"
    "    float light_scales[3] = float[3](1.78, 0.86, 0.62);\n"
    "    for (int i = 0; i < 3; ++i) {\n"
    "        vec3 l = light_dirs[i];\n"
    "        float ndl = max(dot(n, l), 0.0);\n"
    "        if (ndl > 0.0) {\n"
    "            vec3 h = normalize(view_dir + l);\n"
    "            float ndh = max(dot(n, h), 0.0);\n"
    "            float vdh = max(dot(view_dir, h), 0.0);\n"
    "            vec3 f = fresnel_schlick(vdh, f0);\n"
    "            float d = ggx_d(ndh, rough);\n"
    "            float g = schlick_ggx(ndl, rough) * schlick_ggx(ndv, rough);\n"
    "            vec3 spec = (d * g * f) / max(4.0 * ndl * ndv, 1.0e-4);\n"
    "            col += spec * light_colors[i] * light_scales[i] * ndl;\n"
    "            float coat_d = ggx_d(ndh, coat_rough);\n"
    "            float coat_g = schlick_ggx(ndl, coat_rough) * "
    "schlick_ggx(ndv, coat_rough);\n"
    "            float coat = (coat_d * coat_g) / max(4.0 * ndl * ndv, "
    "1.0e-4);\n"
    "            col += vec3(0.98, 0.99, 1.0) * coat * ndl * light_scales[i] * "
    "0.26;\n"
    "        }\n"
    "    }\n"
    "    float fresnel = pow(1.0 - ndv, 5.0);\n"
    "    float volume_fresnel = pow(1.0 - ndv, 2.2);\n"
    "    float top_sheen = pow(max(dot(refl_dir, normalize(vec3(0.04, 0.95, "
    "0.30))), 0.0), 22.0);\n"
    "    float strip_reflection = exp(-dot((refl_dir.xy - vec2(-0.26, 0.34)) / "
    "vec2(0.24, 0.045), (refl_dir.xy - vec2(-0.26, 0.34)) / vec2(0.24, "
    "0.045)));\n"
    "    float key_reflection = exp(-dot((refl_dir.xy - vec2(0.14, 0.28)) / "
    "vec2(0.10, 0.065), (refl_dir.xy - vec2(0.14, 0.28)) / vec2(0.10, "
    "0.065)));\n"
    "    vec3 rim = vec3(0.34, 0.48, 0.76) * pow(1.0 - ndv, 2.9) * (0.06 + "
    "edge_bevel * 0.11);\n"
    "    col += reflect_env * fresnel * 0.72;\n"
    "    col += env * 0.06;\n"
    "    col += vec3(0.94, 0.97, 1.0) * top_sheen * 0.16;\n"
    "    col += vec3(0.90, 0.94, 1.0) * key_reflection * 0.18;\n"
    "    col += vec3(0.72, 0.84, 1.00) * strip_reflection * 0.08;\n"
    "    col += rim;\n"
    "    col += volume_mix * (0.20 + volume_mask * 0.50 + volume_fresnel * "
    "0.12);\n"
    "    col += vec3(0.08, 0.11, 0.17) * edge_bevel * 0.14;\n"
    "    \n"
    "    vec3 alp = abs(v_local_pos);\n"
    "    vec2 fp;\n"
    "    if (alp.x >= alp.y && alp.x >= alp.z) fp = v_local_pos.yz;\n"
    "    else if (alp.y >= alp.z)               fp = v_local_pos.xz;\n"
    "    else                                    fp = v_local_pos.xy;\n"
    "    float face_mask = (1.0 - smoothstep(0.70, 1.0, abs(fp.x)))\n"
    "                    * (1.0 - smoothstep(0.70, 1.0, abs(fp.y)));\n"
    "    float fl = dot(col, vec3(0.299, 0.587, 0.114));\n"
    "    vec3 frosted = mix(col, vec3(fl), 0.85) * 0.10\n"
    "                 + vec3(0.035, 0.045, 0.075);\n"
    "    col = mix(col, frosted, face_mask * 0.94);\n"
    "    \n"
    "    if (tex.a > 0.0) {\n"
    "        float content_edge = smoothstep(0.01, 0.12, edge_dist);\n"
    "        float inner_occ = mix(0.90, 1.0, content_edge);\n"
    "        float wl = dot(col, vec3(0.299, 0.587, 0.114));\n"
    "        vec3 widget_frost = mix(col, vec3(wl), 0.7) * 0.12\n"
    "                          + vec3(0.02, 0.028, 0.05);\n"
    "        col = mix(col, widget_frost, 0.92 * clamp(tex.a, 0.0, 1.0));\n"
    "        vec3 content = tex.rgb * vec3(0.97, 0.98, 1.00) * inner_occ;\n"
    "        col = mix(col, content, clamp(tex.a, 0.0, 1.0) * 0.98);\n"
    "    }\n"
    "    col = col / (1.0 + col * 0.16);\n"
    "    col = pow(col, vec3(0.90));\n"
    "    float alpha = clamp(0.92 + fresnel * 0.04 + edge_bevel * 0.02, 0.90, "
    "0.98) * u_alpha;\n"
    "    fragColor = vec4(col, alpha);\n"
    "}\n";

static const char *cube_glow_frag_src =
    "#version 330 core\n"
    "in vec2 v_uv;\n"
    "in float v_face_id;\n"
    "in vec3 v_normal;\n"
    "in vec3 v_world_pos;\n"
    "out vec4 fragColor;\n"
    "uniform float u_alpha;\n"
    "void main() {\n"
    "    vec3 n = normalize(v_normal);\n"
    "    vec3 view_dir = normalize(-v_world_pos);\n"
    "    float fresnel = pow(1.0 - max(dot(n, view_dir), 0.0), 3.0);\n"
    "    float height = smoothstep(-0.4, 0.9, n.y * 0.5 + 0.5);\n"
    "    float strength = fresnel * mix(0.05, 0.12, height);\n"
    "    vec3 glow = (v_face_id < -0.5)\n"
    "        ? vec3(0.05, 0.07, 0.12)\n"
    "        : vec3(0.56, 0.66, 0.90);\n"
    "    fragColor = vec4(glow * strength * u_alpha, strength * u_alpha);\n"
    "}\n";

static bool init_cube_shaders(plexy_greeter_t *g) {
  GLuint vs = compile_shader(GL_VERTEX_SHADER, cube_vert_src);
  GLuint fs = compile_shader(GL_FRAGMENT_SHADER, cube_frag_src);
  if (!vs || !fs) {
    glDeleteShader(vs);
    glDeleteShader(fs);
    return false;
  }

  g->cube_program = glCreateProgram();
  glAttachShader(g->cube_program, vs);
  glAttachShader(g->cube_program, fs);
  glBindAttribLocation(g->cube_program, 0, "a_pos");
  glBindAttribLocation(g->cube_program, 1, "a_uv");
  glBindAttribLocation(g->cube_program, 2, "a_face_id");
  glBindAttribLocation(g->cube_program, 3, "a_normal");
  glLinkProgram(g->cube_program);
  glDeleteShader(vs);
  glDeleteShader(fs);

  GLint ok = 0;
  glGetProgramiv(g->cube_program, GL_LINK_STATUS, &ok);
  if (!ok) {
    char log[256];
    glGetProgramInfoLog(g->cube_program, sizeof(log), NULL, log);
    syslog(LOG_ERR, "plexy-dm: cube shader link: %s", log);
    glDeleteProgram(g->cube_program);
    g->cube_program = 0;
    return false;
  }

  GLuint glow_vs = compile_shader(GL_VERTEX_SHADER, cube_vert_src);
  GLuint glow_fs = compile_shader(GL_FRAGMENT_SHADER, cube_glow_frag_src);
  if (!glow_vs || !glow_fs) {
    glDeleteShader(glow_vs);
    glDeleteShader(glow_fs);
    g->cube_glow_program = 0;
    return true;
  }

  g->cube_glow_program = glCreateProgram();
  glAttachShader(g->cube_glow_program, glow_vs);
  glAttachShader(g->cube_glow_program, glow_fs);
  glBindAttribLocation(g->cube_glow_program, 0, "a_pos");
  glBindAttribLocation(g->cube_glow_program, 1, "a_uv");
  glBindAttribLocation(g->cube_glow_program, 2, "a_face_id");
  glBindAttribLocation(g->cube_glow_program, 3, "a_normal");
  glLinkProgram(g->cube_glow_program);
  glDeleteShader(glow_vs);
  glDeleteShader(glow_fs);

  glGetProgramiv(g->cube_glow_program, GL_LINK_STATUS, &ok);
  if (!ok) {
    char log[256];
    glGetProgramInfoLog(g->cube_glow_program, sizeof(log), NULL, log);
    syslog(LOG_WARNING, "plexy-dm: cube glow shader link: %s", log);
    glDeleteProgram(g->cube_glow_program);
    g->cube_glow_program = 0;
  }
  return true;
}

static void init_cube_geometry(plexy_greeter_t *g) {

  static const float verts[24 * 9] = {

      -1, -1, +1, 0, 0, 0,  0,  0,  1,  +1, -1, +1, 1, 0, 0,  0,  0,  1,
      +1, +1, +1, 1, 1, 0,  0,  0,  1,  -1, +1, +1, 0, 1, 0,  0,  0,  1,

      +1, -1, +1, 0, 0, 1,  1,  0,  0,  +1, -1, -1, 1, 0, 1,  1,  0,  0,
      +1, +1, -1, 1, 1, 1,  1,  0,  0,  +1, +1, +1, 0, 1, 1,  1,  0,  0,

      +1, -1, -1, 0, 0, -1, 0,  0,  -1, -1, -1, -1, 1, 0, -1, 0,  0,  -1,
      -1, +1, -1, 1, 1, -1, 0,  0,  -1, +1, +1, -1, 0, 1, -1, 0,  0,  -1,

      -1, -1, -1, 0, 0, -1, -1, 0,  0,  -1, -1, +1, 1, 0, -1, -1, 0,  0,
      -1, +1, +1, 1, 1, -1, -1, 0,  0,  -1, +1, -1, 0, 1, -1, -1, 0,  0,

      -1, +1, +1, 0, 0, -1, 0,  1,  0,  +1, +1, +1, 1, 0, -1, 0,  1,  0,
      +1, +1, -1, 1, 1, -1, 0,  1,  0,  -1, +1, -1, 0, 1, -1, 0,  1,  0,

      -1, -1, -1, 0, 0, -1, 0,  -1, 0,  +1, -1, -1, 1, 0, -1, 0,  -1, 0,
      +1, -1, +1, 1, 1, -1, 0,  -1, 0,  -1, -1, +1, 0, 1, -1, 0,  -1, 0,
  };

  static const unsigned short idx[36] = {
      0,  1,  2,  0,  2,  3,  4,  5,  6,  4,  6,  7,  8,  9,  10, 8,  10, 11,
      12, 13, 14, 12, 14, 15, 16, 17, 18, 16, 18, 19, 20, 21, 22, 20, 22, 23,
  };

  glGenVertexArrays(1, &g->cube_vao);
  glBindVertexArray(g->cube_vao);

  glGenBuffers(1, &g->cube_vbo);
  glBindBuffer(GL_ARRAY_BUFFER, g->cube_vbo);
  glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);

  glGenBuffers(1, &g->cube_ebo);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g->cube_ebo);
  glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(idx), idx, GL_STATIC_DRAW);

  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 36, (void *)0);

  glEnableVertexAttribArray(1);
  glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 36, (void *)12);

  glEnableVertexAttribArray(2);
  glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, 36, (void *)20);

  glEnableVertexAttribArray(3);
  glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, 36, (void *)24);

  glBindVertexArray(0);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
}

static void cleanup_cube(plexy_greeter_t *g) {
  if (g->cube_program)
    glDeleteProgram(g->cube_program);
  if (g->cube_glow_program)
    glDeleteProgram(g->cube_glow_program);
  if (g->cube_vbo)
    glDeleteBuffers(1, &g->cube_vbo);
  if (g->cube_ebo)
    glDeleteBuffers(1, &g->cube_ebo);
  if (g->cube_vao)
    glDeleteVertexArrays(1, &g->cube_vao);
  g->cube_program = 0;
  g->cube_glow_program = 0;
  g->cube_vbo = g->cube_ebo = g->cube_vao = 0;
}

static void cube_anim_tick(plexy_greeter_t *g, double delta_ms) {
  switch (g->cube_anim) {

  case CUBE_IDLE:
    g->cube_scale = 1.0f;
    g->cube_shake_x = 0.0f;

    break;

  case CUBE_TRANSITION: {
    g->cube_anim_timer += delta_ms;
    double t = g->cube_anim_timer / 500.0;
    if (t > 1.0)
      t = 1.0;
    float te = (float)(t * t * (3.0 - 2.0 * t));
    int target_face = (g->cube_active_face == 0) ? 1 : 0;
    float target_y = (target_face == 1) ? -90.0f : 0.0f;
    g->cube_angle_y =
        g->cube_anim_start_y + te * (target_y - g->cube_anim_start_y);
    if (t >= 1.0) {
      g->cube_angle_y = target_y;
      g->cube_anim = CUBE_IDLE;
      g->cube_active_face = target_face;
      g->ui = (g->cube_active_face == 1) ? g->ui_password : g->ui_userselect;
    }
    break;
  }

  case CUBE_SPINOUT: {
    g->cube_anim_timer += delta_ms;
    double t = g->cube_anim_timer / 600.0;
    if (t > 1.0)
      t = 1.0;
    g->cube_angle_y += 720.0f * (float)(delta_ms / 600.0);
    g->cube_scale = 1.0f - (float)t;
    g->cube_shake_x = 0.0f;
    if (t >= 1.0)
      g->cube_anim = CUBE_DONE;
    break;
  }

  case CUBE_SHAKE: {
    g->cube_anim_timer += delta_ms;
    double t = g->cube_anim_timer / 400.0;
    if (t > 1.0)
      t = 1.0;
    float env = (float)(1.0 - t);
    g->cube_shake_x = sinf((float)(t * M_PI * 5.0)) * 0.06f * env;
    if (t >= 1.0) {
      g->cube_shake_x = 0.0f;
      g->cube_anim = CUBE_IDLE;
    }
    break;
  }

  case CUBE_DONE:
    g->cube_fade_alpha += (float)(delta_ms / 400.0);
    if (g->cube_fade_alpha > 1.0f)
      g->cube_fade_alpha = 1.0f;
    break;
  }
}

static void render_cube_glow(plexy_greeter_t *g, const mat4 model,
                             const mat4 mvp, float alpha) {
  if (!g->cube_glow_program || alpha <= 0.0f)
    return;

  glDisable(GL_DEPTH_TEST);
  glDepthMask(GL_FALSE);
  glEnable(GL_CULL_FACE);
  glCullFace(GL_FRONT);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE);

  glUseProgram(g->cube_glow_program);
  glUniformMatrix4fv(glGetUniformLocation(g->cube_glow_program, "u_mvp"), 1,
                     GL_FALSE, mvp);
  glUniformMatrix4fv(glGetUniformLocation(g->cube_glow_program, "u_model"), 1,
                     GL_FALSE, model);
  glUniform1f(glGetUniformLocation(g->cube_glow_program, "u_expand"), 0.03f);
  glUniform1f(glGetUniformLocation(g->cube_glow_program, "u_alpha"),
              alpha * 0.02f);

  glBindVertexArray(g->cube_vao);
  glDrawElements(GL_TRIANGLES, 36, GL_UNSIGNED_SHORT, 0);
  glBindVertexArray(0);

  glUseProgram(0);
  glDepthMask(GL_TRUE);
}

static void render_cube(plexy_greeter_t *g) {
  PlexyCanvas *cv_front = greeter_ui_get_canvas(g->ui_userselect);
  PlexyCanvas *cv_right = greeter_ui_get_canvas(g->ui_password);
  if (!cv_front || !cv_right)
    return;

  GLuint tex0 = plexy_canvas_get_texture(cv_front);
  GLuint tex1 = plexy_canvas_get_texture(cv_right);
  if (!tex0 || !tex1 || !g->scene_texture)
    return;

  float aspect = (float)g->drm.mode.hdisplay / (float)g->drm.mode.vdisplay;

  const float cube_distance = -3.10f;
  const float cube_base_scale = 0.40f;
  const float cube_center_y = 0.0f;

  mat4 proj, rot_y, rot_x, rot, trans, sc, model, mvp;
  mat4_perspective(proj, (float)(45.0 * M_PI / 180.0), aspect, 0.1f, 20.0f);
  mat4_translate(trans, g->cube_shake_x, cube_center_y, cube_distance);
  mat4_rotate_y(rot_y, g->cube_angle_y * (float)(M_PI / 180.0));
  mat4_rotate_x(rot_x, 16.0f * (float)(M_PI / 180.0));
  mat4_scale_uniform(sc, cube_base_scale *
                             (g->cube_scale > 0.0f ? g->cube_scale : 0.0f));

  mat4_mul(rot, rot_x, rot_y);
  mat4_mul(model, rot, sc);
  mat4_mul(model, trans, model);
  mat4_mul(mvp, proj, model);

  float alpha = (g->cube_anim == CUBE_SPINOUT)
                    ? (1.0f - (float)(g->cube_anim_timer / 600.0))
                    : 1.0f;
  if (alpha < 0.0f)
    alpha = 0.0f;

  glClear(GL_DEPTH_BUFFER_BIT);

  glEnable(GL_DEPTH_TEST);
  glDepthFunc(GL_LESS);
  glEnable(GL_CULL_FACE);
  glCullFace(GL_BACK);
  glFrontFace(GL_CCW);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

  glUseProgram(g->cube_program);

  glUniformMatrix4fv(glGetUniformLocation(g->cube_program, "u_mvp"), 1,
                     GL_FALSE, mvp);
  glUniformMatrix4fv(glGetUniformLocation(g->cube_program, "u_model"), 1,
                     GL_FALSE, model);
  glUniformMatrix4fv(glGetUniformLocation(g->cube_program, "u_proj"), 1,
                     GL_FALSE, proj);
  glUniform1i(glGetUniformLocation(g->cube_program, "u_tex_front"), 0);
  glUniform1i(glGetUniformLocation(g->cube_program, "u_tex_right"), 1);
  glUniform1i(glGetUniformLocation(g->cube_program, "u_scene_tex"), 2);
  glUniform3f(glGetUniformLocation(g->cube_program, "u_light_dir"), 0.06f,
              0.78f, 0.62f);
  glUniform2f(glGetUniformLocation(g->cube_program, "u_scene_texel"),
              g->scene_width > 0 ? 1.0f / (float)g->scene_width : 0.0f,
              g->scene_height > 0 ? 1.0f / (float)g->scene_height : 0.0f);
  glUniform1f(glGetUniformLocation(g->cube_program, "u_time"), g->scene_time_s);
  glUniform1f(glGetUniformLocation(g->cube_program, "u_expand"), 0.0f);
  glUniform1f(glGetUniformLocation(g->cube_program, "u_alpha"), alpha);

  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, tex0);
  glActiveTexture(GL_TEXTURE1);
  glBindTexture(GL_TEXTURE_2D, tex1);
  glActiveTexture(GL_TEXTURE2);
  glBindTexture(GL_TEXTURE_2D, g->scene_texture);

  glBindVertexArray(g->cube_vao);
  glDrawElements(GL_TRIANGLES, 36, GL_UNSIGNED_SHORT, 0);
  glBindVertexArray(0);

  glUseProgram(0);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_CULL_FACE);
  glDisable(GL_BLEND);
}

static void render_fade_overlay(plexy_greeter_t *g) {
  float a = g->cube_fade_alpha;
  if (a <= 0.0f)
    return;

  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

  glUseProgram(g->ui_blit_program);

  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

  glUseProgram(0);

  static GLuint fade_prog = 0;
  static GLuint fade_vao = 0;
  if (!fade_prog) {
    const char *vs = "#version 330 core\n"
                     "in vec2 a_pos;\n"
                     "void main() { gl_Position = vec4(a_pos, 0.0, 1.0); }\n";
    const char *fs =
        "#version 330 core\n"
        "out vec4 fragColor;\n"
        "uniform float u_alpha;\n"
        "void main() { fragColor = vec4(0.0, 0.0, 0.0, u_alpha); }\n";
    GLuint sv = compile_shader(GL_VERTEX_SHADER, vs);
    GLuint sf = compile_shader(GL_FRAGMENT_SHADER, fs);
    fade_prog = glCreateProgram();
    glAttachShader(fade_prog, sv);
    glAttachShader(fade_prog, sf);
    glBindAttribLocation(fade_prog, 0, "a_pos");
    glLinkProgram(fade_prog);
    glDeleteShader(sv);
    glDeleteShader(sf);

    float q[] = {-1, -1, 1, -1, -1, 1, 1, 1};
    GLuint vbo;
    glGenVertexArrays(1, &fade_vao);
    glBindVertexArray(fade_vao);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(q), q, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 8, 0);
    glBindVertexArray(0);
  }

  glUseProgram(fade_prog);
  glUniform1f(glGetUniformLocation(fade_prog, "u_alpha"), a);
  glBindVertexArray(fade_vao);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  glBindVertexArray(0);
  glUseProgram(0);
  glDisable(GL_BLEND);
}

static void sync_password_face_selection(plexy_greeter_t *g) {
  if (!g || !g->ui_userselect || !g->ui_password)
    return;

  const char *username = greeter_ui_get_selected_user(g->ui_userselect);
  if (!username)
    return;

  for (int i = 0; i < g->user_count; i++) {
    if (strcmp(g->users[i].username, username) != 0)
      continue;

    greeter_ui_select_user(g->ui_password, i);
    greeter_ui_set_selected_username(
        g->ui_password,
        g->users[i].realname[0] ? g->users[i].realname : g->users[i].username);
    return;
  }
}

static void start_2d_dialog_transition(plexy_greeter_t *g,
                                       greeter_ui_ctx_t *from,
                                       greeter_ui_ctx_t *to, int direction) {
  if (!g || !from || !to || from == to)
    return;

  g->ui_transition_from = from;
  g->ui_transition_to = to;
  g->ui_transition_timer = 0.0;
  g->ui_transition_dir = direction < 0 ? -1 : 1;
  g->ui = to;
}

static const char *resolve_wallpaper(const plexy_dm_config_t *cfg, char *buf,
                                     size_t bufsz) {

  if (cfg->background_path[0] && access(cfg->background_path, R_OK) == 0)
    return cfg->background_path;

  const char *roots[] = {cfg->runtime_root, "/opt/plexydesk/current", "/usr",
                         NULL};
  const char *names[] = {"background/wallpaper.jpeg",
                         "background/wallpaper-5.jpeg",
                         "background/background-debian-dark.png", NULL};

  for (const char **r = roots; *r; r++) {
    if (!(*r)[0])
      continue;
    for (const char **n = names; *n; n++) {
      snprintf(buf, bufsz, "%s/share/plexydesk/%s", *r, *n);
      if (access(buf, R_OK) == 0)
        return buf;
    }
  }

  syslog(LOG_WARNING, "plexy-dm: no wallpaper found, using gradient");
  return NULL;
}

static const char *find_cursor_theme_path(char *out, size_t out_sz) {
  const char *theme = getenv("XCURSOR_THEME");
  if (!theme)
    theme = "Adwaita";

  const char *home = getenv("HOME");
  if (home) {
    snprintf(out, out_sz, "%s/.icons/%s/cursors", home, theme);
    if (access(out, F_OK) == 0)
      return out;
    snprintf(out, out_sz, "%s/.local/share/icons/%s/cursors", home, theme);
    if (access(out, F_OK) == 0)
      return out;
  }
  snprintf(out, out_sz, "/usr/share/icons/%s/cursors", theme);
  if (access(out, F_OK) == 0)
    return out;
  snprintf(out, out_sz, "/usr/local/share/icons/%s/cursors", theme);
  if (access(out, F_OK) == 0)
    return out;

  snprintf(out, out_sz, "/usr/share/icons/Adwaita/cursors");
  return out;
}

static void greeter_init_hw_cursor(plexy_greeter_t *g) {
  drm_state_t *drm = &g->drm;

  uint64_t cw = 0, ch = 0;
  if (drmGetCap(drm->drm_fd, DRM_CAP_CURSOR_WIDTH, &cw) != 0)
    cw = 64;
  if (drmGetCap(drm->drm_fd, DRM_CAP_CURSOR_HEIGHT, &ch) != 0)
    ch = 64;

  drm->hw_cursor_width = (uint32_t)cw;
  drm->hw_cursor_height = (uint32_t)ch;

  char theme_path[512];
  find_cursor_theme_path(theme_path, sizeof(theme_path));

  char cursor_path[640];
  snprintf(cursor_path, sizeof(cursor_path), "%s/left_ptr", theme_path);

  XcursorImages *images = XcursorFilenameLoadImages(cursor_path, 24);
  if (!images || images->nimage == 0) {
    syslog(LOG_WARNING, "plexy-dm: failed to load cursor from %s", cursor_path);
    if (images)
      XcursorImagesDestroy(images);
    return;
  }

  XcursorImage *img = images->images[0];
  drm->hw_cursor_hotspot_x = (int)img->xhot;
  drm->hw_cursor_hotspot_y = (int)img->yhot;

  drm->hw_cursor_bo =
      gbm_bo_create(drm->gbm_dev, drm->hw_cursor_width, drm->hw_cursor_height,
                    GBM_FORMAT_ARGB8888, GBM_BO_USE_CURSOR | GBM_BO_USE_WRITE);
  if (!drm->hw_cursor_bo) {
    syslog(LOG_WARNING, "plexy-dm: hardware cursor BO creation failed");
    XcursorImagesDestroy(images);
    return;
  }

  size_t buf_pixels = drm->hw_cursor_width * drm->hw_cursor_height;
  uint32_t *buf = calloc(buf_pixels, sizeof(uint32_t));
  if (!buf) {
    gbm_bo_destroy(drm->hw_cursor_bo);
    drm->hw_cursor_bo = NULL;
    XcursorImagesDestroy(images);
    return;
  }

  uint32_t copy_w =
      img->width < drm->hw_cursor_width ? img->width : drm->hw_cursor_width;
  uint32_t copy_h =
      img->height < drm->hw_cursor_height ? img->height : drm->hw_cursor_height;
  for (uint32_t row = 0; row < copy_h; row++) {
    memcpy(&buf[row * drm->hw_cursor_width], &img->pixels[row * img->width],
           copy_w * sizeof(uint32_t));
  }
  XcursorImagesDestroy(images);

  if (gbm_bo_write(drm->hw_cursor_bo, buf, buf_pixels * sizeof(uint32_t)) !=
      0) {
    syslog(LOG_WARNING, "plexy-dm: gbm_bo_write for cursor failed");
    free(buf);
    gbm_bo_destroy(drm->hw_cursor_bo);
    drm->hw_cursor_bo = NULL;
    return;
  }
  free(buf);

  uint32_t bo_handle = gbm_bo_get_handle(drm->hw_cursor_bo).u32;
  if (drmModeSetCursor2(drm->drm_fd, drm->crtc_id, bo_handle,
                        drm->hw_cursor_width, drm->hw_cursor_height,
                        drm->hw_cursor_hotspot_x,
                        drm->hw_cursor_hotspot_y) != 0) {
    syslog(
        LOG_WARNING,
        "plexy-dm: drmModeSetCursor2 failed: %s; keeping cursor BO for retry",
        strerror(errno));
    drm->hw_cursor_supported = true;
    return;
  }

  drm->hw_cursor_supported = true;

  for (int i = 0; i < drm->mirror_count; i++) {
    drmModeSetCursor2(drm->drm_fd, drm->mirrors[i].crtc_id, bo_handle,
                      drm->hw_cursor_width, drm->hw_cursor_height,
                      drm->hw_cursor_hotspot_x, drm->hw_cursor_hotspot_y);
  }

  syslog(LOG_INFO,
         "plexy-dm: hardware cursor enabled (%ux%u, hotspot %d,%d) from %s",
         drm->hw_cursor_width, drm->hw_cursor_height, drm->hw_cursor_hotspot_x,
         drm->hw_cursor_hotspot_y, theme_path);
}

plexy_greeter_t *greeter_create(int vt, const plexy_dm_config_t *cfg,
                                const greeter_callbacks_t *cbs) {
  plexy_greeter_t *g = calloc(1, sizeof(*g));
  if (!g)
    return NULL;

  g->vt = vt;
  g->cfg = cfg;
  g->use_3d_theme = cfg ? cfg->use_3d_theme : false;
  if (cbs)
    g->cbs = *cbs;
  g->state = GREETER_STATE_USER_SELECT;

  if (!init_drm(&g->drm)) {
    free(g);
    return NULL;
  }

  if (!init_gbm_egl(&g->drm)) {
    close(g->drm.drm_fd);
    free(g);
    return NULL;
  }

  greeter_init_hw_cursor(g);

  if (!init_input(&g->input)) {
    syslog(LOG_WARNING, "plexy-dm: input init failed, continuing");
  }

  eglMakeCurrent(g->drm.egl_display, g->drm.egl_surface, g->drm.egl_surface,
                 g->drm.egl_context);

  float scale = detect_drm_ui_scale(&g->drm);

  int face_size = g->drm.mode.hdisplay < g->drm.mode.vdisplay
                      ? g->drm.mode.hdisplay
                      : g->drm.mode.vdisplay;
  int ui_width = g->use_3d_theme ? face_size : g->drm.mode.hdisplay;
  int ui_height = g->use_3d_theme ? face_size : g->drm.mode.vdisplay;

  g->ui_userselect = greeter_ui_create(ui_width, ui_height, scale);
  if (!g->ui_userselect) {
    syslog(LOG_ERR, "plexy-dm: failed to create user-select UI");
    shutdown_input(&g->input);
    close(g->drm.drm_fd);
    free(g);
    return NULL;
  }

  g->ui_password = greeter_ui_create_password(ui_width, ui_height, scale);
  if (!g->ui_password) {
    syslog(LOG_ERR, "plexy-dm: failed to create password UI");
    greeter_ui_destroy(g->ui_userselect);
    shutdown_input(&g->input);
    close(g->drm.drm_fd);
    free(g);
    return NULL;
  }

  g->ui = g->ui_userselect;

  greeter_ui_set_clock_24h(g->ui_userselect, cfg->clock_24h);
  greeter_ui_set_clock_24h(g->ui_password, cfg->clock_24h);

  char wp_buf[512];
  const char *wp_path = NULL;
  if (cfg->video_path[0] && access(cfg->video_path, R_OK) == 0) {
    wp_path = cfg->video_path;
  } else {
    wp_path = resolve_wallpaper(cfg, wp_buf, sizeof(wp_buf));
  }
  init_wallpaper(g, g->use_3d_theme ? NULL : wp_path);
  if (!init_scene_buffer(g))
    syslog(LOG_WARNING,
           "plexy-dm: scene buffer init failed; glass refraction disabled");
  if (!g->use_3d_theme && g->scene_texture) {
    PlexyCanvas *user_canvas = greeter_ui_get_canvas(g->ui_userselect);
    PlexyCanvas *pass_canvas = greeter_ui_get_canvas(g->ui_password);
    if (user_canvas)
      plexy_canvas_set_blur_texture(user_canvas, g->scene_texture);
    if (pass_canvas)
      plexy_canvas_set_blur_texture(pass_canvas, g->scene_texture);
  }

  init_ui_blit(g);
  if (g->use_3d_theme)
    init_floor_fx(g);

  if (g->use_3d_theme) {
    if (!init_cube_shaders(g))
      syslog(LOG_WARNING,
             "plexy-dm: cube shaders failed — login cube disabled");
    else
      init_cube_geometry(g);

    init_msaa_fbo(g);
  }

  g->timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);

  g->cube_anim = g->use_3d_theme ? CUBE_IDLE : CUBE_DONE;
  g->cube_angle_y = 0.0f;
  g->cube_scale = 1.0f;
  g->cube_active_face = 0;
  g->scene_time_s = 0.0f;
  clock_gettime(CLOCK_MONOTONIC, &g->last_frame_time);
  init_weather(g);

  g->user_count = user_list_enumerate(g->users, PLEXY_DM_MAX_USERS,
                                      cfg->min_uid, cfg->max_uid);
  greeter_ui_set_users(g->ui_userselect, g->users, g->user_count);
  greeter_ui_set_users(g->ui_password, g->users, g->user_count);

  sync_password_face_selection(g);

  if (!wifi_is_connected()) {
    greeter_ui_set_wifi_mode(g->ui_userselect, true);
    greeter_ui_set_wifi_networks(g->ui_userselect, NULL, 0);
    greeter_ui_set_status(g->ui_userselect, "Scanning for networks…");
    g->wifi_scanning = true;
    wifi_scan_start_async();
    g->state = GREETER_STATE_WIFI;
    greeter_ui_set_state(g->ui_userselect, GREETER_STATE_WIFI);
    greeter_ui_set_state(g->ui_password, GREETER_STATE_WIFI);
    syslog(LOG_INFO,
           "plexy-dm: no connectivity — showing WiFi screen (scanning)");
  }

  syslog(LOG_INFO, "plexy-dm: greeter created (%dx%d, %d users)",
         g->drm.mode.hdisplay, g->drm.mode.vdisplay, g->user_count);

  return g;
}

void greeter_destroy(plexy_greeter_t *g) {
  if (!g)
    return;

  if (g->timer_fd >= 0)
    close(g->timer_fd);

  shutdown_weather(g);

  greeter_ui_destroy(g->ui_userselect);
  greeter_ui_destroy(g->ui_password);
  g->ui = NULL;
  cleanup_wallpaper(g);
  cleanup_scene_buffer(g);
  cleanup_msaa_fbo(g);
  cleanup_ui_blit(g);
  cleanup_floor_fx(g);
  cleanup_cube(g);
  shutdown_input(&g->input);

  if (g->drm.egl_display != EGL_NO_DISPLAY) {
    eglMakeCurrent(g->drm.egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                   EGL_NO_CONTEXT);
    if (g->drm.egl_surface != EGL_NO_SURFACE)
      eglDestroySurface(g->drm.egl_display, g->drm.egl_surface);
    if (g->drm.egl_context != EGL_NO_CONTEXT)
      eglDestroyContext(g->drm.egl_display, g->drm.egl_context);
    eglTerminate(g->drm.egl_display);
  }

  if (g->drm.hw_cursor_supported) {
    drmModeSetCursor(g->drm.drm_fd, g->drm.crtc_id, 0, 0, 0);
    for (int i = 0; i < g->drm.mirror_count; i++)
      drmModeSetCursor(g->drm.drm_fd, g->drm.mirrors[i].crtc_id, 0, 0, 0);
  }
  if (g->drm.hw_cursor_bo)
    gbm_bo_destroy(g->drm.hw_cursor_bo);

  if (g->drm.gbm_surface)
    gbm_surface_destroy(g->drm.gbm_surface);
  if (g->drm.gbm_dev)
    gbm_device_destroy(g->drm.gbm_dev);
  if (g->drm.drm_fd >= 0)
    close(g->drm.drm_fd);

  free(g);
}

void greeter_set_users(plexy_greeter_t *g, const plexy_dm_user_t *users,
                       int count) {
  if (!g)
    return;

  int n = count < PLEXY_DM_MAX_USERS ? count : PLEXY_DM_MAX_USERS;
  memcpy(g->users, users, (size_t)n * sizeof(plexy_dm_user_t));
  g->user_count = n;
  greeter_ui_set_users(g->ui_userselect, g->users, g->user_count);
  greeter_ui_set_users(g->ui_password, g->users, g->user_count);
  sync_password_face_selection(g);
}

void greeter_set_sessions(plexy_greeter_t *g,
                          const plexy_dm_session_t *sessions, int count) {

  (void)g;
  (void)sessions;
  (void)count;
}

void greeter_set_state(plexy_greeter_t *g, greeter_state_t state) {
  if (!g)
    return;
  greeter_state_t prev = g->state;
  g->state = state;

  greeter_ui_set_state(g->ui_userselect, state);
  greeter_ui_set_state(g->ui_password, state);

  if (g->use_3d_theme) {

    if (state == GREETER_STATE_PASSWORD && prev == GREETER_STATE_USER_SELECT &&
        g->cube_active_face == 0) {

      g->cube_anim = CUBE_TRANSITION;
      g->cube_anim_start_y = g->cube_angle_y;
      g->cube_anim_timer = 0;
      greeter_ui_set_error(g->ui_userselect, NULL);
      sync_password_face_selection(g);

    } else if (state == GREETER_STATE_USER_SELECT &&
               prev == GREETER_STATE_PASSWORD && g->cube_active_face == 1) {

      g->cube_anim = CUBE_TRANSITION;
      g->cube_anim_start_y = g->cube_angle_y;
      g->cube_anim_timer = 0;

    } else if (state == GREETER_STATE_SWITCHING) {
      g->cube_anim = CUBE_SPINOUT;
      g->cube_anim_timer = 0;
      g->cube_fade_alpha = 0.0f;

    } else if (state == GREETER_STATE_AUTH_FAILED) {
      g->cube_anim = CUBE_SHAKE;
      g->cube_anim_timer = 0;
    }
  } else {
    if (state == GREETER_STATE_PASSWORD) {
      greeter_ui_set_error(g->ui_userselect, NULL);
      sync_password_face_selection(g);
      if (prev == GREETER_STATE_USER_SELECT)
        start_2d_dialog_transition(g, g->ui_userselect, g->ui_password, 1);
      else
        g->ui = g->ui_password;
    } else if (state == GREETER_STATE_LOCKED ||
               state == GREETER_STATE_AUTH_FAILED) {
      g->ui_transition_from = NULL;
      g->ui_transition_to = NULL;
      g->ui_transition_dir = 0;
      g->ui = g->ui_password;
    } else if (state == GREETER_STATE_USER_SELECT) {
      if (prev == GREETER_STATE_PASSWORD)
        start_2d_dialog_transition(g, g->ui_password, g->ui_userselect, -1);
      else
        g->ui = g->ui_userselect;
    } else if (state == GREETER_STATE_SWITCHING) {
      g->ui_transition_from = NULL;
      g->ui_transition_to = NULL;
      g->ui_transition_dir = 0;
      g->cube_fade_alpha = 0.0f;
    }
  }

  greeter_request_frame(g);
}

void greeter_enter_lock(plexy_greeter_t *g, const char *username) {
  if (!g)
    return;
  snprintf(g->lock_username, sizeof(g->lock_username), "%s", username);

  for (int i = 0; i < g->user_count; i++) {
    if (strcmp(g->users[i].username, username) == 0) {
      greeter_ui_select_user(g->ui_userselect, i);
      greeter_ui_select_user(g->ui_password, i);
      sync_password_face_selection(g);
      break;
    }
  }

  greeter_set_state(g, GREETER_STATE_LOCKED);
}

void greeter_show_error(plexy_greeter_t *g, const char *message) {
  if (!g)
    return;

  if (g->state == GREETER_STATE_LOCKED) {
    greeter_ui_set_error(g->ui_password, message);
    greeter_set_state(g, GREETER_STATE_AUTH_FAILED);
    return;
  }

  greeter_ui_clear_password(g->ui_password);
  greeter_ui_set_error(g->ui_password, message);
  greeter_ui_set_error(g->ui_userselect, message);
  greeter_set_state(g, GREETER_STATE_USER_SELECT);
}

int greeter_get_drm_fd(const plexy_greeter_t *g) {
  return g ? g->drm.drm_fd : -1;
}

int greeter_get_input_fd(const plexy_greeter_t *g) {
  return (g && g->input.li) ? libinput_get_fd(g->input.li) : -1;
}

int greeter_get_timer_fd(const plexy_greeter_t *g) {
  return g ? g->timer_fd : -1;
}

void greeter_handle_drm_event(plexy_greeter_t *g) {
  if (!g)
    return;

  drmEventContext ev_ctx = {
      .version = 2,
      .page_flip_handler = page_flip_handler,
  };
  drmHandleEvent(g->drm.drm_fd, &ev_ctx);

  if (!g->drm.flip_pending && g->frame_pending) {
    g->frame_pending = false;
    greeter_render_frame(g);
  }
}

void greeter_drain_input(plexy_greeter_t *g) {
  if (!g || !g->input.li)
    return;
  libinput_dispatch(g->input.li);
  struct libinput_event *event;
  while ((event = libinput_get_event(g->input.li)) != NULL)
    libinput_event_destroy(event);
}

void greeter_handle_input(plexy_greeter_t *g) {
  if (!g || !g->input.li)
    return;

  libinput_dispatch(g->input.li);
  struct libinput_event *event;
  bool needs_redraw = false;

  while ((event = libinput_get_event(g->input.li)) != NULL) {
    enum libinput_event_type type = libinput_event_get_type(event);

    switch (type) {
    case LIBINPUT_EVENT_DEVICE_ADDED: {
      struct libinput_device *device = libinput_event_get_device(event);
      if (libinput_device_config_tap_get_finger_count(device) > 0) {
        libinput_device_config_tap_set_enabled(device,
                                               LIBINPUT_CONFIG_TAP_ENABLED);
        libinput_device_config_tap_set_drag_enabled(
            device, LIBINPUT_CONFIG_DRAG_ENABLED);
        libinput_device_config_tap_set_drag_lock_enabled(
            device, LIBINPUT_CONFIG_DRAG_LOCK_ENABLED);
      }
      if (libinput_device_config_dwt_is_available(device)) {
        libinput_device_config_dwt_set_enabled(device,
                                               LIBINPUT_CONFIG_DWT_ENABLED);
      }
      break;
    }
    case LIBINPUT_EVENT_KEYBOARD_KEY: {
      struct libinput_event_keyboard *kb =
          libinput_event_get_keyboard_event(event);
      uint32_t key = libinput_event_keyboard_get_key(kb);
      enum libinput_key_state state = libinput_event_keyboard_get_key_state(kb);

      if (state == LIBINPUT_KEY_STATE_PRESSED) {
        greeter_ui_input_t ui_input = {0};

        if (g->input.xkb_state) {
          xkb_state_update_key(g->input.xkb_state, key + 8, XKB_KEY_DOWN);

          xkb_mod_index_t caps_idx =
              xkb_keymap_mod_get_index(g->input.xkb_keymap, XKB_MOD_NAME_CAPS);
          if (caps_idx != XKB_MOD_INVALID) {
            bool caps = xkb_state_mod_index_is_active(
                g->input.xkb_state, caps_idx, XKB_STATE_MODS_EFFECTIVE);
            greeter_ui_set_caps_lock(g->ui_password, caps);
          }
        }

        switch (key) {
        case KEY_BACKSPACE:
          ui_input.type = UI_KEY_BACKSPACE;
          break;
        case KEY_ENTER:
        case KEY_KPENTER:
          ui_input.type = UI_KEY_ENTER;
          break;
        case KEY_ESC:
          ui_input.type = UI_KEY_ESCAPE;
          break;
        case KEY_TAB:
          ui_input.type = UI_KEY_TAB;
          break;
        case KEY_LEFT:
          ui_input.type = UI_KEY_LEFT;
          break;
        case KEY_RIGHT:
          ui_input.type = UI_KEY_RIGHT;
          break;
        case KEY_UP:
          ui_input.type = UI_KEY_UP;
          break;
        case KEY_DOWN:
          ui_input.type = UI_KEY_DOWN;
          break;
        case KEY_DELETE:
          ui_input.type = UI_KEY_DELETE;
          break;
        case KEY_F1:
          ui_input.type = UI_KEY_F1;
          break;
        case KEY_F2:
          g->debug_term_requested = true;
          break;
        default: {

          if (g->input.xkb_state) {
            uint32_t cp = xkb_state_key_get_utf32(g->input.xkb_state, key + 8);
            if (cp >= 32 && cp != 127) {
              ui_input.type = UI_KEY_CHAR;
              ui_input.codepoint = cp;
            }
          }
          break;
        }
        }

        if (ui_input.type != UI_KEY_NONE) {
          if (g->use_3d_theme && (g->cube_anim == CUBE_TRANSITION ||
                                  g->cube_anim == CUBE_SPINOUT)) {
            break;
          }

          if (ui_input.type == UI_KEY_ESCAPE &&
              g->state == GREETER_STATE_PASSWORD) {
            greeter_ui_clear_password(g->ui_password);
            greeter_set_state(g, GREETER_STATE_USER_SELECT);
            needs_redraw = true;
            break;
          }

          bool changed = greeter_ui_handle_key(g->ui, &ui_input);
          needs_redraw = needs_redraw || changed;

          if (changed && g->ui == g->ui_userselect)
            sync_password_face_selection(g);

          if (g->state == GREETER_STATE_WIFI ||
              g->state == GREETER_STATE_WIFI_PASSWORD) {

            if (greeter_ui_wifi_skip_requested(g->ui_userselect)) {

              greeter_ui_set_wifi_mode(g->ui_userselect, false);
              greeter_ui_set_users(g->ui_userselect, g->users, g->user_count);
              g->state = GREETER_STATE_USER_SELECT;
              greeter_ui_set_state(g->ui_userselect, GREETER_STATE_USER_SELECT);
              greeter_ui_set_state(g->ui_password, GREETER_STATE_USER_SELECT);
              needs_redraw = true;

            } else if (greeter_ui_wifi_connect_requested(g->ui_userselect)) {
              int idx = greeter_ui_get_selected_wifi(g->ui_userselect);
              if (idx >= 0 && idx < g->wifi_count) {
                plexy_dm_wifi_ap_t *ap = &g->wifi_aps[idx];
                if (ap->secured) {

                  snprintf(g->wifi_pending_ssid, sizeof(g->wifi_pending_ssid),
                           "%s", ap->ssid);
                  greeter_ui_set_selected_username(g->ui_password, ap->ssid);
                  greeter_ui_clear_password(g->ui_password);
                  g->state = GREETER_STATE_WIFI_PASSWORD;
                  if (!g->use_3d_theme)
                    start_2d_dialog_transition(g, g->ui_userselect,
                                               g->ui_password, 1);
                  else
                    g->ui = g->ui_password;
                } else {

                  if (wifi_connect(ap->ssid, NULL) == 0) {
                    greeter_ui_set_status(g->ui_userselect, NULL);
                    greeter_ui_set_wifi_mode(g->ui_userselect, false);
                    greeter_ui_set_users(g->ui_userselect, g->users,
                                         g->user_count);
                    g->state = GREETER_STATE_USER_SELECT;
                    greeter_ui_set_state(g->ui_userselect,
                                         GREETER_STATE_USER_SELECT);
                    greeter_ui_set_state(g->ui_password,
                                         GREETER_STATE_USER_SELECT);
                  } else {
                    greeter_ui_set_status(
                        g->ui_userselect,
                        "Connection failed — try again or Tab to skip");
                    g->state = GREETER_STATE_WIFI;
                    greeter_ui_set_state(g->ui_userselect, GREETER_STATE_WIFI);
                    greeter_ui_set_state(g->ui_password, GREETER_STATE_WIFI);
                  }
                }
                needs_redraw = true;
              }
            }
          }

          if (g->state == GREETER_STATE_WIFI_PASSWORD &&
              ui_input.type == UI_KEY_ENTER) {
            const char *pass = greeter_ui_get_password(g->ui_password);
            if (pass && pass[0]) {
              if (wifi_connect(g->wifi_pending_ssid, pass) == 0) {
                greeter_ui_clear_password(g->ui_password);
                greeter_ui_set_error(g->ui_password, NULL);
                greeter_ui_set_status(g->ui_userselect, NULL);
                greeter_ui_set_wifi_mode(g->ui_userselect, false);
                greeter_ui_set_users(g->ui_userselect, g->users, g->user_count);
                g->state = GREETER_STATE_USER_SELECT;
                if (!g->use_3d_theme)
                  start_2d_dialog_transition(g, g->ui_password,
                                             g->ui_userselect, -1);
                else
                  g->ui = g->ui_userselect;
                greeter_ui_set_state(g->ui_userselect,
                                     GREETER_STATE_USER_SELECT);
                greeter_ui_set_state(g->ui_password, GREETER_STATE_USER_SELECT);
              } else {
                greeter_ui_set_error(g->ui_password, "Wi-Fi connection failed");
                g->state = GREETER_STATE_WIFI_PASSWORD;
                greeter_ui_set_state(g->ui_password,
                                     GREETER_STATE_WIFI_PASSWORD);
              }
              needs_redraw = true;
            }
          }

          if (g->state == GREETER_STATE_WIFI_PASSWORD &&
              ui_input.type == UI_KEY_ESCAPE) {
            greeter_ui_clear_password(g->ui_password);
            g->state = GREETER_STATE_WIFI;
            if (!g->use_3d_theme)
              start_2d_dialog_transition(g, g->ui_password, g->ui_userselect,
                                         -1);
            else
              g->ui = g->ui_userselect;
            needs_redraw = true;
          }

          if (ui_input.type == UI_KEY_ENTER && g->state != GREETER_STATE_WIFI &&
              g->state != GREETER_STATE_WIFI_PASSWORD) {

            const char *username =
                greeter_ui_get_selected_user(g->ui_userselect);

            const char *password = greeter_ui_get_password(g->ui_password);

            syslog(LOG_WARNING, "plexy-dm: login attempt user='%s' state=%d",
                   username ? username : "(null)", g->state);

            if (username && password && password[0]) {
              if (g->state == GREETER_STATE_LOCKED) {
                if (g->cbs.on_unlock)
                  g->cbs.on_unlock(username, password, g->cbs.user_data);
                greeter_ui_clear_password(g->ui_password);
              } else {

                greeter_ui_set_authenticating(g->ui_password, true);
                snprintf(g->deferred_username, sizeof(g->deferred_username),
                         "%s", username);
                snprintf(g->deferred_password, sizeof(g->deferred_password),
                         "%s", password);
                g->deferred_login = true;
                needs_redraw = true;
              }
            } else if (username && g->state == GREETER_STATE_USER_SELECT) {

              greeter_set_state(g, GREETER_STATE_PASSWORD);
            }
          }

          greeter_ui_power_result_t pwr =
              greeter_ui_get_power_result(g->ui_userselect);
          if (pwr == POWER_RESULT_NONE)
            pwr = greeter_ui_get_power_result(g->ui_password);
          if (pwr != POWER_RESULT_NONE && g->cbs.on_power_action) {
            power_action_t action;
            switch (pwr) {
            case POWER_RESULT_SHUTDOWN:
              action = POWER_ACTION_SHUTDOWN;
              break;
            case POWER_RESULT_REBOOT:
              action = POWER_ACTION_REBOOT;
              break;
            case POWER_RESULT_SUSPEND:
              action = POWER_ACTION_SUSPEND;
              break;
            default:
              action = POWER_ACTION_SHUTDOWN;
              break;
            }
            g->cbs.on_power_action(action, g->cbs.user_data);
          }
        }
      } else {

        if (g->input.xkb_state) {
          xkb_state_update_key(g->input.xkb_state, key + 8, XKB_KEY_UP);
        }
      }
      break;
    }

    case LIBINPUT_EVENT_POINTER_MOTION: {
      struct libinput_event_pointer *ptr =
          libinput_event_get_pointer_event(event);
      g->input.mouse_x += libinput_event_pointer_get_dx(ptr);
      g->input.mouse_y += libinput_event_pointer_get_dy(ptr);

      if (g->input.mouse_x < 0)
        g->input.mouse_x = 0;
      if (g->input.mouse_y < 0)
        g->input.mouse_y = 0;
      if (g->input.mouse_x >= g->drm.mode.hdisplay)
        g->input.mouse_x = g->drm.mode.hdisplay - 1;
      if (g->input.mouse_y >= g->drm.mode.vdisplay)
        g->input.mouse_y = g->drm.mode.vdisplay - 1;

      if (g->drm.hw_cursor_supported) {
        drmModeMoveCursor(g->drm.drm_fd, g->drm.crtc_id, (int)g->input.mouse_x,
                          (int)g->input.mouse_y);
        for (int i = 0; i < g->drm.mirror_count; i++)
          drmModeMoveCursor(g->drm.drm_fd, g->drm.mirrors[i].crtc_id,
                            (int)g->input.mouse_x, (int)g->input.mouse_y);
      }
      needs_redraw = true;
      break;
    }

    case LIBINPUT_EVENT_POINTER_BUTTON: {
      struct libinput_event_pointer *ptr =
          libinput_event_get_pointer_event(event);
      bool pressed = libinput_event_pointer_get_button_state(ptr) ==
                     LIBINPUT_BUTTON_STATE_PRESSED;
      greeter_ui_handle_pointer(g->ui, (int)g->input.mouse_x,
                                (int)g->input.mouse_y, pressed);

      if (pressed && g->state == GREETER_STATE_USER_SELECT) {
        int clicked_user = greeter_ui_user_click_requested(g->ui_userselect);
        if (clicked_user >= 0 && clicked_user < g->user_count) {
          greeter_ui_select_user(g->ui_userselect, clicked_user);
          sync_password_face_selection(g);
          greeter_set_state(g, GREETER_STATE_PASSWORD);
          needs_redraw = true;
        }
      }
      needs_redraw = true;
      break;
    }

    default:
      break;
    }

    libinput_event_destroy(event);
  }

  if (needs_redraw)
    greeter_request_frame(g);
}

void greeter_handle_timer(plexy_greeter_t *g) {
  if (!g || g->timer_fd < 0)
    return;

  uint64_t expirations;
  read(g->timer_fd, &expirations, sizeof(expirations));

  if (g->wifi_scanning && g->state == GREETER_STATE_WIFI) {
    int count = 0;
    wifi_scan_state_t st =
        wifi_scan_poll(g->wifi_aps, PLEXY_DM_MAX_WIFI_APS, &count);
    if (st == WIFI_SCAN_DONE) {
      g->wifi_scanning = false;
      g->wifi_count = count;
      greeter_ui_set_status(g->ui_userselect, NULL);
      if (count > 0) {
        greeter_ui_set_wifi_networks(g->ui_userselect, g->wifi_aps, count);
      } else {
        greeter_ui_set_error(g->ui_userselect,
                             "No networks found — Tab to skip");
      }
      syslog(LOG_INFO, "plexy-dm: wifi scan done: %d APs", count);
    } else if (st == WIFI_SCAN_FAILED) {
      g->wifi_scanning = false;
      greeter_ui_set_status(g->ui_userselect, NULL);
      greeter_ui_set_error(g->ui_userselect, "Scan failed — Tab to skip");
      syslog(LOG_WARNING, "plexy-dm: wifi scan failed");
    }
  }

  if (g->deferred_login) {
    g->deferred_login = false;
    greeter_ui_set_authenticating(g->ui_password, false);

    if (g->cbs.on_login) {
      int ret = g->cbs.on_login(g->deferred_username, g->deferred_password,
                                g->cbs.user_data);
      greeter_ui_clear_password(g->ui_password);
      if (ret < 0)
        greeter_show_error(g, "Incorrect password");
    } else {
      greeter_ui_clear_password(g->ui_password);
    }

    memset(g->deferred_username, 0, sizeof(g->deferred_username));
    memset(g->deferred_password, 0, sizeof(g->deferred_password));
  }

  greeter_request_frame(g);
}

void greeter_request_frame(plexy_greeter_t *g) {
  if (!g || g->suspended)
    return;

  if (g->drm.flip_pending) {
    g->frame_pending = true;
    return;
  }

  greeter_render_frame(g);
}

void greeter_render_frame(plexy_greeter_t *g) {
  if (!g || g->suspended)
    return;

  eglMakeCurrent(g->drm.egl_display, g->drm.egl_surface, g->drm.egl_surface,
                 g->drm.egl_context);

  glViewport(0, 0, g->drm.mode.hdisplay, g->drm.mode.vdisplay);

  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  double delta_ms = (double)(now.tv_sec - g->last_frame_time.tv_sec) * 1000.0 +
                    (double)(now.tv_nsec - g->last_frame_time.tv_nsec) / 1.0e6;
  if (delta_ms <= 0.0 || delta_ms > 200.0)
    delta_ms = 16.67;
  g->last_frame_time = now;
  g->scene_time_s += (float)(delta_ms / 1000.0);
  if (g->scene_time_s > 10000.0f)
    g->scene_time_s = 0.0f;

  if (g->video_thread_started)
    video_wallpaper_update(g);

  if (g->wp_program && g->scene_fbo && g->scene_texture) {
    glBindFramebuffer(GL_FRAMEBUFFER, g->scene_fbo);
    glViewport(0, 0, g->scene_width, g->scene_height);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    render_wallpaper(g);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, g->drm.mode.hdisplay, g->drm.mode.vdisplay);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    blit_texture_fullscreen(g, g->scene_texture);
  } else if (g->wp_program) {

    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    render_wallpaper(g);
  } else {

    glClearColor(0.42f, 0.55f, 0.68f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
  }

  if (!g->use_3d_theme) {
    bool dialog_transition = g->ui_transition_dir != 0 &&
                             g->ui_transition_from && g->ui_transition_to;

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, g->drm.mode.hdisplay, g->drm.mode.vdisplay);

    if (dialog_transition) {
      greeter_ui_render(g->ui_transition_from, delta_ms);
      greeter_ui_render(g->ui_transition_to, delta_ms);

      g->ui_transition_timer += delta_ms;
      float raw_t =
          (float)(g->ui_transition_timer / GREETER_2D_DIALOG_TRANSITION_MS);
      float t = greeter_smoothstep01(raw_t);
      float shift =
          (GREETER_2D_DIALOG_TRANSITION_PX * 2.0f) /
          (float)(g->drm.mode.vdisplay > 0 ? g->drm.mode.vdisplay : 1);
      float dir = (float)g->ui_transition_dir;
      GLuint from_tex = plexy_canvas_get_texture(
          greeter_ui_get_canvas(g->ui_transition_from));
      GLuint to_tex =
          plexy_canvas_get_texture(greeter_ui_get_canvas(g->ui_transition_to));

      blit_texture_fullscreen_ex(g, from_tex, 1.0f - t, 0.0f, -dir * shift * t);
      blit_texture_fullscreen_ex(g, to_tex, t, 0.0f, dir * shift * (1.0f - t));

      if (g->ui_transition_timer >= GREETER_2D_DIALOG_TRANSITION_MS) {
        g->ui_transition_from = NULL;
        g->ui_transition_to = NULL;
        g->ui_transition_dir = 0;
        g->ui_transition_timer = 0.0;
      }
    } else {
      greeter_ui_ctx_t *active = g->ui ? g->ui : g->ui_userselect;
      greeter_ui_render(active, delta_ms);
      blit_texture_fullscreen(
          g, plexy_canvas_get_texture(greeter_ui_get_canvas(active)));
    }

    if (g->state == GREETER_STATE_SWITCHING) {
      g->cube_fade_alpha += (float)(delta_ms / 220.0);
      if (g->cube_fade_alpha > 1.0f)
        g->cube_fade_alpha = 1.0f;
      render_fade_overlay(g);
    }
  } else {

    greeter_ui_render(g->ui_userselect, delta_ms);
    greeter_ui_render(g->ui_password, delta_ms);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, g->drm.mode.hdisplay, g->drm.mode.vdisplay);

    cube_anim_tick(g, delta_ms);

    if (g->cube_anim != CUBE_DONE && g->cube_program && g->cube_vao) {
      int w = g->drm.mode.hdisplay;
      int h = g->drm.mode.vdisplay;

      if (g->msaa_fbo) {

        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g->msaa_fbo);
        glBlitFramebuffer(0, 0, w, h, 0, 0, w, h,
                          GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT,
                          GL_NEAREST);

        glBindFramebuffer(GL_FRAMEBUFFER, g->msaa_fbo);
        glViewport(0, 0, w, h);
        glEnable(GL_MULTISAMPLE);

        render_cube(g);

        glDisable(GL_MULTISAMPLE);

        glBindFramebuffer(GL_READ_FRAMEBUFFER, g->msaa_fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT,
                          GL_LINEAR);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
      } else {
        render_cube(g);
      }
    } else {

      if (!g->cube_program)
        blit_ui_overlay(g);
      render_fade_overlay(g);
    }
  }

  eglSwapBuffers(g->drm.egl_display, g->drm.egl_surface);

  struct gbm_bo *bo = gbm_surface_lock_front_buffer(g->drm.gbm_surface);
  if (!bo)
    return;

  uint32_t fb_id = get_fb_for_bo(g->drm.drm_fd, bo);
  if (!fb_id) {
    gbm_surface_release_buffer(g->drm.gbm_surface, bo);
    return;
  }

  int ret = drmModePageFlip(g->drm.drm_fd, g->drm.crtc_id, fb_id,
                            DRM_MODE_PAGE_FLIP_EVENT, g);
  if (ret == 0) {
    g->drm.flip_pending = true;

    if (g->drm.prev_bo)
      gbm_surface_release_buffer(g->drm.gbm_surface, g->drm.prev_bo);
    g->drm.prev_bo = bo;
    g->drm.prev_fb = fb_id;
  } else {

    ret = drmModeSetCrtc(g->drm.drm_fd, g->drm.crtc_id, fb_id, 0, 0,
                         &g->drm.connector_id, 1, &g->drm.mode);
    if (ret != 0) {
      syslog(LOG_WARNING,
             "plexy-dm: drmModeSetCrtc fallback failed for connector %u: %s",
             g->drm.connector_id, strerror(errno));
    }
    if (g->drm.prev_bo)
      gbm_surface_release_buffer(g->drm.gbm_surface, g->drm.prev_bo);
    g->drm.prev_bo = bo;
    g->drm.prev_fb = fb_id;
  }

  for (int i = 0; i < g->drm.mirror_count; i++) {
    drmModeSetCrtc(g->drm.drm_fd, g->drm.mirrors[i].crtc_id, fb_id, 0, 0,
                   &g->drm.mirrors[i].connector_id, 1, &g->drm.mode);
  }

  bool cube_animating = g->use_3d_theme && (g->cube_anim != CUBE_DONE ||
                                            g->cube_fade_alpha < 1.0f);
  bool two_d_fading = !g->use_3d_theme && g->state == GREETER_STATE_SWITCHING &&
                      g->cube_fade_alpha < 1.0f;
  bool two_d_dialog_transition = !g->use_3d_theme &&
                                 g->ui_transition_dir != 0 &&
                                 g->ui_transition_from && g->ui_transition_to;
  greeter_ui_ctx_t *active_ui =
      g->use_3d_theme ? NULL : (g->ui ? g->ui : g->ui_userselect);
  bool ui_animating = g->use_3d_theme
                          ? (greeter_ui_animating(g->ui_userselect) ||
                             greeter_ui_animating(g->ui_password))
                          : greeter_ui_animating(active_ui);

  if (g->timer_fd >= 0) {
    struct itimerspec its = {0};
    bool two_d_rain_animating = !g->use_3d_theme && g->wp_texture != 0;
    if (cube_animating || two_d_fading || two_d_dialog_transition ||
        ui_animating || two_d_rain_animating) {
      its.it_value.tv_nsec = 16666667;
      its.it_interval.tv_nsec = 16666667;
    } else if (!g->use_3d_theme) {
      its.it_value.tv_nsec = 500000000;
      its.it_interval.tv_nsec = 500000000;
    }
    timerfd_settime(g->timer_fd, 0, &its, NULL);
  }
}

void greeter_suspend(plexy_greeter_t *g) {
  if (!g)
    return;
  g->suspended = true;

  eglMakeCurrent(g->drm.egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                 EGL_NO_CONTEXT);

  if (g->drm.drm_fd >= 0) {
    drmDropMaster(g->drm.drm_fd);
    syslog(LOG_INFO, "plexy-dm: dropped DRM master for user session");
  }
}

void greeter_resume(plexy_greeter_t *g) {
  if (!g)
    return;

  if (g->drm.drm_fd >= 0) {
    if (drmSetMaster(g->drm.drm_fd) < 0)
      syslog(LOG_WARNING, "plexy-dm: drmSetMaster failed: %s", strerror(errno));
    else
      syslog(LOG_INFO, "plexy-dm: reclaimed DRM master");
  }

  refresh_drm_outputs(&g->drm);

  g->suspended = false;

  if (g->input.xkb_state && g->input.xkb_keymap) {
    xkb_mod_index_t caps_idx =
        xkb_keymap_mod_get_index(g->input.xkb_keymap, XKB_MOD_NAME_CAPS);
    if (caps_idx != XKB_MOD_INVALID) {
      bool caps = xkb_state_mod_index_is_active(g->input.xkb_state, caps_idx,
                                                XKB_STATE_MODS_EFFECTIVE);
      greeter_ui_set_caps_lock(g->ui_password, caps);
    }
  }

  eglMakeCurrent(g->drm.egl_display, g->drm.egl_surface, g->drm.egl_surface,
                 g->drm.egl_context);

  if (g->drm.hw_cursor_supported && g->drm.hw_cursor_bo) {
    uint32_t handle = gbm_bo_get_handle(g->drm.hw_cursor_bo).u32;
    if (drmModeSetCursor2(g->drm.drm_fd, g->drm.crtc_id, handle,
                          g->drm.hw_cursor_width, g->drm.hw_cursor_height,
                          g->drm.hw_cursor_hotspot_x,
                          g->drm.hw_cursor_hotspot_y) != 0) {
      syslog(LOG_WARNING, "plexy-dm: hardware cursor retry failed: %s",
             strerror(errno));
    }
    for (int i = 0; i < g->drm.mirror_count; i++)
      drmModeSetCursor2(g->drm.drm_fd, g->drm.mirrors[i].crtc_id, handle,
                        g->drm.hw_cursor_width, g->drm.hw_cursor_height,
                        g->drm.hw_cursor_hotspot_x, g->drm.hw_cursor_hotspot_y);
  }

  greeter_request_frame(g);
}

bool greeter_debug_term_requested(plexy_greeter_t *g) {
  if (!g || !g->debug_term_requested)
    return false;
  g->debug_term_requested = false;
  return true;
}
