/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "cp0_audio_volume_step.hpp"

#include <condition_variable>
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

    void enqueue(int delta)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_) return;
            if (!worker_.joinable()) worker_ = std::thread([this] { run(); });
            pending_.push_back(delta);
        }
        wake_.notify_one();
    }

    void stop()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
            pending_.clear();
            result_.reset();
        }
        wake_.notify_one();
        if (worker_.joinable()) worker_.join();
    }

    std::optional<Result> take_result(bool &idle)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        idle = !busy_ && pending_.empty();
        return std::exchange(result_, std::nullopt);
    }

private:
    void run()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        while (true) {
            wake_.wait(lock, [this] { return stopping_ || !pending_.empty(); });
            if (stopping_) return;

            const int delta = pending_.front();
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
    std::deque<int> pending_;
    // A burst only needs its latest completed result painted.
    std::optional<Result> result_;
    bool busy_ = false;
    bool stopping_ = true;
    std::thread worker_;
};
