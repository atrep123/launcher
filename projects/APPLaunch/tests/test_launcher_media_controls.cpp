/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "../main/ui/launcher_media_controls.h"

#include "commount.h"
#include "hal_lvgl_bsp.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <functional>
#include <future>
#include <iterator>
#include <list>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

extern "C" int cp0_backlight_write(int) { return -1; }

namespace {

int sink_volume = 63;
int configured_volume = 80;
bool volume_read_failed = false;
bool config_save_failed = false;
bool config_save_throws = false;
int backlight_maximum = 255;
int backlight_value = 102;
int configured_brightness = 102;
bool brightness_config_missing = false;
bool backlight_write_failed = false;
int config_writes = 0;
bool sink_muted = true;
bool volume_write_failed = false;
int mixer_id = 1;
int mute_writes = 0;
bool lose_unmute_reply = false;
bool replace_mixer_on_save = false;
std::vector<std::string> audio_commands;

std::size_t count_commands(const std::string &command)
{
    return static_cast<std::size_t>(
        std::count(audio_commands.begin(), audio_commands.end(), command));
}

bool wait_for_cached_volume(int expected, int timeout_ms = 3000)
{
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (launcher_media_controls::cached_volume() != expected) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return true;
}

void reply(const std::function<void(int, std::string)> &callback,
           int code, const std::string &data)
{
    if (callback)
        callback(code, data);
}

void test_startup_restore_during_shortcut()
{
    using namespace std::chrono_literals;

    int level = 60;
    int saved = 60;
    bool muted = true;
    std::promise<void> restore_waiting, step_written, restore_written, release_restore;
    auto restore_ready = restore_waiting.get_future();
    auto step_ready = step_written.get_future();
    auto restored = restore_written.get_future().share();
    auto release = release_restore.get_future();

    const auto audio = cp0_signal_audio_api.append([&](auto args, auto callback) {
        const auto &command = args.front();
        if (command == "DefaultSink") {
            reply(callback, 0, "alsa_output.platform-sound.stereo-fallback");
        } else if (command == "VolumeRead") {
            // The restore pass has observed a transient level and decided to
            // rewrite the saved value. Hold the write before it reaches the mixer.
            reply(callback, 0, "50");
        } else if (command == "VolumeWrite") {
            restore_waiting.set_value();
            assert(release.wait_for(5s) == std::future_status::ready);
            level = std::stoi(args.back());
            restore_written.set_value();
        } else if (command == "VolumeStepRead") {
            reply(callback, 0, "1 " + std::to_string(level) + (muted ? " 1" : " 0"));
        } else if (command == "VolumeStepVolume") {
            level = std::stoi(args.back());
            step_written.set_value();
            // On the unfixed launcher this forces the stale write to land
            // between the shortcut's volume write and unmute.
            assert(restored.wait_for(5s) == std::future_status::ready);
            reply(callback, 0, args.back());
        } else if (command == "VolumeStepMute") {
            muted = args.back() == "1";
            reply(callback, 0, args.back());
        } else {
            assert(false);
        }
    });
    const auto config = cp0_signal_config_api.append([&](auto args, auto callback) {
        if (args.front() == "Save") {
            reply(callback, 0, "");
            return;
        }
        const auto &key = *std::next(args.begin());
        if (args.front() == "GetInt") {
            reply(callback, 0, key == "volume" ? std::to_string(saved) : "-1");
        } else {
            assert(args.front() == "SetManyAndSave" || args.front() == "SetInt");
            saved = std::stoi(args.back());
            reply(callback, 0, "");
        }
    });

    init_lvgl_saved_settings();
    assert(restore_ready.wait_for(5s) == std::future_status::ready);
    std::promise<void> shortcut_started;
    auto started = shortcut_started.get_future();
    auto shortcut = std::async(std::launch::async, [&] {
        shortcut_started.set_value();
        return launcher_media_controls::adjust_volume(-10);
    });
    assert(started.wait_for(5s) == std::future_status::ready);
    const bool overlapped = step_ready.wait_for(250ms) == std::future_status::ready;
    release_restore.set_value();
    const int shown = shortcut.get().percent;
    cp0_saved_volume_restore_stop();
    cp0_signal_audio_api.remove(audio);
    cp0_signal_config_api.remove(config);

    std::fprintf(stderr, "startup restore: overlap=%d level=%d muted=%d saved=%d OSD=%d\n",
                 overlapped, level, muted, saved, shown);
    assert(!overlapped && level == 50 && !muted && saved == 50 && shown == 50);
}

void test_settings_write_during_shortcut()
{
    using namespace std::chrono_literals;

    int level = 60;
    int unmuted_level = -1;
    int saved = -1;
    int level_at_save = -1;
    std::promise<void> verified, release_unmute, settings_offered;
    auto at_unmute = verified.get_future();
    auto release = release_unmute.get_future();
    auto offered = settings_offered.get_future();

    const auto audio = cp0_signal_audio_api.append([&](auto args, auto callback) {
        const auto &command = args.front();
        if (command == "VolumeStepRead") {
            reply(callback, 0, "1 60 1");
        } else if (command == "VolumeStepVolume" || command == "VolumeWrite") {
            level = std::stoi(args.back());
            reply(callback, 0, args.back());
        } else {
            assert(command == "VolumeStepMute" && args.back() == "0");
            verified.set_value();
            assert(release.wait_for(5s) == std::future_status::ready);
            unmuted_level = level;
            reply(callback, 0, "0");
        }
    });
    const auto config = cp0_signal_config_api.append([&](auto args, auto callback) {
        assert(args.front() == "SetManyAndSave");
        saved = std::stoi(args.back());
        level_at_save = level;
        reply(callback, 0, "");
    });

    auto shortcut = std::async(std::launch::async, [] {
        return launcher_media_controls::adjust_volume(-10);
    });
    assert(at_unmute.wait_for(5s) == std::future_status::ready);
    auto settings = std::async(std::launch::async, [&] {
        settings_offered.set_value();
        cp0::audio::invoke_mixer_api({"VolumeWrite", "60"}, nullptr);
    });
    assert(offered.wait_for(5s) == std::future_status::ready);
    const bool overlapped = settings.wait_for(250ms) == std::future_status::ready;
    release_unmute.set_value();
    const auto result = shortcut.get();
    settings.get();
    cp0_signal_audio_api.remove(audio);
    cp0_signal_config_api.remove(config);

    std::fprintf(stderr, "Settings write: overlap=%d level at unmute=%d level at save=%d saved=%d\n",
                 overlapped, unmuted_level, level_at_save, saved);
    assert(!overlapped && unmuted_level == 50 && level_at_save == 50 &&
           saved == 50 && result.percent == 50 && level == 60);
}

void test_startup_restore_after_failed_read()
{
    using namespace std::chrono_literals;

    std::atomic<bool> hardware_ready{false};
    std::promise<void> scanned, restored;
    auto first_pass = scanned.get_future();
    auto written = restored.get_future();
    const auto audio = cp0_signal_audio_api.append([&](auto args, auto callback) {
        const auto &command = args.front();
        if (command == "DefaultSink") {
            if (hardware_ready.load()) {
                reply(callback, 0, "alsa_output.platform-sound.stereo-fallback");
            } else {
                scanned.set_value();
                reply(callback, 0, "auto_null");
            }
        } else if (command == "VolumeStepRead") {
            reply(callback, -1, "");
        } else if (command == "VolumeRead") {
            reply(callback, 0, "20");
        } else {
            assert(command == "VolumeWrite" && args.back() == "60");
            restored.set_value();
        }
    });
    const auto config = cp0_signal_config_api.append([](auto args, auto callback) {
        assert(args.front() == "GetInt");
        reply(callback, 0, *std::next(args.begin()) == "volume" ? "60" : "-1");
    });

    init_lvgl_saved_settings();
    assert(first_pass.wait_for(5s) == std::future_status::ready);
    const auto result = launcher_media_controls::adjust_volume(-10);
    hardware_ready.store(true);
    const bool recovered = written.wait_for(2s) == std::future_status::ready;
    cp0_saved_volume_restore_stop();
    cp0_signal_audio_api.remove(audio);
    cp0_signal_config_api.remove(config);

    std::fprintf(stderr, "cold start: restored after failed read=%d\n", recovered);
    assert(recovered && !result.mute_known);
}

} // namespace

int main()
{
    const auto audio = cp0_signal_audio_api.append(
        [](std::list<std::string> arguments,
           std::function<void(int, std::string)> callback) {
            assert(!arguments.empty());
            const std::string command = arguments.front();
            audio_commands.push_back(command);

            if (command == "VolumeStepRead") {
                reply(callback, volume_read_failed ? -1 : 0,
                      std::to_string(mixer_id) + " " + std::to_string(sink_volume) +
                          (sink_muted ? " 1" : " 0"));
            } else if (command == "VolumeStepVolume" || command == "VolumeStepMute") {
                assert(arguments.size() == 3);
                if (std::stoi(*std::next(arguments.begin())) != mixer_id) {
                    reply(callback, -1, "");
                    return;
                }
                const int value = std::stoi(arguments.back());
                if (command == "VolumeStepMute") {
                    ++mute_writes;
                    sink_muted = value != 0;
                    if (!sink_muted && lose_unmute_reply) return;
                } else if (volume_write_failed) {
                    reply(callback, -1, "");
                    return;
                } else {
                    sink_volume = value;
                }
                reply(callback, 0, arguments.back());
            } else if (command == "MuteRead") {
                reply(callback, 0, sink_muted ? "1" : "0");
            } else if (command == "MuteToggle") {
                sink_muted = !sink_muted;
                reply(callback, 0, sink_muted ? "1" : "0");
            } else if (command == "VolumeRead") {
                reply(callback, volume_read_failed ? -1 : 0, std::to_string(sink_volume));
            } else if (command == "VolumeWrite") {
                assert(arguments.size() == 2);
                if (volume_write_failed) {
                    reply(callback, -1, "");
                    return;
                }
                sink_volume = std::stoi(*std::next(arguments.begin()));
                reply(callback, 0, std::to_string(sink_volume));
            } else {
                assert(false);
            }
        });

    const auto config = cp0_signal_config_api.append(
        [](std::list<std::string> arguments,
           std::function<void(int, std::string)> callback) {
            assert(!arguments.empty());
            const std::string command = arguments.front();
            if (command == "SetManyAndSave") {
                ++config_writes;
                assert(arguments.size() == 3 && *std::next(arguments.begin()) == "volume");
                if (replace_mixer_on_save) {
                    replace_mixer_on_save = false;
                    ++mixer_id;
                    sink_muted = false;
                    sink_volume = 50;
                }
                if (config_save_throws) throw std::runtime_error("injected persistence failure");
                if (!config_save_failed) configured_volume = std::stoi(arguments.back());
                reply(callback, config_save_failed ? -1 : 0, "");
            } else if (command == "GetInt") {
                const std::string key = *std::next(arguments.begin());
                if (key == "brightness" && brightness_config_missing) {
                    reply(callback, 0, *std::next(arguments.begin(), 2));
                    return;
                }
                reply(callback, 0, std::to_string(
                    key == "brightness" ? configured_brightness : configured_volume));
            } else if (command == "SetInt") {
                ++config_writes;
                assert(arguments.size() == 3);
                const std::string key = *std::next(arguments.begin());
                const int value = std::stoi(*std::next(arguments.begin(), 2));
                if (key == "brightness")
                    configured_brightness = value;
                else
                    configured_volume = value;
                reply(callback, 0, "");
            } else if (command == "Save") {
                reply(callback, config_save_failed ? -1 : 0, "");
            } else {
                assert(false);
            }
        });

    cp0_signal_settings_api.append(
        [](std::list<std::string> arguments,
           std::function<void(int, std::string)> callback) {
            assert(!arguments.empty());
            const std::string command = arguments.front();
            if (command == "BacklightMax") {
                reply(callback, 0, std::to_string(backlight_maximum));
            } else if (command == "BacklightRead") {
                reply(callback, 0, std::to_string(backlight_value));
            } else if (command == "BacklightWrite") {
                assert(arguments.size() == 2);
                if (backlight_write_failed) {
                    reply(callback, -1, "write failed");
                    return;
                }
                backlight_value = std::stoi(*std::next(arguments.begin()));
                reply(callback, 0, std::to_string(backlight_value));
            } else {
                assert(false);
            }
        });

    sink_volume = 60;
    sink_muted = true;
    volume_write_failed = true;
    assert(launcher_media_controls::adjust_volume(-10).percent == 60);
    assert(sink_volume == 60 && sink_muted && mute_writes == 0);
    volume_write_failed = false;

    lose_unmute_reply = true;
    assert(launcher_media_controls::adjust_volume(-10).percent == 60);
    assert(sink_volume == 60 && sink_muted && mute_writes == 2);
    lose_unmute_reply = false;

    sink_volume = 100;
    const auto saturated = launcher_media_controls::adjust_volume(10);
    assert(saturated.percent == 100 && saturated.muted);
    assert(sink_volume == 100 && sink_muted && mute_writes == 2);

    sink_volume = 60;
    config_save_failed = replace_mixer_on_save = true;
    assert(launcher_media_controls::adjust_volume(-10).percent == 50);
    assert(sink_volume == 50 && !sink_muted && mute_writes == 3);
    config_save_failed = false;
    sink_muted = true;
    mute_writes = 0;

    assert(launcher_media_controls::VOLUME_STEP_PERCENT == 10);

    // The launcher reads the mixer once at startup and caches it. The worker
    // waits for two equal readings so a pre-settle level cannot be captured.
    sink_volume = 63;
    launcher_media_controls::start_volume_cache();
    assert(wait_for_cached_volume(63));
    launcher_media_controls::stop_volume_cache();
    assert(launcher_media_controls::has_cached_volume());
    const std::size_t startup_reads = count_commands("VolumeRead");
    assert(startup_reads >= 2);

    // Keep upstream rounding and unmute behavior for a live level off the step grid.
    assert(launcher_media_controls::adjust_volume(-10).percent == 50);
    assert(!sink_muted);
    assert(sink_volume == 50);
    assert(configured_volume == 50);
    assert(launcher_media_controls::cached_volume() == 50);
    assert(mute_writes == 1);
    assert(count_commands("VolumeRead") == startup_reads);

    // #264: an external app changed the mixer while HOME still had 100%.
    launcher_media_controls::update_cached_volume(100);
    configured_volume = 100;
    sink_volume = 20;
    assert(launcher_media_controls::adjust_volume(-10).percent == 10);
    assert(sink_volume == 10);
    assert(configured_volume == 10);
    assert(launcher_media_controls::cached_volume() == 10);

    // A read failure must not write volume, unmute, or persist a cached step.
    volume_read_failed = true;
    sink_muted = true;
    const auto commands_before_failure = audio_commands.size();
    const int config_writes_before_failure = config_writes;
    assert(launcher_media_controls::adjust_volume(10).percent == 10);
    assert(audio_commands.size() == commands_before_failure + 1);
    assert(audio_commands.back() == "VolumeStepRead");
    assert(sink_muted);
    assert(sink_volume == 10);
    assert(config_writes == config_writes_before_failure);
    assert(launcher_media_controls::cached_volume() == 10);
    volume_read_failed = false;

    // A failed save restores the previous mixer and configuration values.
    sink_volume = 70;
    config_save_failed = true;
    assert(launcher_media_controls::adjust_volume(10).percent == 70);
    assert(sink_volume == 70);
    assert(configured_volume == 10);
    assert(launcher_media_controls::cached_volume() == 70);
    config_save_failed = false;

    launcher_media_controls::update_cached_volume(150);
    assert(launcher_media_controls::cached_volume() == 100);
    launcher_media_controls::update_cached_volume(-10);
    assert(launcher_media_controls::cached_volume() == 0);

    // A launcher restart re-reads the mixer and replaces the cache.
    sink_volume = 63;
    launcher_media_controls::start_volume_cache();
    assert(wait_for_cached_volume(63));
    launcher_media_controls::stop_volume_cache();
    assert(launcher_media_controls::cached_volume() == 63);

    assert(launcher_media_controls::adjust_brightness(-10) == 30);
    assert(backlight_value == 76);
    assert(configured_brightness == 76);

    // Simulate Settings changing the shared hardware/config value after the shortcut cache exists.
    backlight_value = 178;
    configured_brightness = 178;
    assert(launcher_media_controls::adjust_brightness(-10) == 60);
    assert(backlight_value == 153);
    assert(configured_brightness == 153);
    assert(launcher_media_controls::adjust_brightness(10) == 70);
    assert(backlight_value == 178);

    backlight_value = 25;
    configured_brightness = 25;
    assert(launcher_media_controls::adjust_brightness(-10) == 10);
    assert(backlight_value == 25);
    assert(backlight_value > 0);

    backlight_value = 255;
    configured_brightness = 255;
    assert(launcher_media_controls::adjust_brightness(10) == 100);
    assert(backlight_value == 255);

    // Screen-off suspends the panel without touching the persisted brightness.
    backlight_value = 178;
    configured_brightness = 178;
    const int suspended = launcher_media_controls::suspend_backlight();
    assert(suspended == 178);
    assert(backlight_value == 0);
    assert(configured_brightness == 178);

    launcher_media_controls::restore_backlight(suspended);
    assert(backlight_value == 178);
    assert(configured_brightness == 178);

    // Nothing captured means there is nothing to restore.
    launcher_media_controls::restore_backlight(0);
    launcher_media_controls::restore_backlight(-1);
    assert(backlight_value == 178);

    // A zero reading carries no restore information, so the persisted step is
    // used instead of leaving the panel dark after wake-up.
    backlight_value = 0;
    const int fallback = launcher_media_controls::suspend_backlight();
    assert(fallback > 0 && fallback <= backlight_maximum);
    assert(backlight_value == 0);
    launcher_media_controls::restore_backlight(fallback);
    assert(backlight_value == fallback);
    assert(configured_brightness == 178);

    // Restart after screen-off must restore the saved raw value, even when it
    // comes from a legacy brightness level outside the current 10% steps.
    const int previous_config_writes = config_writes;
    configured_brightness = 64;
    backlight_value = 0;
    assert(launcher_media_controls::restore_startup_backlight());
    assert(backlight_value == 64);
    assert(configured_brightness == 64);

    for (int saved : {0, -1, 999}) {
        configured_brightness = saved;
        backlight_value = 0;
        assert(launcher_media_controls::restore_startup_backlight());
        assert(backlight_value == backlight_maximum);
        assert(configured_brightness == saved);
    }
    brightness_config_missing = true;
    backlight_value = 0;
    assert(launcher_media_controls::restore_startup_backlight());
    assert(backlight_value == backlight_maximum);
    brightness_config_missing = false;

    backlight_write_failed = true;
    backlight_value = 0;
    assert(!launcher_media_controls::restore_startup_backlight());
    assert(backlight_value == 0);
    backlight_write_failed = false;
    assert(config_writes == previous_config_writes);

    sink_volume = configured_volume = 60;
    sink_muted = true;
    config_save_throws = true;
    const auto rolled_back = launcher_media_controls::adjust_volume(-10);
    assert(rolled_back.percent == 60 && rolled_back.mute_known && rolled_back.muted);
    assert(sink_volume == 60 && sink_muted && configured_volume == 60);
    config_save_throws = false;
    assert(launcher_media_controls::adjust_volume(-10).percent == 50);
    assert(sink_volume == 50 && !sink_muted && configured_volume == 50);
    std::fprintf(stderr, "throwing persist: restored muted 60; next step saved 50\n");

    cp0_signal_audio_api.remove(audio);
    cp0_signal_config_api.remove(config);
    test_startup_restore_during_shortcut();
    test_settings_write_during_shortcut();
    test_startup_restore_after_failed_read();
}
