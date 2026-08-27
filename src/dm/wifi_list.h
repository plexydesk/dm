/*
 * Copyright (C) 2024-2026 Siraj Razick
 * SPDX-License-Identifier: AGPL-3.0-only
 */


#ifndef PLEXY_DM_WIFI_LIST_H
#define PLEXY_DM_WIFI_LIST_H

#include <stdbool.h>

#define PLEXY_DM_MAX_WIFI_APS 20
#define PLEXY_DM_MAX_SSID 64

typedef struct {
  char ssid[PLEXY_DM_MAX_SSID];
  int signal;
  bool secured;
  bool connected;
} plexy_dm_wifi_ap_t;

int wifi_list_scan(plexy_dm_wifi_ap_t *aps, int max_aps);

bool wifi_is_connected(void);

int wifi_connect(const char *ssid, const char *password);

typedef enum {
  WIFI_SCAN_RUNNING = 0,
  WIFI_SCAN_DONE,
  WIFI_SCAN_FAILED,
} wifi_scan_state_t;

void wifi_scan_start_async(void);

wifi_scan_state_t wifi_scan_poll(plexy_dm_wifi_ap_t *aps, int max_aps,
                                 int *count_out);

void wifi_scan_reset(void);

#endif
