// SPDX-License-Identifier: Apache-2.0
//
// Copyright 2026 Caleb Buahin
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/**
 * @file      ExplicitKokkosSurfaceSolver.hpp
 * @brief     Kokkos (OpenMP/CUDA/HIP/SYCL) port of the explicit local-inertial
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
 *          Determinism: face/cell kernels write disjoint outputs in fixed CSR
 *          order. Evaporation drains in fixed host cell order. CPU/OpenMP
 *          parity is tested with explicit tolerances; no cross-device bitwise
 *          equivalence or GPU validation is implied.
 *
 * @ingroup engine_2d_gpu
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
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
#include "KokkosPerfCounters.hpp"

namespace openswmm::twoD::gpu {

class ExplicitKokkosSurfaceSolver final : public ISurfaceSolver {
public:
    /**
     * @brief Construct an inactive marcher with empty device views and no borrowed host owners.
     * @note Does not initialize process-wide Kokkos; the plugin/host does that before
     * initialize(). Default counters/flags do not prove runtime availability or allocate mesh
     * state.
     */
    ExplicitKokkosSurfaceSolver() = default;
    /**
     * @brief Retire optional telemetry through finalize() then release owned Kokkos view
     * handles.
     * @note Caller first stops/fences work and preserves process-wide Kokkos through view
     * destruction. Does not publish unfinished device physics or finalize the global Kokkos
     * runtime. Borrowed host owners are not destroyed. Unexpected failure from finalize during
     * destruction terminates.
     */
    ~ExplicitKokkosSurfaceSolver() override { finalize(); }

    /**
     * @brief Initialize device geometry, canonical water and publication receipt views.
     * @param[in,out] mesh Prepared SI all-triangle mesh with complete geometry/stencils/coupling
     * data. Borrowed for lifetime; device geometry is copied once and must remain compatible.
     * @param[in,out] state Prepared matching water-only state and optional borrowed
     * boundary/node arrays. Copies initial water/forcings, allocates publication receipt arrays
     * and borrows the owner for later copyback; canonical owner and all optional dependencies
     * outlive the marcher.
     * @param[in,out] opts Stable local-inertial controls, borrowed; lts_tiers is clamped
     * internally to 1..8.
     * @throws std::invalid_argument Nonlocal-inertial momentum, active transport or nontriangle
     * mesh.
     * @note Requires initialized Kokkos and exclusive owner access. Builds device/host views,
     * seeds momentum/reconstruction, resets counters/telemetry and marks initialized. Empty mesh
     * returns before activation after borrowing pointers. Allocation/runtime failures can leave
     * partial views/bindings; no rollback. This routine neither initializes nor finalizes the
     * process-wide Kokkos runtime.
     */
    void initialize(MeshData& mesh, SurfaceStateData& state, SolverOptions2D& opts) override;
    /**
     * @brief Advance the accepted surface window and publish additive host receipts.
     * @param[in] t_current Finite elapsed start s, consistent with accepted owner state.
     * @param[in] t_target Finite target s, at least t_current; no general finite validation is
     * performed.
     * @return t_target, including uninitialized/nonforward no-op paths. Successful forward work
     * uploads held forcing/frozen nodes, runs local tiers and exact-landing global tails,
     * settles accumulators, then copies canonical water/head/depth/flux and additive receipts to
     * host.
     * @note Requires initialized stable borrowed owners/options and exclusive solver/device
     * access. Device state remains authoritative inside a window; no native species/heat is
     * supported. Allocation or Kokkos failure is not caught and can leave partial device
     * progress; not an atomic window. Does not advance 1D node canonical state or engine
     * calendar. Runtime launch/copy synchronization follows Kokkos.
     */
    double advance(double t_current, double t_target) override;
    /**
     * @brief Upload externally edited host volumes and reset stale device momentum.
     * @param[in] t0 Retained interface time, ignored by this implementation.
     * @note If initialized, uploads authoritative host volumes and clears interior/boundary
     * momentum and side accumulators, reconstructs device head/depth and forces an
     * active-set/tier rebuild on the next advance. Does not immediately copy reconstructed
     * fields back or reset producer ledgers/cumulative counters. Requires compatible shapes,
     * initialized Kokkos and exclusive solver/host access; copy/kernel failures are not rolled
     * back.
     */
    void reinitialize(double t0) override;
    /**
     * @brief Upload authoritative host volumes while retaining device momentum.
     * @param[in] t0 Retained interface time, ignored by this implementation.
     * @note If initialized, uploads authoritative host volumes while preserving
     * interior/boundary momentum and side accumulators, reconstructs device head/depth and
     * forces an active-set/tier rebuild on the next advance. Does not immediately copy
     * reconstructed fields back or reset producer ledgers/cumulative counters. Requires
     * compatible shapes, initialized Kokkos and exclusive solver/host access; copy/kernel
     * failures are not rolled back.
     */
    void resyncFromVolumes(double t0) override;
    /**
     * @brief Write optional telemetry and retire the marcher activation flag.
     * @note If initialized, dumps performance counters and optionally overwrites the configured
     * telemetry CSV, clears telemetry and marks uninitialized. Failure to open telemetry is
     * silently ignored. Does not release every Kokkos view, clear borrowed pointers, copy
     * unfinished device state to host or finalize process Kokkos. Caller first finishes/fences
     * work and serializes owner lifetime. Destructor calls this method; unexpected
     * runtime/mutex/allocation exceptions there terminate destruction.
     */
    void finalize() override;

    /**
     * @brief Read the most recent advance base-substep count.
     * @return Base substeps executed in the most recent forward advance;
     * uninitialized/nonforward advance leaves the prior value. Reset by initialize(); no
     * synchronization or state change occurs.
     */
    long last_num_steps() const noexcept override { return last_steps_; }
    /**
     * @brief Read the most recent accepted base or tail duration.
     * @return Last base or equal-tail duration s, or remaining idle span when no active cells.
     * Zero after initialization. No device reduction or synchronization; caller reads stable
     * owner telemetry.
     */
    double last_step_size() const noexcept override { return last_dt_; }
    /**
     * @brief Borrow the published integrated live junction exchange.
     * @return Borrowed host vector of integrated live junction exchange m3 in non-outfall
     * coupling order, positive surface-to-node. Replaced by publication/initialization; requires
     * stable owner. No new copy, node mutation, receipt consumption or device synchronization
     * occurs.
     */
    const std::vector<double>& last_coupling_exchange() const noexcept override {
        return exch_host_;
    }
    /**
     * @brief Snapshot accumulated Kokkos marcher work and active-tier telemetry.
     * @return Value snapshot of cumulative substeps/face work, last duration, telemetry-sample
     * active fraction statistics and stored tier occupancy. Not a live device query; mean
     * weights rebuild samples, not elapsed time. No allocation or reset; caller serializes with
     * advances/finalization.
     */
    RunStats run_stats() const noexcept override;
    /**
     * @brief Read whether nonempty device initialization completed.
     * @return Stored activation flag, true only after successful nonempty initialization and
     * before finalize. Does not probe Kokkos runtime health, validate borrowed arrays or
     * synchronize device work.
     */
    bool is_initialized() const noexcept override { return initialized_; }

private:
    friend struct ExplicitKokkosSurfaceSolverTestAccess;
    kperf::Counters perf_counters_;
    // Host-side handles (owned by SurfaceRouter2D; outlive the solver).
    MeshData* mesh_ = nullptr;
    SurfaceStateData* state_ = nullptr;
    SolverOptions2D* opts_ = nullptr;
    void refreshConveyance(); ///< Upload changed face factors and refresh LI cell discharge.
    InertialEdges edges_; ///< host CSR build; mirrored below
    DView d_conveyance_;  ///< mutable ψ, separate from immutable edge geometry

    // ---- device geometry (const after initialize) -------------------------
    DView d_tri_area_, d_tri_cz_, d_tri_cx_, d_tri_cy_, d_vz_, d_vx_, d_vy_;
    IView d_cell_v_;            ///< padded [cell*kMaxCellVerts + k] connectivity
    DView d_vs_wt_;             ///< vertex-stencil weights
    IView d_vs_ptr_, d_vs_idx_; ///< vertex-stencil CSR
    DView d_edge_length_, d_mannings_n_;
    // InertialEdges mirror
    IView d_cL_, d_cR_, d_slotL_, d_slotR_, d_cell_ptr_, d_cell_edge_;
    DView d_xi_, d_inv_dx_, d_zface_, d_ze_lo_, d_ze_hi_, d_nx_, d_ny_, d_mx_, d_my_;
    DView d_n2_, d_sign_, d_lchar_;

    // ---- device state -----------------------------------------------------
    DView d_volume_, d_head_, d_depth_;
    DView d_q_, d_faccL_, d_faccR_, d_qcx_, d_qcy_;
    DView d_rain_, d_coup_, d_evap_, d_infil_;

    /// Infiltration APPLIED depth (m) accumulated on device since the last
    /// publish, then drained ADDITIVELY into state_->infil_applied and zeroed.
    /// Additive because the host array is consumed on the routing-step cadence
    /// while this one drains on the publish cadence — a plain overwrite would
    /// lose or double-count whatever fell between the two.
    DView d_infil_applied_;
    DView d_evap_applied_;
    std::vector<double> evap_applied_host_;
    /// Signed per-cell coupling exchange volume (m³), mirroring
    /// SurfaceStateData::coupling_applied. Kept on device beside
    /// d_infil_applied_ and drained by the same host pass — a GPU run that
    /// left it zero would report "no coupling" on exactly the decks the
    /// plugin is chosen for.
    DView d_coupling_applied_;
    /// C3: node full_volume (1D ft³) on device, so the spill cap can use the
    /// PONDED share (volume − full_volume) rather than total node storage.
    /// Static for the run, uploaded once beside d_node_invert_.
    DView d_node_fullvol_;
    std::vector<double> infil_applied_host_;    ///< drain scratch (nt)
    std::vector<double> coupling_applied_host_; ///< drain scratch (nt)
    DView d_edge_flux_;                         ///< kMaxCellVerts·nt flat slots (published)
    IView d_active_, d_pin_t0_, d_tier_, d_face_tier_;
    IView d_cells_compact_, d_edges_compact_; ///< per-tier segments
    IView d_scratch_;                         ///< seed flags scratch (nt)
    DView d_dtcell_;                          ///< per-cell CFL dt (rebuild scratch)

    // Boundary edges (non-WALL): built host-side at initialize.
    IView d_bc_cell_, d_bc_slot_, d_bc_type_;
    DView d_bc_accum_, d_bc_slope_, d_bc_head_, d_bc_flow_;
    DView d_bc_in_, d_bc_out_; ///< gross applied volumes since publication
    std::vector<double> bc_in_host_, bc_out_host_;
    DView d_bc_requested_, d_bc_last_flow_, d_bc_last_dt_;
    std::vector<double> bc_requested_host_, bc_last_flow_host_, bc_last_dt_host_;
    DView d_bc_q_; ///< prognostic boundary-edge discharge (m²/s, inflow-
                   ///< positive) — mirrors ExplicitInertialSolver::bc_q_
    std::vector<int> bc_cell_host_, bc_slot_host_;

    // Live junction exchange (windowless coupling).
    IView d_cp_cell_, d_cp_vertex_, d_cp_node_;
    DView d_cp_cd_, d_cp_area_;
    // WEIR_ORIFICE per point: law, structure {z_crest, l_weir, a_orif} and
    // coefficients {c_w, c_sw, c_oi, c_oo, h_eps}; COUPLING_HEAD cell runs.
    IView d_cp_law_, d_cp_head_b_, d_cp_head_e_, d_head_cells_, d_cp_eq_;
    DView d_cp_geo_, d_cp_coef_;
    DView d_node_pond_area_; ///< COUPLING_EQUALIZE 1D area per node (m²)
    DView d_exch_, d_node_drawn_;
    DView d_node_head_, d_node_depth_, d_node_volume_;
    DView d_node_invert_, d_node_fulldepth_;
    std::vector<double> exch_host_;

    // ---- host mirrors / control -------------------------------------------
    std::array<int, 9> tier_off_{};  ///< cells_compact_ segment offsets
    std::array<int, 9> ftier_off_{}; ///< edges_compact_ segment offsets
    int n_active_ = 0;
    double dt0_ = 0.0;
    int K_ = 1;
    bool have_perot_ = false;
    double t_last_sync_ = 0.0;
    int cycles_since_rebuild_ = 1000;
    double last_dt_ = 0.0;
    long last_steps_ = 0, substeps_run_ = 0, face_passes_ = 0;
    bool initialized_ = false;

    std::array<long, 8> tier_occupancy_{};
    std::vector<std::pair<double, int>> telemetry_;
    std::string telemetry_path_;

    // ---- internals (public for nvcc extended-lambda access) ----------------
public:
    /**
     * @brief Launch reconstruction of every device cell head/depth from canonical device volume.
     * @note Uses prepared triangle geometry and selected FLAT/VFR closure. Changes only device
     * head/depth; no host copyback, forcing, receipts or clock updates. Requires initialized
     * compatible views and exclusive launch ordering in a live Kokkos execution space; no local
     * fence is added.
     */
    void reconstructAllDev();
    /**
     * @brief Gather and zero every cell's own pending lateral side packets on device.
     * @note Adds signed m3 to device volume with a zero floor, reconstructs changed head/depth
     * and clears only that side's accumulator. No held sources, boundary/node receipts or host
     * arrays are processed. Must run before re-tiering/active-set changes; requires matching CSR
     * and serialized launch ordering.
     */
    void settleAccumulatorsDev();
    /**
     * @brief Apply held cell sources to currently inactive cells through a synchronization
     * horizon.
     * @param[in] t Finite elapsed horizon s. Nonpositive t-t_last_sync_ returns without changes.
     * @note Applies sourceBudget over the gap using frozen
     * rain/coupling/infiltration/evaporation rates, updates device volume/head/depth and applied
     * depth/volume ledgers, then advances t_last_sync_. Active cells are skipped because their
     * firing already owns source time. No host/node copyback; requires initialized views,
     * positive geometry and exclusive ordered launches. No local fence.
     */
    void lazySourcesDev(double t);
    /**
     * @brief Settle device receipts/sources and rebuild active halos, CFL tiers and compact
     * lists.
     * @param[in] t Finite elapsed synchronization time s; consistent with last completed
     * cell/source work.
     * @note Consumes side accumulators, lazily advances inactive source cells, applies
     * activation hysteresis and forced tier-zero pins, reduces dt0 and compacts cell/face lists
     * in deterministic index order. Appends (t,active-count) telemetry and updates occupancy.
     * Allocates telemetry and can throw after partial device work; no host publication or
     * rollback. Requires exclusive initialized views.
     */
    void syncAndRebuild(double t);
    /**
     * @brief Tighten the current base duration against active-cell CFL limits.
     * @note Reads active device depths/optional Perot speed with existing tiers, min-reduces the
     * current CFL requirement divided by 2^tier, and only decreases dt0_. No active cells
     * returns. Does not change tiers, settle receipts or allow growth. Reduction synchronizes
     * its scalar result; requires stable initialized views, finite positive geometry/CFL
     * controls and exclusive launch ordering.
     */
    void refreshDt0();         /**
     * @brief < tighten-only dt0_ between rebuilds (== serial)
     *
     * @note Settles pending side packets first, sets every active cell to tier zero and only
     * active- active faces to tier zero, recompacts lists and replaces offsets/count. Inactive
     * topology remains excluded. Does not apply lazy sources, alter dt0_, fire physics or copy
     * host fields. Requires valid prepared views and exclusive launch ordering; scan reductions
     * return compact counts.
     */
    void collapseToGlobalDt(); /**
     * @brief < tail: everything to tier 0
     *
     * @details Fire one device face tier or a global tail selection.
     * @param[in] tier Valid tier index [0,K_), called k in the definition; unchecked.
     * @param[in] dt_f Finite positive accepted firing duration s.
     * @param[in] global_step True selects every compact tier segment with global positivity
     * refire count; false selects only the indicated tier range.
     * @note Update selected interior momentum, apply Froude/positivity budgets and book
     * equal-opposite side m3. Updates cumulative face-work count on host; dry faces reset
     * momentum and do not transfer water. Requires initialized views, fresh compact lists and
     * ordered nonconcurrent owner access. Kernels do not validate all shapes or expose rollback;
     * host fields remain unchanged until publication.
     */
    void fireFaces(int tier, double dt_f, bool global_step = false);
    /**
     * @brief Fire one device cell tier or a global tail selection.
     * @param[in] tier Valid tier index [0,K_), called k in the definition; unchecked.
     * @param[in] dt_c Finite positive accepted firing duration s.
     * @param[in] global_step True selects every compact tier segment with global positivity
     * refire count; false selects only the indicated tier range.
     * @note Gather selected cell packets, apply held source budgets and reconstruct
     * volume/head/depth/Perot vectors. At tier zero also runs boundary and live junction
     * exchange serially on device to preserve shared donor ordering; books gross receipts and
     * integrated junction m3. Requires initialized views, fresh compact lists and ordered
     * nonconcurrent owner access. Kernels do not validate all shapes or expose rollback; host
     * fields remain unchanged until publication.
     */
    void fireCells(int tier, double dt_c, bool global_step = false);
    /**
     * @brief Execute accepted binary local-time substeps using the current compact tier lists.
     * @param[in] dt0 Finite positive base duration s, already bounded by CFL.
     * @param[in] nsub Nonnegative number of base substeps; caller aligns it with scheduled tier
     * horizons.
     * @note Faces fire at multiples of 2^tier before due cell gathers; increments total/last
     * substep counters for every base substep. Does not rebuild, publish, apply inactive lazy
     * time or change an external time argument. Requires initialized ordered device work and
     * valid tier views.
     */
    void runMacroCycle(double dt0, int nsub);
    /**
     * @brief Upload current held surface/boundary forcing to existing device views.
     * @note Copies m/s rain, coupling, evaporation and infiltration; active snow substitutes
     * liquid rain and exposed evaporation. Packs configured boundary type/slope/head m/flow m3/s
     * by prepared slots. Host shapes must match device extents and borrowed boundary owner
     * remain alive. Allocates temporary snow/boundary vectors; may fail after partial uploads.
     * Does not update device water or sample project series. Requires initialized Kokkos and
     * exclusive stable host/device access.
     */
    void pushForcings();
    /**
     * @brief Upload frozen live-junction node state for one surface window.
     * @note No-op without borrowed nodes/device node slots. Copies native node head/depth/volume
     * arrays to existing matching extents; optional pond area copies only for exact size match.
     * Does not modify canonical 1D state or reset integrated exchanges. Requires stable borrowed
     * arrays, initialized Kokkos and serialized copy/launch ordering; failures are not rolled
     * back.
     */
    void pushNodeState();
    /**
     * @brief Publish device water/flux and drain applied receipts additively into host owners.
     * @param[in] t_current Finite elapsed publication interval start s.
     * @param[in] t_target Finite elapsed end s; positive span converts integrated boundary m3 to
     * mean m3/s.
     * @note Rebuilds interior outward-sign edge flux, writes mean boundary flux, fences, copies
     * volume/ head/depth/flux and integrated coupling m3, folds gross boundary and evaporation
     * receipts, then zeros device receipts after adoption. Infiltration depth and coupling m3
     * drain together only when the host infiltration vector matches prepared scratch size.
     * Native species/quality maps are owned elsewhere. Requires exact prepared host/device
     * shapes and exclusive access; partial copies/receipts can survive runtime failure. Does not
     * advance engine calendar or 1D node state, and does not rebuild tiers.
     */
    void publishAndCopyBack(double t_current, double t_target);
};

} // namespace openswmm::twoD::gpu

#endif // OPENSWMM_ENGINE_2D_GPU_EXPLICIT_KOKKOS_SURFACE_SOLVER_HPP
