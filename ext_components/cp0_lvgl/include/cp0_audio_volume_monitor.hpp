/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <atomic>
#include <chrono>
#include <functional>

namespace cp0::pulse {

using VolumeTime = std::chrono::steady_clock::time_point;

struct VolumeSample {
    int percent = -1;
    VolumeTime observed_at{};
};

// The timestamp is captured when libpulse delivers the sink volume.
VolumeSample read_default_sink_volume_sample(const std::atomic<bool> *stop_requested = nullptr);

// Device backend only. Blocks on the calling worker until stopped or the
// server connection fails. Calls refresh after subscribing and when a sink
// or the server default changes. The callback must not touch LVGL.
void watch_default_sink_changes(const std::atomic<bool> &stop_requested,
                                std::atomic<bool> &refresh_requested,
                                const std::function<void()> &refresh);

// Resolve and read one sink, then adjust it by index on that same connection.
// Persist receives the previous and written percentages. On failure the
// adjustment attempts to restore all channels, then returns -1 or the code
// below if rollback was not confirmed. observed reports a confirmed write or
// restore, or a timestamped unknown volume if rollback failed.
inline constexpr int kVolumeRollbackFailed = -2;
int adjust_default_sink_volume(const std::function<int(int)> &target,
                               const std::function<bool(int, int)> &persist,
                               VolumeSample *observed = nullptr);

} // namespace cp0::pulse
