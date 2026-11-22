/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "model/setup_value_policy.hpp"

#include "cp0_audio_volume_monitor.hpp"

namespace launcher_media_controls {

constexpr int VOLUME_STEP_PERCENT =
    setup_values::volume_metric(setup_values::VolumeMetric::StepPercent);

/* Shared mixer volume used by the shortcut OSD and Settings -> Speaker.
 * cached_volume() returns -1 until the first value is available. */
bool has_cached_volume();
int cached_volume();
/* Read the settled volume off the LVGL thread and follow mixer events. */
void start_volume_cache();
/* Join the mixer reader so teardown cannot race a signal call. */
void stop_volume_cache();
/* Queue a fresh read on the worker after an app exits. */
void refresh_volume_cache();
/* Read the mixer and timestamp the completed observation. */
cp0::pulse::VolumeSample read_mixer_volume(const std::atomic<bool> *stop_requested = nullptr);
/* Publish a mixer observation and return the value the shared cache holds. */
int publish_mixer_volume(cp0::pulse::VolumeSample sample);

int adjust_volume(int delta_percent);
int adjust_brightness(int delta_percent);
bool toggle_mute();

// Apply saved brightness at startup, defaulting to maximum for missing/invalid
// values. Does not persist changes or reuse a screen-off hardware reading.
bool restore_startup_backlight();

/* Drive the panel backlight to zero without persisting anything, for the
 * screen-off gesture.  Returns the raw value to hand back to
 * restore_backlight(), or -1 when the backlight could not be suspended.  The
 * persisted brightness must survive untouched: the normal step-based control
 * can never express zero, so reusing it here would overwrite the user's
 * setting and leave the panel dark after wake-up. */
int suspend_backlight();
void restore_backlight(int raw);

} // namespace launcher_media_controls
