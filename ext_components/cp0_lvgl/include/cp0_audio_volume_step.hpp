/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <algorithm>
#include <functional>
#include <list>
#include <mutex>
#include <string>

namespace cp0::audio {

// Hold this across a complete mixer transaction, including persistence and recovery.
std::mutex &volume_operation_mutex();

// Synchronous mixer commands from Settings. VolumeWrite shares the transaction lock.
void invoke_mixer_api(std::list<std::string> args, std::function<void(int, std::string)> callback);

// Called under the transaction lock once a shortcut can issue a mixer write.
void cancel_saved_volume_restore();

struct VolumeStepResult {
    int percent = -1;
    bool muted = false;
    bool mute_known = true;
};

// A backend must acknowledge exact writes and only observe or write the original
// output during recovery. An unavailable or mismatched observation returns -1.
template <typename WriteVolume, typename SetMute, typename Observe, typename RestoreVolume>
VolumeStepResult apply_volume_step(VolumeStepResult previous, int target,
                                   const std::function<bool(int, int)> &persist,
                                   WriteVolume write_volume, SetMute set_mute, Observe observe,
                                   RestoreVolume restore_volume)
{
    const int written = std::clamp(target, 0, 100);
    auto confirmed = previous;
    bool unmute_uncertain = false;
    if (write_volume(written)) {
        confirmed.percent = written;
        if (previous.muted && written != previous.percent) {
            unmute_uncertain = !set_mute(false);
            confirmed.mute_known = !unmute_uncertain;
            if (!unmute_uncertain) confirmed.muted = false;
        }
        // A saturated step can persist the current level while muted. An
        // unacknowledged unmute here could expose that very same level.
        if (!unmute_uncertain) {
            try {
                if (persist(previous.percent, written)) return confirmed;
            } catch (...) {
                // Recover the original sink while it is still part of this transaction.
            }
        }
    }

    auto live = observe();
    if (live.percent < 0) return confirmed;
    const bool unmute_seen = !live.muted;
    if (previous.muted && unmute_seen) {
        set_mute(true);
        const auto remuted = observe();
        if (remuted.percent < 0) {
            live.mute_known = false;
            return live;
        }
        live = remuted;
    }
    // If the unmute timed out and the sink is still muted on reread, it may
    // still unmute later. Keep the safe target instead of restoring the old level.
    if (!previous.muted || (live.muted && (!unmute_uncertain || unmute_seen))) {
        if (restore_volume(live, written)) live.percent = previous.percent;
    }
    const auto settled = observe();
    return settled.percent < 0 ? live : settled;
}

} // namespace cp0::audio

namespace cp0::pulse {

using audio::VolumeStepResult;

// Writes the step to the sink it read. Calls persist(previous, written) after
// verifying the written volume on the original connection, and reconnects to
// recover on failure. Returns the last confirmed level and whether mute is
// known, or percent -1 if the initial read failed.
VolumeStepResult step_default_sink_volume(const std::function<int(int)> &target,
                                          const std::function<bool(int, int)> &persist);

} // namespace cp0::pulse
