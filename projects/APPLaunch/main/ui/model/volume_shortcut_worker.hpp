/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "cp0_audio_volume_step.hpp"

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

class VolumeShortcutWorker
{
public:
    using Result = cp0::audio::VolumeStepResult;
    using Execute = std::function<Result(int)>;

    explicit VolumeShortcutWorker(Execute execute) : execute_(std::move(execute)) {}
    ~VolumeShortcutWorker() { stop(); }

    void start()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = false;
    }

    bool enqueue(int delta, uint32_t repeat_key = 0)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            // Preserve FIFO and reserve the last slot for explicit Mute.
            const auto limit = delta == 0 ? kMaxPending : kMaxPending - 1;
            if (stopping_ || pending_.size() >= limit) return false;
            if (repeat_key != 0 && std::any_of(pending_.begin(), pending_.end(),
                [delta](const Command &command) {
                    return command.repeat_key != 0 && command.delta == delta;
                })) return false;
            if (!worker_.joinable()) worker_ = std::thread([this] { run(); });
            pending_.push_back({delta, repeat_key});
        }
        wake_.notify_one();
        return true;
    }

    void release(uint32_t key)
    {
        if (key == 0) return;
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.erase(std::remove_if(pending_.begin(), pending_.end(),
            [key](const Command &command) { return command.repeat_key == key; }), pending_.end());
    }

    void close()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
            pending_.clear();
            result_.reset();
        }
        wake_.notify_one();
    }

    void stop()
    {
        close();
        if (worker_.joinable()) worker_.join();
    }

    std::optional<Result> take_result(bool &idle)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        idle = !busy_ && pending_.empty();
        return std::exchange(result_, std::nullopt);
    }

private:
    static constexpr std::size_t kMaxPending = 4;
    struct Command {
        int delta;
        uint32_t repeat_key;
    };

    void run()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        while (true) {
            wake_.wait(lock, [this] { return stopping_ || !pending_.empty(); });
            if (stopping_) return;

            const int delta = pending_.front().delta;
            pending_.pop_front();
            busy_ = true;
            lock.unlock();
            std::optional<Result> result;
            try {
                result = execute_(delta);
            } catch (...) {
            }
            lock.lock();
            if (result && !stopping_) result_ = result;
            busy_ = false;
        }
    }

    Execute execute_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Command> pending_;
    // A burst only needs its latest completed result painted.
    std::optional<Result> result_;
    bool busy_ = false;
    bool stopping_ = true;
    std::thread worker_;
};
