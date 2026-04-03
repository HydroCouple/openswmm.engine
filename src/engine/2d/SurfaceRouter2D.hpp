/**
 * @file SurfaceRouter2D.hpp
 * @brief Top-level orchestrator for the optional 2D surface routing module.
 *
 * @details Manages the full 2D workflow within the engine lifecycle:
 *          - Mesh topology construction (after parse)
 *          - CVODE solver initialization
 *          - Per-step coupling, rainfall update, solver advance
 *          - Statistics and finalization
 *
 *          Integrates with SWMMEngine via lifecycle hooks:
 *          initialize() → step() → finalize()
 *
 * @see TWO_DIMENSIONAL_SURFACE_ROUTING_IMPLEMENTATION_STRATEGY.md §4.1, §8.3
 * @ingroup engine_2d
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_ENGINE_2D_SURFACE_ROUTER_HPP
#define OPENSWMM_ENGINE_2D_SURFACE_ROUTER_HPP

#include "data/MeshData.hpp"
#include "data/SurfaceStateData.hpp"
#include "data/SolverOptions2D.hpp"
#include "coupling/NodeCoupling.hpp"

#ifdef OPENSWMM_HAS_2D
#include "solver/CvodeSurfaceSolver.hpp"
#endif

namespace openswmm {
struct SimulationContext;
}

namespace openswmm::twoD {

/**
 * @brief Top-level orchestrator for the 2D surface routing module.
 */
class SurfaceRouter2D {
public:
    SurfaceRouter2D() = default;
    ~SurfaceRouter2D() = default;

    // Non-copyable, movable
    SurfaceRouter2D(const SurfaceRouter2D&) = delete;
    SurfaceRouter2D& operator=(const SurfaceRouter2D&) = delete;
    SurfaceRouter2D(SurfaceRouter2D&&) = default;
    SurfaceRouter2D& operator=(SurfaceRouter2D&&) = default;

    /**
     * @brief Initialize the 2D module after input parsing is complete.
     *
     * Builds mesh topology, vertex stencils, resolves coupling names,
     * and initializes the CVODE solver.
     *
     * @param ctx Simulation context (must have mesh_2d populated from parsing).
     */
    void initialize(SimulationContext& ctx);

    /**
     * @brief Advance the 2D surface routing by one SWMM routing step.
     *
     * Sequence:
     * 1. Update outfall boundary heads from 2D state (before 1D routing)
     * 2. After 1D routing: compute coupling exchange flows
     * 3. Update 2D rainfall from system gages
     * 4. Advance CVODE by dt_swmm
     * 5. Transfer outfall discharges into 2D cells
     * 6. Update statistics
     *
     * @param ctx  Simulation context.
     * @param dt   SWMM routing timestep (seconds).
     * @param t    Current simulation time (seconds from start).
     */
    void step(SimulationContext& ctx, double dt, double t);

    /**
     * @brief Pre-routing hook: update outfall boundaries from 2D surface heads.
     *
     * Must be called BEFORE the 1D routing step, after setOutfallDepths().
     *
     * @param ctx Simulation context.
     */
    void updateOutfallsPreRouting(SimulationContext& ctx);

    /**
     * @brief Post-routing hook: compute coupling exchange and advance 2D solver.
     *
     * Must be called AFTER the 1D routing step.
     *
     * @param ctx  Simulation context.
     * @param dt   SWMM routing timestep (seconds).
     * @param t    Current simulation time (seconds from start).
     */
    void advancePostRouting(SimulationContext& ctx, double dt, double t);

    /**
     * @brief Finalize the 2D module at simulation end.
     */
    void finalize();

    /**
     * @brief Compute a CFL-like stability hint for the 2D domain.
     *
     * Returns an advisory maximum dt based on mesh resolution and wave speeds.
     * CVODE handles its own sub-stepping, but this prevents the coupling
     * interval from being too large.
     *
     * @param ctx Simulation context.
     * @return Advisory maximum timestep (seconds).
     */
    double computeCflHint(const SimulationContext& ctx) const;

    /// Check if the 2D module is active.
    bool isActive() const noexcept { return active_; }

    /// Access mesh data (read-only).
    const MeshData& mesh() const noexcept { return mesh_; }

    /// Access surface state (read-only).
    const SurfaceStateData& state() const noexcept { return state_; }

    /// Access surface state (mutable, for forcing).
    SurfaceStateData& state() noexcept { return state_; }

    /// Access solver options (read-only).
    const SolverOptions2D& options() const noexcept { return options_; }

    /// Access solver options (mutable).
    SolverOptions2D& options() noexcept { return options_; }

    /// Get total 2D surface volume (sum of depth * area).
    double totalVolume() const;

    /// Get total exchange flow (sum of coupling flows, m³/s).
    double totalExchangeFlow() const;

#ifdef OPENSWMM_HAS_2D
    /// Access CVODE solver statistics.
    long lastCvodeSteps() const { return cvode_solver_.last_num_steps(); }
    double lastCvodeStepSize() const { return cvode_solver_.last_step_size(); }
#else
    long lastCvodeSteps() const { return 0; }
    double lastCvodeStepSize() const { return 0.0; }
#endif

private:
    MeshData         mesh_;
    SurfaceStateData state_;
    SolverOptions2D  options_;

    std::vector<CouplingPoint> coupling_points_;

    bool   active_           = false;
    int    coupling_counter_ = 0;
    double sim_time_         = 0.0;

#ifdef OPENSWMM_HAS_2D
    CvodeSurfaceSolver cvode_solver_;
#endif

    /// Update rainfall from system rain gages.
    void updateRainfall(SimulationContext& ctx);
};

} // namespace openswmm::twoD

#endif // OPENSWMM_ENGINE_2D_SURFACE_ROUTER_HPP
