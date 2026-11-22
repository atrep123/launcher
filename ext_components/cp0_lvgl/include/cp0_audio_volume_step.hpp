/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <functional>

namespace cp0::pulse {

struct VolumeStepResult {
    int percent = -1;
    bool muted = false;
};

// Writes the step to the sink it read. Calls persist(previous, written) after
// an acknowledged write and reconnects to recover on failure. Returns the
// last confirmed state of that sink, or percent -1 if the initial read failed.
VolumeStepResult step_default_sink_volume(const std::function<int(int)> &target,
                                          const std::function<bool(int, int)> &persist);

} // namespace cp0::pulse
