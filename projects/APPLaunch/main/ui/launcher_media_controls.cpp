/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "launcher_media_controls.h"

#include "cp0_audio_volume_monitor.hpp"
#include "hal_lvgl_bsp.h"
#include "model/brightness_operation.hpp"
#include "model/launcher_media_model.hpp"
#include "model/setup_value_policy.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

namespace {

LauncherMediaControlsModel model;

constexpr int kStartupVolumeReadAttempts = 5;
constexpr std::chrono::milliseconds kStartupVolumeReadInterval{400};

/* Keep the startup settle window for WirePlumber, then follow mixer events
 * on the same worker. Teardown joins it before the audio signal is released. */
class VolumeCacheWorker
{
public:
    ~VolumeCacheWorker() { stop(); }

    void start()
    {
        stop();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = false;
            refresh_requested_ = invalidate_;
        }
        worker_ = std::thread([this] {
            try {
                read_startup_volume();
#if defined(CONFIG_CP0_LVGL_INIT_AUDIO) && !defined(HAL_PLATFORM_SDL)
                follow_mixer();
#else
                while (wait_for_refresh()) refresh_if_requested();
#endif
            } catch (...) {
            }
        });
    }

    void stop()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        wake_.notify_all();
        if (worker_.joinable())
            worker_.join();
    }

    int volume() const { return volume_.load(); }

    void request_refresh(bool invalidate = false)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stop_.load()) return;
            invalidate_ |= invalidate;
            refresh_requested_ = true;
        }
        wake_.notify_one();
    }

private:
    bool wait_for_stop(std::chrono::milliseconds interval)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        return wake_.wait_for(lock, interval, [this] { return stop_.load(); });
    }

    bool wait_for_refresh(std::chrono::milliseconds timeout = std::chrono::milliseconds::max())
    {
        std::unique_lock<std::mutex> lock(mutex_);
        const auto ready = [this] { return stop_.load() || refresh_requested_.load(); };
        if (timeout == std::chrono::milliseconds::max())
            wake_.wait(lock, ready);
        else
            wake_.wait_for(lock, timeout, ready);
        return !stop_.load();
    }

    void publish_if_running(int volume)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!stop_.load() && !invalidate_ && volume >= 0)
            volume_ = LauncherMediaControlsModel::clamp_percent(volume);
    }

    void refresh_mixer_volume()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stop_.load()) return;
            if (invalidate_) {
                volume_ = -1;
                invalidate_ = false;
            }
        }
        publish_if_running(launcher_media_controls::read_mixer_volume(&stop_));
    }

    void refresh_if_requested()
    {
        if (refresh_requested_.exchange(false) && !stop_.load())
            refresh_mixer_volume();
    }

    void read_startup_volume()
    {
        int previous = -1;
        int last_valid = -1;
        for (int attempt = 0; attempt < kStartupVolumeReadAttempts; ++attempt) {
            if (attempt > 0 && wait_for_stop(kStartupVolumeReadInterval))
                return;
            if (stop_.load()) return;

            const auto sample = launcher_media_controls::read_mixer_volume(&stop_);
            if (sample < 0) {
                previous = -1;
                continue;
            }
            last_valid = sample;
            if (sample == previous) break;
            previous = sample;
        }
        publish_if_running(last_valid);
    }

#if defined(CONFIG_CP0_LVGL_INIT_AUDIO) && !defined(HAL_PLATFORM_SDL)
    void follow_mixer()
    {
        while (!stop_.load()) {
            cp0::pulse::watch_default_sink_changes(stop_, refresh_requested_, [this] { refresh_mixer_volume(); });
            if (!wait_for_refresh(std::chrono::seconds(2))) return;
            // Explicit HOME requests also work when a subscription could not be established.
            refresh_if_requested();
        }
    }
#endif

    std::mutex mutex_;
    std::condition_variable wake_;
    std::atomic<int> volume_{-1};
    bool invalidate_ = false;
    std::atomic<bool> stop_{true};
    std::atomic<bool> refresh_requested_{false};
    std::thread worker_;
};

VolumeCacheWorker volume_cache_worker;

int read_config_int(const char *key, int fallback)
{
    int value = fallback;
    cp0_signal_config_api({"GetInt", key, std::to_string(fallback)},
                          [&](int code, std::string data) {
                              int parsed = 0;
                              if (code == 0 && LauncherMediaControlsModel::parse_int(data, parsed))
                                  value = parsed;
                          });
    return value;
}

bool call_config(const std::list<std::string> &arguments)
{
    bool succeeded = false;
    cp0_signal_config_api(arguments, [&](int code, std::string) { succeeded = code == 0; });
    return succeeded;
}

bool write_config_int(const char *key, int value, int fallback)
{
    const int previous = read_config_int(key, fallback);
    if (!call_config({"SetInt", key, std::to_string(value)})) return false;
    if (call_config({"Save"})) return true;

    if (call_config({"SetInt", key, std::to_string(previous)})) call_config({"Save"});
    return false;
}

int volume_after_step(int current, int delta_percent)
{
    const int base = setup_values::round_volume_percent(current);
    const int delta = delta_percent > 0 ? launcher_media_controls::VOLUME_STEP_PERCENT
                    : delta_percent < 0 ? -launcher_media_controls::VOLUME_STEP_PERCENT : 0;
    return LauncherMediaControlsModel::clamp_percent(base + delta);
}

bool persist_volume(int previous, int written)
{
    return write_config_int("volume", written, previous);
}

#if !defined(CONFIG_CP0_LVGL_INIT_AUDIO) || defined(HAL_PLATFORM_SDL)
int adjust_local_volume(int delta_percent)
{
    const int previous = launcher_media_controls::read_mixer_volume();
    if (previous < 0) return -1;
    const int target = volume_after_step(previous, delta_percent);
    int written = -1;
    cp0_signal_audio_api({"VolumeWrite", std::to_string(target)}, [&](int code, std::string data) {
        int parsed = 0;
        if (code == 0 && LauncherMediaControlsModel::parse_percent(data, parsed))
            written = parsed;
    });
    if (written < 0) return -1;
    if (!persist_volume(previous, written)) {
        int restored = cp0::pulse::kVolumeRollbackFailed;
        cp0_signal_audio_api({"VolumeWrite", std::to_string(previous)}, [&](int code, std::string) {
            if (code == 0) restored = -1;
        });
        return restored;
    }

    cp0_signal_audio_api({"MuteRead"}, [](int code, std::string data) {
        if (code == 0 && data == "1")
            (void)launcher_media_controls::toggle_mute();
    });
    return written;
}
#endif

int backlight_max()
{
    int maximum = 100;
    cp0_signal_settings_api({"BacklightMax"}, [&](int code, std::string data) {
        int parsed = 0;
        if (code == 0 && LauncherMediaControlsModel::parse_int(data, parsed) && parsed > 0)
            maximum = parsed;
    });
    return maximum > 0 ? maximum : 100;
}

int read_backlight_raw(int maximum)
{
    int raw = -1;
    cp0_signal_settings_api({"BacklightRead"}, [&](int code, std::string data) {
        int parsed = 0;
        if (code == 0 && LauncherMediaControlsModel::parse_int(data, parsed) && parsed >= 0)
            raw = parsed;
    });
    if (raw < 0) {
        const int fallback_index = setup_values::brightness_step_index(
            model.brightness_or(setup_values::kBrightnessMaxPercent));
        const int fallback = setup_values::brightness_step_value(fallback_index, maximum);
        raw = read_config_int(setup_values::kBrightnessConfigKey, fallback);
    }
    return raw;
}

int write_backlight_raw(int raw)
{
    int written = -1;
    cp0_signal_settings_api({"BacklightWrite", std::to_string(raw)}, [&](int code, std::string data) {
        int parsed = 0;
        if (code == 0 && LauncherMediaControlsModel::parse_int(data, parsed) && parsed >= 0)
            written = parsed;
    });
    return written;
}

int read_brightness()
{
    const int maximum = backlight_max();
    const int raw = read_backlight_raw(maximum);
    const int percent = setup_values::brightness_step_percent_from_raw(raw, maximum);
    model.set_brightness(percent);
    return percent;
}

int write_brightness(int previous_percent, int percent)
{
    const int target_index = setup_values::brightness_step_index(percent);
    const int maximum = backlight_max();
    const int raw = setup_values::brightness_step_value(target_index, maximum);

    const int written = write_backlight_raw(raw);
    if (written < 0) return previous_percent;
    const int previous_raw = setup_values::brightness_step_value(
        setup_values::brightness_step_index(previous_percent), maximum);
    if (!write_config_int(setup_values::kBrightnessConfigKey, written, previous_raw)) {
        cp0_signal_settings_api({"BacklightWrite", std::to_string(previous_raw)}, nullptr);
        return previous_percent;
    }
    const int applied_percent = setup_values::brightness_step_percent_from_raw(written, maximum);
    model.set_brightness(applied_percent);
    return applied_percent;
}

} // namespace

namespace launcher_media_controls {

bool has_cached_volume()
{
    return cached_volume() >= 0;
}

int cached_volume()
{
    return volume_cache_worker.volume();
}

void start_volume_cache()
{
    volume_cache_worker.start();
}

void stop_volume_cache()
{
    volume_cache_worker.stop();
}

void refresh_volume_cache()
{
    volume_cache_worker.request_refresh();
}

int read_mixer_volume(const std::atomic<bool> *stop_requested)
{
#if defined(CONFIG_CP0_LVGL_INIT_AUDIO) && !defined(HAL_PLATFORM_SDL)
    return cp0::pulse::read_default_sink_volume(stop_requested);
#else
    if (stop_requested && stop_requested->load()) return -1;
    int volume = -1;
    try {
        cp0_signal_audio_api({"VolumeRead"}, [&](int code, std::string data) {
            int parsed = 0;
            if (code == 0 && LauncherMediaControlsModel::parse_percent(data, parsed))
                volume = parsed;
        });
    } catch (...) {
        return -1;
    }
    return volume;
#endif
}

int adjust_volume(int delta_percent)
{
#if defined(CONFIG_CP0_LVGL_INIT_AUDIO) && !defined(HAL_PLATFORM_SDL)
    const int written = cp0::pulse::adjust_default_sink_volume(
        [delta_percent](int current) { return volume_after_step(current, delta_percent); },
        persist_volume);
#else
    const int written = adjust_local_volume(delta_percent);
#endif
    if (written == cp0::pulse::kVolumeRollbackFailed)
        volume_cache_worker.request_refresh(true);
    else if (written >= 0)
        refresh_volume_cache();

    if (written >= 0) {
        model.set_volume(written);
        return written;
    }

    // Keep the OSD usable during a disconnect without using its value for writes.
    const int cached = written == cp0::pulse::kVolumeRollbackFailed ? -1 : cached_volume();
    return cached >= 0 ? cached : read_config_int("volume", model.volume_or(50));
}

int adjust_brightness(int delta_percent)
{
    std::unique_lock<std::mutex> operation_lock(
        brightness_control::operation_mutex(), std::try_to_lock);
    if (!operation_lock.owns_lock()) {
        return setup_values::brightness_step_percent(
            setup_values::brightness_step_index(
                model.brightness_or(setup_values::kBrightnessMaxPercent)));
    }

    const int current = read_brightness();
    const int target = setup_values::brightness_step_percent_after(current, delta_percent);
    return write_brightness(current, target);
}

bool toggle_mute()
{
    const bool previous = model.muted();
    bool muted = previous;
    cp0_signal_audio_api({"MuteToggle"}, [&](int code, std::string data) {
        int parsed = 0;
        if (code == 0 && LauncherMediaControlsModel::parse_int(data, parsed) &&
            (parsed == 0 || parsed == 1)) {
            muted = parsed != 0;
            model.set_mute(muted);
        }
    });
    return muted;
}

bool restore_startup_backlight()
{
    std::lock_guard<std::mutex> operation_lock(brightness_control::operation_mutex());
    const int maximum = backlight_max();
    const int saved = read_config_int(setup_values::kBrightnessConfigKey, maximum);
    const int target = saved > 0 ? std::min(saved, maximum) : maximum;
    const int written = write_backlight_raw(target);
    if (written <= 0 || written > maximum) return false;
    model.set_brightness(setup_values::brightness_step_percent_from_raw(written, maximum));
    return written == target;
}

int suspend_backlight()
{
    std::lock_guard<std::mutex> operation_lock(brightness_control::operation_mutex());
    const int maximum = backlight_max();
    int previous = read_backlight_raw(maximum);
    if (previous <= 0) {
        /* A zero reading carries no restore information; fall back to the
         * persisted step so waking up cannot leave the panel dark. */
        const int fallback_index = setup_values::brightness_step_index(
            model.brightness_or(setup_values::kBrightnessMaxPercent));
        previous = setup_values::brightness_step_value(fallback_index, maximum);
    }
    if (write_backlight_raw(0) < 0) return -1;
    return previous;
}

void restore_backlight(int raw)
{
    if (raw <= 0) return;
    std::lock_guard<std::mutex> operation_lock(brightness_control::operation_mutex());
    (void)write_backlight_raw(raw);
}

} // namespace launcher_media_controls
