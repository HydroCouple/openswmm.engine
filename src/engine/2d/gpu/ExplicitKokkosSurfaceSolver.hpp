/**
 * @file ExplicitKokkosSurfaceSolver.hpp
 * @brief Kokkos (OpenMP/CUDA/HIP/SYCL) port of the explicit local-inertial
 *        FV marcher with tiered local timestepping.
 *
 * @details P5 of the 2026-07-29 2D reimplementation plan. Same numerical
 *          scheme as the serial ExplicitInertialSolver — the scalar kernel
 *          bodies are the SAME code (InertialKernels.hpp / VfrClosure.hpp,
 *          compiled here with OPENSWMM_KERNEL_FN = KOKKOS_INLINE_FUNCTION) —
 *          only the loop structure changes:
 *
 *            face sweep / positivity / cell gather  → parallel_for per tier
 *            active-set + tier compaction           → parallel_scan (device)
 *            dt0                                    → min-reduce (exact in FP)
 *            boundary edges + live junction exchange → single-thread device
 *                    kernel (serial order preserved → deterministic shared-
 *                    cell/node budgets, exactly the serial semantics)
 *
 *          Host↔device traffic happens ONLY at advance() boundaries (the
 *          co-advance sync batches): forcings + frozen 1D node arrays in,
 *          {volume, head, depth, edge_flux, ∫Q dt} out. All marching state
 *          (V, η, h, q, accumulators, tiers, active sets) is device-resident.
 *
 *          Determinism: every kernel writes disjoint outputs in fixed CSR
 *          order and the only reductions are min (FP-exact) — on the OpenMP
 *          backend results are bit-identical to the serial marcher for any
 *          thread count (gated by test_2d_omp_explicit).
 *
 * @ingroup engine_2d_gpu
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_ENGINE_2D_GPU_EXPLICIT_KOKKOS_SURFACE_SOLVER_HPP
#define OPENSWMM_ENGINE_2D_GPU_EXPLICIT_KOKKOS_SURFACE_SOLVER_HPP

#include <array>
#include <string>
#include <utility>
#include <vector>

#include "../solver/ISurfaceSolver.hpp"
#include "../solver/InertialEdges.hpp"
#include "KokkosTypes.hpp"

namespace openswmm::twoD::gpu {

class ExplicitKokkosSurfaceSolver final : public ISurfaceSolver {
public:
    ExplicitKokkosSurfaceSolver() = default;
    ~ExplicitKokkosSurfaceSolver() override { finalize(); }

    void   initialize(MeshData& mesh, SurfaceStateData& state,
                      SolverOptions2D& opts) override;
    double advance(double t_current, double t_target) override;
    void   reinitialize(double t0) override;
    void   resyncFromVolumes(double t0) override;
    void   finalize() override;

    long   last_num_steps() const noexcept override { return last_steps_; }
    double last_step_size() const noexcept override { return last_dt_; }
    const std::vector<double>& last_coupling_exchange()
        const noexcept override { return exch_host_; }
    RunStats run_stats() const noexcept override;
    bool is_initialized() const noexcept override { return initialized_; }

private:
    // Host-side handles (owned by SurfaceRouter2D; outlive the solver).
    MeshData*         mesh_  = nullptr;
    SurfaceStateData* state_ = nullptr;
    SolverOptions2D*  opts_  = nullptr;
    InertialEdges     edges_;          ///< host CSR build; mirrored below

    // ---- device geometry (const after initialize) -------------------------
    DView d_tri_area_, d_tri_cz_, d_tri_cx_, d_tri_cy_, d_vz_, d_vx_, d_vy_;
    IView d_tri_v0_, d_tri_v1_, d_tri_v2_;
    DView d_vs_wt_;                    ///< vertex-stencil weights
    IView d_vs_ptr_, d_vs_idx_;        ///< vertex-stencil CSR
    DView d_edge_length_, d_mannings_n_;
    // InertialEdges mirror
    IView d_cL_, d_cR_, d_slotL_, d_slotR_, d_cell_ptr_, d_cell_edge_;
    DView d_xi_, d_inv_dx_, d_zface_, d_ze_lo_, d_ze_hi_, d_nx_, d_ny_, d_mx_, d_my_;
    DView d_n2_, d_sign_, d_lchar_;

    // ---- device state -----------------------------------------------------
    DView d_volume_, d_head_, d_depth_;
    DView d_q_, d_faccL_, d_faccR_, d_qcx_, d_qcy_;
    DView d_rain_, d_coup_, d_evap_;
    DView d_edge_flux_;                ///< 3·nt flat slots (published)
    IView d_active_, d_pin_t0_, d_tier_, d_face_tier_;
    IView d_cells_compact_, d_edges_compact_;  ///< per-tier segments
    IView d_scratch_;                  ///< seed flags scratch (nt)
    DView d_dtcell_;                   ///< per-cell CFL dt (rebuild scratch)

    // Boundary edges (non-WALL): built host-side at initialize.
    IView d_bc_cell_, d_bc_slot_, d_bc_type_;
    DView d_bc_accum_, d_bc_slope_, d_bc_head_, d_bc_flow_;
    std::vector<int> bc_cell_host_, bc_slot_host_;

    // Live junction exchange (windowless coupling).
    IView d_cp_cell_, d_cp_vertex_, d_cp_node_;
    DView d_cp_cd_, d_cp_area_;
    DView d_exch_, d_node_drawn_;
    DView d_node_head_, d_node_depth_, d_node_volume_;
    DView d_node_invert_, d_node_fulldepth_;
    std::vector<double> exch_host_;

    // ---- host mirrors / control -------------------------------------------
    std::array<int, 9>  tier_off_{};   ///< cells_compact_ segment offsets
    std::array<int, 9>  ftier_off_{};  ///< edges_compact_ segment offsets
    int    n_active_ = 0;
    double dt0_ = 0.0;
    int    K_ = 1;
    bool   have_perot_ = false;
    double t_last_sync_ = 0.0;
    int    cycles_since_rebuild_ = 1000;
    double last_dt_ = 0.0;
    long   last_steps_ = 0, substeps_run_ = 0, face_passes_ = 0;
    bool   initialized_ = false;

    std::array<long, 8> tier_occupancy_{};
    std::vector<std::pair<double, int>> telemetry_;
    std::string telemetry_path_;

    // ---- internals (public for nvcc extended-lambda access) ----------------
public:
    void reconstructAllDev();
    void settleAccumulatorsDev();
    void lazySourcesDev(double t);
    void syncAndRebuild(double t);
    void collapseToGlobalDt();         ///< tail: everything to tier 0
    void fireFaces(int tier, double dt_f);
    void fireCells(int tier, double dt_c);
    void runMacroCycle(double dt0, int nsub);
    void pushForcings();
    void pushNodeState();
    void publishAndCopyBack(double t_current, double t_target);
};

} // namespace openswmm::twoD::gpu

#endif // OPENSWMM_ENGINE_2D_GPU_EXPLICIT_KOKKOS_SURFACE_SOLVER_HPP
