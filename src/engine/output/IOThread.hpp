/**
 * @file IOThread.hpp
 * @brief Separate IO writer thread with a bounded task queue (Phase 5, R17).
 *
 * @details IOThread decouples simulation computation from output writing.
 * The main simulation thread posts WriteTask objects; the IO thread drains
 * them by calling PluginFactory::update_all() on each snapshot.
 *
 * ### Producer / consumer model
 *
 * ```
 * Main thread (producer)          IO thread (consumer)
 * ─────────────────────           ────────────────────
 * IOThread::post(snapshot) ──→    dequeue WriteTask
 *                                 plugin_factory.update_all(task.snapshot)
 * IOThread::stop()         ──→    finish queue, exit thread
 * ```
 *
 * ### Bounded queue
 *
 * The queue capacity is configurable (default: 8 items). If the queue is
 * full, `post()` blocks the main thread until space is available. This
 * provides back-pressure if the IO thread is slower than the simulation.
 *
 * ### Thread safety
 *
 * - `post()` is safe to call from any thread (typically main sim thread).
 * - `stop()` must be called from the main thread exactly once.
 * - `start()` must be called before `post()`.
 *
 * @see WriteTask.hpp
 * @see src/engine/plugins/PluginFactory.hpp
 * @see docs/MASTER_IMPLEMENTATION_PLAN.md Phase 5
 * @ingroup engine_io
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_ENGINE_IO_THREAD_HPP
#define OPENSWMM_ENGINE_IO_THREAD_HPP

#include "WriteTask.hpp"

#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <atomic>
#include <functional>

namespace openswmm {

class PluginFactory;

/**
 * @brief Producer-consumer IO thread for writing simulation snapshots.
 *
 * @details One IOThread per SWMMEngine instance. Shared with PluginFactory.
 *
 * @ingroup engine_io
 */
class IOThread {
public:
    /** @brief Default maximum queue depth. */
    static constexpr std::size_t DEFAULT_QUEUE_CAPACITY = 8;

    /**
     * @brief Construct (does NOT start the thread).
     * @param factory   Plugin factory whose update_all() is called per task.
     * @param capacity  Maximum number of queued snapshots before post() blocks.
     */
    explicit IOThread(PluginFactory& factory,
                      std::size_t capacity = DEFAULT_QUEUE_CAPACITY);

    ~IOThread();

    // Non-copyable, non-movable (owns a thread)
    IOThread(const IOThread&) = delete;
    IOThread& operator=(const IOThread&) = delete;

    // -----------------------------------------------------------------------
    // Control
    // -----------------------------------------------------------------------

    /**
     * @brief Start the IO worker thread.
     * @pre  Thread must not already be running.
     */
    void start();

    /**
     * @brief Post a snapshot to the write queue.
     *
     * @details Blocks if the queue is at capacity. Thread-safe.
     * @param snap  Snapshot to write (moved into the queue).
     */
    void post(SimulationSnapshot snap);

    /**
     * @brief Signal the IO thread to finish and join it.
     *
     * @details Waits for all queued tasks to be processed, then joins the
     *          thread. Safe to call even if start() was never called.
     */
    void stop();

    // -----------------------------------------------------------------------
    // Diagnostics
    // -----------------------------------------------------------------------

    /** @brief True if the thread is running. */
    bool running() const noexcept { return running_.load(std::memory_order_relaxed); }

    /** @brief Number of tasks processed so far. */
    int tasks_completed() const noexcept { return tasks_completed_.load(std::memory_order_relaxed); }

    /** @brief Last error code from a plugin update (0 = no error). */
    int last_error() const noexcept { return last_error_.load(std::memory_order_relaxed); }

private:
    void run();  ///< Worker thread function

    PluginFactory&           factory_;
    const std::size_t        capacity_;

    std::thread              thread_;
    std::queue<WriteTask>    queue_;
    std::mutex               mutex_;
    std::condition_variable  cv_not_full_;   ///< Wakes producer when space available
    std::condition_variable  cv_not_empty_;  ///< Wakes consumer when task available
    std::atomic<bool>        stop_flag_      {false};
    std::atomic<bool>        running_        {false};
    std::atomic<int>         tasks_completed_{0};
    std::atomic<int>         last_error_     {0};
    int                      next_sequence_  = 0;
};

} /* namespace openswmm */

#endif /* OPENSWMM_ENGINE_IO_THREAD_HPP */
