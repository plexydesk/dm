/*
 * Copyright (C) 2024-2026 Siraj Razick
 * SPDX-License-Identifier: AGPL-3.0-only
 */


#include "wifi_list.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

bool wifi_is_connected(void) {
  FILE *fp = popen("nmcli -t -f CONNECTIVITY general status 2>/dev/null", "r");
  if (!fp)
    return false;

  char line[64];
  bool connected = false;
  if (fgets(line, sizeof(line), fp)) {

    if (strncmp(line, "full", 4) == 0 || strncmp(line, "limited", 7) == 0)
      connected = true;
  }
  pclose(fp);
  return connected;
}

int wifi_list_scan(plexy_dm_wifi_ap_t *aps, int max_aps) {
  if (!aps || max_aps <= 0)
    return 0;

  FILE *fp = popen("nmcli -t -f SSID,SIGNAL,SECURITY,ACTIVE device wifi list"
                   " --rescan yes 2>/dev/null",
                   "r");
  if (!fp) {
    syslog(LOG_WARNING, "plexy-dm: wifi_list_scan: popen failed");
    return 0;
  }

  int count = 0;
  char line[256];

  while (count < max_aps && fgets(line, sizeof(line), fp)) {

    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
      line[--len] = '\0';

    if (len == 0)
      continue;

    char ssid[PLEXY_DM_MAX_SSID] = {0};
    char sig_str[16] = {0};
    char sec_str[64] = {0};
    char active_str[8] = {0};

    int field = 0;
    int si = 0, di = 0;
    while (line[si] && field < 4) {
      char c = line[si];
      if (c == '\\' && line[si + 1] == ':') {

        if (field == 0 && di < (int)sizeof(ssid) - 1)
          ssid[di++] = ':';
        si += 2;
        continue;
      }
      if (c == ':') {
        field++;
        si++;
        di = 0;
        continue;
      }
      switch (field) {
      case 0:
        if (di < (int)sizeof(ssid) - 1)
          ssid[di++] = c;
        break;
      case 1:
        if (di < (int)sizeof(sig_str) - 1)
          sig_str[di++] = c;
        break;
      case 2:
        if (di < (int)sizeof(sec_str) - 1)
          sec_str[di++] = c;
        break;
      case 3:
        if (di < (int)sizeof(active_str) - 1)
          active_str[di++] = c;
        break;
      }
      si++;
    }

    if (ssid[0] == '\0')
      continue;

    plexy_dm_wifi_ap_t *ap = &aps[count];
    snprintf(ap->ssid, sizeof(ap->ssid), "%s", ssid);
    ap->signal = atoi(sig_str);
    ap->secured = (sec_str[0] != '\0' && strcmp(sec_str, "--") != 0);
    ap->connected = (active_str[0] == 'y' || active_str[0] == 'Y');
    count++;
  }

  pclose(fp);
  syslog(LOG_INFO, "plexy-dm: wifi_list_scan: found %d networks", count);
  return count;
}

int wifi_connect(const char *ssid, const char *password) {
  if (!ssid || ssid[0] == '\0')
    return -1;

  char cmd[512];
  if (password && password[0]) {
    snprintf(cmd, sizeof(cmd),
             "nmcli device wifi connect \"%s\" password \"%s\" 2>/dev/null",
             ssid, password);
  } else {
    snprintf(cmd, sizeof(cmd), "nmcli device wifi connect \"%s\" 2>/dev/null",
             ssid);
  }

  syslog(LOG_INFO, "plexy-dm: wifi_connect: ssid='%s'", ssid);
  int ret = system(cmd);
  return (ret == 0) ? 0 : -1;
}

static struct {
  pthread_t thread;
  atomic_int state;
  pthread_mutex_t lock;
  plexy_dm_wifi_ap_t results[PLEXY_DM_MAX_WIFI_APS];
  int count;
} g_scan = {
    .state = WIFI_SCAN_RUNNING,
    .lock = PTHREAD_MUTEX_INITIALIZER,
};

static void *scan_thread(void *arg) {
  (void)arg;

  plexy_dm_wifi_ap_t tmp[PLEXY_DM_MAX_WIFI_APS];
  int count = wifi_list_scan(tmp, PLEXY_DM_MAX_WIFI_APS);

  pthread_mutex_lock(&g_scan.lock);
  memcpy(g_scan.results, tmp, count * sizeof(plexy_dm_wifi_ap_t));
  g_scan.count = count;

  atomic_store(&g_scan.state, count >= 0 ? WIFI_SCAN_DONE : WIFI_SCAN_FAILED);
  pthread_mutex_unlock(&g_scan.lock);

  syslog(LOG_INFO, "plexy-dm: async wifi scan complete: %d APs", count);
  return NULL;
}

void wifi_scan_start_async(void) {
  int cur = atomic_load(&g_scan.state);
  if (cur == WIFI_SCAN_RUNNING && g_scan.thread) {

    return;
  }

  atomic_store(&g_scan.state, WIFI_SCAN_RUNNING);
  g_scan.count = 0;

  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  if (pthread_create(&g_scan.thread, &attr, scan_thread, NULL) != 0) {
    syslog(LOG_ERR, "plexy-dm: wifi_scan_start_async: pthread_create failed");
    atomic_store(&g_scan.state, WIFI_SCAN_FAILED);
  }
  pthread_attr_destroy(&attr);
}

wifi_scan_state_t wifi_scan_poll(plexy_dm_wifi_ap_t *aps, int max_aps,
                                 int *count_out) {
  wifi_scan_state_t st = (wifi_scan_state_t)atomic_load(&g_scan.state);
  if (st == WIFI_SCAN_RUNNING)
    return WIFI_SCAN_RUNNING;

  pthread_mutex_lock(&g_scan.lock);
  int n = g_scan.count < max_aps ? g_scan.count : max_aps;
  if (aps && n > 0)
    memcpy(aps, g_scan.results, n * sizeof(plexy_dm_wifi_ap_t));
  if (count_out)
    *count_out = n;
  pthread_mutex_unlock(&g_scan.lock);

  return st;
}

void wifi_scan_reset(void) {
  pthread_mutex_lock(&g_scan.lock);
  g_scan.count = 0;
  g_scan.thread = 0;
  pthread_mutex_unlock(&g_scan.lock);
  atomic_store(&g_scan.state, WIFI_SCAN_RUNNING);
}
