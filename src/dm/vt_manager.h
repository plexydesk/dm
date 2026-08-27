/*
 * Copyright (C) 2024-2026 Siraj Razick
 * SPDX-License-Identifier: AGPL-3.0-only
 */


#ifndef PLEXY_DM_VT_MANAGER_H
#define PLEXY_DM_VT_MANAGER_H

#include "plexy_dm.h"

typedef struct plexy_vt_manager plexy_vt_manager_t;

plexy_vt_manager_t *vt_manager_create(int greeter_vt);

void vt_manager_destroy(plexy_vt_manager_t *mgr);

int vt_manager_claim_greeter(plexy_vt_manager_t *mgr);

int vt_manager_alloc_session_vt(plexy_vt_manager_t *mgr);

void vt_manager_release_vt(plexy_vt_manager_t *mgr, int vt);

int vt_manager_switch_to(plexy_vt_manager_t *mgr, int vt);

int vt_manager_switch_to_greeter(plexy_vt_manager_t *mgr);

int vt_manager_greeter_vt(const plexy_vt_manager_t *mgr);

int vt_manager_active_vt(const plexy_vt_manager_t *mgr);

int vt_manager_get_signal_fd(const plexy_vt_manager_t *mgr);

void vt_manager_handle_switch(plexy_vt_manager_t *mgr);

void vt_manager_set_lock(plexy_vt_manager_t *mgr, bool locked);

#endif
