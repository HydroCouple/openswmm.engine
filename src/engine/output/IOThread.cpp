/**
 * @file IOThread.cpp
 * @brief IOThread — producer/consumer output writer implementation.
 *
 * @see IOThread.hpp
 * @ingroup engine_io
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#include "IOThread.hpp"
#include "../plugins/PluginFactory.hpp"

namespace openswmm {

// ============================================================================
// Constructor / Destructor
// ============================================================================

IOThread::IOThread(PluginFactory& factory, std::size_t capacity)
    : factory_(factory)
    , capacity_(capacity)
{}

IOThread::~IOThread() {
    stop();
}

// ============================================================================
// start()
// ============================================================================

void IOThread::start() {
    if (running_.load(std::memory_order_relaxed)) return;
    stop_flag_.store(false, std::memory_order_relaxed);
    thread_ = std::thread(&IOThread::run, this);
}

// ============================================================================
// post()  — called from main simulation thread
// ============================================================================

void IOThread::post(SimulationSnapshot snap) {
    {
        std::unique_lock<std::mutex> lock(mutex_);
        // Block if the queue is full (back-pressure on the sim thread)
        cv_not_full_.wait(lock, [this] {
            return queue_.size() < capacity_ ||
                   stop_flag_.load(std::memory_order_relaxed);
        });

        if (stop_flag_.load(std::memory_order_relaxed)) return;

        queue_.emplace(std::move(snap), next_sequence_++);
    }
    cv_not_empty_.notify_one();
}

// ============================================================================
// stop()
// ============================================================================

void IOThread::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_flag_.store(true, std::memory_order_relaxed);
    }
    cv_not_empty_.notify_all();
    cv_not_full_.notify_all();

    if (thread_.joinable()) {
        thread_.join();
    }
    running_.store(false, std::memory_order_relaxed);
}

// ============================================================================
// run()  — worker thread body
// ============================================================================

void IOThread::run() {
    running_.store(true, std::memory_order_relaxed);

    while (true) {
        WriteTask task;

        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_not_empty_.wait(lock, [this] {
                return !queue_.empty() ||
                       stop_flag_.load(std::memory_order_relaxed);
            });

            // If stopped and queue is empty, exit
            if (queue_.empty()) break;

            task = std::move(queue_.front());
            queue_.pop();
        }

        // Notify producer that space is available
        cv_not_full_.notify_one();

        // Deliver to all plugins
        const int rc = factory_.update_all(task.snapshot);
        if (rc != 0) {
            last_error_.store(rc, std::memory_order_relaxed);
        }
        tasks_completed_.fetch_add(1, std::memory_order_relaxed);
    }

    running_.store(false, std::memory_order_relaxed);
}

} /* namespace openswmm */
