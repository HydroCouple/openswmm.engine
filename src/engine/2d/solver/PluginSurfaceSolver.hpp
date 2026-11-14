// SPDX-License-Identifier: Apache-2.0

/**
 * @file      PluginSurfaceSolver.hpp
 * @brief     Core-owned adapter for matched surface solver plugins.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#pragma once
#include "GpuExecutionOwner.hpp"
#include "ISurfaceSolver.hpp"
#include <cstdio>

namespace openswmm::twoD {
// The core owns this adapter. The matched plugin owns its implementation and
// destroys it on the same controlling thread that created it.
class PluginSurfaceSolver final : public ISurfaceSolver {
public:
    using Destroy = void (*)(void*);
    /**
     * @brief Take ownership of a matched plugin solver and its destruction route.
     * @param[in] solver Required owned ISurfaceSolver pointer allocated by the matched loaded
     * plugin.
     * @param[in] destroy Required matching plugin deallocator; consumes solver exactly once
     * during teardown.
     * @param[in] owner Required shared controlling-thread owner used for all backend
     * dispatch/destruction.
     * @pre The plugin module remains loaded and pinned through teardown. Constructor does not
     * validate pointers or initialize the backend. Caller serializes this adapter's public calls
     * and cached reads.
     */
    PluginSurfaceSolver(ISurfaceSolver* solver, Destroy destroy,
                        std::shared_ptr<GpuExecutionOwner> owner)
        : solver_(solver), destroy_(destroy), owner_(std::move(owner)) {}
    /**
     * @brief Destroy the plugin implementation synchronously on its execution owner.
     * @pre No outstanding calls or cached-reference readers; plugin and owner remain live.
     * @note Invokes the allocating plugin's deallocator and catches all failures, reporting a
     * teardown message to stderr. Does not finalize/unload the process Kokkos runtime. An
     * exceptional failed dispatch cannot guarantee plugin resources were released.
     */
    ~PluginSurfaceSolver() override {
        try {
            owner_->invoke([&] { destroy_(solver_); });
        } catch (...) {
            std::fprintf(stderr, "[openswmm 2D] plugin teardown failed\n");
        }
    }
    /**
     * @brief Initialize the plugin backend synchronously and cache its status.
     * @param[in,out] m Borrowed built SI mesh retained by the plugin; must outlive backend use.
     * @param[in,out] s Borrowed surface state mutated/retained by the plugin.
     * @param[in,out] o Borrowed validated options retained by the plugin.
     * @post Synchronously initializes on the owner thread, then refreshes the adapter
     * status/receipt cache.
     * @note Callback exceptions propagate; partial backend mutation may occur and caches refresh
     * only after a successful backend call. No model ownership is transferred; all referenced
     * buffers remain valid through finalization/destruction.
     */
    void initialize(MeshData& m, SurfaceStateData& s, SolverOptions2D& o) override {
        owner_->invoke([&] {
            solver_->initialize(m, s, o);

            refresh();
        });
    }
    /**
     * @brief Advance the plugin backend synchronously to a requested endpoint.
     * @param[in] from Finite elapsed start time, seconds, matching backend state.
     * @param[in] to Finite target time, seconds, at least from.
     * @return Actual elapsed time reached from the backend; synchronously refreshes copied
     * telemetry/receipts before returning.
     * @note Runs on the controlling thread. Exceptions propagate; no adapter rollback. Caller
     * serializes model edits, adapter methods and cached getters.
     */
    double advance(double from, double to) override {
        return owner_->invoke([&] {
            const double t = solver_->advance(from, to);
            refresh();
            return t;
        });
    }
    /**
     * @brief Reseed the plugin backend after external state edits.
     * @param[in] t Finite elapsed simulation time in seconds for the edited state.
     * @post Synchronously delegates to the plugin on the owner thread, then refreshes cached
     * telemetry/receipts.
     * @note Reseeds using the backend reinitialize contract. Errors propagate and can leave
     * backend/cache state different; caller provides exclusive access and valid retained arrays.
     */
    void reinitialize(double t) override {
        owner_->invoke([&] {
            solver_->reinitialize(t);
            refresh();
        });
    }
    /**
     * @brief Resynchronize backend time while preserving its volume-owner semantics.
     * @param[in] t Finite elapsed simulation time in seconds for the edited state.
     * @post Synchronously delegates to the plugin on the owner thread, then refreshes cached
     * telemetry/receipts.
     * @note Preserves signed volumes only when the concrete backend implements that override.
     * Errors propagate and can leave backend/cache state different; caller provides exclusive
     * access and valid retained arrays.
     */
    void resyncFromVolumes(double t) override {
        owner_->invoke([&] {
            solver_->resyncFromVolumes(t);
            refresh();
        });
    }
    /**
     * @brief Release plugin backend resources on the execution-owner thread.
     * @post Synchronously finalizes plugin resources on the owner thread, then refreshes cached
     * status and receipts. The plugin object itself is still owned until adapter destruction.
     * @note Caller serializes teardown with all operations/getters. Backend or cache-copy
     * exceptions propagate; no rollback or library unload occurs.
     */
    void finalize() override {
        owner_->invoke([&] {
            solver_->finalize();
            refresh();
        });
    }
    /**
     * @brief Read the adapter's last synchronized backend status.
     * @return Cached completed-source horizon in elapsed seconds; -1 initially/unavailable.
     * @note No dispatch or allocation; cache may be stale after a failed backend call. Fields
     * are not atomic, so serialize reads with adapter mutations.
     */
    double completedSourceTime() const noexcept override { return completed_; }
    /**
     * @brief Read the adapter's last synchronized backend status.
     * @return Cached last-advance substep count, zero before the first refresh.
     * @note No dispatch or allocation; cache may be stale after a failed backend call. Fields
     * are not atomic, so serialize reads with adapter mutations.
     */
    long last_num_steps() const noexcept override { return steps_; }
    /**
     * @brief Read the adapter's last synchronized backend status.
     * @return Cached last substep size in seconds, zero initially.
     * @note No dispatch or allocation; cache may be stale after a failed backend call. Fields
     * are not atomic, so serialize reads with adapter mutations.
     */
    double last_step_size() const noexcept override { return step_; }
    /**
     * @brief Borrow the most recently refreshed coupling-volume snapshot.
     * @return Borrowed adapter-owned copied coupling volumes in m3, positive surface-to-network.
     * Valid until the next refresh or adapter destruction; never free/modify it.
     * @note No plugin dispatch; caller serializes cached-reference use with
     * advance/reinitialize/finalize. Empty cache means no live exchange tuple is available.
     */
    const std::vector<double>& last_coupling_exchange() const noexcept override {
        return exchange_;
    }
    /**
     * @brief Read the adapter's last synchronized backend status.
     * @return Value-owned copy of the last refreshed cumulative backend statistics.
     * @note No dispatch or allocation; cache may be stale after a failed backend call. Fields
     * are not atomic, so serialize reads with adapter mutations.
     */
    RunStats run_stats() const noexcept override { return stats_; }
    /**
     * @brief Read the adapter's last synchronized backend status.
     * @return Last refreshed backend initialization flag; false initially.
     * @note No dispatch or allocation; cache may be stale after a failed backend call. Fields
     * are not atomic, so serialize reads with adapter mutations.
     */
    bool is_initialized() const noexcept override { return initialized_; }

private:
    /**
     * @brief Copy backend telemetry/receipts into the core adapter's cached snapshot.
     * @pre Called on the controlling owner thread with a live solver and exclusive adapter
     * access.
     * @post Reads all status accessors and copies the exchange vector. Vector allocation may
     * throw after scalar fields were updated; the refresh is not transactional. Does not advance
     * the solver or retain a reference to the plugin's exchange vector.
     */
    void refresh() {
        steps_ = solver_->last_num_steps();
        step_ = solver_->last_step_size();
        completed_ = solver_->completedSourceTime();
        stats_ = solver_->run_stats();
        exchange_ = solver_->last_coupling_exchange();
        initialized_ = solver_->is_initialized();
    }
    ISurfaceSolver* solver_;
    Destroy destroy_;
    std::shared_ptr<GpuExecutionOwner> owner_;
    long steps_ = 0;
    double step_ = 0, completed_ = -1;
    bool initialized_ = false;
    RunStats stats_{};
    std::vector<double> exchange_;
};
}
