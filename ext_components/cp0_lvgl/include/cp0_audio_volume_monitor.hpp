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
                                const std::function<void()> &refresh);

} // namespace cp0::pulse
