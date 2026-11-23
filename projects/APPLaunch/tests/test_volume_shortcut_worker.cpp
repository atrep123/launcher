/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "../main/ui/model/volume_shortcut_worker.hpp"

#include <cassert>
#include <chrono>
#include <future>
#include <vector>

namespace {
using namespace std::chrono_literals;
constexpr uint32_t kDown = 114;
constexpr uint32_t kUp = 115;

void test_hold_release_and_mute()
{
    std::promise<void> entered, release, muted;
    auto running = entered.get_future();
    auto proceed = release.get_future();
    auto finished = muted.get_future();
    std::vector<int> executed;
    VolumeShortcutWorker worker([&](int delta) {
        executed.push_back(delta);
        if (executed.size() == 1) {
            entered.set_value();
            assert(proceed.wait_for(5s) == std::future_status::ready);
        }
        if (delta == 0) muted.set_value();
        return VolumeShortcutWorker::Result{};
    });

    worker.start();
    assert(worker.enqueue(-10));
    assert(running.wait_for(5s) == std::future_status::ready);
    int repeats = 0;
    for (int i = 0; i < 400; ++i) {
        repeats += worker.enqueue(-10, kDown);
        repeats += worker.enqueue(10, kUp);
    }
    assert(repeats == 2);
    assert(worker.enqueue(10));
    assert(!worker.enqueue(-10));
    assert(worker.enqueue(0)); // Mute is admitted even with all volume slots in use.
    assert(!worker.enqueue(0));
    worker.release(kDown); // Discard the held key's repeat, keeping explicit presses.
    release.set_value();
    assert(finished.wait_for(5s) == std::future_status::ready);
    worker.stop();
    assert((executed == std::vector<int>{-10, 10, 10, 0}));
}

void test_close_with_pending_commands()
{
    std::promise<void> entered, release;
    auto running = entered.get_future();
    auto proceed = release.get_future();
    std::vector<int> executed;
    VolumeShortcutWorker worker([&](int delta) {
        executed.push_back(delta);
        if (executed.size() == 1) {
            entered.set_value();
            assert(proceed.wait_for(5s) == std::future_status::ready);
        }
        return VolumeShortcutWorker::Result{50, false};
    });

    worker.start();
    assert(worker.enqueue(-10));
    assert(running.wait_for(5s) == std::future_status::ready);
    assert(worker.enqueue(-10, kDown));
    assert(worker.enqueue(10, kUp));
    assert(worker.enqueue(10));
    assert(worker.enqueue(0));
    worker.close();
    assert(!worker.enqueue(0));
    release.set_value();
    worker.stop();

    bool idle;
    assert(!worker.take_result(idle) && idle);
    assert((executed == std::vector<int>{-10}));
}
} // namespace

int main()
{
    test_hold_release_and_mute();
    test_close_with_pending_commands();
}
