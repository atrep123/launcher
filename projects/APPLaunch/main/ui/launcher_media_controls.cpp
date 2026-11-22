/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "launcher_media_controls.h"

#include "hal_lvgl_bsp.h"
#if defined(CONFIG_CP0_LVGL_INIT_AUDIO) && !defined(HAL_PLATFORM_SDL)
#include "cp0_audio_volume_monitor.hpp"
#endif
#include "model/brightness_operation.hpp"
#include "model/launcher_media_model.hpp"
#include "model/setup_value_policy.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace {

LauncherMediaControlsModel model;

// These values are shared by the UI and the mixer worker. The revision
// prevents an in-flight read from overwriting a newer Settings or OSD update.
struct VolumeCache {
    std::mutex mutex;
    int volume = -1;
    std::uint64_t next_revision = 0;
    std::uint64_t applied_revision = 0;
    bool subscription_current = false;
};

VolumeCache volume_cache;

constexpr int kStartupVolumeReadAttempts = 5;
constexpr std::chrono::milliseconds kStartupVolumeReadInterval{400};

int read_mixer_volume()
{
    int volume = -1;
    try {
        cp0_signal_audio_api({"VolumeRead"}, [&](int code, std::string data) {
            int parsed = 0;
            if (code == 0 && LauncherMediaControlsModel::parse_percent(data, parsed)) volume = parsed;
        });
    } catch (...) {
        return -1;
    }
    return volume;
}

std::uint64_t begin_volume_read()
{
    std::lock_guard<std::mutex> lock(volume_cache.mutex);
    return ++volume_cache.next_revision;
}

void publish_volume_locked(int volume, std::uint64_t revision)
{
    volume_cache.volume = LauncherMediaControlsModel::clamp_percent(volume);
    volume_cache.applied_revision = revision;
}

int refresh_mixer_volume(bool from_subscription = false)
{
    std::uint64_t revision;
    {
        std::lock_guard<std::mutex> lock(volume_cache.mutex);
        revision = ++volume_cache.next_revision;
        if (from_subscription) volume_cache.subscription_current = false;
    }

    const int volume = read_mixer_volume();
    std::lock_guard<std::mutex> lock(volume_cache.mutex);
    if (volume >= 0 && revision > volume_cache.applied_revision)
        publish_volume_locked(volume, revision);
    if (from_subscription || volume < 0) volume_cache.subscription_current = volume >= 0;
    return volume_cache.volume;
}

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
        }
        worker_ = std::thread([this] {
            try {
                read_startup_volume();
#if defined(CONFIG_CP0_LVGL_INIT_AUDIO) && !defined(HAL_PLATFORM_SDL)
                follow_mixer();
#endif
            } catch (...) {
            }
            std::lock_guard<std::mutex> lock(volume_cache.mutex);
            volume_cache.subscription_current = false;
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

private:
    bool wait_for_stop(std::chrono::milliseconds interval)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        return wake_.wait_for(lock, interval, [this] { return stop_.load(); });
    }

    void publish_startup_volume(int volume, std::uint64_t revision)
    {
        std::lock_guard<std::mutex> lock(volume_cache.mutex);
        if (revision > volume_cache.applied_revision && !stop_.load())
            publish_volume_locked(volume, revision);
    }

    void read_startup_volume()
    {
        const auto revision = begin_volume_read();
        int previous = -1;
        int last_valid = -1;
        for (int attempt = 0; attempt < kStartupVolumeReadAttempts; ++attempt) {
            if (attempt > 0 && wait_for_stop(kStartupVolumeReadInterval))
                return;
            if (stop_.load())
                return;

            const int volume = read_mixer_volume();
            if (volume < 0) {
                previous = -1;
                continue;
            }
            last_valid = volume;
            if (volume == previous) {
                publish_startup_volume(volume, revision);
                return;
            }
            previous = volume;
        }
        if (last_valid >= 0)
            publish_startup_volume(last_valid, revision);
    }

#if defined(CONFIG_CP0_LVGL_INIT_AUDIO) && !defined(HAL_PLATFORM_SDL)
    void follow_mixer()
    {
        while (!stop_.load()) {
            cp0::pulse::watch_default_sink_changes(stop_, [] { refresh_mixer_volume(true); });
            {
                std::lock_guard<std::mutex> lock(volume_cache.mutex);
                volume_cache.subscription_current = false;
            }
            if (wait_for_stop(std::chrono::seconds(2))) return;
        }
    }
#endif

    std::mutex mutex_;
    std::condition_variable wake_;
    std::atomic<bool> stop_{false};
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

int read_volume()
{
    {
        std::lock_guard<std::mutex> lock(volume_cache.mutex);
        if (volume_cache.subscription_current && volume_cache.volume >= 0)
            return volume_cache.volume;
    }

    const int volume = refresh_mixer_volume();
    if (volume >= 0) return volume;

    const int fallback = read_config_int("volume", model.volume_or(50));
    std::lock_guard<std::mutex> lock(volume_cache.mutex);
    if (volume_cache.volume < 0)
        publish_volume_locked(fallback, ++volume_cache.next_revision);
    return volume_cache.volume;
}

bool read_mute()
{
    bool muted = model.muted();
    cp0_signal_audio_api({"MuteRead"}, [&](int code, std::string data) {
        int parsed = 0;
        if (code == 0 && LauncherMediaControlsModel::parse_int(data, parsed) &&
            (parsed == 0 || parsed == 1)) {
            muted = parsed != 0;
            model.set_mute(muted);
        }
    });
    return muted;
}

int write_volume(int previous, int percent)
{
    percent = LauncherMediaControlsModel::clamp_percent(percent);
    int written = -1;
    cp0_signal_audio_api({"VolumeWrite", std::to_string(percent)}, [&](int code, std::string data) {
        int parsed = 0;
        if (code == 0 && LauncherMediaControlsModel::parse_percent(data, parsed)) written = parsed;
    });
    if (written < 0) return previous;
    if (!write_config_int("volume", written, previous)) {
        cp0_signal_audio_api({"VolumeWrite", std::to_string(previous)}, nullptr);
        return previous;
    }
    model.set_volume(written);
    launcher_media_controls::update_cached_volume(written);
    return written;
}

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
    std::lock_guard<std::mutex> lock(volume_cache.mutex);
    return volume_cache.volume;
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
    refresh_mixer_volume();
}

void update_cached_volume(int percent)
{
    std::lock_guard<std::mutex> lock(volume_cache.mutex);
    publish_volume_locked(percent, ++volume_cache.next_revision);
}

int adjust_volume(int delta_percent)
{
    if (read_mute())
        (void)toggle_mute();
    const int current = read_volume();
    const int base = setup_values::round_volume_percent(current);
    const int delta = delta_percent > 0 ? VOLUME_STEP_PERCENT
                                       : delta_percent < 0 ? -VOLUME_STEP_PERCENT : 0;
    return write_volume(current, base + delta);
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
