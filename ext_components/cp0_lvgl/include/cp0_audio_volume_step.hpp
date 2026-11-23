/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <algorithm>
#include <functional>

namespace cp0::audio {

struct VolumeStepResult {
    int percent = -1;
    bool muted = false;
};

// A backend must acknowledge exact writes and only observe or write the original
// output during recovery. An unavailable or mismatched observation returns -1.
template <typename WriteVolume, typename SetMute, typename Observe>
VolumeStepResult apply_volume_step(VolumeStepResult previous, int target,
                                   const std::function<bool(int, int)> &persist,
                                   WriteVolume write_volume, SetMute set_mute, Observe observe)
{
    const int written = std::clamp(target, 0, 100);
    auto confirmed = previous;
    bool unmute_uncertain = false;
    if (write_volume(written)) {
        confirmed.percent = written;
        if (previous.muted && written != previous.percent) {
            unmute_uncertain = !set_mute(false);
            if (!unmute_uncertain) confirmed.muted = false;
        }
        // A saturated step can persist the current level while muted. An
        // unacknowledged unmute here could expose that very same level.
        if (!unmute_uncertain && persist(previous.percent, written)) return confirmed;
    }

    auto live = observe();
    if (live.percent < 0) return confirmed;
    const bool unmute_seen = !live.muted;
    if (previous.muted && unmute_seen) {
        if (set_mute(true)) live.muted = true;
    }
    // If the unmute timed out and the sink is still muted on reread, it may
    // still unmute later. Keep the safe target instead of restoring the old level.
    if (live.percent == written && written != previous.percent &&
        (!previous.muted || (live.muted && (!unmute_uncertain || unmute_seen)))) {
        if (write_volume(previous.percent)) live.percent = previous.percent;
    }
    const auto settled = observe();
    return settled.percent < 0 ? live : settled;
}

} // namespace cp0::audio

namespace cp0::pulse {

using audio::VolumeStepResult;

// Writes the step to the sink it read. Calls persist(previous, written) after
// an acknowledged write and reconnects to recover on failure. Returns the
// last confirmed state of that sink, or percent -1 if the initial read failed.
VolumeStepResult step_default_sink_volume(const std::function<int(int)> &target,
                                          const std::function<bool(int, int)> &persist);

} // namespace cp0::pulse
