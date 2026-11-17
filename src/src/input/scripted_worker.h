// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <utility>

namespace Input::Scripted {

/// Owns the thread that replays an input script. The script runs alongside the title, so it has
/// to be stopped before the VR runtime and the controllers it reads are taken down, and the stop
/// must not hang the process if the thread is stuck in a driver call: hence one owned worker
/// (never detached while the emulator runs) and a bounded wait for it to finish.
///
/// The body passed to Start should check StopRequested() often and return promptly once it is
/// set. It runs once; Start may be called again after Stop.
class Worker {
public:
    Worker() = default;
    ~Worker() {
        Stop();
    }

    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;

    /// Whether a worker is running (started and not yet returned).
    bool Running() const {
        return running.load(std::memory_order_relaxed);
    }

    /// The stop flag the body polls. Set by Stop.
    bool StopRequested() const {
        return stop.load(std::memory_order_relaxed);
    }

    /// Starts `body` on its own thread. False when a worker is already running; a worker that
    /// finished but was never joined is reaped first.
    template <typename Body>
    bool Start(Body body) {
        std::unique_lock lock{mutex};
        if (thread.joinable()) {
            if (!finished) {
                return false;
            }
            lock.unlock();
            thread.join();
            lock.lock();
        }
        stop.store(false, std::memory_order_relaxed);
        finished = false;
        running.store(true, std::memory_order_relaxed);
        thread = std::thread{[this, body = std::move(body)]() mutable {
            try {
                body();
            } catch (...) {
                // A script thread that throws must not terminate the process; the thread is
                // simply considered over and shutdown continues.
            }
            running.store(false, std::memory_order_relaxed);
            {
                std::scoped_lock lock{mutex};
                finished = true;
            }
            finished_cv.notify_all();
        }};
        return true;
    }

    /// Asks the body to return, then waits up to `timeout` for it and joins it.
    /// Returns false if it did not finish in time; the thread is then left detached (a bounded
    /// wait is safer than a shutdown that can never complete). Safe to call when no worker was
    /// ever started, and safe to call again after a timeout.
    bool Stop(std::chrono::milliseconds timeout = std::chrono::seconds(2)) {
        std::unique_lock lock{mutex};
        stop.store(true, std::memory_order_relaxed);
        if (!thread.joinable()) {
            return true;
        }
        if (!finished_cv.wait_for(lock, timeout, [this] { return finished; })) {
            // Leave Running() as it is: the body is still going somewhere, out of our hands.
            thread.detach();
            return false;
        }
        // The body has already signalled that it is over, so this join does not wait on it.
        thread.join();
        return true;
    }

private:
    mutable std::mutex mutex;
    std::condition_variable finished_cv;
    std::thread thread;
    std::atomic<bool> stop{false};
    std::atomic<bool> running{false};
    bool finished{true}; ///< Guarded by `mutex`; true before the first Start.
};

} // namespace Input::Scripted
