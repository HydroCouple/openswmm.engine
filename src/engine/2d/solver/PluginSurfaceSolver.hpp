// SPDX-License-Identifier: Apache-2.0
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
    PluginSurfaceSolver(ISurfaceSolver* solver, Destroy destroy,
                        std::shared_ptr<GpuExecutionOwner> owner)
        : solver_(solver), destroy_(destroy), owner_(std::move(owner)) {}
    ~PluginSurfaceSolver() override {
        try { owner_->invoke([&] { destroy_(solver_); }); }
        catch (...) { std::fprintf(stderr, "[openswmm 2D] plugin teardown failed\n"); }
    }
    void initialize(MeshData& m, SurfaceStateData& s, SolverOptions2D& o) override {
        owner_->invoke([&] { solver_->initialize(m,s,o); refresh(); });
    }
    double advance(double from, double to) override {
        return owner_->invoke([&] { const double t=solver_->advance(from,to); refresh(); return t; });
    }
    void reinitialize(double t) override {
        owner_->invoke([&] { solver_->reinitialize(t); refresh(); });
    }
    void resyncFromVolumes(double t) override {
        owner_->invoke([&] { solver_->resyncFromVolumes(t); refresh(); });
    }
    void finalize() override {
        owner_->invoke([&] { solver_->finalize(); refresh(); });
    }
    double completedSourceTime() const noexcept override { return completed_; }
    long last_num_steps() const noexcept override { return steps_; }
    double last_step_size() const noexcept override { return step_; }
    const std::vector<double>& last_coupling_exchange() const noexcept override { return exchange_; }
    RunStats run_stats() const noexcept override { return stats_; }
    bool is_initialized() const noexcept override { return initialized_; }
private:
    void refresh() {
        steps_=solver_->last_num_steps(); step_=solver_->last_step_size();
        completed_=solver_->completedSourceTime(); stats_=solver_->run_stats();
        exchange_=solver_->last_coupling_exchange(); initialized_=solver_->is_initialized();
    }
    ISurfaceSolver* solver_;
    Destroy destroy_;
    std::shared_ptr<GpuExecutionOwner> owner_;
    long steps_=0;
    double step_=0,completed_=-1;
    bool initialized_=false;
    RunStats stats_{};
    std::vector<double> exchange_;
};
}
