/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

void init_lvgl_event();
void init_lvgl_event_cpp();
void init_lvgl_env();
void init_lvgl_saved_settings();
void init_sudo_signals();
#ifdef __cplusplus
void deinit_sudo(void) noexcept;
void cp0_saved_volume_restore_stop(void) noexcept;
#else
void deinit_sudo(void);
void cp0_saved_volume_restore_stop(void);
#endif

#ifdef __cplusplus
}
#endif
