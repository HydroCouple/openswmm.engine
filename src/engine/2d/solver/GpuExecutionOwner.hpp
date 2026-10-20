// SPDX-License-Identifier: Apache-2.0
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
    static std::shared_ptr<GpuExecutionOwner> shared() {
        // Deliberately process-resident, like the Kokkos runtime/plugins.
        // A static shared_ptr destructor would join during DLL teardown on
        // Windows, where thread synchronization can deadlock on the loader
        // lock. The factory pins the containing module before first use.
        static const auto* owner = new std::shared_ptr<GpuExecutionOwner>(
            std::make_shared<GpuExecutionOwner>());
        return *owner;
    }
    GpuExecutionOwner() : worker_([this] { work(); }) {}
    ~GpuExecutionOwner() {
        { std::lock_guard lock(mutex_); stopping_ = true; }
        ready_.notify_one();
        worker_.join();
    }
    GpuExecutionOwner(const GpuExecutionOwner&) = delete;
    template<class F> auto invoke(F&& fn) -> std::invoke_result_t<F> {
        if (std::this_thread::get_id() == worker_.get_id()) return fn();
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
    void work() {
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock lock(mutex_);
                ready_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
                if (queue_.empty()) return;
                task = std::move(queue_.front()); queue_.pop_front();
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
