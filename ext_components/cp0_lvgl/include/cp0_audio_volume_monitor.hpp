/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <atomic>
#include <functional>

namespace cp0::pulse {

// Device backend only. Blocks on the calling worker until stopped or the
// server connection fails. Calls refresh after subscribing and when a sink
// or the server default changes. The callback must not touch LVGL.
void watch_default_sink_changes(const std::atomic<bool> &stop_requested,
                                std::atomic<bool> &refresh_requested,
                                const std::function<void()> &refresh);

// Resolve and read one sink, then adjust it by index on that same connection.
// Persist receives the previous and written percentages. On failure the
// adjustment restores the previous volume on the same sink and returns -1.
int adjust_default_sink_volume(const std::function<int(int)> &target,
                               const std::function<bool(int, int)> &persist);

} // namespace cp0::pulse
