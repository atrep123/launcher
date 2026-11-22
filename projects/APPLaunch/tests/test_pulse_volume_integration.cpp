/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 * SPDX-License-Identifier: MIT
 */

#include "cp0_audio_volume_monitor.hpp"
#include "cp0_pulse_client.hpp"
#include "hal_lvgl_bsp.h"
#include "launcher_media_controls.h"

#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>

using AudioSignal = eventpp::CallbackList<void(std::list<std::string>,
                                              std::function<void(int, std::string)>)>;
AudioSignal cp0_signal_audio_api;
AudioSignal cp0_signal_config_api;
AudioSignal cp0_signal_settings_api;

namespace {

using namespace std::chrono_literals;
std::mutex watcher_mutex;
std::condition_variable watcher_changed;
bool hold_notification = false;
bool notification_waiting = false;
unsigned delivered_notifications = 0;
int saved_volume = 100;

void run(const char *command)
{
    assert(std::system(command) == 0);
}

void start_server()
{
    run("pulseaudio --daemonize=yes -n --exit-idle-time=-1 "
        "--log-target=file:\"$VOLUME_TEST_DIR/server.log\" -F \"$VOLUME_TEST_DIR/pulse.conf\"");
}

void wait_for_volume(int expected)
{
    const auto deadline = std::chrono::steady_clock::now() + 6s;
    while (launcher_media_controls::cached_volume() != expected &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(10ms);
    assert(launcher_media_controls::cached_volume() == expected);
}

} // namespace

namespace cp0::pulse {

void watch_test_server(const std::atomic<bool> &, std::atomic<bool> &,
                       const std::function<void()> &);

void watch_default_sink_changes(const std::atomic<bool> &stop, std::atomic<bool> &requested,
                                const std::function<void()> &refresh)
{
    watch_test_server(stop, requested, [&] {
        std::unique_lock<std::mutex> lock(watcher_mutex);
        if (hold_notification) {
            notification_waiting = true;
            watcher_changed.notify_all();
            assert(watcher_changed.wait_for(lock, 6s, [] { return !hold_notification; }));
        }
        lock.unlock();
        refresh();
        lock.lock();
        ++delivered_notifications;
        watcher_changed.notify_all();
    });
}

} // namespace cp0::pulse

int main()
{
    using namespace cp0::pulse;
    using namespace launcher_media_controls;

    cp0_signal_audio_api.append([](auto args, auto reply) {
        assert(args.front() == "VolumeRead");
        const int value = read_default_sink_volume();
        reply(value < 0 ? -1 : 0, std::to_string(value));
    });
    cp0_signal_config_api.append([](auto args, auto reply) {
        if (args.front() == "SetInt") saved_volume = std::stoi(args.back());
        reply(0, args.front() == "GetInt" ? std::to_string(saved_volume) : "");
    });

    start_server();
    run("pactl set-sink-volume launcher_a 100%");
    run("pactl set-sink-volume launcher_b 60%");
    start_volume_cache();
    wait_for_volume(100);
    {
        std::unique_lock<std::mutex> lock(watcher_mutex);
        assert(watcher_changed.wait_for(lock, 6s, [] { return delivered_notifications > 0; }));
        hold_notification = true;
    }
    run("pactl set-sink-volume launcher_a 20%");
    {
        std::unique_lock<std::mutex> lock(watcher_mutex);
        assert(watcher_changed.wait_for(lock, 6s, [] { return notification_waiting; }));
    }
    assert(cached_volume() == 100);
    assert(adjust_volume(-10) == 10);
    assert(read_default_sink_volume() == 10 && saved_volume == 10 && cached_volume() == 10);
    {
        std::lock_guard<std::mutex> lock(watcher_mutex);
        hold_notification = false;
        watcher_changed.notify_all();
    }
    std::puts("PASS delayed notification: 100% cached, 20% mixer -> 10%");

    run("pactl set-sink-volume launcher_a 20%");
    assert(adjust_default_sink_volume(
               [](int current) {
                   assert(current == 20);
                   run("pactl set-default-sink launcher_b");
                   return current - 10;
               },
               [](int previous, int written) { return previous == 20 && written == 10; }) == 10);
    assert(read_default_sink_volume() == 60);
    wait_for_volume(60);
    run("pactl set-default-sink launcher_a");
    assert(read_default_sink_volume() == 10);
    wait_for_volume(10);
    std::puts("PASS sink switch between read and write: A 20% -> 10%, B stays at 60%");

    // Persistence failure also restores the same sink after a default change.
    assert(adjust_default_sink_volume(
               [](int current) { return current + 10; },
               [](int, int) {
                   run("pactl set-default-sink launcher_b");
                   return false;
               }) == -1);
    assert(read_default_sink_volume() == 60);
    run("pactl set-default-sink launcher_a");
    assert(read_default_sink_volume() == 10);
    wait_for_volume(10);

    run("pactl exit");
    const auto before_home = std::chrono::steady_clock::now();
    refresh_volume_cache();
    assert(std::chrono::steady_clock::now() - before_home < 100ms);
    assert(adjust_volume(-10) == 10 && saved_volume == 10);
    start_server();
    run("pactl set-sink-volume launcher_a 40%");
    refresh_volume_cache();
    wait_for_volume(40);
    assert(adjust_volume(-10) == 30 && read_default_sink_volume() == 30 && saved_volume == 30);
    stop_volume_cache();
    std::puts("PASS HOME request returns during outage; reconnect reads 40% and steps to 30%");
}
