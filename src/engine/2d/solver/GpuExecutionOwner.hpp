// SPDX-License-Identifier: Apache-2.0

/**
 * @file      GpuExecutionOwner.hpp
 * @brief     Host-thread ownership checks for core-controlled Kokkos operations.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#pragma once
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>

namespace openswmm::twoD {
// All core-controlled Kokkos entry points share one controlling host thread.
// Calls are synchronous to the engine: borrowed buffers remain valid until
// completion, and two GUI workers cannot initialize/dispatch concurrently.
class GpuExecutionOwner {
public:
    /**
     * @brief Acquire the process-resident controlling thread for matched GPU plugins.
     * @return Shared ownership handle to the one intentionally resident owner; a static
     * allocation retains it through process teardown.
     * @note First use allocates and starts a thread and may throw. Factory must pin the
     * containing module before first use, avoiding thread join under the Windows loader lock.
     * Later acquisitions are synchronized by C++ static initialization.
     */
    static std::shared_ptr<GpuExecutionOwner> shared() {
        // Deliberately process-resident, like the Kokkos runtime/plugins.
        // A static shared_ptr destructor would join during DLL teardown on
        // Windows, where thread synchronization can deadlock on the loader
        // lock. The factory pins the containing module before first use.
        static const auto* owner =
            new std::shared_ptr<GpuExecutionOwner>(std::make_shared<GpuExecutionOwner>());
        return *owner;
    }
    /**
     * @brief Start a dedicated synchronous-dispatch worker.
     * @post Constructs an empty queue and a worker running work(). Thread creation may throw
     * std::system_error; no partially live owner is returned.
     * @note Normally use shared(). This constructor does not initialize Kokkos or load a plugin.
     */
    GpuExecutionOwner() : worker_([this] { work(); }) {}
    /**
     * @brief Stop and join a nonresident execution owner after queued work drains.
     * @pre No concurrent or later invoke() calls; destructor must run outside its worker thread.
     * Never destroy under a DLL loader lock.
     * @post Sets stopping, wakes the worker, drains already queued tasks and joins it.
     * Process-resident shared() instances intentionally avoid teardown-time destruction.
     */
    ~GpuExecutionOwner() {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        ready_.notify_one();
        worker_.join();
    }
    GpuExecutionOwner(const GpuExecutionOwner&) = delete;
    /**
     * @brief Execute one callable on the controlling thread and wait for completion.
     * @tparam F Callable type; must be invocable with no arguments.
     * @param[in] fn Forwarded callable, moved/copied into an owned queued task; captures may
     * borrow buffers only until invoke returns.
     * @return Callable result, including void/reference results according to
     * std::invoke_result_t<F>. Exceptions from the callable are rethrown through the future;
     * allocation/queue failures may also propagate.
     * @note Reentrant calls from the worker execute inline. Other callers enqueue under a mutex
     * and block; queued calls are serialized. Owner must remain alive, and destruction must not
     * race this call. Does not retain caller buffers after the synchronous result is delivered.
     */
    template <class F> auto invoke(F&& fn) -> std::invoke_result_t<F> {
        if (std::this_thread::get_id() == worker_.get_id())
            return fn();
        using R = std::invoke_result_t<F>;
        auto task = std::make_shared<std::packaged_task<R()>>(std::forward<F>(fn));
        auto result = task->get_future();
        {
            std::lock_guard lock(mutex_);
            queue_.emplace_back([task] { (*task)(); });
        }
        ready_.notify_one();
        return result.get();
    }

private:
    /**
     * @brief Drain queued packaged tasks on the execution-owner thread.
     * @post Waits for stop or work, removes one task under the mutex, then executes outside the
     * lock. Exits once the queue is empty after stopping; already queued work is drained.
     * @note Private worker entry; packaged tasks capture callable exceptions for their waiting
     * callers. Queue/task storage is owner-owned; no model pointers are interpreted here.
     */
    void work() {
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock lock(mutex_);
                ready_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
                if (queue_.empty())
                    return;
                task = std::move(queue_.front());
                queue_.pop_front();
            }
            task();
        }
    }
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<std::function<void()>> queue_;
    bool stopping_ = false;
    std::thread worker_;
};
}
