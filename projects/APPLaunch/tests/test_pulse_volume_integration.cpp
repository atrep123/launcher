/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 * SPDX-License-Identifier: MIT
 */

#include "cp0_audio_volume_monitor.hpp"
#include "cp0_pulse_client.hpp"
#include "hal_lvgl_bsp.h"
#include "launcher_media_controls.h"

#include <cassert>
#include <cstring>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

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
std::atomic<bool> hold_completed_read{false};
std::atomic<bool> sample_waiting{false};
bool passed = true;

void expect(bool condition, const char *scenario)
{
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", scenario);
    passed &= condition;
}

void wait_until(const std::atomic<bool> &flag)
{
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (!flag.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(1ms);
    assert(flag.load());
}

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

void exercise_stalled_read()
{
    // Accept a libpulse connection without replying to its handshake.
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const std::string path = std::string(std::getenv("VOLUME_TEST_DIR")) + "/stalled";
    assert(path.size() < sizeof(address.sun_path));
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    const int server = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    assert(server >= 0);
    assert(bind(server, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) == 0);
    assert(listen(server, 1) == 0);

    const char *original_server = std::getenv("PULSE_SERVER");
    assert(original_server);
    const std::string saved_server(original_server);
    assert(setenv("PULSE_SERVER", ("unix:" + path).c_str(), 1) == 0);
    launcher_media_controls::start_volume_cache();
    pollfd incoming{server, POLLIN, 0};
    assert(poll(&incoming, 1, 2000) == 1 && (incoming.revents & POLLIN));
    const int client = accept(server, nullptr, nullptr);
    assert(client >= 0);
    const auto start = std::chrono::steady_clock::now();
    launcher_media_controls::stop_volume_cache();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    std::printf("shutdown during PulseAudio read took %lld ms\n", static_cast<long long>(elapsed.count()));
    expect(elapsed < 200ms, "shutdown cancels the in-flight PulseAudio read");
    close(client);
    close(server);
    assert(setenv("PULSE_SERVER", saved_server.c_str(), 1) == 0);
}

} // namespace

namespace cp0::pulse {

VolumeSample read_test_server_volume_sample(const std::atomic<bool> *);

VolumeSample read_default_sink_volume_sample(const std::atomic<bool> *stop)
{
    const auto sample = read_test_server_volume_sample(stop);
    if (stop && sample.percent >= 0 && hold_completed_read.load()) {
        sample_waiting = true;
        // Return an already completed observation even if stop is requested
        // while it is in transit to the worker's cache publication.
        wait_until(*stop);
    }
    return sample;
}

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

    run("pactl set-sink-volume launcher_a 12345 31789");
    assert(adjust_default_sink_volume([](int) { return 55; }, [](int, int) { return false; }) == -1);
    expect(std::system("pactl get-sink-volume launcher_a | grep -Eq 'front-left: 12345 .*front-right: 31789 '") == 0,
           "rollback restores the exact unequal per-channel values");

    run("pactl set-sink-volume launcher_a 20%");
    assert(adjust_default_sink_volume(
               [](int) { return 55; },
               [](int, int) {
                   std::this_thread::sleep_for(2100ms);
                   return false;
               }) == -1);
    expect(read_default_sink_volume() == 20, "rollback has a fresh budget after the write deadline expires");

    VolumeSample rollback_sample;
    const int rollback = adjust_default_sink_volume(
        [](int) { return 55; },
        [](int, int) {
            run(R"SH(kill -STOP $(cat "$PULSE_RUNTIME_PATH/pid"))SH");
            return false;
        },
        &rollback_sample);
    run(R"SH(kill -CONT $(cat "$PULSE_RUNTIME_PATH/pid"))SH");
    expect(rollback == kVolumeRollbackFailed && rollback_sample.percent == -1 &&
               rollback_sample.observed_at != VolumeTime{},
           "an unconfirmed rollback returns a distinct status and an unknown mixer sample");

    // Reestablish the expected value before exercising the server outage.
    run("pactl set-sink-volume launcher_a 10%");
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
    wait_for_volume(30);
    hold_completed_read = true;
    run("pactl set-sink-volume launcher_a 40%");
    refresh_volume_cache();
    wait_until(sample_waiting);
    stop_volume_cache();
    hold_completed_read = false;
    expect(cached_volume() == 30, "a completed refresh cannot publish after shutdown");
    std::puts("PASS HOME request returns during outage; reconnect reads 40% and steps to 30%");

    // Settings starts first but does not access the mixer until after the worker read.
    run("pactl set-sink-volume launcher_a 100%");
    std::mutex settings_mutex;
    std::condition_variable settings_cv;
    bool settings_started = false;
    bool finish_settings = false;
    VolumeSample settings_sample;
    std::thread settings([&] {
        std::unique_lock<std::mutex> lock(settings_mutex);
        settings_started = true;
        settings_cv.notify_one();
        settings_cv.wait(lock, [&] { return finish_settings; });
        lock.unlock();
        settings_sample = read_mixer_volume();
    });
    {
        std::unique_lock<std::mutex> lock(settings_mutex);
        settings_cv.wait(lock, [&] { return settings_started; });
    }
    const auto worker_sample = read_mixer_volume();
    assert(worker_sample.percent == 100);
    assert(publish_mixer_volume(worker_sample) == 100);
    run("pactl set-sink-volume launcher_a 20%");
    {
        std::lock_guard<std::mutex> lock(settings_mutex);
        finish_settings = true;
    }
    settings_cv.notify_one();
    settings.join();
    assert(settings_sample.percent == 20);
    expect(publish_mixer_volume(settings_sample) == 20 && cached_volume() == 20,
           "Settings started first but observed 20% after the worker observed 100%");

    exercise_stalled_read();
    return passed ? 0 : 1;
}
