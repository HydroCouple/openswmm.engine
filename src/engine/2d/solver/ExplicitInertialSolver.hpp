/**
 * @file ExplicitInertialSolver.hpp
 * @brief Explicit local-inertial FV time-marcher for the 2D surface.
 *
 * @details Phase 1 of the 2026-07-29 2D reimplementation plan. A CFL-driven
 *          explicit marcher over the unique-face layout (InertialEdges):
 *          cell volume V is the conserved state (SurfaceStateData::volume,
 *          in place), one prognostic unit-width discharge q per interior
 *          face, semi-implicit friction, positivity limiter (V ≥ 0 always —
 *          no negative-volume debt machinery), and a flux-active cell set
 *          with lazy source-only integration for rain-on-grid thin films.
 *
 *          advance(t0, t1) ALWAYS reaches t1 (the step size is known a
 *          priori from the CFL bound), so none of the router's failed/frozen/
 *          partial-window machinery ever engages on this path. No linear
 *          solver, no Jacobian, no preconditioner.
 *
 *          Tiered local timestepping (LTS_TIERS > 1) lands in Phase 2; this
 *          phase steps the whole active set at the global CFL dt.
 *
 * @ingroup engine_2d
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_ENGINE_2D_EXPLICIT_INERTIAL_SOLVER_HPP
#define OPENSWMM_ENGINE_2D_EXPLICIT_INERTIAL_SOLVER_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "ISurfaceSolver.hpp"
#include "InertialEdges.hpp"

namespace openswmm::twoD {

class ExplicitInertialSolver final : public ISurfaceSolver {
public:
    void initialize(MeshData& mesh, SurfaceStateData& state,
                    SolverOptions2D& opts) override;
    double advance(double t_current, double t_target) override;
    void reinitialize(double t0) override;
    void resyncFromVolumes(double t0) override;
    void finalize() override;

    long   last_num_steps() const noexcept override { return last_steps_; }
    double last_step_size() const noexcept override { return last_dt_; }
    RunStats run_stats() const noexcept override;
    bool is_initialized() const noexcept override { return initialized_; }

private:
    // One explicit substep of length dt over the active set.
    void substep(double dt);
    // Recompute η/depth from state volumes for cells [first, last) or the
    // whole mesh (first < 0).
    void reconstructAll();
    // Apply lazily-accumulated sources (rain/held coupling) to INACTIVE cells
    // over [t_last_sync_, t] and rebuild the active cell/edge lists.
    void syncAndRebuild(double t);
    // Global CFL-stable dt over the active set.
    double stableDt() const;

    MeshData*         mesh_  = nullptr;
    SurfaceStateData* state_ = nullptr;
    SolverOptions2D*  opts_  = nullptr;

    InertialEdges edges_;
    std::vector<double>  q_;            ///< per unique interior face (m²/s)
    std::vector<double>  qcx_, qcy_;    ///< Perot cell discharge vector (θ < 1)
    std::vector<uint8_t> cell_active_;
    std::vector<int>     active_cells_;
    std::vector<int>     active_edges_;
    std::vector<int>     bc_cell_;      ///< cells with a non-WALL boundary edge
    std::vector<int>     bc_slot_;      ///< matching flat mesh edge slot
    std::vector<double>  bc_accum_;     ///< ∫F_applied dt per BC entry (m³),
                                        ///< inflow-positive, reset per advance

    double t_last_sync_ = 0.0;          ///< lazy-source clock
    long   substeps_run_ = 0;           ///< cumulative substeps (whole run)
    long   face_passes_  = 0;           ///< cumulative face-kernel passes
    long   last_steps_   = 0;           ///< substeps in the last advance()
    double last_dt_      = 0.0;
    bool   initialized_  = false;

    // Active-fraction telemetry: (sim time, active cells) sampled at every
    // rebuild; dumped as CSV at finalize() when OPENSWMM_2D_MARCHER_TELEMETRY
    // names a file. The Phase-1 gate reads this to verify the thin-film budget
    // assumption on the Bellinge storm slice.
    std::vector<std::pair<double, int>> telemetry_;
    std::string telemetry_path_;
};

} // namespace openswmm::twoD

#endif // OPENSWMM_ENGINE_2D_EXPLICIT_INERTIAL_SOLVER_HPP
