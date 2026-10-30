/**
 * @file      ExplicitInertialSolver.cpp
 * @brief     Implementation of the explicit local-inertial FV marcher with
 *        power-of-two tiered local timestepping (LTS).
 *
 * @details Marching order (halving scheme): a macro cycle of 2^{K−1} base
 *          substeps of length dt0; tier k fires every 2^k substeps with
 *          Δt = 2^k·dt0. A face belongs to the FINER of its incident cells'
 *          tiers, so it always integrates at the rate the sharper side needs,
 *          reading the coarser cell's surface frozen at that cell's last
 *          firing. Every face firing books the identical ±ΔM = q·ξ·Δt_f into
 *          per-side accumulators; each cell applies its own side at its own
 *          firing — conservation across tier interfaces is exact by
 *          construction (same FP products, booked at different times).
 *
 *          Positivity: at face cadence each exporting face is capped at
 *          (β/nv)·V of its exporting cell, so a cell's ≤ nv outgoing faces can
 *          take at most β·V per own-step — V ≥ 0 without any cross-face
 *          coordination (exact at K = 1 where V is fresh every substep; the
 *          α-margin covers within-cycle depth drift for K > 1, with a
 *          zero-floor backstop at the cell update).
 *
 * @see ExplicitInertialSolver.hpp, InertialKernels.hpp
 * @ingroup engine_2d
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include "core/ProcessPerf.hpp"
#include "ExplicitInertialSolver.hpp"
#include "../coupling/Rk2RuntimeSources.hpp"
#include "core/FileIO.hpp" // issue #7: UTF-8 paths on Windows

#include "../subsurface/SubsurfaceSolver.hpp"
#include "../subsurface/CommonSourceStage.hpp"
#include "../subsurface/SourceResultHistory.hpp"
#include "../subsurface/EngineSourceResultHistory.hpp"
#include "../subsurface/EngineSourceResultStream.hpp"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <cmath>
#if defined(SWMM_USE_OPENMP)
#include <omp.h>
#endif
#include <cstdio>
#include <cstdlib>
#include <stdexcept> // std::runtime_error ([2D_BOUNDARY_QUALITY] resolve)
#include <string>    // std::to_string

#include "../data/MeshData.hpp"
#include "../mesh/VertexReconstruction.hpp"
#include "../data/SolverOptions2D.hpp"
#include "../data/SurfaceStateData.hpp"
#include "../data/BoundaryData.hpp"
#include "InertialKernels.hpp"
#include "../KernelPointers.hpp"
// Inline the small SWE kernel chain in this CPU translation unit. Keep the
// shared header's annotation unchanged for other backends and callers.
#pragma push_macro("OPENSWMM_KERNEL_FN")
#undef OPENSWMM_KERNEL_FN
#if defined(_MSC_VER)
#define OPENSWMM_KERNEL_FN __forceinline
#elif defined(__GNUC__) || defined(__clang__)
#define OPENSWMM_KERNEL_FN inline __attribute__((always_inline))
#else
#define OPENSWMM_KERNEL_FN inline
#endif
#include "SweKernels.hpp"
#pragma pop_macro("OPENSWMM_KERNEL_FN")
#include "DiffusiveKernels.hpp"
#include "SurfaceFluxCalculator.hpp"    // evapSink, computeBoundaryEdgeFlux
#include "../coupling/NodeCoupling.hpp" // computeNodeCouplingQ (live exchange)
#include "../../data/NodeData.hpp"
#include "../../core/SimulationContext.hpp"
#include "../../transport/components/ReactionModule/ReactionsWriter.hpp"
#include "../../transport/components/HeatFluxModules/SurfaceExchange.hpp"
#include "../gw/GwTransportData.hpp"
#include "../gw/GwSourceResolver.hpp"
#include "../heat/ChannelGround.hpp"

namespace openswmm::twoD {

namespace {
/// Active-set / tier rebuild cadence in macro cycles. Between rebuilds the
/// lists are frozen and inactive cells accumulate their (rain / held-coupling)
/// sources lazily — this is where rain-on-grid thin films become near-free.
constexpr int kRebuildEveryCycles = 4;
/// Halo depth (rings) when FRONT_REBUILD is on — enough for a 2√(gh) front
/// to cross a macro cycle of 2^(K−1) base substeps at CFL ½ (K = 4 ⇒ ~3–4
/// cells). The breach flag then triggers the rebuild at the next cycle.
constexpr int kFrontHaloRings = 5;
/**
 * @brief Fill integer lists in input order using fixed logical partitions.
 * @param count Nonnegative prepared input extent.
 * @param groups Positive number of primary destination lists.
 * @param workers Positive requested worker count (nonpositive values use one).
 * @param counts Reusable caller-owned partition counts/offsets; contents are scratch.
 * @param group Stable group of each input, in [0,groups), or negative to skip it.
 * @param value Value of each accepted input; called once per accepted item.
 * @param output Mutable destination vector for each group; IncludeAll adds group `groups`.
 * @note Caller owns the operation exclusively. Callbacks are nonthrowing and safe for
 * concurrent disjoint input indices. Group is called twice and must retain its result;
 * independent idempotent writes for that same index are allowed. Reads must not alias
 * destination storage invalidated by resize. Output allocation occurs outside parallel
 * regions. Fixed logical ranges preserve order even if the two teams have different sizes.
 */
template <bool IncludeAll, class Group, class Value, class Output>
void compactOrdered(int count, int groups, int workers, std::vector<int>& counts,
                    Group group, Value value, Output output) {
    workers = std::max(1, workers);
    const int columns = groups + (IncludeAll ? 1 : 0);
    // Leave at least 128 bytes between the used portions of distinct partition rows.
    const std::size_t stride = ((std::size_t(columns) + 31) / 32 + 1) * 32;
    counts.assign(std::size_t(workers) * stride, 0);
#if defined(SWMM_USE_OPENMP)
#pragma omp parallel for schedule(static) num_threads(workers)
#endif
    for (int part = 0; part < workers; ++part) {
        int* row = counts.data() + std::size_t(part) * stride;
        const int begin = int(std::int64_t(count) * part / workers);
        const int end = int(std::int64_t(count) * (part + 1) / workers);
        for (int i = begin; i < end; ++i) {
            const int g = group(i);
            if (g >= 0) {
                ++row[g];
                if constexpr (IncludeAll)
                    ++row[groups];
            }
        }
    }
    for (int g = 0; g < columns; ++g) {
        int total = 0;
        for (int part = 0; part < workers; ++part) {
            int& entry = counts[std::size_t(part) * stride + g];
            const int size = entry;
            entry = total;
            total += size;
        }
        output(g).resize(std::size_t(total));
    }
#if defined(SWMM_USE_OPENMP)
#pragma omp parallel for schedule(static) num_threads(workers)
#endif
    for (int part = 0; part < workers; ++part) {
        int* row = counts.data() + std::size_t(part) * stride;
        const int begin = int(std::int64_t(count) * part / workers);
        const int end = int(std::int64_t(count) * (part + 1) / workers);
        for (int i = begin; i < end; ++i) {
            const int g = group(i);
            if (g >= 0) {
                const int v = value(i);
                output(g)[std::size_t(row[g]++)] = v;
                if constexpr (IncludeAll)
                    output(groups)[std::size_t(row[groups]++)] = v;
            }
        }
    }
}

// Small/serial rebuilds retain their original single-pass loops.
bool useParallelCompaction(int count, int workers) noexcept {
#if defined(SWMM_USE_OPENMP)
    return workers > 1 && count >= 65536;
#else
    (void)count;
    (void)workers;
    return false;
#endif
}
std::vector<int>& compactionCounts() {
    // Derived scratch, borrowed only until both parallel loops have joined.
    // Keep it outside copied/serialized solver state and obtain it only on a parallel path.
    static thread_local std::vector<int> counts;
    return counts;
}
struct SurfaceLatentIntervalLimit : std::runtime_error {
    using std::runtime_error::runtime_error;
};
} // namespace

void ExplicitInertialSolver::initialize(MeshData& mesh, SurfaceStateData& state,
                                        SolverOptions2D& opts) {
    wall_drag_slots_.clear();
    wall_drag_energy_ = 0;
    if (state.boundary) {
        const auto& b = *state.boundary;
        for (int slot = 0; slot < int(b.edge_wall_drag.size()); ++slot) {
            const double cw = b.edge_wall_drag[slot];
            if (!std::isfinite(cw) || cw < 0)
                throw std::invalid_argument("WALL_DRAG requires finite nonnegative Cw.");
            if (cw == 0) continue;
            const int i = MeshData::slot_cell(slot), e = MeshData::slot_local(slot);
            if (i >= mesh.n_cells() || e >= mesh.cell_vertex_count(i) ||
                mesh.cell_neighbour(i, e) >= 0 || b.edge_bc_type[slot] != int8_t(BoundaryType::WALL))
                throw std::invalid_argument("WALL_DRAG must name an exterior WALL edge.");
            wall_drag_slots_.push_back(slot);
        }
    }
    opts.wall_drag_enabled = !wall_drag_slots_.empty();
    if (!std::isfinite(opts.eddy_viscosity_background) || opts.eddy_viscosity_background < 0)
        throw std::invalid_argument("EDDY_VISCOSITY_BACKGROUND must be finite and nonnegative.");
    if (opts.momentumPhysics()) {
        if (opts.momentum != Momentum2D::FULL_SWE || opts.lts_tiers != 1)
            throw std::invalid_argument("Coriolis/Smagorinsky/wall drag require FULL_SWE and LTS_TIERS 1.");
        if (opts.coriolis == Coriolis2D::LATITUDE && !opts.coriolis_latitude_set)
            throw std::invalid_argument("CORIOLIS LATITUDE requires CORIOLIS_LATITUDE.");
        if (opts.coriolis == Coriolis2D::CONSTANT_F && !opts.coriolis_f_set)
            throw std::invalid_argument("CORIOLIS CONSTANT_F requires CORIOLIS_F.");
        if (!std::isfinite(opts.coriolisParameter()) || !std::isfinite(opts.smagorinsky_cs) ||
            !std::isfinite(opts.mixing_length_coefficient) || opts.mixing_length_coefficient < 0 ||
            opts.smagorinsky_cs < 0 || !std::isfinite(opts.coriolis_latitude) ||
            std::abs(opts.coriolis_latitude) > 90)
            throw std::invalid_argument("Invalid Coriolis/Smagorinsky parameters.");
        if (opts.groundwater == 1 || state.transport.active() || state.heat.enabled ||
            state.snow.active() || (state.node_coupling && !state.node_coupling->empty()))
            throw std::invalid_argument("Coriolis/Smagorinsky/wall drag currently require uncoupled surface hydraulics.");
        std::fprintf(stderr, "[openswmm 2D] optional momentum physics: f=%.17g s^-1, "
                     "turbulence=%s Cs=%.17g, mixing-length c=%.17g, background nu=%.17g m2/s, "
                     "CPU global stepping.\n",
                     opts.coriolisParameter(), turbulenceName(opts.turbulence), opts.smagorinsky_cs,
                     opts.mixing_length_coefficient, opts.eddy_viscosity_background);
    }
    momentum_physics_ = MomentumPhysics2D{};
    mesh_ = &mesh;
    state_ = &state;
    opts_ = &opts;
    rk_boundary_signals_.clear();
    rk_boundary_anchor_ = 0;
    common_stage_ = nullptr;
    rk_reactions_ = nullptr;
    rk_reaction_rows_.clear();
    rk_decay_.clear();
    rk_reacted_.clear();
    rk_reacted_reported_.clear();
    rk_reaction_identity_.clear();
    channel_ground_ = nullptr;
    channel_sources_.clear();
    channel_source_ready_.clear();
    channel_temperature_mixed_.clear();
    source_history_ = nullptr;
    engine_source_history_ = nullptr;
    engine_source_stream_ = nullptr;
    checkpoint_ready_ = true;
    source_phase_end_ = 0.0;

    const int nt = mesh.n_triangles();
    if (nt <= 0)
        return;

    edges_.build(mesh);
    const auto ne = static_cast<std::size_t>(edges_.ne);
    q_.assign(ne, 0.0);
    face_accum_stride_ = opts.momentum == Momentum2D::FULL_SWE ? 6 : 2;
    face_accum_.assign(ne * face_accum_stride_, 0.0);
    face_tier_.assign(ne, 0);

    // Momentum closure (MOMENTUM_EQUATION). FULL_SWE carries cell momentum
    // accumulators and a positivity-safe CFL; DIFFUSIVE_WAVE carries the
    // per-cell slope its Δx² step bound needs.
    mode_ = opts.momentum;
    second_order_ = false;

    dw_slope_.clear();
    if (mode_ == Momentum2D::FULL_SWE) {

        // First-order Godunov with hydrostatic reconstruction is positivity-
        // preserving at Courant ≤ ½ on triangles/quads; the β-share is the
        // backstop, not the guarantee, so the bound is enforced here.
        if (opts.cfl_number > 0.5) {
            std::fprintf(stderr,
                         "[openswmm 2D] MOMENTUM_EQUATION FULL_SWE: CFL_NUMBER %.3g "
                         "reduced to 0.5 (positivity bound of the Godunov update).\n",
                         opts.cfl_number);
            opts.cfl_number = 0.5;
        }
        if (opts.reconstruction_order == 2 && opts.lts_tiers > 1) {
            std::fprintf(stderr,
                         "[openswmm 2D] RECONSTRUCTION_ORDER 2 runs in global-dt mode: "
                         "LTS_TIERS %d reduced to 1.\n",
                         opts.lts_tiers);
            opts.lts_tiers = 1;
        }
        second_order_ = (opts.reconstruction_order == 2);
        if (second_order_ && state.transport.active() && !opts.groundwater) {
            std::fprintf(stderr, "[openswmm 2D] RECONSTRUCTION_ORDER 2 with overland transport "
                                 "is not supported yet; using first order.\n");
            second_order_ = false;
        }
        if (second_order_) {
            if (mesh.vert_stencil_ptr.size() != std::size_t(mesh.n_vertices() + 1))
                buildVertexStencils(mesh);
            const auto un = static_cast<std::size_t>(nt);
            gex_.assign(2 * un, 0.0);
            gey_.assign(2 * un, 0.0);
            gux_.assign(un, 0.0);
            guy_.assign(un, 0.0);
            gvx_.assign(un, 0.0);
            gvy_.assign(un, 0.0);
        } else {
            gex_.clear();
            gey_.clear();
            gux_.clear();
            guy_.clear();
            gvx_.clear();
            gvy_.clear();
        }
    } else if (mode_ == Momentum2D::DIFFUSIVE_WAVE) {
        dw_slope_.assign(static_cast<std::size_t>(nt), 0.0);
    }
    // FRONT_REBUILD AUTO: on for FULL_SWE only. A Godunov front travels
    // ~2√(gh) and can cross the 1-ring halo inside the 4-cycle cadence; a
    // diffusive-wave front cannot (its Δx²-bounded step moves it a fraction
    // of a cell per cycle), so DIFFUSIVE_WAVE keeps LI's cheap cadence
    // (2D_PERF_REGRESSION_DIAGNOSIS §4 fix 3).
    front_rebuild_ =
        (opts.front_rebuild < 0) ? (mode_ == Momentum2D::FULL_SWE) : (opts.front_rebuild != 0);
    frontier_.assign(front_rebuild_ ? static_cast<std::size_t>(nt) : 0, 0);
    front_breach_ = false;
    // Resolved once: MeshData::n_quads() is an O(n_cells) scan (F1).
    has_quads_ = mesh.n_quads() > 0;
    // FULL_SWE: WALL boundary slots per cell, CSR (F5). Slot order within a
    // cell is ascending k, the order the per-cell scan used to visit them.
    wall_ptr_.clear();
    wall_slot_.clear();
    if (mode_ == Momentum2D::FULL_SWE) {
        wall_ptr_.reserve(static_cast<std::size_t>(nt) + 1);
        wall_ptr_.push_back(0);
        for (int i = 0; i < nt; ++i) {
            const int nvc = mesh.cell_vertex_count(i);
            for (int k2 = 0; k2 < nvc; ++k2) {
                if (mesh.cell_neighbour(i, k2) >= 0)
                    continue;
                const int slot = MeshData::slot(i, k2);
                if (state.boundary && static_cast<BoundaryType>(
                                          state.boundary->edge_bc_type[slot]) != BoundaryType::WALL)
                    continue;
                wall_slot_.push_back(slot);
            }
            wall_ptr_.push_back(static_cast<int>(wall_slot_.size()));
        }
    }
    // S1: species accumulators, sized only when transport is live so a
    // hydrodynamics-only model allocates nothing here.
    species_on_ = state.transport.active(); // S7: see the member note
    if (second_order_ && opts.groundwater && state.transport.age_row >= 0)
        state.transport.generated_age.assign(state.transport.n_species, 0);
    if (species_on_) {
        const auto ns = static_cast<std::size_t>(state.transport.n_species);
        sacc_L_.assign(ns * ne, 0.0);
        sacc_R_.assign(ns * ne, 0.0);
    } else {
        sacc_L_.clear();
        sacc_R_.clear();
    }
    const auto nledger = static_cast<std::size_t>(SourceLedger::Count) *
                         static_cast<std::size_t>(state.transport.n_species);
    source_ledger_stride_ = ((nledger + 1 + 7) / 8) * 8;
    source_ledgers_.assign(source_ledger_stride_ * std::max(1, opts.num_threads), 0.0);
    // The Perot cell vectors serve the θ-blend AND the convective term, so
    // ADVECTION forces them on even at θ = 1. FULL_SWE uses the same arrays
    // as its prognostic cell momentum.
    qcx_.clear();
    qcy_.clear();
    if (opts.theta < 1.0 || opts.advection || mode_ == Momentum2D::FULL_SWE) {
        qcx_.assign(static_cast<std::size_t>(nt), 0.0);
        qcy_.assign(static_cast<std::size_t>(nt), 0.0);
    }
    cell_active_.assign(static_cast<std::size_t>(nt), 0);
    tier_.assign(static_cast<std::size_t>(nt), 0);

    const int K = std::clamp(opts.lts_tiers, 1, 8);
    cells_by_tier_.assign(static_cast<std::size_t>(K), {});
    edges_by_tier_.assign(static_cast<std::size_t>(K), {});

    // Non-WALL boundary entries, evaluated at their owning cell's firing.
    bc_cell_.clear();
    bc_slot_.clear();
    if (state.boundary) {
        for (int i = 0; i < nt; ++i) {
            const int nvc = mesh.cell_vertex_count(i);
            for (int e = 0; e < nvc; ++e) {
                const int idx = MeshData::slot(i, e);
                const auto ty = static_cast<BoundaryType>(state.boundary->edge_bc_type[idx]);
                const bool interior = mesh.cell_neighbour(i, e) >= 0;
                if (!interior && ty != BoundaryType::WALL) {
                    bc_cell_.push_back(i);
                    bc_slot_.push_back(idx);
                }
            }
        }
    }
    bc_accum_.assign(bc_cell_.size(), 0.0);
    bc_q_.assign(bc_cell_.size(), 0.0);

    // S2: resolve [2D_BOUNDARY_QUALITY] rows onto THIS solver's boundary-edge
    // list. bc_conc is [bc_slot * ns + s]; a row naming an edge that is not a
    // non-WALL boundary edge cannot take effect and is fatal (a WALL admits
    // no water, so a concentration on it describes nothing).
    if (state.transport.active()) {
        auto& tr = state.transport;
        const auto ns = static_cast<std::size_t>(tr.n_species);
        tr.bc_conc.assign(bc_cell_.size() * ns, 0.0);
        for (const auto& row : tr.bc_quality_rows) {
            std::size_t k = 0;
            bool found = false;
            for (; k < bc_slot_.size(); ++k)
                if (bc_slot_[k] == row.slot) {
                    found = true;
                    break;
                }
            if (!found)
                throw std::runtime_error("[2D_BOUNDARY_QUALITY] edge slot " +
                                         std::to_string(row.slot) +
                                         " is not a non-WALL boundary edge, so its concentration "
                                         "could never enter the mesh.");
            if (row.species < 0 || static_cast<std::size_t>(row.species) >= ns)
                throw std::runtime_error("[2D_BOUNDARY_QUALITY] species index out of range.");
            tr.bc_conc[k * ns + static_cast<std::size_t>(row.species)] = row.conc;
        }
    }

    // Live junction exchange (windowless coupling): one ∫Q dt accumulator per
    // point; spill budget tracked per 1D node. Their cells pin to tier 0 (the
    // exchange forcing changes at the fastest cadence), as do BC cells.
    exch_.assign(state.node_coupling ? state.node_coupling->size() : 0, 0.0);
    node_drawn_.assign(state.nodes_1d ? state.nodes_1d->volume.size() : 0, 0.0);
    pin_t0_.assign(static_cast<std::size_t>(nt), 0);
    for (int i : bc_cell_)
        pin_t0_[static_cast<std::size_t>(i)] = 1;
    if (state.node_coupling)
        for (const auto& cp : *state.node_coupling)
            if (cp.cell_idx >= 0)
                pin_t0_[static_cast<std::size_t>(cp.cell_idx)] = 1;

    reconstructAll();

    // Seed face momentum from the optional [2D_INITIAL_VELOCITY] rows: per
    // interior face, q_e = mean of the two incident cells' (h·u, h·v)
    // projected onto the face normal (depth from the volume-primary IC the
    // router seeded before this call). t = 0 only — reinitialize() (hotstart
    // / external state edits) still zeroes face momentum. Without this a
    // depth-only IC cannot represent solutions with v(t=0) ≠ 0 (e.g. the
    // SWASHES Thacker planar oscillation).
    {
        bool any_uv = false;
        for (int i = 0; i < nt && !any_uv; ++i)
            any_uv = mesh.tri_init_u[i] != 0.0 || mesh.tri_init_v[i] != 0.0;
        if (any_uv && mode_ == Momentum2D::FULL_SWE) {
            // Cell momentum seeds directly: q⃗ = h·u⃗ (nothing on the faces).
            for (int i = 0; i < nt; ++i) {
                qcx_[i] = state.depth[i] * mesh.tri_init_u[i];
                qcy_[i] = state.depth[i] * mesh.tri_init_v[i];
            }
        } else if (any_uv) {
            for (int e = 0; e < edges_.ne; ++e) {
                const int a = edges_.cL[e], b = edges_.cR[e];
                const double qax = state.depth[a] * mesh.tri_init_u[a];
                const double qay = state.depth[a] * mesh.tri_init_v[a];
                const double qbx = state.depth[b] * mesh.tri_init_u[b];
                const double qby = state.depth[b] * mesh.tri_init_v[b];
                q_[e] = edges_.conveyance[e] > 0.0
                            ? 0.5 * ((qax + qbx) * edges_.nx[e] + (qay + qby) * edges_.ny[e])
                            : 0.0;
            }
            // Perot gather so the θ-blend and CFL speed see the seeded
            // momentum from the very first firing (same stencil as fireCells).
            if (!qcx_.empty()) {
                const auto& ed = edges_;
                for (int i = 0; i < nt; ++i) {
                    double sx = 0.0, sy = 0.0;
                    for (int p = ed.cell_ptr[i]; p < ed.cell_ptr[i + 1]; ++p) {
                        const int e = ed.cell_edge[p];
                        double fq = static_cast<double>(ed.cell_sign[p]) * q_[e] * ed.xi[e];
                        if (mode_ == Momentum2D::LOCAL_INERTIAL)
                            fq *= ed.conveyance[e];
                        sx += fq * (ed.mx[e] - mesh.tri_cx[i]);
                        sy += fq * (ed.my[e] - mesh.tri_cy[i]);
                    }
                    const double inv_a = 1.0 / mesh.tri_area[i];
                    qcx_[i] = sx * inv_a;
                    qcy_[i] = sy * inv_a;
                }
            }
        }
    }
    t_last_sync_ = 0.0;
    substeps_run_ = face_passes_ = last_steps_ = 0;
    last_dt_ = 0.0;
    telemetry_.clear();
    active_frac_min_ = 1.0e30;
    active_frac_max_ = -1.0e30;
    active_frac_sum_ = 0.0;
    active_samples_ = 0;
    if (const char* p = std::getenv("OPENSWMM_2D_MARCHER_TELEMETRY"))
        telemetry_path_ = p;
    refreshSubsurfaceRows();
    if (state_->boundary)
        for (int slot : bc_slot_)
            state_->boundary->ensureReceipt(slot, state_->transport.n_species);
    initialized_ = true;
}

void ExplicitInertialSolver::refreshSubsurfaceRows() {
    gw_row_map_.assign(state_->transport.n_species, -1);
    if (gw_ && gw_->transport().active()) {
        const auto& surface = state_->transport;
        const auto& groundwater = gw_->transport();
        if (surface.row_names.size() != static_cast<std::size_t>(surface.n_species) ||
            groundwater.row_names.size() != static_cast<std::size_t>(groundwater.n_species))
            throw std::invalid_argument(
                "Surface and groundwater transport require a name for every species row.");
        for (int r = 0; r < surface.n_species; ++r)
            for (int g = 0; g < groundwater.n_species; ++g)
                if (surface.row_names[r] == groundwater.row_names[g])
                    gw_row_map_[r] = g;
    }
}

double ExplicitInertialSolver::donorConc(int s, int cell) const noexcept {
    if (rk_transport_stage_) {
        const auto k = state_->transport.idx(s, cell);
        return rk_stage_volume_[cell] > 0 ? rk_stage_mass_[k] / rk_stage_volume_[cell] : 0;
    }

    const auto& tr = state_->transport;
    const double v = state_->volume[static_cast<std::size_t>(cell)];
    // Guard on V > 0 ONLY — deliberately NOT on the dry-depth threshold.
    // A face fires when its FACE depth (from the two heads) exceeds
    // dry_depth, which near a front can happen while the exporter's cell
    // VOLUME sits below dry_depth·area. A depth-based guard here would let
    // that face export water at zero species, concentrating what stays
    // behind and breaking the uniform-concentration property exactly at the
    // wet/dry front — where gate 1 looks hardest. The positivity share
    // already bounds every take at β·V, so any V > 0 can export and its
    // concentration is simply m/V. (The REPORTED concentration keeps the
    // dry threshold: that is a display choice, this is a flux.)
    if (!(v > 0.0))
        return 0.0;
    return tr.cell_mass[tr.idx(s, cell)] / v;
}

void ExplicitInertialSolver::sinkMassAtCellConc(int i, double dv_m3, std::vector<double>& ledger,
                                                double* per_point_ledger) noexcept {
    if (!(dv_m3 > 0.0))
        return;
    auto& tr = state_->transport;
    if (!tr.active())
        return;
    // S7 (D-A22): water leaving through an open edge or a drain to a node
    // is runoff the cell produced; infiltration is not (it does not wash off).
    if (!tr.cell_runoff_vol.empty() && &ledger != &tr.lost_infiltration)
        tr.cell_runoff_vol[static_cast<std::size_t>(i)] += dv_m3;
    // Concentration is read BEFORE the volume was reduced (callers pass the
    // pre-sink volume through state_->volume unchanged until after this
    // call), so the mass removed is exactly dv * c_old — the mass the water
    // that left was carrying.
    for (int s = 0; s < tr.n_species; ++s) {
        const double c = donorConc(s, i);
        if (c == 0.0)
            continue;
        const double dm = dv_m3 * c;
        double& m = tr.cell_mass[tr.idx(s, i)];
        // Cannot remove more than is there (the volume clamps identically);
        // the residual is a rounding artefact, not a modelling event. The
        // SIGNED temperature row (S4) has no floor: °C·m³ below zero is a
        // state, and "min(dm, m)" on two negatives would empty the row.
        const double taken = tr.signedRow(s) ? dm : ((dm < m) ? dm : m);
        m -= taken;
        const auto kind = &ledger == &tr.lost_infiltration ? SourceLedger::Infiltration
                          : &ledger == &tr.lost_boundary   ? SourceLedger::Boundary
                                                           : SourceLedger::CouplingOut;
        bookSourceLedger(kind, s, taken);
        if (per_point_ledger)
            per_point_ledger[s] += taken;
    }
}

int ExplicitInertialSolver::sourceThread() const noexcept {
#if defined(SWMM_USE_OPENMP)
    return omp_in_parallel() ? omp_get_thread_num() : 0;
#else
    return 0;
#endif
}

void ExplicitInertialSolver::bookSourceLedger(SourceLedger ledger, int species,
                                              double mass) noexcept {
    const auto ns = static_cast<std::size_t>(state_->transport.n_species);
    const auto offset = static_cast<std::size_t>(sourceThread()) * source_ledger_stride_;
    source_ledgers_[offset + static_cast<std::size_t>(ledger) * ns + species] += mass;
}

void ExplicitInertialSolver::flushSourceLedgers() {
    auto& tr = state_->transport;
    const auto ns = static_cast<std::size_t>(tr.n_species);
    std::vector<double>* ledgers[] = {
        &tr.lost_infiltration, &tr.lost_coupling,       &tr.gained_rainfall,
        &tr.gained_coupling,   &tr.gained_exfiltration, &tr.lost_boundary,
        &tr.gained_external,   &tr.lost_external,       &tr.gained_bed};
    for (std::size_t base = 0; base < source_ledgers_.size(); base += source_ledger_stride_) {
        for (std::size_t k = 0; k < static_cast<std::size_t>(SourceLedger::Count); ++k)
            for (std::size_t r = 0; r < ns; ++r) {
                double& delta = source_ledgers_[base + k * ns + r];
                if (!ledgers[k]->empty())
                    (*ledgers[k])[r] += delta;
                delta = 0.0;
            }
        double& evap = source_ledgers_[base + static_cast<std::size_t>(SourceLedger::Count) * ns];
        state_->evap_loss_total += evap;
        evap = 0.0;
    }
}

// These shared helpers also run inside the fused per-row loop. Avoid adding
// a function call per species when the surrounding OpenMP kernel is large.
#if defined(_MSC_VER)
#define OPENSWMM_SOURCE_INLINE __forceinline
#elif defined(__GNUC__) || defined(__clang__)
#define OPENSWMM_SOURCE_INLINE inline __attribute__((always_inline))
#else
#define OPENSWMM_SOURCE_INLINE inline
#endif
/**
 * @brief Reserve one cell's held source water budget and stage its shared native/thermal sink
 * tuple.
 * @param[in] i Valid prepared mesh/state cell index (declaration: cell), with positive SI area.
 * @param[in] dt Finite positive firing duration s.
 * @param[out] source Replaced on true with accepted
 * rain/coupling/infiltration/evaporation/outflow m3, available/final water, area*dt and pointer
 * to this worker's native ledger scratch. On false its incoming contents are retained; physical
 * bookkeeping may still have occurred.
 * @return True when a source/heat tuple must be processed; false for an empty held tuple after
 * any soil atmospheric demand booking. Does not include later independent runtime sources.
 * @note Consumes queued groundwater return water, proportions competing held sinks against donor
 * water, reserves accepted groundwater intake/headroom, updates requested/applied forcing
 * receipts, infiltration depth/coupling/evaporation ledgers and ET soil demand. RK2 preserves
 * beginning-donor budgets and separates snow/latent paths. Without a surface temperature row,
 * pairs qualified intake with external rainfall-source temperature. Does not install final
 * surface volume or process every native row; caller runs applyCellSourceRow() then adopts
 * final_volume once. Requires live matching native/ground/snow/thermal owners and correctly
 * provisioned worker scratch. Can allocate/throw after prior receipts/reservations changed; no
 * local rollback. Independent cell producers may run in parallel only under the owning prepared
 * source protocol.
 */
OPENSWMM_SOURCE_INLINE bool ExplicitInertialSolver::prepareCellSources(int i, double dt,
                                                                       CellSourceStep& source) {
    const double area = mesh_->tri_area[i];
    const double area_dt = area * dt;
    const bool split_snow = second_order_ && gw_ && state_->snow.owns(i);
    const double rain = split_snow ? 0 : std::max(0.0, state_->liquidRainfall(i)) * area_dt;
    if (!state_->rk2_rain_pending.empty())
        state_->rk2_rain_pending[i] += rain;
    const double coupling = state_->coupling_flux[i] * area_dt;
    const double requested_infil =
        std::max(0.0, infilSink(state_->runtime_forcings.rate(3, i, state_->infil_rate[i]),
                                state_->depth[i], opts_->dry_depth)) *
        area_dt;
    double evap_demand = second_order_ && gw_
                             ? state_->runtime_forcings.rate(
                                   2, i, opts_->evaporation != 0 ? state_->evap_rate[i] : 0)
                             : state_->exposedEvaporation(i);
    if (split_snow)
        evap_demand *= state_->snow.exposedFractions()[i];
    const double requested_evap =
        std::max(0.0, evapSink(evap_demand, state_->depth[i], opts_->dry_depth)) * area_dt;
    const double back = gw_ ? gw_->takeToSurface(i) : 0.0;
    if (rain == 0.0 && coupling == 0.0 && requested_infil == 0.0 && requested_evap == 0.0 &&
        back == 0.0 && !state_->heat.enabled) {
        if (gw_)
            gw_->bookSurfaceEt(i, dt, evap_demand, 0.0);
        return false;
    }

    const double incoming = rain + std::max(coupling, 0.0) + back;
    const double available = std::max(0.0, state_->volume[i] + incoming);
    const double requested_out = std::max(-coupling, 0.0);
    const double requested = requested_infil + requested_evap + requested_out;
    double donor_water = available;
    if (rk_transport_stage_ &&
        (!state_->rk2_rain_pending.empty() || !bc_cell_.empty() || surfaceLatentCooling())) {
        double exported = 0;
        for (int p = edges_.cell_ptr[i]; p < edges_.cell_ptr[i + 1]; ++p) {
            const int edge = edges_.cell_edge[p];
            const double receipt = edges_.cell_sign[p] > 0 ? face_accum_[face_accum_stride_ * (edge)] : face_accum_[face_accum_stride_ * (edge) + 1];
            exported += std::max(0., -receipt);
        }
        donor_water = std::min(available, std::max(0., rk_stage_volume_[i] - exported));
    }
    const double scale = requested > donor_water && requested > 0.0 ? donor_water / requested : 1.0;
    // Proportional sharing gives all held sinks the same satisfaction ratio.
    const double infil =
        gw_ ? gw_->acceptSurfaceInfiltration(i, requested_infil * scale,
                                             state_->runtime_forcings.get(3, i) != nullptr)
            : requested_infil * scale;
    const double evap = requested_evap * scale;
    if (rk_transport_stage_ && state_->heat.enabled)
        state_->heat.evaporated_m3[i] += evap;
    if (gw_)
        gw_->bookSurfaceEt(i, dt, evap_demand, evap);
    const double out = requested_out * scale;
    if (gw_ && gw_->heat().active() && state_->transport.temp_row < 0) {
        // Without a surface temperature state, surface water is an external
        // prescribed-temperature reservoir. Use the existing rainfall/source
        // temperature rather than inventing zero-degree infiltration. Return
        // water and its paired heat leave the modeled thermal domain together.
        gw_->takeToSurfaceMass(i, gw_->transport().temp_row);
        const double temperature =
            gw_->heat().context->heat_config.source_temp(HeatSource::RAINFALL, -1);
        gw_->bookInfiltrationMass(i, gw_->transport().temp_row, infil * temperature);
        if (auto* row = const_cast<RuntimeForcing*>(state_->runtime_forcings.get(3, i))) {
            row->receipt.heat_requested +=
                requested_infil * temperature * opts_->water_heat_capacity;
            row->receipt.heat_applied += infil * temperature * opts_->water_heat_capacity;
        }
    }
    if (auto* row = const_cast<RuntimeForcing*>(state_->runtime_forcings.get(3, i))) {
        auto& receipt = row->receipt;
        receipt.requested += requested_infil;
        receipt.applied += infil;
        receipt.water_out += infil;
        receipt.last_flow = infil / dt;
        receipt.last_dt = dt;
    }
    state_->infil_applied[i] += infil / area;
    state_->coupling_applied[i] += std::max(coupling, 0.0) - out;
    const auto ns = static_cast<std::size_t>(state_->transport.n_species);
    double* const local_ledger =
        source_ledgers_.data() + static_cast<std::size_t>(sourceThread()) * source_ledger_stride_;
    local_ledger[static_cast<std::size_t>(SourceLedger::Count) * ns] += evap;

    if (species_on_ && !state_->transport.cell_runoff_vol.empty())
        state_->transport.cell_runoff_vol[i] += out - std::max(coupling, 0.0);
    source = {area_dt,
              rain,
              coupling,
              available,
              infil,
              evap,
              out,
              gw_ ? std::max(0.0, available - infil - evap - out)
                  : (requested >= available ? 0.0 : available - requested),
              requested_infil,
              local_ledger};
    return true;
}

/**
 * @brief Mix paired groundwater return, accepted rain and positive held coupling into one native
 * row.
 * @param[in] i Valid prepared cell (declaration: cell).
 * @param[in] r Valid native transport row (declaration: row).
 * @param[in,out] mass Writable current cell inventory in native concentration*m3, signed for
 * temperature.
 * @param[in] source Prepared tuple from prepareCellSources(), with valid worker-owned ledger
 * pointer.
 * @note Drains matching groundwater native return once, chooses runtime rainfall composition
 * over default rain_conc where available, and adds source.coupling's native rate-density
 * payload. Books Exfiltration/Rainfall/CouplingIn in scratch. Does not change water, intensive
 * sinks, canonical thermal energy or clocks. Requires stable name mapping/buffer extents and
 * exclusive cell row access; no general finite-value validation or allocation.
 */
OPENSWMM_SOURCE_INLINE void ExplicitInertialSolver::mixCellSourceRow(int i, int r, double& mass,
                                                                     const CellSourceStep& source) {
    auto& tr = state_->transport;
    const auto ns = static_cast<std::size_t>(tr.n_species);
    const auto book = [&source, ns](SourceLedger kind, int row, double amount) {
        source.ledger[static_cast<std::size_t>(kind) * ns + row] += amount;
    };
    if (gw_ && r < static_cast<int>(gw_row_map_.size()) && gw_row_map_[r] >= 0) {
        const double dm = gw_->takeToSurfaceMass(i, gw_row_map_[r]);
        mass += dm;
        book(SourceLedger::Exfiltration, r, dm);
    }
    const auto* rain_row = state_->runtime_forcings.get(4, i);
    if (!rain_row || rain_row->concentrations.empty())
        rain_row = state_->runtime_forcings.get(1, i);
    const auto& rain_conc =
        rain_row && !rain_row->concentrations.empty() ? rain_row->concentrations : tr.rain_conc;
    const double rain_mass =
        static_cast<std::size_t>(r) < rain_conc.size() ? source.rain * rain_conc[r] : 0.0;
    const double coupling_mass = source.coupling > 0.0 && !tr.coupling_src.empty()
                                     ? tr.coupling_src[tr.idx(r, i)] * source.area_dt
                                     : 0.0;
    mass += rain_mass + coupling_mass;
    book(SourceLedger::Rainfall, r, rain_mass);
    book(SourceLedger::CouplingIn, r, coupling_mass);
}

/**
 * @brief Apply a prepared cell's mixed native row, finite contacts and accepted intensive water
 * sinks.
 * @param[in] i Valid prepared cell (declaration: cell).
 * @param[in] r Valid native row (declaration: row), matching ground and heat mappings.
 * @param[in,out] mass Cell native inventory (concentration*m3); updated in place.
 * @param[in] source Accepted water tuple with stable positive available volume where needed,
 * area*dt and writable worker ledger. Must be consumed once for each row before final surface
 * water adoption.
 * @note Mixes arrivals unless shared channel temperature already mixed, applies ordinary
 * finite-bed species/thermal contact before intensive sinks, then debits infiltration/coupling
 * and age/temperature carried by evaporation. RK2 uses beginning-stage donor concentration and
 * unsigned availability, with latent thermal debit/forcing heat receipts. Solutes remain after
 * evaporation. Pairs accepted infiltration native amount with groundwater and optional
 * standalone-ground sensible advection. Books native scratch and forcing/thermal ledgers. Does
 * not install final surface volume itself; thermal failure can be flagged rather than thrown.
 * Requires exclusive prepared cell/coupled owner access; allocation/delegated exceptions may
 * follow partial row/ledger mutation, no local rollback.
 */
OPENSWMM_SOURCE_INLINE void
ExplicitInertialSolver::applyCellSourceRow(int i, int r, double& mass,
                                           const CellSourceStep& source) {
    auto& tr = state_->transport;
    const auto ns = static_cast<std::size_t>(tr.n_species);
    const auto book = [&source, ns](SourceLedger kind, int row, double amount) {
        source.ledger[static_cast<std::size_t>(kind) * ns + row] += amount;
    };
    const bool mixed = r == tr.temp_row && std::size_t(i) < channel_temperature_mixed_.size() &&
                       channel_temperature_mixed_[i];
    if (!mixed) {
        mixCellSourceRow(i, r, mass, source);
        if (r != tr.temp_row && state_->heat.enabled && state_->heat.bed_species > 0 &&
            !rk_transport_stage_) {
            const double before = mass;
            mass = applyLocalBedSpecies(*mesh_, state_->heat, i, r, mass, source.available,
                                        source.area_dt / mesh_->tri_area[i]);
            book(SourceLedger::BedExchange, r, mass - before);
            if (mass != before && gw_ && state_->heat.ground_column &&
                std::size_t(i) < state_->heat.aquifer_owned.size() &&
                state_->heat.aquifer_owned[std::size_t(i)])
                gw_->heat().refreshView(i, gw_->transport());
        }
        if (r == tr.temp_row && state_->heat.enabled && !rk_transport_stage_) {
            const double before = mass;
            mass = applyMeshHeatCell(*mesh_, *opts_, state_->heat, i, mass, source.available,
                                     source.evap, source.area_dt / mesh_->tri_area[i]);
            if (gw_ && state_->heat.ground_column && state_->heat.aquifer_owned[i])
                gw_->heat().refreshView(i, gw_->transport());
            // Surface-row continuity includes heat gained from both atmosphere
            // and bed. The combined thermal budget separately cancels bed exchange.
            book(SourceLedger::ExternalIn, r, mass - before);
        }
    }
    // All sinks see the same mixed concentration. Passing cell-local
    // transfers directly to groundwater avoids shared-ledger deltas.
    double concentration = source.available > 0.0 ? mass / source.available : 0.0;
    if (rk_transport_stage_) {
        concentration = donorConc(r, i);
        const double requested =
            source.infil + source.out + ((r == tr.age_row || r == tr.temp_row) ? source.evap : 0);
        if (!tr.signedRow(r) && requested > 0)
            concentration = std::min(concentration, std::max(0., mass) / requested);
    }
    const double mi = concentration * source.infil;
    if (r == tr.temp_row && state_->heat.ground_column)
        advectStandaloneGround(state_->heat, i, source.infil, concentration);
    const double mc = concentration * source.out;
    const bool intensive = r == tr.age_row || r == tr.temp_row;
    const double me = intensive ? concentration * source.evap : 0.0;
    mass -= mi + mc + me;
    if (!tr.signedRow(r) && mass < 0.0)
        mass = 0.0;
    book(SourceLedger::Infiltration, r, mi);
    book(SourceLedger::CouplingOut, r, mc);
    if (rk_transport_stage_)
        book(SourceLedger::ExternalOut, r, me);
    if (rk_transport_stage_ && r == tr.temp_row && state_->heat.enabled && state_->heat.surface &&
        source.evap > 0) {
        auto& heat = state_->heat;
        const double latent = transport::heat::latentHeatOfVaporization(concentration);
        const double joules = heat.context->options.water_density * latent * source.evap;
        if (!(latent > 0) || !std::isfinite(joules) ||
            !std::isfinite(mass - joules / opts_->water_heat_capacity))
            heat.failures[i] = 1;
        else {
            mass -= joules / opts_->water_heat_capacity;
            heat.latent_j[i] -= joules;
            heat.atmospheric_j[i] -= joules;
            heat.water_j[i] -= joules;
            book(SourceLedger::ExternalIn, r, -joules / opts_->water_heat_capacity);
        }
    }
    if (auto* row = const_cast<RuntimeForcing*>(state_->runtime_forcings.get(3, i))) {
        row->receipt.requested_mass[r] += concentration * source.requested_infil;
        row->receipt.applied_mass[r] += mi;
        if (r == tr.temp_row) {
            row->receipt.heat_requested +=
                concentration * source.requested_infil * opts_->water_heat_capacity;
            row->receipt.heat_applied += mi * opts_->water_heat_capacity;
        }
    }
    if (gw_ && r < static_cast<int>(gw_row_map_.size()) && gw_row_map_[r] >= 0)
        gw_->bookInfiltrationMass(i, gw_row_map_[r], mi);
}

// Water-only held sources; keep prepareCellSources() expression and booking order.
bool ExplicitInertialSolver::applyPlainCellSources(int i, double dt) {
    const double area = mesh_->tri_area[i];
    const double area_dt = area * dt;
    const double rain = std::max(0.0, state_->rainfall[i]) * area_dt;
    const double coupling = state_->coupling_flux[i] * area_dt;
    const double requested_infil =
        std::max(0.0, infilSink(state_->infil_rate[i], state_->depth[i], opts_->dry_depth)) *
        area_dt;
    const double evap_demand = state_->evap_rate[i] * 1.;
    const double requested_evap =
        std::max(0.0, evapSink(evap_demand, state_->depth[i], opts_->dry_depth)) * area_dt;
    if (rain == 0.0 && coupling == 0.0 && requested_infil == 0.0 && requested_evap == 0.0)
        return false;
    const double incoming = rain + std::max(coupling, 0.0) + 0.0;
    const double available = std::max(0.0, state_->volume[i] + incoming);
    const double requested_out = std::max(-coupling, 0.0);
    const double requested = requested_infil + requested_evap + requested_out;
    const double scale = requested > available && requested > 0.0 ? available / requested : 1.0;
    const double infil = requested_infil * scale;
    const double evap = requested_evap * scale;
    const double out = requested_out * scale;
    state_->infil_applied[i] += infil / area;
    state_->coupling_applied[i] += std::max(coupling, 0.0) - out;
    double* const local_ledger =
        source_ledgers_.data() + static_cast<std::size_t>(sourceThread()) * source_ledger_stride_;
    local_ledger[0] += evap;
    state_->volume[i] = requested >= available ? 0.0 : available - requested;
    return true;
}

/**
 * @brief Consume one accepted held cell-source tuple and then any ordinary runtime
 * prescriptions.
 * @param[in] i Valid prepared cell (declaration: cell), with matching water/native/physical
 * mappings.
 * @param[in] dt Finite positive firing duration s.
 * @param[in,out] boundary_donor Optional current boundary donor m3, reduced by accepted held
 * infiltration+evaporation+outflow with a zero floor. Must not alias canonical state slots.
 * @return True when held tuple applied or eligible runtime source work occurs; false when no
 * owned source work. Common-source-owned cells bypass ordinary held water and run only qualified
 * finite native contact plus enabled runtime water sources.
 * @note Reuses prepared shared channel tuple when present, processes all native rows, installs
 * final surface volume and applies runtime sources. Call exactly once per accepted cell firing
 * and flush thread ledgers after producer completion. No general shape/finite validation or
 * whole-call rollback; requires exclusive cell/coupled ownership and live prepared worker
 * scratch.
 */
bool ExplicitInertialSolver::applyCellSources(int i, double dt, double* boundary_donor) {
    if (plain_sources_)
        return applyPlainCellSources(i, dt);
    if (common_stage_ && common_stage_->ownsCell(i)) {
        // The common source stage owns mesh-only water bookkeeping, but the
        // shared surface/ground quality profile still needs its local bed
        // contact at each accepted 2D cell firing. Keep this finite-store
        // exchange here so it runs when source-water kernels are bypassed.
        if (common_stage_->meshOnlyGroundQuality() && species_on_ && state_->heat.enabled &&
            state_->heat.bed && state_->heat.ground_column && i >= 0 &&
            std::size_t(i) < state_->heat.aquifer_owned.size() &&
            state_->heat.aquifer_owned[std::size_t(i)]) {
            auto& tr = state_->transport;
            bool changed = false;
            for (int r = 0; r < tr.n_species && r < state_->heat.bed_species; ++r) {
                if (r == tr.temp_row)
                    continue;
                double& mass = tr.cell_mass[tr.idx(r, i)];
                const double before = mass;
                mass =
                    applyLocalBedSpecies(*mesh_, state_->heat, i, r, mass, state_->volume[i], dt);
                const double transfer = mass - before;
                if (transfer != 0.0) {
                    bookSourceLedger(SourceLedger::BedExchange, r, transfer);
                    changed = true;
                }
            }
            if (changed && gw_)
                gw_->heat().refreshView(i, gw_->transport());
        }
        return common_stage_->runtimeWaterEnabled() && applyRuntimeSources(i, dt);
    }
    CellSourceStep source;
    if (std::size_t(i) < channel_source_ready_.size() && channel_source_ready_[i]) {
        source = channel_sources_[i];
        source.ledger =
            source_ledgers_.data() + std::size_t(sourceThread()) * source_ledger_stride_;
    } else if (!prepareCellSources(i, dt, source))
        return applyRuntimeSources(i, dt);
    if (species_on_) {
        auto& tr = state_->transport;
        for (int r = 0; r < tr.n_species; ++r)
            applyCellSourceRow(i, r, tr.cell_mass[tr.idx(r, i)], source);
    }
    if (boundary_donor)
        *boundary_donor = std::max(0., *boundary_donor - source.infil - source.evap - source.out);
    state_->volume[i] = source.final_volume;
    applyRuntimeSources(i, dt);
    return true;
}

#undef OPENSWMM_SOURCE_INLINE

namespace {
/**
 * @brief Advance the qualified runtime source operator by two Euler evaluations and conservative
 * SSP blend.
 * @param[in,out] state Prepared staged water/native/runtime source owner with a valid RK2
 * runtime profile. Rebuilds source cell lookup and updates canonical volume/mass and
 * provider/transport ledgers.
 * @param[in] options Resolved finite positive water_heat_capacity J/(m3 K), used for independent
 * heat.
 * @param[in] seconds Finite positive source-operator interval s, validated by the owning caller.
 * @throws std::runtime_error Invalid runtime profile, nonfinite proposal/state/blend or unsigned
 * inventory underflow. Temporary allocation can also throw.
 * @note Each Euler evaluation shares beginning water/native donor budgets so incoming mass/water
 * cannot fund same-stage exports; signed temperature retains signed inventory. Blends physical
 * stores and cumulative receipts with the saved start, checks resulting profile, and leaves
 * diagnostic last flow/duration fields to owner scheduling. No head/depth reconstruction,
 * provider callback, independent thermal contact or engine calendar advance. Exceptions may
 * follow partial staged changes; enclosing coupled RK2 transaction owns rollback and serial
 * access.
 */
void advanceRk2RuntimeSources(SurfaceStateData& state, const SolverOptions2D& options,
                              double seconds) {
    if (!state.runtime_sources.active())
        return;
    if (auto error = rk2SurfaceRuntimeProfile(state); !error.empty())
        throw std::runtime_error(error);
    const auto volume0 = state.volume;
    const auto transport0 = state.transport;
    const auto sources0 = state.runtime_sources;
    auto& tr = state.transport;
    auto& sources = state.runtime_sources;
    sources.rebuild(int(state.volume.size()));
    for (int stage = 0; stage < 2; ++stage)
        for (int cell = 0; cell < int(state.volume.size()); ++cell) {
            const auto& rows = sources.cells[cell];
            if (rows.empty())
                continue;
            const double water = state.volume[cell];
            double incoming = 0, requested = 0;
            for (auto k : rows) {
                incoming += std::max(0., sources.rows[k].flow) * seconds;
                requested += std::max(0., -sources.rows[k].flow) * seconds;
            }
            if (!std::isfinite(incoming) || !std::isfinite(requested))
                throw std::runtime_error("Nonfinite RK2 runtime water proposal.");
            const double scale = requested > water ? water / requested : 1.;
            double remaining_water = water;
            std::vector<double> accepted(rows.size());
            for (std::size_t j = 0; j < rows.size(); ++j) {
                accepted[j] = std::min(remaining_water,
                                       std::max(0., -sources.rows[rows[j]].flow) * seconds * scale);
                remaining_water -= accepted[j];
            }
            for (int r = 0; r < tr.n_species; ++r) {
                auto& mass = tr.cell_mass[tr.idx(r, cell)];
                const double old = mass, concentration = water > 0 ? old / water : 0;
                double remaining = old, added = 0, sink = 0;
                for (auto k : rows)
                    sink += std::max(0., -sources.rows[k].rates[r]) * seconds;
                if (!std::isfinite(sink))
                    throw std::runtime_error("Nonfinite RK2 runtime species proposal.");
                for (std::size_t j = 0; j < rows.size(); ++j) {
                    auto& row = sources.rows[rows[j]];
                    const double wanted = std::max(0., -row.flow) * seconds * concentration;
                    const double taken = tr.signedRow(r)
                                             ? accepted[j] * concentration
                                             : std::min(remaining, accepted[j] * concentration);
                    remaining -= taken;
                    row.receipt.requested_mass[r] -= wanted;
                    row.receipt.applied_mass[r] -= taken;
                    tr.lost_external[r] += taken;
                }
                const double factor = sink > 0 ? std::min(1., remaining / sink) : 1.;
                for (auto k : rows) {
                    auto& row = sources.rows[k];
                    const double wanted = std::max(0., -row.rates[r]) * seconds;
                    const double taken = wanted > 0 ? std::min(remaining, wanted * factor) : 0;
                    remaining -= taken;
                    row.receipt.requested_mass[r] -= wanted;
                    row.receipt.applied_mass[r] -= taken;
                    tr.lost_external[r] += taken;
                    const double heat =
                        r == tr.temp_row ? row.heat * seconds / options.water_heat_capacity : 0;
                    const double heat_applied = water > 0 || incoming > 0 ? heat : 0;
                    const double receipt =
                        std::max(0., row.flow) * seconds * row.concentrations[r] +
                        std::max(0., row.rates[r]) * seconds;
                    row.receipt.requested_mass[r] += receipt + heat;
                    row.receipt.applied_mass[r] += receipt + heat_applied;
                    added += receipt + heat_applied;
                    tr.gained_external[r] += receipt + heat_applied;
                }
                mass = remaining + added;
                if (!std::isfinite(mass) || (!tr.signedRow(r) && mass < 0))
                    throw std::runtime_error("Invalid RK2 runtime species inventory.");
            }
            state.volume[cell] = remaining_water + incoming;
            if (!std::isfinite(state.volume[cell]))
                throw std::runtime_error("Nonfinite RK2 runtime water inventory.");
            for (std::size_t j = 0; j < rows.size(); ++j) {
                auto& row = sources.rows[rows[j]];
                auto& receipt = row.receipt;
                const double in = std::max(0., row.flow) * seconds;
                receipt.requested += row.flow * seconds;
                receipt.applied += in - accepted[j];
                receipt.water_in += in;
                receipt.water_out += accepted[j];
                receipt.heat_requested += row.heat * seconds;
                if (water > 0 || incoming > 0)
                    receipt.heat_applied += row.heat * seconds;
            }
        }
    const auto blend = [](double& value, double old) {
        value = old + .5 * (value - old);
        if (!std::isfinite(value))
            throw std::runtime_error("Nonfinite RK2 runtime blend.");
    };
    for (std::size_t i = 0; i < state.volume.size(); ++i)
        blend(state.volume[i], volume0[i]);
    for (std::size_t k = 0; k < tr.cell_mass.size(); ++k)
        blend(tr.cell_mass[k], transport0.cell_mass[k]);
    for (int r = 0; r < tr.n_species; ++r) {
        blend(tr.gained_external[r], transport0.gained_external[r]);
        blend(tr.lost_external[r], transport0.lost_external[r]);
    }
    for (std::size_t k = 0; k < sources.rows.size(); ++k) {
        auto& b = sources.rows[k].receipt;
        const auto& old = sources0.rows[k].receipt;
        blend(b.requested, old.requested);
        blend(b.applied, old.applied);
        blend(b.water_in, old.water_in);
        blend(b.water_out, old.water_out);
        blend(b.heat_requested, old.heat_requested);
        blend(b.heat_applied, old.heat_applied);
        for (int r = 0; r < tr.n_species; ++r) {
            blend(b.requested_mass[r], old.requested_mass[r]);
            blend(b.applied_mass[r], old.applied_mass[r]);
        }
    }
    if (auto error = rk2SurfaceRuntimeProfile(state); !error.empty())
        throw std::runtime_error(error);
}
}
/**
 * @brief Apply ordinary runtime cell water/native/heat prescriptions with one shared sink
 * budget.
 * @param[in] i Valid prepared nonnegative cell (declaration: cell); high/out-of-range source
 * lookup or no rows returns false. Negative i is not guarded.
 * @param[in] dt Finite positive elapsed firing duration s; caller validated.
 * @return True after processing any mapped runtime rows, false when no rows or coupled
 * groundwater RK2 owns the separate symmetric source operator.
 * @note Mixes all provider inflows/composition/positive mass/heat before water withdrawal;
 * proportionally limits water sinks against mixed available m3, then independent native sinks
 * against remaining mass. Updates requested/applied water/native/J receipts and worker external
 * ledgers, installs nonnegative surface volume. Does not reconstruct head/depth, call providers,
 * move engine time or flush workers. Requires prevalidated row extents/units, stable lookup and
 * exclusive cell/receipt access. Mutates progressively without general finite checks or
 * rollback; producer validation owns admissibility.
 */
bool ExplicitInertialSolver::applyRuntimeSources(int i, double dt) {
    if (second_order_ && gw_)
        return false; // symmetric owner, outside both water Euler evaluations
    auto& store = state_->runtime_sources;
    if (i >= static_cast<int>(store.cells.size()) || store.cells[i].empty())
        return false;
    auto& tr = state_->transport;
    double incoming = 0.0, outgoing = 0.0;
    for (auto k : store.cells[i]) {
        const auto& s = store.rows[k];
        incoming += std::max(0.0, s.flow) * dt;
        outgoing += std::max(0.0, -s.flow) * dt;
    }
    const double available = state_->volume[i] + incoming;
    const double scale = outgoing > available ? available / outgoing : 1.0;
    // All providers mix before any withdrawal, independent of registration order.
    for (int r = 0; r < tr.n_species; ++r) {
        double& mass = tr.cell_mass[tr.idx(r, i)];
        for (auto k : store.cells[i]) {
            auto& s = store.rows[k];
            double dm =
                std::max(0.0, s.flow) * dt * s.concentrations[r] + dt * std::max(0.0, s.rates[r]);
            if (r == tr.temp_row && available > 0.0)
                dm += s.heat * dt / opts_->water_heat_capacity;
            mass += dm;
            s.receipt.requested_mass[r] += dm;
            s.receipt.applied_mass[r] += dm;
            bookSourceLedger(SourceLedger::ExternalIn, r, dm);
        }
        const double conc = available > 0.0 ? mass / available : 0.0;
        for (auto k : store.cells[i]) {
            auto& s = store.rows[k];
            const double requested = std::max(0.0, -s.flow) * dt * conc;
            const double applied = requested * scale;
            mass -= applied;
            s.receipt.requested_mass[r] -= requested;
            s.receipt.applied_mass[r] -= applied;
            bookSourceLedger(SourceLedger::ExternalOut, r, applied);
        }
        // Independent species removals share the remaining mass after water
        // withdrawals; no provider can remove mass another provider already took.
        double sink = 0.0;
        for (auto k : store.cells[i])
            sink += std::max(0.0, -store.rows[k].rates[r]) * dt;
        const double mass_scale = sink > 0.0 ? std::min(1.0, std::max(0.0, mass) / sink) : 1.0;
        for (auto k : store.cells[i]) {
            auto& s = store.rows[k];
            const double requested = std::max(0.0, -s.rates[r]) * dt;
            const double applied = requested * mass_scale;
            mass -= applied;
            s.receipt.requested_mass[r] -= requested;
            s.receipt.applied_mass[r] -= applied;
            bookSourceLedger(SourceLedger::ExternalOut, r, applied);
        }
        if (!tr.signedRow(r))
            mass = std::max(0.0, mass);
    }
    for (auto k : store.cells[i]) {
        auto& s = store.rows[k];
        const double in = std::max(0.0, s.flow) * dt;
        const double out = std::max(0.0, -s.flow) * dt * scale;
        s.receipt.requested += s.flow * dt;
        s.receipt.applied += in - out;
        s.receipt.last_flow = (in - out) / dt;
        s.receipt.last_dt = dt;
        s.receipt.water_in += in;
        s.receipt.water_out += out;
        s.receipt.heat_requested += s.heat * dt;
        if (available > 0.0)
            s.receipt.heat_applied += s.heat * dt;
    }
    state_->volume[i] = std::max(0.0, available - outgoing * scale);
    return true;
}

void ExplicitInertialSolver::reconstructAll() {
    const int nt = mesh_->n_triangles();
#pragma omp parallel for schedule(static) num_threads(opts_->num_threads)
    for (int i = 0; i < nt; ++i) {
        inertial::cellEtaDepth(*mesh_, *opts_, i, state_->volume[i], state_->head[i],
                               state_->depth[i]);
    }
}

void ExplicitInertialSolver::settleAccumulators() {
    // G1: the subsurface settles on the SAME trigger and for the same
    // reason. A re-tier re-indexes the tier lists, so any volume still
    // sitting in a side accumulator would be gathered by a cell that no
    // longer fires at that cadence — the stranded-flux hazard. Done first so
    // Dunne water settled here is visible to the surface pass below.
    if (gw_) {
        gw_->settle(*state_);
        gw_->compactPending();
    }
    if (!accumulators_pending_)
        return;
    accumulators_pending_ = false;
    const int nt = mesh_->n_triangles();
    const auto& ed = edges_;
#pragma omp parallel for schedule(static) num_threads(opts_->num_threads)
    for (int i = 0; i < nt; ++i) {
        double pending = 0.0;
        for (int p = ed.cell_ptr[i]; p < ed.cell_ptr[i + 1]; ++p) {
            const int e = ed.cell_edge[p];
            if (ed.cell_sign[p] > 0) {
                pending += face_accum_[face_accum_stride_ * (e)];
                face_accum_[face_accum_stride_ * (e)] = 0.0;
            } else {
                pending += face_accum_[face_accum_stride_ * (e) + 1];
                face_accum_[face_accum_stride_ * (e) + 1] = 0.0;
            }
        }
        // S1: settle the species side in the SAME sweep. A cell re-tiered
        // or deactivated with a pending species accumulator would strand
        // mass exactly as the header warns for volume.
        if (species_on_) {
            auto& tr = state_->transport;
            const auto ns = static_cast<std::size_t>(tr.n_species);
            for (std::size_t s = 0; s < ns; ++s) {
                double dm = 0.0;
                for (int p = ed.cell_ptr[i]; p < ed.cell_ptr[i + 1]; ++p) {
                    const auto e = static_cast<std::size_t>(ed.cell_edge[p]);
                    if (ed.cell_sign[p] > 0) {
                        dm += sacc_L_[e * ns + s];
                        sacc_L_[e * ns + s] = 0.0;
                    } else {
                        dm += sacc_R_[e * ns + s];
                        sacc_R_[e * ns + s] = 0.0;
                    }
                }
                if (dm != 0.0) {
                    double& m = tr.cell_mass[tr.idx(static_cast<int>(s), i)];
                    m += dm;
                    // Backstop is for nonnegative rows only: the SIGNED
                    // temperature row holds °C·m³ below zero as a state.
                    if (m < 0.0 && !tr.signedRow(static_cast<int>(s)))
                        m = 0.0;
                }
            }
        }
        // FULL_SWE: settle the MOMENTUM accumulators in the same sweep, for
        // the same reason as the species rows above — a cell re-tiered or
        // deactivated between its firings would otherwise strand the momentum
        // of mass that has already been applied, and pick it up stale on
        // reactivation.
        if ((face_accum_stride_ == 6 && !face_accum_.empty())) {
            double dmx = 0.0, dmy = 0.0;
            for (int p = ed.cell_ptr[i]; p < ed.cell_ptr[i + 1]; ++p) {
                const int e = ed.cell_edge[p];
                if (ed.cell_sign[p] > 0) {
                    dmx += face_accum_[face_accum_stride_ * (e) + 2];
                    face_accum_[face_accum_stride_ * (e) + 2] = 0.0;
                    dmy += face_accum_[face_accum_stride_ * (e) + 4];
                    face_accum_[face_accum_stride_ * (e) + 4] = 0.0;
                } else {
                    dmx += face_accum_[face_accum_stride_ * (e) + 3];
                    face_accum_[face_accum_stride_ * (e) + 3] = 0.0;
                    dmy += face_accum_[face_accum_stride_ * (e) + 5];
                    face_accum_[face_accum_stride_ * (e) + 5] = 0.0;
                }
            }
            if (dmx != 0.0 || dmy != 0.0) {
                const double A = mesh_->tri_area[i];
                qcx_[i] += dmx / A; // same normalisation as fireCells
                qcy_[i] += dmy / A;
            }
        }
        if (species_on_ && !state_->transport.cell_runoff_vol.empty())
            state_->transport.cell_runoff_vol[i] -= pending;
        if (pending == 0.0)
            continue;
        double v = state_->volume[i] + pending;
        state_->volume[i] = (v > 0.0) ? v : 0.0;
        inertial::cellEtaDepth(*mesh_, *opts_, i, state_->volume[i], state_->head[i],
                               state_->depth[i]);
    }
}

void ExplicitInertialSolver::lazySourcesOnly(double t) {
    const double dt_lazy = t - t_last_sync_;
    if (dt_lazy <= 0.0)
        return;
    const int nt = mesh_->n_triangles();
#pragma omp parallel for schedule(static) num_threads(opts_->num_threads)
    for (int i = 0; i < nt; ++i) {
        if (cell_active_[i])
            continue;
        // An inactive cell with no applied sources retains its closure.
        if (applyCellSources(i, dt_lazy))
            inertial::cellEtaDepth(*mesh_, *opts_, i, state_->volume[i], state_->head[i],
                                   state_->depth[i]);
    }
    flushSourceLedgers();
    t_last_sync_ = t;
}

void ExplicitInertialSolver::syncAndRebuild(double t) {
    perf::ProcessTimer process_timer(perf::ProcessPhase::surface_rebuild);
    settleAccumulators();
    const double dt_lazy = t - t_last_sync_;
    const int nt = mesh_->n_triangles();

    // 2. Seed: hysteretic depth threshold (entering cells need h_on, active
    //    cells stay until h_off), plus concentrated sources (held coupling)
    //    and non-wall boundary cells. Rain alone does NOT activate — that is
    //    the point of the lazy tier.
    //    The hysteresis band scales with H_MOVE (capped at the historical
    //    ±1 mm): a fixed ±1 mm band made H_MOVE 1e-4 require 1.1 mm to
    //    activate — 10× the requested threshold — freezing wetting/drying
    //    fronts on shallow benchmarks (Thacker). Bit-identical at the
    //    default h_move = 0.003 (band = 1 mm either way).
    const double band = std::min(0.001, 0.5 * opts_->h_move);
    const double h_on = opts_->h_move + band;
    h_on_front_ = h_on;
    const double h_off = std::max(0.0, opts_->h_move - band);
    rebuild_seed_.assign(static_cast<std::size_t>(nt), 0);
    std::vector<uint8_t>& next = rebuild_seed_;
#pragma omp parallel for schedule(static) num_threads(opts_->num_threads)
    for (int i = 0; i < nt; ++i) {
        // Settle this inactive cell before testing its activation threshold.
        // Source and seed work share a partition, retaining ledger fold order.
        if (dt_lazy > 0.0 && !cell_active_[i] && applyCellSources(i, dt_lazy))
            inertial::cellEtaDepth(*mesh_, *opts_, i, state_->volume[i], state_->head[i],
                                   state_->depth[i]);
        const double thresh = cell_active_[i] ? h_off : h_on;
        // Covered RK2 cells participate in both surface source evaluations,
        // even while dry. Lazy settlement would book atmospheric demand only
        // after the aquifer step and leave a half-blended ET backlog.
        if (state_->depth[i] >= thresh || state_->coupling_flux[i] != 0.0 || pin_t0_[i] ||
            state_->heat.enabled ||
            (second_order_ && gw_ &&
             (gw_->covers(i) || !state_->runtime_sources.rows.empty() ||
              !state_->runtime_forcings.rows.empty())))
            next[i] = 1;
    }

    if (dt_lazy > 0.0) {
        flushSourceLedgers();
        t_last_sync_ = t;
    }

    // G1: a cell the aquifer owes water to must route it. Exfiltration and
    // Dunne excess are booked into the subsurface's xacc_to_surface and
    // drained by the SURFACE cell's firing — so a dry cell above a rising
    // table would hold that water forever, because on a dry bed nothing else
    // ever activates it. This is the one seed that does not come from the
    // surface's own state. Serial: the list is short (cells that saturated),
    // and settleAccumulators() above already compacted it, so every entry is
    // still owed.
    if (gw_) {
        for (const int i : gw_->pendingSurfaceCells())
            if (i >= 0 && i < nt)
                next[static_cast<std::size_t>(i)] = 1;
    }

    // 3. One-ring halo so fronts can enter their neighbours within a rebuild
    //    period, then compact the work lists. A face flows only when BOTH
    //    incident cells are active — a one-sided face would export volume into
    //    a cell whose update never runs (measured as an 18 % basin loss); the
    //    halo guarantees the front always has an active receiving cell.
    // Optional stresses/rotation can accelerate any wet neighbor. Include all
    // cells in the qualified global mode, including a newly wetted dry cell.
    if (opts_->momentumPhysics()) std::fill(next.begin(), next.end(), uint8_t{1});
    cell_active_ = next;
    const int n_seed = static_cast<int>(std::count(next.begin(), next.end(), uint8_t{1}));
    if (n_seed != 0 && n_seed != nt) {
        for (int e = 0; e < edges_.ne; ++e) {
            const int a = edges_.cL[e], b = edges_.cR[e];
            if (next[a] && !next[b])
                cell_active_[b] = 1;
            else if (next[b] && !next[a])
                cell_active_[a] = 1;
        }
    }
    // FRONT_REBUILD: a dry-bed front travels ~2√(gh) — about one cell per
    // base substep at CFL ½ — so between two macro cycles it can cross
    // several rings. Widen the halo so the front never runs out of active
    // receiving cells before the breach-triggered rebuild lands.
    if (front_rebuild_ && n_seed != 0 && n_seed != nt) {
        // Cell-parallel Jacobi rings (each thread writes only its own cell,
        // reading the previous ring's copy): the same set as the serial
        // edge walk produced, without the O(n_edges) serial passes.
        for (int ring = 1; ring < kFrontHaloRings; ++ring) {
            rebuild_seed_ = cell_active_;
#pragma omp parallel for schedule(static) num_threads(opts_->num_threads)
            for (int i = 0; i < nt; ++i) {
                if (rebuild_seed_[i])
                    continue;
                const int nvc = mesh_->cell_vertex_count(i);
                for (int k2 = 0; k2 < nvc; ++k2) {
                    const int j = mesh_->cell_neighbour(i, k2);
                    if (j >= 0 && rebuild_seed_[j]) {
                        cell_active_[i] = 1;
                        break;
                    }
                }
            }
        }
    }
    active_cells_.clear();
    if (useParallelCompaction(nt, opts_->num_threads)) {
        compactOrdered<false>(nt, 1, opts_->num_threads, compactionCounts(),
            [&](int i) { return cell_active_[i] ? 0 : -1; }, [](int i) { return i; },
            [&](int) -> std::vector<int>& { return active_cells_; });
    } else {
        for (int i = 0; i < nt; ++i)
            if (cell_active_[i])
                active_cells_.push_back(i);
    }
    if (front_rebuild_) {
        // Outer ring of the halo: an active cell with an inactive neighbour.
#pragma omp parallel for schedule(static) num_threads(opts_->num_threads)
        for (int i = 0; i < nt; ++i) {
            uint8_t f = 0;
            if (cell_active_[i]) {
                const int nvc = mesh_->cell_vertex_count(i);
                for (int k2 = 0; k2 < nvc; ++k2) {
                    const int j = mesh_->cell_neighbour(i, k2);
                    if (j >= 0 && !cell_active_[j]) {
                        f = 1;
                        break;
                    }
                }
            }
            frontier_[i] = f;
        }
        front_breach_ = false;
    }

    // 4. Tier assignment from the local CFL step. Pinned to tier 0: cells with
    //    concentrated sources (coupling points) and boundary cells — their
    //    forcing changes fastest. dt0_ = the finest active requirement.
    const int K = static_cast<int>(cells_by_tier_.size());
    const int na = static_cast<int>(active_cells_.size());
    if (mode_ == Momentum2D::DIFFUSIVE_WAVE)
        refreshDiffusiveSlopes();
    dt0_ = 1.0e30;
    rebuild_dt_cell_.resize(static_cast<std::size_t>(na));
    std::vector<double>& dt_cell = rebuild_dt_cell_;
    // Parallel, exactly like its twin refreshDt0(): each iteration writes only
    // dt_cell[k], and the min is folded from per-thread partials. min over
    // doubles is exact and order-independent, so the partition cannot change
    // dt0_ — this was the longest serial stretch of the rebuild.
    const int nthr = std::max(1, opts_->num_threads);
    rebuild_dt_partial_.assign(static_cast<std::size_t>(nthr), 1.0e30);
#pragma omp parallel num_threads(nthr)
    {
#if defined(SWMM_USE_OPENMP)
        const int tid = omp_get_thread_num();
#else
        const int tid = 0;
#endif
        double local = 1.0e30;
#pragma omp for schedule(static) nowait
        for (int k = 0; k < na; ++k) {
            const int i = active_cells_[static_cast<std::size_t>(k)];
            const double h = state_->depth[i];
            double speed = 0.0;
            if (!qcx_.empty() && h > 1.0e-6)
                speed = inertial::qMagnitude(qcx_[i], qcy_[i]) / h;
            double dt = (h > opts_->dry_depth) ? inertial::cellCflDt(opts_->cfl_number,
                                                                     (mode_ == Momentum2D::FULL_SWE)
                                                                         ? edges_.cell_lpos[i]
                                                                         : edges_.cell_lchar[i],
                                                                     h, speed)
                                               : 1.0e30;
            if (mode_ == Momentum2D::DIFFUSIVE_WAVE && h > opts_->dry_depth)
                dt = diffusive::cellDiffusiveDt(
                    opts_->cfl_number, edges_.cell_lchar[i], h, mesh_->mannings_n[i], dw_slope_[i],
                    opts_->flux_dh_eps / std::max(edges_.cell_lchar[i], 1.0e-9));
            dt = std::min(dt, opts_->max_timestep);
            if (state_->heat.ground_column)
                dt = std::min(dt, state_->heat.groundOwner(i).config->options.thermal_max_step);
            dt_cell[static_cast<std::size_t>(k)] = dt;
            if (dt < local)
                local = dt;
        }
        rebuild_dt_partial_[static_cast<std::size_t>(tid)] = local;
    }
    for (double v : rebuild_dt_partial_)
        dt0_ = std::min(dt0_, v);
    // G1-c item 2 (2026-09-19): the aquifer's own stability bound joins the
    // base step. Before this the ladder's dt0 came from the ACTIVE surface
    // cells alone: a groundwater cell whose Δt_g / Δt_u was finer than every
    // wet cell's (or a whole domain with a dry surface, where dt0 fell back
    // to MAX_TIMESTEP) landed on rung 0 and fired beyond its own bound. The
    // scan is the one refreshDtCell() already does for assignTiers() below.
    if (gw_) {
        gw_->refreshDtCell(*mesh_, edges_);
        dt0_ = std::min(dt0_, std::min(gw_->minDtCell(), opts_->max_timestep));
    }
    if (dt0_ >= 1.0e30)
        dt0_ = opts_->max_timestep; // fully quiescent

    const bool parallel_tier_lists = useParallelCompaction(na, nthr);
    for (auto& v : cells_by_tier_)
        v.clear();
    for (auto& v : edges_by_tier_)
        v.clear();
    for (std::size_t k = 0; k < active_cells_.size(); ++k) {
        const int i = active_cells_[k];
        int tk = 0;
        if (K > 1) {
            const double ratio = dt_cell[k] / dt0_;
            // ilogb(x) IS floor(log2(x)) for finite positive x, in a few
            // cycles instead of a libm call (std::log2 measured 1.8 % of the
            // run). They can only disagree where log2's <=1 ulp error crosses
            // an integer — within an ulp of an exact power of two, where
            // ilogb is the exactly-correct one.
            tk = (ratio >= 2.0) ? std::min(K - 1, std::ilogb(ratio)) : 0;
            if (state_->coupling_flux[i] != 0.0 || pin_t0_[i])
                tk = 0;
        }
        tier_[i] = static_cast<uint8_t>(tk);
        if (!front_rebuild_ && !parallel_tier_lists)
            cells_by_tier_[static_cast<std::size_t>(tk)].push_back(i);
    }
    if (front_rebuild_) {
        // Dry halo cells got the coarsest tier (dt = ∞), so a front could
        // enter them only every 2^(K−1) substeps — one ring per macro cycle
        // regardless of the halo width. Pull every dry active cell down to
        // the finest tier among its active neighbours, propagated across the
        // halo rings, so the receiving cells update at the front's cadence.
        // Kept SERIAL (Gauss-Seidel over the edges) on purpose. The halo and
        // frontier sweeps above are exact Jacobi rewrites of their edge loops,
        // but this one is not: Gauss-Seidel propagates a pulled-down tier
        // several rings within a single pass, so a Jacobi form reaches a
        // different fixed point inside the kFrontHaloRings budget. Measured on
        // the Bellinge 30-min slice, that changed DIFFUSIVE_WAVE materially
        // (rainfall inflow +1.5 %, 1D→2D spill +165 %) and shifted the FULL_SWE
        // LTS occupancy census, for ~4 % of the FULL_SWE deck time and nothing
        // on LOCAL_INERTIAL or DIFFUSIVE_WAVE. It is O(n_edges) once per
        // rebuild, not per firing.
        for (int ring = 0; ring < kFrontHaloRings; ++ring) {
            bool changed = false;
            for (int e = 0; e < edges_.ne; ++e) {
                const int a = edges_.cL[e], b = edges_.cR[e];
                if (!cell_active_[a] || !cell_active_[b])
                    continue;
                const bool dry_a = state_->depth[a] <= opts_->dry_depth;
                const bool dry_b = state_->depth[b] <= opts_->dry_depth;
                if (dry_a && tier_[a] > tier_[b]) {
                    tier_[a] = tier_[b];
                    changed = true;
                }
                if (dry_b && tier_[b] > tier_[a]) {
                    tier_[b] = tier_[a];
                    changed = true;
                }
            }
            if (!changed)
                break;
        }
        if (!parallel_tier_lists)
            for (const int i : active_cells_)
                cells_by_tier_[static_cast<std::size_t>(tier_[i])].push_back(i);
    }
    if (parallel_tier_lists) {
        compactOrdered<false>(na, K, nthr, compactionCounts(),
            [&](int k) { return int(tier_[active_cells_[static_cast<std::size_t>(k)]]); },
            [&](int k) { return active_cells_[static_cast<std::size_t>(k)]; },
            [&](int k) -> std::vector<int>& { return cells_by_tier_[static_cast<std::size_t>(k)]; });
    }

    // Initial velocities may seed q before any active-face list exists.
    // Establish the zero-inactive-face invariant with the first full sweep.
    if (na < nt / 4 && active_samples_ > 0) {
        // Faces outside the previous active set already carry zero momentum.
        for (const int e : active_faces_)
            if (!cell_active_[edges_.cL[e]] || !cell_active_[edges_.cR[e]])
                q_[e] = 0.0;
        active_faces_.clear();
        for (const int i : active_cells_)
            for (int p = edges_.cell_ptr[i]; p < edges_.cell_ptr[i + 1]; ++p) {
                const int e = edges_.cell_edge[p];
                if (edges_.cL[e] == i && cell_active_[edges_.cR[e]])
                    active_faces_.push_back(e);
            }
        // Retain the full-sweep ordering, including on mixed meshes.
        std::sort(active_faces_.begin(), active_faces_.end());
        for (const int e : active_faces_) {
            const auto ft = std::min(tier_[edges_.cL[e]], tier_[edges_.cR[e]]);
            face_tier_[e] = ft;
            edges_by_tier_[ft].push_back(e);
        }
    } else {
        active_faces_.clear();
        if (useParallelCompaction(edges_.ne, nthr)) {
            compactOrdered<true>(edges_.ne, K, nthr, compactionCounts(),
                [&](int e) {
                    const int a = edges_.cL[e], b = edges_.cR[e];
                    if (cell_active_[a] && cell_active_[b])
                        return int(std::min(tier_[a], tier_[b]));
                    // Each pass owns this edge; the repeated zero is idempotent.
                    q_[e] = 0.0;
                    return -1;
                },
                [&](int e) {
                    face_tier_[e] = std::min(tier_[edges_.cL[e]], tier_[edges_.cR[e]]);
                    return e;
                },
                [&](int k) -> std::vector<int>& {
                    return k == K ? active_faces_ : edges_by_tier_[static_cast<std::size_t>(k)];
                });
        } else {
            for (int e = 0; e < edges_.ne; ++e) {
                const int a = edges_.cL[e], b = edges_.cR[e];
                if (cell_active_[a] && cell_active_[b]) {
                    const auto ft = std::min(tier_[a], tier_[b]);
                    face_tier_[e] = ft;
                    edges_by_tier_[ft].push_back(e);
                    active_faces_.push_back(e);
                } else {
                    q_[e] = 0.0; // walled faces carry no stale momentum
                }
            }
        }
    }

    for (std::size_t tk = 0; tk < cells_by_tier_.size() && tk < tier_occupancy_.size(); ++tk)
        tier_occupancy_[tk] += static_cast<long>(cells_by_tier_[tk].size());

    // G1: re-tier the subsurface on the same trigger, from ITS OWN stability
    // steps against the same dt0_ and the same ladder height (G-A). A GW cell
    // slower than 2^(K−1)·dt0_ lands on the coarsest rung and simply fires
    // more often than it strictly needs — conservative, and it keeps the
    // ladder height a single [2D_OPTIONS] LTS_TIERS rather than a second,
    // silently different number. (D-N1's runtime tier count would let the
    // ladder grow to `gw_->requiredTiers(dt0_)`; it is deliberately not taken
    // here — see the handoff.)
    if (gw_) { // refreshDtCell() ran above, before dt0_ was fixed
        gw_->assignTiers(dt0_, static_cast<int>(cells_by_tier_.size()));
    }

    // Fold the sample into the run_stats() statistics now (same arithmetic,
    // same order as the old end-of-run fold); keep the sample itself only
    // when a telemetry CSV was requested.
    const int n_active = static_cast<int>(active_cells_.size());
    if (mesh_ && mesh_->n_triangles() > 0) {
        const double frac = n_active / static_cast<double>(mesh_->n_triangles());
        active_frac_min_ = std::min(active_frac_min_, frac);
        active_frac_max_ = std::max(active_frac_max_, frac);
        active_frac_sum_ += frac;
        ++active_samples_;
    }
    if (!telemetry_path_.empty())
        telemetry_.emplace_back(t, n_active);
}

void ExplicitInertialSolver::refreshDt0() {
    // Between rebuilds the tier lists are frozen and depths keep evolving, so
    // a dt0_ computed up to kRebuildEveryCycles macro cycles ago can realize
    // an effective CFL well above the configured bound (measured: the
    // union-jack closed-lake seiche grows at CFL_NUMBER 0.7 with dt frozen
    // for 32 base substeps). Tightening mid-flight is unconditionally safe —
    // every tier still satisfies dt_cell ≥ 2^k·dt0 — while GROWING dt0 must
    // wait for syncAndRebuild, which reassigns the tiers.
    const int na = static_cast<int>(active_cells_.size());
    if (na == 0)
        return;
    if (mode_ == Momentum2D::DIFFUSIVE_WAVE)
        refreshDiffusiveSlopes();
    double fresh = 1.0e30;
    const int nthr = std::max(1, opts_->num_threads);
    // One 128-byte-separated partial per worker; unused slots retain the sentinel.
    // Reuse rebuild scratch, which is not part of the checkpoint state.
    constexpr std::size_t stride = 128 / sizeof(double);
    rebuild_dt_partial_.assign(static_cast<std::size_t>(nthr) * stride, 1.0e30);
#pragma omp parallel num_threads(nthr)
    {
#if defined(SWMM_USE_OPENMP)
        const int tid = omp_get_thread_num();
#else
        const int tid = 0;
#endif
        double local = 1.0e30;
#pragma omp for schedule(static) nowait
        for (int k = 0; k < na; ++k) {
            const int i = active_cells_[static_cast<std::size_t>(k)];
            const double h = state_->depth[i];
            if (h <= opts_->dry_depth)
                continue;
            double speed = 0.0;
            if (!qcx_.empty() && h > 1.0e-6)
                speed = inertial::qMagnitude(qcx_[i], qcy_[i]) / h;
            double dt = inertial::cellCflDt(opts_->cfl_number,
                                            (mode_ == Momentum2D::FULL_SWE) ? edges_.cell_lpos[i]
                                                                            : edges_.cell_lchar[i],
                                            h, speed);
            if (mode_ == Momentum2D::DIFFUSIVE_WAVE)
                dt = diffusive::cellDiffusiveDt(
                    opts_->cfl_number, edges_.cell_lchar[i], h, mesh_->mannings_n[i], dw_slope_[i],
                    opts_->flux_dh_eps / std::max(edges_.cell_lchar[i], 1.0e-9));
            dt /= (1 << tier_[i]);
            if (dt < local)
                local = dt;
        }
        rebuild_dt_partial_[static_cast<std::size_t>(tid) * stride] = local;
    }
    for (int tid = 0; tid < nthr; ++tid)
        if (rebuild_dt_partial_[static_cast<std::size_t>(tid) * stride] < fresh)
            fresh = rebuild_dt_partial_[static_cast<std::size_t>(tid) * stride];
    if (fresh < dt0_)
        dt0_ = fresh;
}

void ExplicitInertialSolver::fireFaces(const std::vector<int>& faces, double dt_f,
                                       bool global_step) {
    perf::ProcessTimer process_timer(perf::ProcessPhase::surface_faces);
    switch (mode_) {
    case Momentum2D::FULL_SWE:
        fireFacesSwe(faces, dt_f, global_step);
        return;
    case Momentum2D::DIFFUSIVE_WAVE:
        fireFacesDiffusive(faces, dt_f, global_step);
        return;
    case Momentum2D::LOCAL_INERTIAL:
        break;
    }
    fireFacesInertial(faces, dt_f, global_step);
}

void ExplicitInertialSolver::refreshDiffusiveSlopes() {
    // Largest |Δη·inv_dx| over each active cell's faces (walled faces count
    // as zero): the conductance the explicit step bound must survive.
    const auto& ed = edges_;
    const int na = static_cast<int>(active_cells_.size());
#pragma omp parallel for schedule(static) num_threads(opts_->num_threads)
    for (int k = 0; k < na; ++k) {
        const int i = active_cells_[static_cast<std::size_t>(k)];
        double smax = 0.0;
        for (int p = ed.cell_ptr[i]; p < ed.cell_ptr[i + 1]; ++p) {
            const int e = ed.cell_edge[p];
            const double sl =
                std::fabs((state_->head[ed.cR[e]] - state_->head[ed.cL[e]]) * ed.inv_dx_normal[e]);
            if (sl > smax)
                smax = sl;
        }
        dw_slope_[i] = smax;
    }
}

void ExplicitInertialSolver::fireFacesInertial(const std::vector<int>& faces, double dt_f,
                                               bool global_step) {
    const auto& ed = edges_;
    // Distinct owned vectors; these inputs stay read-only through face booking.
    const double* OPENSWMM_RESTRICT head = state_->head.data();
    const double* OPENSWMM_RESTRICT depth = state_->depth.data();
    const double* OPENSWMM_RESTRICT volume = state_->volume.data();
    const int na = static_cast<int>(faces.size());
    const double theta = opts_->theta;
    const double dry = opts_->dry_depth, froude_max = opts_->froude_max;
    const bool advection = opts_->advection;
    const bool physical_time_blend = opts_->theta_reference_step > 0.0;
    const double blend =
        inertial::momentumBlendFraction(theta, dt_f, opts_->theta_reference_step);
    // Positivity share per face: a cell's ≤ nv outgoing faces may take at most
    // β·V in total, so each takes β/nv of its EXPORTER's volume. Triangle
    // meshes keep the historical β/3 exactly.
    const double beta3 = opts_->exchange_beta / 3.0;
    const double beta4 = opts_->exchange_beta / 4.0;
    // All-triangle meshes skip the per-face cell_nv load entirely (resolved
    // once in initialize(): the scan is O(n_cells)).
    const bool has_quads = has_quads_;
    const bool species = species_on_;
    // VFR_FACE: block/convey at the shared edge's TRUE crest via the B&S
    // Eq. 14 wetted-edge depth; MEAN keeps the centroid zface bit-identical.
    const bool vfr_face = (opts_->face_reconstruction == FaceDepth2D::VFR_FACE);

#pragma omp parallel for schedule(static) num_threads(opts_->num_threads)
    for (int k = 0; k < na; ++k) {
        const int e = faces[static_cast<std::size_t>(k)];
        if (ed.conveyance[e] == 0.0) {
            q_[e] = 0.0;
            continue; // Impermeable face: no momentum, water or species exchange.
        }
        const double width = ed.xi[e] * ed.conveyance[e];
        const int a = ed.cL[e], b = ed.cR[e];
        const double hf =
            vfr_face ? inertial::faceFlowDepthVfr(head[a], head[b], ed.ze_lo[e],
                                                  ed.ze_hi[e])
                     : inertial::faceFlowDepth(head[a], head[b], ed.zface[e]);
        if (hf <= dry) {
            q_[e] = 0.0;
            continue;
        }
        double qhat = q_[e];
        double q_mag = std::fabs(q_[e]);
        if (!qcx_.empty()) {
            const double qfx = 0.5 * (qcx_[a] + qcx_[b]);
            const double qfy = 0.5 * (qcy_[a] + qcy_[b]);
            const double qn = qfx * ed.nx[e] + qfy * ed.ny[e];
            qhat = physical_time_blend ? q_[e] + blend * (qn - q_[e])
                                      : theta * q_[e] + (1.0 - theta) * qn;
            // Friction magnitude: the face flow VECTOR, floored at |q_n| so
            // a face whose reconstruction lags its own discharge (front
            // arrival, first firing after activation) never under-damps.
            q_mag = std::max(q_mag, inertial::qMagnitude(qfx, qfy));
        }
        double deta = head[b] - head[a];
        if (std::fabs(deta) < inertial::kEtaDeadband)
            deta = 0.0;
        const double slope = deta * ed.inv_dx_normal[e];
        // Convective momentum flux (ADVECTION, opt-in): both cells must be
        // wet — a wet/dry front keeps the pure local-inertial law, whose
        // robustness there is the reason this scheme exists.
        double adv = 0.0;
        if (advection && !qcx_.empty()) {
            const double hL = depth[a], hR = depth[b];
            if (hL > dry && hR > dry) {
                const double unL = (qcx_[a] * ed.nx[e] + qcy_[a] * ed.ny[e]) / hL;
                const double unR = (qcx_[b] * ed.nx[e] + qcy_[b] * ed.ny[e]) / hR;
                adv = inertial::inertialAdvection(q_[e], unL, hL, unR, hR, ed.inv_dx_normal[e]);
            }
        }
        // CPU shortcut: the positive friction denominator cannot change an
        // exactly zero momentum numerator. Preserve its sign as well.
        double qn1 = qhat - dt_f * (inertial::kGravity * hf * slope + adv);
        if (qn1 != 0.0)
            qn1 = inertial::inertialFaceUpdate(q_[e], qhat, hf, dt_f, slope, ed.n2_face[e], q_mag,
                                               adv);
        qn1 = inertial::froudeCap(qn1, hf, froude_max);

        // Positivity at face cadence: this face may take at most a β/3 share
        // of its exporting cell's volume over the exporter's WHOLE cell cycle.
        // The exporter republishes V only at its own firings, and a finer face
        // fires 2^(k_exp − t_face) times in between — divide the share by that
        // ratio or the repeated takes drain the cell into the backstop
        // (measured: a dam-break basin discarded to exactly zero).
        const int exp_cell = (qn1 > 0.0) ? a : b;
        const int refire = global_step ? 1 : (1 << (tier_[exp_cell] - face_tier_[e]));
        const double beta_share =
            (has_quads && mesh_->cell_vertex_count(exp_cell) == 4) ? beta4 : beta3;
        const double budget = beta_share / refire * std::max(volume[exp_cell], 0.0);
        const double take = std::fabs(qn1) * width * dt_f;
        if (take > budget)
            qn1 *= (take > 0.0) ? budget / take : 0.0;
        q_[e] = qn1;

        // Book the identical ±ΔM on both sides (single writer per face).
        const double dM = qn1 * width * dt_f; // positive = cL→cR
        face_accum_[face_accum_stride_ * (e)] -= dM;
        face_accum_[face_accum_stride_ * (e) + 1] += dM;

        if (species)
            bookFaceSpecies(e, a, b, dM, hf, dt_f, global_step);
    }
    face_passes_ += na;
    accumulators_pending_ = true;
}

void ExplicitInertialSolver::refreshConveyance() {
    if (mode_ != Momentum2D::LOCAL_INERTIAL || !edges_.refreshConveyance(*mesh_))
        return;
    const auto& ed = edges_;
    for (int e = 0; e < ed.ne; ++e)
        if (ed.conveyance[e] == 0.0)
            q_[e] = 0.0; // Closing and reopening a face must not revive stale momentum.
    if (qcx_.empty())
        return;
    for (int i = 0; i < mesh_->n_cells(); ++i) {
        double sx = 0.0, sy = 0.0;
        for (int p = ed.cell_ptr[i]; p < ed.cell_ptr[i + 1]; ++p) {
            const int e = ed.cell_edge[p];
            const double f =
                static_cast<double>(ed.cell_sign[p]) * q_[e] * ed.xi[e] * ed.conveyance[e];
            sx += f * ed.cell_arm_x[p];
            sy += f * ed.cell_arm_y[p];
        }
        qcx_[i] = sx / mesh_->tri_area[i];
        qcy_[i] = sy / mesh_->tri_area[i];
    }
    // Preserve the boundary completion used by fireCells: bc_q is the last
    // accepted inflow per unit width, so its outward contribution has opposite sign.
    for (std::size_t k = 0; k < bc_cell_.size(); ++k) {
        const int i = bc_cell_[k], slot = bc_slot_[k];
        const double fo = -bc_q_[k] * mesh_->edge_length[slot];
        const double inv_a = 1.0 / mesh_->tri_area[i];
        qcx_[i] += fo * (mesh_->edge_mx[slot] - mesh_->tri_cx[i]) * inv_a;
        qcy_[i] += fo * (mesh_->edge_my[slot] - mesh_->tri_cy[i]) * inv_a;
    }
}

// ---------------------------------------------------------------------------
// MOMENTUM_EQUATION FULL_SWE — Godunov face flux (SweKernels.hpp).
// ---------------------------------------------------------------------------
void ExplicitInertialSolver::fireFacesSwe(const std::vector<int>& faces, double dt_f,
                                          bool global_step) {
    // A finite water proposal can still exceed the pressure/kinetic-flux
    // range of double precision. Refuse it before booking any face so a
    // coupled RK2 trial rolls back instead of collapsing its timestep.
    for (int i : active_cells_) {
        const double h = state_->depth[i], qx = qcx_[i], qy = qcy_[i];
        if (!(h >= 0) || !std::isfinite(state_->head[i]) || !std::isfinite(qx) ||
            !std::isfinite(qy) || !std::isfinite(.5 * swe::kGravity * h * h) ||
            (h > opts_->dry_depth && !std::isfinite((qx / h) * qx + (qy / h) * qy)))
            throw std::runtime_error("FULL_SWE pressure or momentum exceeds finite range at cell " +
                                     std::to_string(i));
    }
    const auto& ed = edges_;
    // Distinct owned vectors; these inputs stay read-only through face booking.
    const double* OPENSWMM_RESTRICT head = state_->head.data();
    const double* OPENSWMM_RESTRICT depth = state_->depth.data();
    const double* OPENSWMM_RESTRICT volume = state_->volume.data();
    const int na = static_cast<int>(faces.size());
    const double beta3 = opts_->exchange_beta / 3.0;
    const double beta4 = opts_->exchange_beta / 4.0;
    const double dry = opts_->dry_depth;

    static const bool muscl_off = std::getenv("OPENSWMM_2D_MUSCL_OFF") != nullptr;
    const auto depth_offset = state_->depth.size();
    if (second_order_)
        computeLimitedGradientsSwe();
    const bool so = second_order_ && !muscl_off;
    const bool species = species_on_;
    const bool has_quads = has_quads_;

#pragma omp parallel for schedule(static) num_threads(opts_->num_threads)
    for (int k = 0; k < na; ++k) {
        const int e = faces[static_cast<std::size_t>(k)];
        const int a = ed.cL[e], b = ed.cR[e];
        swe::FaceFlux F;
        double cLx, cLy, cRx, cRy;
        bool wet;
        if (so) {
            // MUSCL: extrapolate eta, depth and velocity to the face.
            // Reconstructing depth with eta removes the artificial bed step
            // that otherwise remains even on a smooth, fully wet slope.
            const double ha = depth[a], hb = depth[b];
            const double za = head[a] - ha, zb = head[b] - hb;
            // These are the same arms stored by InertialEdges::build().
            // Read the face midpoint directly instead of searching both CSR rows.
            const double axa = ed.mx[e] - mesh_->tri_cx[a];
            const double aya = ed.my[e] - mesh_->tri_cy[a];
            const double axb = ed.mx[e] - mesh_->tri_cx[b];
            const double ayb = ed.my[e] - mesh_->tri_cy[b];
            const double ua = (ha > dry) ? qcx_[a] / ha : 0.0, va = (ha > dry) ? qcy_[a] / ha : 0.0;
            const double ub = (hb > dry) ? qcx_[b] / hb : 0.0, vb = (hb > dry) ? qcy_[b] / hb : 0.0;
            const double etaLf = head[a] + gex_[a] * axa + gey_[a] * aya;
            const double etaRf = head[b] + gex_[b] * axb + gey_[b] * ayb;
            const double uLf = ua + gux_[a] * axa + guy_[a] * aya;
            const double vLf = va + gvx_[a] * axa + gvy_[a] * aya;
            const double uRf = ub + gux_[b] * axb + guy_[b] * ayb;
            const double vRf = vb + gvx_[b] * axb + gvy_[b] * ayb;
            const double hLf =
                std::max(0.0, ha + gex_[depth_offset + a] * axa + gey_[depth_offset + a] * aya);
            const double hRf =
                std::max(0.0, hb + gex_[depth_offset + b] * axb + gey_[depth_offset + b] * ayb);
            wet = swe::faceFluxReconBed(etaLf, uLf, vLf, za, ha, etaRf, uRf, vRf, zb, hb, ed.nx[e],
                                        ed.ny[e], dry, F, cLx, cLy, cRx, cRy, etaLf - hLf,
                                        etaRf - hRf);
        } else {
            wet = swe::faceFlux(head[a], depth[a], qcx_[a], qcy_[a],
                                head[b], depth[b], qcx_[b], qcy_[b], ed.nx[e],
                                ed.ny[e], dry, F, cLx, cLy, cRx, cRy);
        }
        // Dry-dry face: no flux, no correction (faceFlux zeroes both), so
        // nothing to book — the accumulator read-modify-writes of zero and
        // the conveyance gather are skipped (F4). Species dispersion still
        // gets its call.
        if (!wet) {
            q_[e] = 0.0;
            if (species) {
                const double hf0 = std::max(head[a], head[b]) - ed.zface[e];
                bookFaceSpecies(e, a, b, 0.0, (hf0 > 0.0) ? hf0 : 0.0, dt_f, global_step);
            }
            continue;
        }
        // Face conveyance (leaky barriers) scales the whole flux vector.
        const double conv = mesh_->edge_conveyance[ed.slotL[e]];
        double fh = F.mass * conv, fmx = F.mx * conv, fmy = F.my * conv;

        // Positivity share (same contract as the inertial law): the mass a
        // face may take from its exporter over the exporter's cycle. Mass
        // and momentum are scaled together so the flux stays consistent.
        if (fh != 0.0) {
            const int exp_cell = (fh > 0.0) ? a : b;
            const int refire = global_step ? 1 : (1 << (tier_[exp_cell] - face_tier_[e]));
            const double beta_share =
                (has_quads && mesh_->cell_vertex_count(exp_cell) == 4) ? beta4 : beta3;
            const double budget = beta_share / refire * std::max(volume[exp_cell], 0.0);
            const double take = std::fabs(fh) * ed.xi[e] * dt_f;
            if (take > budget) {
                const double lam = (take > 0.0) ? budget / take : 0.0;
                fh *= lam;
                fmx *= lam;
                fmy *= lam;
            }
        }
        q_[e] = fh;
        const double xdt = ed.xi[e] * dt_f;
        const double dM = fh * xdt; // m³, + = cL→cR
        face_accum_[face_accum_stride_ * (e)] -= dM;
        face_accum_[face_accum_stride_ * (e) + 1] += dM;
        // Momentum: −flux + bed-slope correction on the exporter side, +flux
        // + correction on the receiver side (units m⁴/s).
        face_accum_[face_accum_stride_ * (e) + 2] += (-fmx + cLx) * xdt;
        face_accum_[face_accum_stride_ * (e) + 4] += (-fmy + cLy) * xdt;
        face_accum_[face_accum_stride_ * (e) + 3] += (fmx + cRx) * xdt;
        face_accum_[face_accum_stride_ * (e) + 5] += (fmy + cRy) * xdt;

        // Species ride the mass flux at the face depth the inertial law
        // would have used (dispersion conductance only).
        if (species) {
            const double hf = std::max(head[a], head[b]) - ed.zface[e];
            bookFaceSpecies(e, a, b, dM, (hf > 0.0) ? hf : 0.0, dt_f, global_step);
        }
    }
    face_passes_ += na;
    accumulators_pending_ = true;
}

// ---------------------------------------------------------------------------
// MOMENTUM_EQUATION DIFFUSIVE_WAVE — Manning quasi-steady face flux.
// ---------------------------------------------------------------------------
void ExplicitInertialSolver::fireFacesDiffusive(const std::vector<int>& faces, double dt_f,
                                                bool global_step) {
    const auto& ed = edges_;
    // Distinct owned vectors; these inputs stay read-only through face booking.
    const double* OPENSWMM_RESTRICT head = state_->head.data();
    const double* OPENSWMM_RESTRICT volume = state_->volume.data();
    const int na = static_cast<int>(faces.size());
    const double beta3 = opts_->exchange_beta / 3.0;
    const double beta4 = opts_->exchange_beta / 4.0;
    const bool vfr_face = (opts_->face_reconstruction == FaceDepth2D::VFR_FACE);
    const double dry = opts_->dry_depth, flux_dh_eps = opts_->flux_dh_eps;
    const bool species = species_on_;
    const bool has_quads = has_quads_;

#pragma omp parallel for schedule(static) num_threads(opts_->num_threads)
    for (int k = 0; k < na; ++k) {
        const int e = faces[static_cast<std::size_t>(k)];
        const int a = ed.cL[e], b = ed.cR[e];
        const double hf =
            vfr_face ? inertial::faceFlowDepthVfr(head[a], head[b], ed.ze_lo[e],
                                                  ed.ze_hi[e])
                     : inertial::faceFlowDepth(head[a], head[b], ed.zface[e]);
        if (hf <= dry) {
            q_[e] = 0.0;
            continue;
        }
        double deta = head[b] - head[a];
        if (std::fabs(deta) < inertial::kEtaDeadband)
            deta = 0.0;
        const double slope = deta * ed.inv_dx_normal[e];
        const double s_eps = flux_dh_eps * ed.inv_dx_normal[e];
        // n_face = sqrt(n2_face) precomputed per face (F7).
        double qn1 = diffusive::faceDischarge(hf, slope, ed.n_face[e], s_eps);
        qn1 *= mesh_->edge_conveyance[ed.slotL[e]];

        // Same positivity share as the other closures.
        const int exp_cell = (qn1 > 0.0) ? a : b;
        const int refire = global_step ? 1 : (1 << (tier_[exp_cell] - face_tier_[e]));
        const double beta_share =
            (has_quads && mesh_->cell_vertex_count(exp_cell) == 4) ? beta4 : beta3;
        const double budget = beta_share / refire * std::max(volume[exp_cell], 0.0);
        const double take = std::fabs(qn1) * ed.xi[e] * dt_f;
        if (take > budget)
            qn1 *= (take > 0.0) ? budget / take : 0.0;
        q_[e] = qn1;
        const double dM = qn1 * ed.xi[e] * dt_f;
        face_accum_[face_accum_stride_ * (e)] -= dM;
        face_accum_[face_accum_stride_ * (e) + 1] += dM;
        if (species)
            bookFaceSpecies(e, a, b, dM, hf, dt_f, global_step);
    }
    face_passes_ += na;
    accumulators_pending_ = true;
}

void ExplicitInertialSolver::bookFaceSpecies(int e, int a, int b, double dM, double hf, double dt_f,
                                             bool global_step) noexcept {
    // Hydrodynamics-only models (no species) have nothing to book: return
    // before the divisions below — this runs once per face evaluation.
    if (!species_on_)
        return;
    perf::ProcessTimer process_timer(perf::ProcessPhase::surface_species);
    const auto& ed = edges_;
    const double beta3 = opts_->exchange_beta / 3.0;
    const double beta4 = opts_->exchange_beta / 4.0;
    // S1 (D-2DT2): species mass rides THIS ΔM, from the FINAL qn1 —
    // after the Froude cap and the positivity share — at the exporter's
    // concentration read NOW, against the same published volume the
    // share budgeted against. Same writer, same face, same substep:
    // the species flux cannot disagree with the volume flux about
    // cadence, direction or magnitude, which is the whole of the
    // conservation argument.
    if (species_on_ && dM != 0.0) {
        const int donor = (dM > 0.0) ? a : b;
        const auto ns = static_cast<std::size_t>(state_->transport.n_species);
        const auto ue = static_cast<std::size_t>(e);
        for (std::size_t s = 0; s < ns; ++s) {
            const double c = donorConc(static_cast<int>(s), donor);
            if (c == 0.0)
                continue;
            const double dMs = dM * c;
            sacc_L_[ue * ns + s] -= dMs;
            sacc_R_[ue * ns + s] += dMs;
        }
    }

    // S2 (D-2DT7): isotropic dispersion, booked on the SAME face, at the
    // SAME cadence, into the SAME accumulators as the advective term —
    // so it inherits the marcher's tier consistency exactly as advection
    // does. Explicit exchange F = D·(h_f·ξ)·(c_a − c_b)/d over dt_f.
    //
    // Explicit diffusion has its own stability limit, D·dt/d² ≤ ~½, and
    // the marcher's dt0 is set by gravity waves, not by D. Rather than
    // couple dt0 to D (a global cost for a local term), the exchange is
    // LIMITED so the pair can never cross: at most the amount that
    // equalises the two concentrations, and at most the same β share of
    // the giver's mass the volume flux is held to. Positivity and the
    // pairwise max principle follow; a bind is COUNTED, because a face
    // that binds is telling the modeller the dispersion is
    // under-resolved at this dt.
    if (species_on_ && opts_->dispersion > 0.0) {
        const double va = state_->volume[a], vb = state_->volume[b];
        if (va > 0.0 && vb > 0.0) {
            const double cond =
                opts_->dispersion * hf * ed.xi[e] * ed.inv_dx_normal[e] * dt_f *
                (mode_ == Momentum2D::LOCAL_INERTIAL ? ed.conveyance[e] : 1.0); // m³ exchanged
            const auto ns = static_cast<std::size_t>(state_->transport.n_species);
            const auto ue = static_cast<std::size_t>(e);
            auto& tr = state_->transport;
            for (std::size_t s = 0; s < ns; ++s) {
                const double ma = tr.cell_mass[tr.idx(static_cast<int>(s), a)];
                const double mb = tr.cell_mass[tr.idx(static_cast<int>(s), b)];
                const double ca = ma / va, cb = mb / vb;
                double dMd = cond * (ca - cb); // + = a → b
                if (dMd == 0.0)
                    continue;
                // Equalisation bound, divided by 3 because PAIRWISE
                // bounds do not compose: a cell receiving from up to
                // three faces, each capped at FULL pairwise equalisation
                // computed from the same start-of-substep state, can end
                // richer than every donor (the check measured 15.31 on
                // an initial max of 10 at D = 20). Each face may close
                // at most a third of its gap; with the harmonic volume
                // ≤ the receiver's own, the sum over ≤ 3 faces is then
                // bounded by the largest donor concentration — the same
                // 1/3 composition argument the volume side's β/3 makes.
                // (A quad receiver has up to four faces: /4.)
                const int giver = (dMd > 0.0) ? a : b;
                const int receiver = (dMd > 0.0) ? b : a;
                const double eq = (ca - cb) * va * vb / (va + vb) /
                                  ((mesh_->cell_vertex_count(receiver) == 4) ? 4.0 : 3.0);
                // β share of the GIVER's mass, divided by the refire
                // ratio exactly as the volume share is.
                const int refire = global_step ? 1 : (1 << (tier_[giver] - face_tier_[e]));
                // The giver-mass share is a POSITIVITY guard: give at
                // most a β share of what is there. The SIGNED temperature
                // row (S4) has no positivity to guard — its mass crosses
                // zero at a warm/cold front, where a mass share would
                // vanish and bind every smoothing exchange. For it the
                // equalisation bound alone is the cap: it is what
                // enforces the pairwise max principle in both directions.
                const double share = ((mesh_->cell_vertex_count(giver) == 4) ? beta4 : beta3) /
                                     refire * std::fabs((dMd > 0.0) ? ma : mb);
                const double cap = tr.signedRow(static_cast<int>(s))
                                       ? std::fabs(eq)
                                       : std::min(std::fabs(eq), share);
                if (std::fabs(dMd) > cap) {
                    dMd = (dMd > 0.0) ? cap : -cap;
#pragma omp atomic
                    ++tr.dispersion_limiter_binds;
                }
                sacc_L_[ue * ns + s] -= dMd;
                sacc_R_[ue * ns + s] += dMd;
            }
        }
    }
}

// The cell kernel is a template on the closure so the LOCAL_INERTIAL /
// DIFFUSIVE_WAVE instantiation contains none of the FULL_SWE momentum code
// (measured: the merged body cost the LI cell pass +17 % — register pressure
// and dead branches inside the OpenMP-outlined loop; the LI instantiation is
// the pre-FULL_SWE kernel again). Dispatch is one branch per firing.
/**
 * @brief Mix accepted finest source samples and solve shared finite surface/channel/soil heat
 * once.
 * @param[in] dt Finite positive source interval s, called seconds in the declaration.
 * @note Requires attached live channel_ground_ and matching prepared thermal/native owners.
 * Coupled surface-ground mode first gathers managed-cell water/temperature packets, prepares
 * accepted source water without changing its gate's pre-firing depth, mixes producer temperature
 * and stores tuple scratch. Runs the joint channel/ground solve, then publishes returned degC*m3
 * and worker thermal ledgers, marking temperatures mixed so ordinary row processing cannot
 * duplicate contacts/arrivals. Without coupled surface heat, passes empty surface samples while
 * channel/soil still solves. Throws std::runtime_error for missing temperature/budget or shared
 * contact refusal. Allocations and external channel callbacks can fail after gathers/earlier
 * owner effects; no local rollback. Caller serializes all shared owners at tier zero and later
 * clears ready/mixed flags after successful firing.
 */
void ExplicitInertialSolver::prepareChannelGround(double dt) {
    std::vector<SurfaceThermalSample> samples;
    auto& tr = state_->transport;
    const bool coupled = state_->heat.enabled && state_->heat.ground_column;
    if (coupled) {
        samples.resize(mesh_->n_cells());
        const auto ns = std::size_t(tr.n_species);
        const int row = tr.temp_row;
        if (row < 0)
            throw std::runtime_error(
                "Shared mesh/channel thermal contact has no surface temperature row.");
        for (int i : channel_ground_->cells()) {
            double volume = 0, mass = 0;
            for (int p = edges_.cell_ptr[i]; p < edges_.cell_ptr[i + 1]; ++p) {
                const int edge = edges_.cell_edge[p];
                const bool left = edges_.cell_sign[p] > 0;
                auto& dv = left ? face_accum_[face_accum_stride_ * (edge)] : face_accum_[face_accum_stride_ * (edge) + 1];
                volume += dv;
                dv = 0;
                auto& dm = (left ? sacc_L_ : sacc_R_)[std::size_t(edge) * ns + row];
                mass += dm;
                dm = 0;
            }
            state_->volume[i] = std::max(0., state_->volume[i] + volume);
            tr.cell_mass[tr.idx(row, i)] += mass;
            if (!tr.cell_runoff_vol.empty())
                tr.cell_runoff_vol[i] -= volume;
            auto& source = channel_sources_[i];
            // Preserve the hydraulic source gate's pre-firing depth, just as
            // both existing cell kernels do. Heat cannot alter accepted water.
            if (!prepareCellSources(i, dt, source))
                throw std::runtime_error("Shared thermal source budget was not prepared.");
            channel_source_ready_[i] = 1;
            mixCellSourceRow(i, row, tr.cell_mass[tr.idx(row, i)], source);
            samples[i] = {tr.cell_mass[tr.idx(row, i)], source.available, source.evap};
        }
    }
    std::vector<double> mass;
    if (auto error = channel_ground_->advance(*opts_, samples, dt, mass); !error.empty())
        throw std::runtime_error("[Channel ground heat] " + error);
    if (coupled)
        for (int i : channel_ground_->cells()) {
            auto& current = tr.cell_mass[tr.idx(tr.temp_row, i)];
            bookSourceLedger(SourceLedger::ExternalIn, tr.temp_row, mass[i] - current);
            current = mass[i];
            channel_temperature_mixed_[i] = 1;
        }
}

void ExplicitInertialSolver::fireCells(const std::vector<int>& cells, double dt_c, bool tier0) {
    perf::ProcessTimer process_timer(perf::ProcessPhase::surface_cells);
    if (tier0 && channel_ground_)
        prepareChannelGround(dt_c);
    if (tier0 && common_stage_) {
        // Managed cells are pinned on the finest tier. Land their accepted
        // face water before gathering requests; momentum still lands below.
        for (int i : common_stage_->cells()) {
            double flux = 0.0;
            for (int p = edges_.cell_ptr[i]; p < edges_.cell_ptr[i + 1]; ++p) {
                const int e = edges_.cell_edge[p];
                double& side = edges_.cell_sign[p] > 0 ? face_accum_[face_accum_stride_ * (e)] : face_accum_[face_accum_stride_ * (e) + 1];
                flux += side;
                side = 0.0;
            }
            state_->volume[i] = std::max(0.0, state_->volume[i] + flux);
            inertial::cellEtaDepth(*mesh_, *opts_, i, state_->volume[i], state_->head[i],
                                   state_->depth[i]);
        }
        const auto error = common_stage_->advance(common_stage_->completedEnd(), source_phase_end_);
        if (!error.empty())
            throw std::runtime_error("[2D_COMMON_SOURCE_STAGE] " + error);
    }
    // Fuse only when at least one full species block amortizes the setup.
    const bool fuse = state_->transport.n_species >= 8;
    if (mode_ == Momentum2D::FULL_SWE) {
        if (fuse)
            fireCellsImpl<true, true>(cells, dt_c, tier0);
        else
            fireCellsImpl<true, false>(cells, dt_c, tier0);
    } else {
        if (fuse)
            fireCellsImpl<false, true>(cells, dt_c, tier0);
        else
            fireCellsImpl<false, false>(cells, dt_c, tier0);
    }
    if (tier0 && channel_ground_)
        for (int cell : channel_ground_->cells()) {
            channel_source_ready_[cell] = 0;
            channel_temperature_mixed_[cell] = 0;
        }
}

template <bool kSwe, bool kFuse>
void ExplicitInertialSolver::fireCellsImpl(const std::vector<int>& cells, double dt_c, bool tier0) {
    const auto& ed = edges_;
    const int nc = static_cast<int>(cells.size());
    // Loop-invariant closure flags, resolved outside the parallel region so
    // the outlined body carries them as firstprivate constants instead of
    // re-reading members through `this` on every cell.
    constexpr bool swe = kSwe;
    const bool perot = !qcx_.empty() && !swe;
    const bool species = species_on_;
    const bool front = front_rebuild_;
    // Keep each owned-side physical face receipt until this cell has
    // debited its beginning donor budget. The neighbour owns the other side.
    const bool boundary_stage = rk_transport_stage_ && gw_ && !bc_cell_.empty();
    const bool preserve_face_water =
        rk_transport_stage_ &&
        (!state_->rk2_rain_pending.empty() || boundary_stage || surfaceLatentCooling());
    std::vector<double> boundary_donor(boundary_stage ? state_->volume.size() : 0);
    const bool physics_boundary = opts_->momentumPhysics() && !bc_cell_.empty();
    const auto physics_volume = physics_boundary ? state_->volume : std::vector<double>{};
    const auto boundary_qx = boundary_stage || physics_boundary ? qcx_ : std::vector<double>{},
               boundary_qy = boundary_stage || physics_boundary ? qcy_ : std::vector<double>{};
    const double dry = opts_->dry_depth;

    int front_breached = 0;
#pragma omp parallel for schedule(static) num_threads(opts_->num_threads)                          \
    reduction(| : front_breached)
    for (int k = 0; k < nc; ++k) {
        const int i = cells[static_cast<std::size_t>(k)];
        // Keep the wall flux at the same Euler-stage state as interior faces.
        const double wall_h0 = kSwe ? state_->depth[i] : 0;
        const double wall_eta0 = kSwe ? state_->head[i] : 0;
        // ONE walk of this cell's CSR row: gather + clear its side of every
        // incident face accumulator, and (when the Perot reconstruction is
        // live) accumulate its discharge vector from the same face ids. The
        // two loops used to load cell_edge/cell_sign twice for every incident
        // face; the arms (m_e - c_i) are precomputed per CSR entry so the
        // midpoint gather and the two subtractions are gone as well.
        double flux_m3 = 0.0, sx = 0.0, sy = 0.0;
        double dmx = 0.0, dmy = 0.0; // FULL_SWE momentum gather (m⁴/s)
        for (int p = ed.cell_ptr[i]; p < ed.cell_ptr[i + 1]; ++p) {
            const int e = ed.cell_edge[p];
            if (ed.cell_sign[p] > 0) {
                flux_m3 += face_accum_[face_accum_stride_ * (e)];
                if (!preserve_face_water)
                    face_accum_[face_accum_stride_ * (e)] = 0.0;
                if constexpr (swe) {
                    dmx += face_accum_[face_accum_stride_ * (e) + 2];
                    face_accum_[face_accum_stride_ * (e) + 2] = 0.0;
                    dmy += face_accum_[face_accum_stride_ * (e) + 4];
                    face_accum_[face_accum_stride_ * (e) + 4] = 0.0;
                }
            } else {
                flux_m3 += face_accum_[face_accum_stride_ * (e) + 1];
                if (!preserve_face_water)
                    face_accum_[face_accum_stride_ * (e) + 1] = 0.0;
                if constexpr (swe) {
                    dmx += face_accum_[face_accum_stride_ * (e) + 3];
                    face_accum_[face_accum_stride_ * (e) + 3] = 0.0;
                    dmy += face_accum_[face_accum_stride_ * (e) + 5];
                    face_accum_[face_accum_stride_ * (e) + 5] = 0.0;
                }
            }
            if (perot) {
                double f = static_cast<double>(ed.cell_sign[p]) * q_[e] * ed.xi[e];
                if (mode_ == Momentum2D::LOCAL_INERTIAL)
                    f *= ed.conveyance[e];
                sx += f * ed.cell_arm_x[p];
                sy += f * ed.cell_arm_y[p];
            }
        }
        if (boundary_stage) {
            double exported = 0;
            for (int p = ed.cell_ptr[i]; p < ed.cell_ptr[i + 1]; ++p) {
                const int edge = ed.cell_edge[p];
                exported += std::max(0., -(ed.cell_sign[p] > 0 ? face_accum_[face_accum_stride_ * (edge)] : face_accum_[face_accum_stride_ * (edge) + 1]));
            }
            boundary_donor[i] = std::max(0., rk_stage_volume_[i] - exported);
        }
        CellSourceStep source;
        bool has_sources = false;
        if constexpr (kFuse) {
            state_->volume[i] = std::max(0.0, state_->volume[i] + flux_m3);
            if (!state_->transport.cell_runoff_vol.empty())
                state_->transport.cell_runoff_vol[i] -= flux_m3;
            if (std::size_t(i) < channel_source_ready_.size() && channel_source_ready_[i]) {
                source = channel_sources_[i];
                source.ledger =
                    source_ledgers_.data() + std::size_t(sourceThread()) * source_ledger_stride_;
                has_sources = true;
            } else
                has_sources = prepareCellSources(i, dt_c, source);
        }
        if (species) {
            perf::ProcessTimer species_timer(perf::ProcessPhase::surface_species_cells);
            auto& tr = state_->transport;
            const auto ns = static_cast<std::size_t>(tr.n_species);
            if constexpr (!kFuse) {
                if (!tr.cell_runoff_vol.empty())
                    tr.cell_runoff_vol[i] -= flux_m3;
            }
            // Adjacent species share an edge accumulator cache line. Gather
            // a bounded block in one CSR walk, preserving each row's edge order.
            constexpr int block_size = 8;
            for (int first = 0; first < tr.n_species; first += block_size) {
                const int count = std::min(block_size, tr.n_species - first);
                double dm[block_size] = {};
                for (int p = ed.cell_ptr[i]; p < ed.cell_ptr[i + 1]; ++p) {
                    const auto e = static_cast<std::size_t>(ed.cell_edge[p]);
                    double* acc =
                        (ed.cell_sign[p] > 0 ? sacc_L_.data() : sacc_R_.data()) + e * ns + first;
                    for (int r = 0; r < count; ++r) {
                        dm[r] += acc[r];
                        acc[r] = 0.0;
                    }
                }
                for (int r = 0; r < count; ++r) {
                    const int row = first + r;
                    double& mass = tr.cell_mass[tr.idx(row, i)];
                    mass += dm[r];
                    if (!tr.signedRow(row) && mass < 0.0)
                        mass = 0.0;
                    if constexpr (kFuse) {
                        if (has_sources)
                            applyCellSourceRow(i, row, mass, source);
                    }
                }
            }
        }
        if constexpr (kFuse) {
            if (has_sources) {
                state_->volume[i] = source.final_volume;
                if (boundary_stage)
                    boundary_donor[i] =
                        std::max(0., boundary_donor[i] - source.infil - source.evap - source.out);
            }
            applyRuntimeSources(i, dt_c);
        } else {
            state_->volume[i] = std::max(0.0, state_->volume[i] + flux_m3);
            applyCellSources(i, dt_c, boundary_stage ? &boundary_donor[i] : nullptr);
        }
        if (preserve_face_water)
            for (int p = ed.cell_ptr[i]; p < ed.cell_ptr[i + 1]; ++p) {
                const int edge = ed.cell_edge[p];
                if (ed.cell_sign[p] > 0)
                    face_accum_[face_accum_stride_ * (edge)] = 0;
                else
                    face_accum_[face_accum_stride_ * (edge) + 1] = 0;
            }
        inertial::cellEtaDepth(*mesh_, *opts_, i, state_->volume[i], state_->head[i],
                               state_->depth[i]);
        // FRONT_REBUILD: the wetting front reached the halo's outer ring.
        if (front && frontier_[i] && state_->depth[i] >= h_on_front_)
            front_breached = 1;
        // Refresh this cell's Perot discharge vector at its own cadence.
        if (perot) {
            const double inv_a = 1.0 / mesh_->tri_area[i];
            qcx_[i] = sx * inv_a;
            qcy_[i] = sy * inv_a;
        }
        // FULL_SWE momentum update: M = A·q⃗ + Σ face bookings (fluxes and
        // bed-slope corrections), then semi-implicit friction against the
        // NEW depth. Sources (rain, coupling, infiltration, evaporation)
        // move volume only, so the velocity adjusts with the depth. A cell
        // that fell below the dry depth carries no momentum.
        if constexpr (swe) {
            const double A = mesh_->tri_area[i];
            const double h_new = state_->depth[i];
            if (wall_h0 > dry) {
                static const bool muscl_off = std::getenv("OPENSWMM_2D_MUSCL_OFF") != nullptr;
                const bool so = second_order_ && !muscl_off;
                for (int w = wall_ptr_[i]; w < wall_ptr_[i + 1]; ++w) {
                    const int slot = wall_slot_[w];
                    const double nx = mesh_->edge_nx[slot], ny = mesh_->edge_ny[slot];
                    double eta = wall_eta0, hf = wall_h0, ux = qcx_[i] / wall_h0,
                           uy = qcy_[i] / wall_h0;
                    if (so) {
                        const double ax = mesh_->edge_mx[slot] - mesh_->tri_cx[i];
                        const double ay = mesh_->edge_my[slot] - mesh_->tri_cy[i];
                        const auto n = state_->depth.size();
                        eta += gex_[i] * ax + gey_[i] * ay;
                        hf = std::max(0., hf + gex_[n + i] * ax + gey_[n + i] * ay);
                        ux += gux_[i] * ax + guy_[i] * ay;
                        uy += gvx_[i] * ax + gvy_[i] * ay;
                    }
                    const double un = ux * nx + uy * ny, ut = -ux * ny + uy * nx;
                    const double ug = -un * nx - ut * ny, vg = -un * ny + ut * nx;
                    swe::FaceFlux F;
                    double cLx, cLy, cRx, cRy;
                    bool wet;
                    if (so) {
                        const double z = wall_eta0 - wall_h0, zf = eta - hf;
                        wet =
                            swe::faceFluxReconBed(eta, ux, uy, z, wall_h0, eta, ug, vg, z, wall_h0,
                                                  nx, ny, dry, F, cLx, cLy, cRx, cRy, zf, zf);
                    } else
                        wet = swe::faceFlux(eta, wall_h0, qcx_[i], qcy_[i], eta, wall_h0,
                                            wall_h0 * ug, wall_h0 * vg, nx, ny, dry, F, cLx, cLy,
                                            cRx, cRy);
                    if (wet) {
                        const double xdt = mesh_->edge_length[slot] * dt_c;
                        dmx += (-F.mx + cLx) * xdt;
                        dmy += (-F.my + cLy) * xdt;
                    }
                }
            }
            if (h_new > dry) {
                double qx = (A * qcx_[i] + dmx) / A;
                double qy = (A * qcy_[i] + dmy) / A;
                swe::frictionUpdate(qx, qy, h_new, mesh_->mannings_n[i], dt_c);
                qcx_[i] = qx;
                qcy_[i] = qy;
            } else {
                qcx_[i] = 0.0;
                qcy_[i] = 0.0;
            }
        }
    }

    front_breach_ = front_breach_ || front_breached;
    flushSourceLedgers();

    // Boundary edges owned by cells of this firing (serial: perimeter-sized).
    perf::ProcessTimer boundary_timer(perf::ProcessPhase::surface_boundary);
    const bool vfr_face_bc = (opts_->face_reconstruction == FaceDepth2D::VFR_FACE);
    for (std::size_t k = 0; tier0 && k < bc_cell_.size(); ++k) {
        const int i = bc_cell_[k];
        if (tier_[i] != 0 || !cell_active_[i])
            continue;
        // BC cells are pinned to tier 0, so they fire with every tier-0 list;
        // guard against double-firing when called for other tiers.
        const int idx = bc_slot_[k];
        const auto bt = static_cast<BoundaryType>(state_->boundary->edge_bc_type[idx]);
        const double L = mesh_->edge_length[idx];
        double f;
        double f_requested_swe = 0.0;
        if (mode_ == Momentum2D::FULL_SWE) {
            // Ghost-cell Riemann boundary: mass AND momentum, no clamp
            // beyond v ≥ 0 (the Riemann flux is bounded by the states).
            swe_bc_dqx_ = swe_bc_dqy_ = 0.0;
            std::array<double, 4> held{};
            if (boundary_stage || physics_boundary) {
                // The optional global mode uses the same beginning Euler
                // state on interior and exterior faces. Sampling a boundary
                // after the interior gather destroys a uniform rotating flow.
                inertial::cellEtaDepth(*mesh_, *opts_, i,
                    physics_boundary ? physics_volume[i] : rk_stage_volume_[i], held[0], held[1]);
                held[2] = boundary_qx[i];
                held[3] = boundary_qy[i];
            }
            f = boundaryFluxSwe(k, i, dt_c, boundary_stage || physics_boundary ? &held : nullptr);
            f_requested_swe = f;
        } else if (mode_ == Momentum2D::DIFFUSIVE_WAVE) {
            // The diffusive-wave boundary law for every type (collapsed
            // Manning conductance against the prescribed stage).
            f = computeBoundaryEdgeFlux(*mesh_, *state_, *opts_, opts_->flux_dh_eps, i, idx);
        } else if (bt == BoundaryType::SPECIFIED_STAGE && L > 1.0e-12) {
            // Inertial stage boundary: the SAME momentum law as an interior
            // face, integrated against a ghost held at the prescribed stage
            // (η = η_bc, zero-gradient q → the θ-blend collapses to the
            // face's own bc_q_). The former collapsed-Manning flux was a
            // diffusive-wave law alien to the inertial interior: its
            // conductance saturated the equilibrium clamp into a Dirichlet
            // cell and every BC-driven steady case floated one head-jump
            // (~v²/2g scale) above the prescribed stage across the single
            // interior edge feeding the BC cell.
            const double eta_bc = state_->boundary->edge_bc_head[idx];
            double hf;
            if (vfr_face_bc) {
                // B&S Eq. 14 depth of the driving surface over the edge's
                // TRUE endpoint beds (same endpoint rule as the interior
                // VFR_FACE path: edge k = (v[(k+1)%nv], v[(k+2)%nv])).
                int va_e, vb_e;
                mesh_->cell_edge_vertices(i, MeshData::slot_local(idx), va_e, vb_e);
                const double za = mesh_->vz[va_e];
                const double zb = mesh_->vz[vb_e];
                hf = inertial::faceDepthFromEta(std::max(state_->head[i], eta_bc), std::min(za, zb),
                                                std::max(za, zb));
            } else {
                hf = inertial::faceFlowDepth(state_->head[i], eta_bc, mesh_->tri_cz[i]);
            }
            if (hf <= opts_->dry_depth) {
                bc_q_[k] = 0.0;
                auto& receipt = state_->boundary->receipts.at(idx);
                receipt.last_flow = 0.0;
                receipt.last_dt = dt_c;
                continue;
            }
            double deta = state_->head[i] - eta_bc;
            if (std::fabs(deta) < inertial::kEtaDeadband)
                deta = 0.0;
            // The ghost sits across the boundary edge at the centroid→edge
            // normal distance: 2A/(3L) for a triangle (expression retained
            // verbatim for bit-identity), edge_dist_c for a quad.
            const double slope = (mesh_->cell_vertex_count(i) == 3)
                                     ? deta * (3.0 * L) / (2.0 * mesh_->tri_area[i])
                                     : deta / mesh_->edge_dist_c[idx];
            const double n = mesh_->mannings_n[i];
            double qn1 = inertial::inertialFaceUpdate(bc_q_[k], bc_q_[k], hf, dt_c, slope, n * n,
                                                      std::fabs(bc_q_[k]));
            qn1 = inertial::froudeCap(qn1, hf, opts_->froude_max);
            f = qn1 * L; // inflow-positive
        } else {
            f = computeBoundaryEdgeFlux(*mesh_, *state_, *opts_, opts_->flux_dh_eps, i, idx);
        }
        auto& receipt = state_->boundary->receipts.at(idx);
        receipt.last_flow = 0.0;
        receipt.last_dt = dt_c;
        if (f == 0.0) {
            bc_q_[k] = 0.0;
            continue;
        }
        if (boundary_stage &&
            (!std::isfinite(f) || !std::isfinite(qcx_[i]) || !std::isfinite(qcy_[i])))
            throw std::runtime_error("Nonfinite RK2 surface boundary flux or momentum.");
        const double requested_volume = f * dt_c;
        // Clamp the exchange in VOLUME space and re-derive the booked flux
        // from the applied change, so booking matches application exactly
        // (no −1 ulp volume dust from the flux-space clamp).
        const double v_old = state_->volume[i];
        double v_new = v_old + dt_c * f;
        if (bt == BoundaryType::SPECIFIED_STAGE && mode_ != Momentum2D::FULL_SWE) {
            // Equilibrium clamp, kept as the tiny-cell / overshoot backstop:
            // one substep moves the cell AT MOST to the prescribed stage. At
            // the inertial law's gravity-scale takes it almost never binds.
            const double v_eq =
                inertial::cellVolumeFromEta(*mesh_, *opts_, i, state_->boundary->edge_bc_head[idx]);
            if (f < 0.0)
                v_new = std::max(v_new, std::min(v_old, v_eq));
            else
                v_new = std::min(v_new, std::max(v_old, v_eq));
        }
        if (boundary_stage) {
            if (!std::isfinite(requested_volume) || !std::isfinite(v_new))
                throw std::runtime_error("Nonfinite RK2 surface boundary water proposal.");
            if (requested_volume < 0)
                v_new = v_old - std::min({-requested_volume, v_old, boundary_donor[i]});
            boundary_donor[i] -= std::max(0., v_old - v_new);
        }
        if (v_new < 0.0)
            v_new = 0.0; // availability clamp (exact floor)
        f = (v_new - v_old) / dt_c;
        // FULL_SWE booked its momentum inside boundaryFluxSwe against the
        // REQUESTED mass; when the clamp above shrank that mass, take the
        // same fraction of the momentum back (what the comment below has
        // always promised, and what the interior positivity cap does).
        if (mode_ == Momentum2D::FULL_SWE && f != f_requested_swe &&
            (swe_bc_dqx_ != 0.0 || swe_bc_dqy_ != 0.0)) {
            const double lam = (f_requested_swe != 0.0) ? f / f_requested_swe : 0.0;
            qcx_[i] += swe_bc_dqx_ * (lam - 1.0);
            qcy_[i] += swe_bc_dqy_ * (lam - 1.0);
        }
        // Momentum matches applied mass (mirrors the interior positivity-cap
        // rescale of qn1) — the prescribed-flux types record theirs here too.
        bc_q_[k] = (L > 1.0e-12) ? f / L : 0.0;
        const double applied_volume = v_new - v_old;
        if (boundary_stage)
            for (int row = 0; row < state_->transport.n_species; ++row) {
                const auto ns = std::size_t(state_->transport.n_species);
                const double c = requested_volume < 0 ? donorConc(row, i)
                                                      : state_->transport.bc_conc[k * ns + row];
                if (!std::isfinite(requested_volume * c) || !std::isfinite(applied_volume * c) ||
                    !std::isfinite(receipt.requested_mass[row] + requested_volume * c) ||
                    !std::isfinite(receipt.applied_mass[row] + applied_volume * c) ||
                    !std::isfinite(state_->transport.cell_mass[state_->transport.idx(row, i)] +
                                   (applied_volume > 0 ? applied_volume * c : 0)) ||
                    (row == state_->transport.temp_row &&
                     (!std::isfinite(requested_volume * c * opts_->water_heat_capacity) ||
                      !std::isfinite(applied_volume * c * opts_->water_heat_capacity))))
                    throw std::runtime_error("Nonfinite RK2 surface boundary quality proposal.");
            }
        receipt.requested += requested_volume;
        receipt.applied += applied_volume;
        receipt.water_in += std::max(0.0, applied_volume);
        receipt.water_out += std::max(0.0, -applied_volume);
        receipt.last_flow = f;
        const auto ns = static_cast<std::size_t>(state_->transport.n_species);
        for (std::size_t r = 0; r < ns; ++r) {
            const double c = requested_volume < 0.0 ? donorConc(static_cast<int>(r), i)
                             : k * ns + r < state_->transport.bc_conc.size()
                                 ? state_->transport.bc_conc[k * ns + r]
                                 : 0.0;
            const double requested = requested_volume * c, applied = applied_volume * c;
            receipt.requested_mass[r] += requested;
            receipt.applied_mass[r] += applied;
            if (static_cast<int>(r) == state_->transport.temp_row) {
                receipt.heat_requested += requested * opts_->water_heat_capacity;
                receipt.heat_applied += applied * opts_->water_heat_capacity;
            }
        }
        if (f != 0.0) {
            // S1: an OUTFLOW boundary carries the cell's concentration out;
            // an inflow boundary brings water at zero concentration until S2
            // gives edges a species column. Applied volume, not requested,
            // so the species booking matches the water booking exactly.
            if (species_on_) {
                auto& tr = state_->transport;
                if (v_new < v_old) {
                    sinkMassAtCellConc(i, v_old - v_new, tr.lost_boundary);
                } else {
                    // S2: INFLOW carries the edge's [2D_BOUNDARY_QUALITY]
                    // concentration (0 when none was given — clean water).
                    if (!tr.cell_runoff_vol.empty()) // S7: inflow
                        tr.cell_runoff_vol[i] -= v_new - v_old;
                    const auto ns = static_cast<std::size_t>(tr.n_species);
                    if (k * ns + ns <= tr.bc_conc.size()) {
                        const double dv = v_new - v_old;
                        for (std::size_t s = 0; s < ns; ++s) {
                            const double c = tr.bc_conc[k * ns + s];
                            if (c == 0.0)
                                continue;
                            const double g = dv * c;
                            tr.cell_mass[tr.idx(static_cast<int>(s), i)] += g;
                            tr.gained_boundary[s] += g;
                        }
                    }
                }
            }
            state_->volume[i] = v_new;
            bc_accum_[k] += dt_c * f;
            inertial::cellEtaDepth(*mesh_, *opts_, i, state_->volume[i], state_->head[i],
                                   state_->depth[i]);
        }
        // Perot completion: the parallel pass above rebuilt this tier-0
        // cell's discharge vector from INTERIOR edges only, so a cell fed
        // through its boundary carried a systematic (1−θ) drag on every
        // face (the SPECIFIED_FLOW entrance jump). Add the boundary edge's
        // own contribution in the interior gather's outward-flux convention.
        if (!qcx_.empty() && mode_ == Momentum2D::LOCAL_INERTIAL && bc_q_[k] != 0.0) {
            int va, vb;
            mesh_->cell_edge_vertices(i, MeshData::slot_local(idx), va, vb);
            const double mxb = 0.5 * (mesh_->vx[va] + mesh_->vx[vb]);
            const double myb = 0.5 * (mesh_->vy[va] + mesh_->vy[vb]);
            const double fo = -f; // outward volumetric flux (m³/s)
            const double inv_a = 1.0 / mesh_->tri_area[i];
            qcx_[i] += fo * (mxb - mesh_->tri_cx[i]) * inv_a;
            qcy_[i] += fo * (myb - mesh_->tri_cy[i]) * inv_a;
        }
    }

    boundary_timer.stop();
    // Live junction exchange at tier-0 cadence (windowless coupling): the
    // orifice law against LIVE 2D heads and the routing step's 1D heads.
    // Drains cap at the exchange-β share of the source cell; spills cap at
    // the node's stored volume for the whole advance (node_drawn_ ledger) —
    // the same water cannot spill twice within a routing step.
    if (!exch_.empty() && tier0 && state_->node_coupling && state_->nodes_1d) {
        perf::ProcessTimer exchange_timer(perf::ProcessPhase::surface_exchange);
        const auto& pts = *state_->node_coupling;
        for (std::size_t k = 0; k < pts.size(); ++k) {
            const auto& cp = pts[k];
            const int ci = cp.cell_idx;
            if (ci < 0 || !cell_active_[ci])
                continue;
            const double h_off =
                (k < exch_head_slope_.size()) ? exch_head_slope_[k] * exch_tau_ : 0.0;
            double Q = computeNodeCouplingQ(cp, *mesh_, *state_, *state_->nodes_1d, *opts_, nullptr,
                                            h_off);
            if (Q == 0.0)
                continue;
            if (Q > 0.0) { // 2D → 1D drain: availability share of the cell
                Q = std::min(Q, opts_->exchange_beta * std::max(state_->volume[ci], 0.0) / dt_c);
            } else {
                // 1D → 2D spill: the node may only give up its PONDED water,
                // i.e. what it holds ABOVE full_volume.
                //
                // C3 (2026-09-07). This used to be the node's TOTAL stored
                // volume, which let a surcharged-but-not-flooding node
                // dewater its own pipes through the manhole cover: the spill
                // physically exits at the rim, so below-crown conveyance
                // storage is not reachable by it. On a surcharging network
                // that is a large, one-signed transfer of water from the 1D
                // system to the 2D surface with nothing to stop it.
                //
                // The pond is the volume above what the 1D router books at
                // the rim (node_pond_base): DW stores ponded water ABOVE
                // rpt_full_volume (commitNodeDepthState: volume =
                // max(old + dV, rpt_full_volume)) — 0 for a plain junction —
                // while FV holds full_volume = MIN_SURFAREA × full depth
                // below its pond. Subtracting full_volume under DW withheld
                // the first MIN_SURFAREA × full depth of every pond from the
                // spill. Coupled junctions always pond — SurfaceRouter2D
                // assigns them the 2D-cell footprint as ponded_area and
                // DynamicWave lets is_coupled bypass ALLOW_PONDING — so this
                // budget is non-zero whenever there is water to give.
                const auto ni = static_cast<std::size_t>(cp.node_idx);
                const double pond_base = state_->node_pond_base ? (*state_->node_pond_base)[ni]
                                                                : state_->nodes_1d->full_volume[ni];
                const double ponded_1d = std::max(0.0, state_->nodes_1d->volume[ni] - pond_base);
                const double avail = ponded_1d * opts_->vol_1d_to_2d - node_drawn_[ni];
                if (avail <= 0.0)
                    continue;
                const double want = -Q * dt_c;
                const double take = std::min(want, avail);
                // The shortfall is what the orifice law asked for and the
                // node could not supply. Booked so it is visible instead of
                // being absorbed by the 1D side's `y_new = max(y_new, 0)`
                // floor, which would create the water silently.
                spill_deficit_ += std::max(0.0, want - take);
                node_drawn_[ni] += take;
                Q = -take / dt_c;
            }
            // S1: a 2D→1D drain (Q > 0) leaves at the cell's concentration
            // and is booked per coupling point so S3's tuple can hand it to
            // the node's qual_mass_in; a 1D→2D spill arrives at zero
            // concentration until S3 carries the node's published value.
            if (species_on_ && Q > 0.0) {
                auto& tr = state_->transport;
                const auto ns = static_cast<std::size_t>(tr.n_species);
                double* pt = (k * ns < tr.exch_mass.size()) ? &tr.exch_mass[k * ns] : nullptr;
                sinkMassAtCellConc(ci, Q * dt_c, tr.lost_coupling, pt);
            } else if (species_on_ && Q < 0.0) {
                // S3 (D-2DT4): the spill arrives at the NODE's published
                // concentration (`nodes.conc`, which all three 1D engines
                // write), frozen for the batch like the node head. Nothing is
                // queued back to the node: the CSTR mix already takes every
                // outflow — the spill included — at the mixed concentration
                // through the reduced inflow volume, so a debit here would
                // remove the mass twice.
                auto& tr = state_->transport;
                const auto ns = static_cast<std::size_t>(tr.n_species);
                const auto ni = static_cast<std::size_t>(cp.node_idx);
                // S4: the router publishes every ROW's node value (pollutant
                // conc, MSX conc, age, temperature) in one ns-strided array,
                // frozen for the batch like the node head. nodes.conc alone
                // is np-strided and would mis-index once age/temp rows exist.
                const auto* conc_p = state_->node_row_conc;
                if (conc_p && (ni + 1) * ns <= conc_p->size()) {
                    const auto& conc = *conc_p;
                    const double v_in = -Q * dt_c;
                    if (!tr.cell_runoff_vol.empty()) // S7: inflow
                        tr.cell_runoff_vol[ci] -= v_in;
                    for (std::size_t s = 0; s < ns; ++s) {
                        const double c = conc[ni * ns + s];
                        if (c == 0.0)
                            continue;
                        const double g = v_in * c;
                        tr.cell_mass[tr.idx(static_cast<int>(s), ci)] += g;
                        tr.gained_coupling[s] += g;
                    }
                    // The spill VOLUME is tracked per point so the router can
                    // pair it against this window's drained mass — see
                    // SurfaceTransportState::exch_spill. (An incremental
                    // per-substep debit here is ORDER-DEPENDENT — a spill
                    // substep that precedes the window's drain finds nothing
                    // to debit — and the check measured the residue; the
                    // pairing must be done at window granularity.)
                    if (static_cast<std::size_t>(k) < tr.exch_spill.size())
                        tr.exch_spill[static_cast<std::size_t>(k)] += v_in;
                }
            }
            // C1: book the signed per-cell abstraction HERE, where the node
            // exchange actually moves water. The other three sites book
            // `coupling_flux`, which only ever carries the FORCING API's
            // prescribed rate — the node coupling below never writes it, so
            // booking only there left the per-cell series identically zero on
            // every genuinely coupled deck while the domain totals grew.
            // Sign follows the volume update: + = into the cell.
            state_->coupling_applied[ci] += -Q * dt_c;
            state_->volume[ci] -= Q * dt_c;
            if (state_->volume[ci] < 0.0)
                state_->volume[ci] = 0.0;
            exch_[k] += Q * dt_c;
            inertial::cellEtaDepth(*mesh_, *opts_, ci, state_->volume[ci], state_->head[ci],
                                   state_->depth[ci]);
        }
    }
    // Head-ramp clock: tier-0 fires once per finest substep, so dt_c here is
    // exactly the wall the batch has advanced since the last exchange pass.
    if (tier0)
        exch_tau_ += dt_c;
    flushSourceLedgers();
}

// RECONSTRUCTION_ORDER 2: conditioned, linear-exact wet-neighbour gradients.
// Bound velocity at both face midpoints and cell vertices. Thin cells,
// ill-conditioned stencils and shoreline velocity retain first-order fallback.
void ExplicitInertialSolver::computeLimitedGradientsSwe() {
    const auto n = state_->depth.size();
    const auto& ed = edges_;
    const double dry = opts_->dry_depth;
    std::vector<std::array<double, 4>> vertex_bounds;
    {
        vertex_bounds.resize(mesh_->n_vertices());
#pragma omp parallel for schedule(static) num_threads(opts_->num_threads)
        for (int v = 0; v < mesh_->n_vertices(); ++v) {
            double ulo = 1e300, uhi = -1e300, vlo = 1e300, vhi = -1e300;
            for (int p = mesh_->vert_stencil_ptr[v]; p < mesh_->vert_stencil_ptr[v + 1]; ++p) {
                const int j = mesh_->vert_stencil_idx[p];
                const double h = state_->depth[j];
                if (!cell_active_[j] || h <= dry || state_->head[j] - mesh_->vz[v] <= dry)
                    continue;
                const double u = qcx_[j] / h, w = qcy_[j] / h;
                ulo = std::min(ulo, u);
                uhi = std::max(uhi, u);
                vlo = std::min(vlo, w);
                vhi = std::max(vhi, w);
            }
            vertex_bounds[v] = {ulo, uhi, vlo, vhi};
        }
    }
#pragma omp parallel for schedule(static) num_threads(opts_->num_threads)
    for (int k = 0; k < int(active_cells_.size()); ++k) {
        const int i = active_cells_[k];
        gex_[n + i] = gey_[n + i] = gex_[i] = gey_[i] = gux_[i] = guy_[i] = gvx_[i] = gvy_[i] = 0;
        const double hi = state_->depth[i];
        if (hi <= 10 * dry)
            continue;
        const double wi[4] = {state_->head[i], qcx_[i] / hi, qcy_[i] / hi, hi};
        double lo[4] = {wi[0], wi[1], wi[2], wi[3]}, hiw[4] = {wi[0], wi[1], wi[2], wi[3]};
        double xx = 0, xy = 0, yy = 0, bx[4] = {}, by[4] = {};
        unsigned connected = 0;
        int count = 0;
        bool shore = false;
        const int begin = ed.cell_ptr[i], end = ed.cell_ptr[i + 1];
        for (int p = begin; p < end; ++p) {
            const int e = ed.cell_edge[p], j = ed.cL[e] == i ? ed.cR[e] : ed.cL[e];
            const double hj = state_->depth[j], ej = state_->head[j];
            const double sill = std::max(wi[0] - hi, ej - hj);
            if (hj <= dry || !cell_active_[j] || wi[0] - sill <= dry || ej - sill <= dry) {
                shore = true;
                continue;
            }
            connected |= 1u << (p - begin);
            ++count;
            const double dx = mesh_->tri_cx[j] - mesh_->tri_cx[i],
                         dy = mesh_->tri_cy[j] - mesh_->tri_cy[i];
            const double wt = 1 / (dx * dx + dy * dy);
            xx += wt * dx * dx;
            xy += wt * dx * dy;
            yy += wt * dy * dy;
            const double wj[4] = {ej, qcx_[j] / hj, qcy_[j] / hj, hj};
            for (int m = 0; m < 4; ++m) {
                bx[m] += wt * dx * (wj[m] - wi[m]);
                by[m] += wt * dy * (wj[m] - wi[m]);
                lo[m] = std::min(lo[m], wj[m]);
                hiw[m] = std::max(hiw[m], wj[m]);
            }
        }
        if (count < 2)
            continue;
        const double det = xx * yy - xy * xy;
        // Retain the existing shoreline conditioning bound (condition number <98).
        if (det <= 1e-2 * (xx + yy) * (xx + yy))
            continue;
        double gx[4] = {}, gy[4] = {}, phi[4] = {1, 1, 1, 1};
        for (int m = 0; m < 4; ++m) {
            if (shore && (m == 1 || m == 2))
                continue;
            gx[m] = (yy * bx[m] - xy * by[m]) / det;
            gy[m] = (xx * by[m] - xy * bx[m]) / det;
        }
        // Keep the existing scalar BJ bounds, dry-sill exclusion and shoreline
        // velocity fallback. Only the fully-wet geometric gradient changes.
        for (int p = begin; p < end; ++p) {
            if (!(connected & (1u << (p - begin))))
                continue;
            for (int m = 0; m < 4; ++m) {
                const double wf = wi[m] + gx[m] * ed.cell_arm_x[p] + gy[m] * ed.cell_arm_y[p];
                phi[m] = std::min(phi[m], swe::bjLimiter(wi[m], wf, lo[m], hiw[m]));
            }
        }
        const int nvc = mesh_->cell_vertex_count(i);
        if (!shore) {
            for (int k = 0; k < nvc; ++k) {
                const int v = mesh_->cell_vertex(i, k);
                if (state_->head[i] - mesh_->vz[v] <= dry) {
                    phi[1] = phi[2] = 0;
                    break;
                }
                const double ax = mesh_->vx[v] - mesh_->tri_cx[i],
                             ay = mesh_->vy[v] - mesh_->tri_cy[i];
                const auto& b = vertex_bounds[v];
                phi[1] =
                    std::min(phi[1], swe::bjLimiter(wi[1], wi[1] + gx[1] * ax + gy[1] * ay,
                                                    std::min(wi[1], b[0]), std::max(wi[1], b[1])));
                phi[2] =
                    std::min(phi[2], swe::bjLimiter(wi[2], wi[2] + gx[2] * ax + gy[2] * ay,
                                                    std::min(wi[2], b[2]), std::max(wi[2], b[3])));
            }
        }
        if (count < nvc) {
            double positive = 1;
            for (int e = 0; e < nvc; ++e) {
                const int a = mesh_->cell_vertex(i, e), b = mesh_->cell_vertex(i, (e + 1) % nvc);
                const double ax = .5 * (mesh_->vx[a] + mesh_->vx[b]) - mesh_->tri_cx[i];
                const double ay = .5 * (mesh_->vy[a] + mesh_->vy[b]) - mesh_->tri_cy[i];
                const double dh = phi[3] * (gx[3] * ax + gy[3] * ay);
                if (dh < -hi)
                    positive = std::min(positive, -hi / dh);
            }
            phi[0] *= positive;
            phi[3] *= positive;
        }
        gex_[i] = phi[0] * gx[0];
        gey_[i] = phi[0] * gy[0];
        gex_[n + i] = phi[3] * gx[3];
        gey_[n + i] = phi[3] * gy[3];
        gux_[i] = phi[1] * gx[1];
        guy_[i] = phi[1] * gy[1];
        gvx_[i] = phi[2] * gx[2];
        gvy_[i] = phi[2] * gy[2];
    }
}

/**
 * @brief Stage immutable project surface time-series/rating bindings for the initial coupled RK2
 * owner.
 * @param[in,out] ctx Prepared project tables/names/start calendar. Read during this call; owned
 * signal/ rating copies are retained, while the outer heat/reaction owners have their separate
 * borrow contracts.
 * @param[in] stage_scale Finite positive authored stage/curve-coordinate to SI m factor.
 * @param[in] flow_scale Finite positive authored flow-ordinate to the boundary's SI flow
 * convention factor; stored per-edge flow is the downstream boundary law's unit discharge, not
 * an extra edge sum.
 * @return Empty for success/no boundary owner, otherwise initial-profile, kind/name/index/table,
 * finite conversion or initial interpolation diagnostic; caught std::exception text is returned.
 * @note Requires initialized initial-time second-order FULL_SWE with groundwater. Accepts only
 * single-column in-memory ordered supported tables, copies rating data and elapsed signal knots,
 * resolves names, skips active runtime-overridden slots and evaluates each initial value
 * privately. Publishes configured signals/calendar anchor and the staged boundary only after all
 * validation. No shared table cursor or engine calendar advance; temporary allocation can fail.
 * Requires exclusive boundary/marcher configuration and matching stable model identity for
 * future RK2 evaluation.
 */
std::string ExplicitInertialSolver::configureRk2BoundarySignals(SimulationContext& ctx,
                                                                double stage_scale,
                                                                double flow_scale) {
    try {
        if (!initialized_ || !second_order_ || !gw_ || mode_ != Momentum2D::FULL_SWE ||
            t_last_sync_ != 0)
            return "RK2 surface signals require their fresh standalone numerical owner.";
        if (!state_->boundary)
            return {};
        auto boundary = *state_->boundary;
        std::vector<Rk2BoundarySignal> signals;
        std::unordered_map<std::string, GwSourceSignal> cache;
        for (int slot = 0; slot < boundary.size(); ++slot) {
            const auto type = BoundaryType(boundary.edge_bc_type.at(slot));
            if (type == BoundaryType::RATING_CURVE) {
                int& index = boundary.edge_bc_rating_curve.at(slot);
                const auto& name = boundary.edge_bc_rating_curve_name.at(slot);
                const int resolved = index == -2 ? ctx.find_curve(name) : index;
                if (resolved < 0 || resolved >= int(ctx.tables.tables.size()) ||
                    (!name.empty() && ctx.find_curve(name) != resolved))
                    return "RK2 rating curve name or table index differs.";
                const auto& table = ctx.tables.tables[resolved];
                if (table.type != TableType::CURVE_RATING || table.is_file_based ||
                    table.file_handle || table.num_cols != 1 || table.x.size() < 2 ||
                    table.x.size() != table.y.size())
                    return "RK2 rating requires a single-column in-memory RATING curve with at least two points.";
                if (!(stage_scale > 0) || !std::isfinite(stage_scale) || !(flow_scale > 0) ||
                    !std::isfinite(flow_scale))
                    return "Invalid RK2 rating curve unit conversion.";
                for (std::size_t k = 0; k < table.x.size(); ++k)
                    if (!std::isfinite(stage_scale * table.x[k]) ||
                        !std::isfinite(flow_scale * table.y[k]) ||
                        (k && table.x[k] <= table.x[k - 1]))
                        return "RK2 rating curve needs finite, strictly increasing stages and finite flows.";
                Rk2BoundarySignal row;
                row.slot = slot;
                row.table = resolved;
                row.scale = flow_scale;
                row.name = table.id;
                row.curve = std::make_shared<Table>(table);
                row.x_scale = stage_scale;
                signals.push_back(std::move(row));
                index = resolved;
                continue;
            }
            for (bool stage : {true, false}) {
                int& index = stage ? boundary.edge_bc_tseries.at(slot)
                                   : boundary.edge_bc_flow_tseries.at(slot);
                const auto& name = stage ? boundary.edge_bc_tseries_name.at(slot)
                                         : boundary.edge_bc_flow_tseries_name.at(slot);
                if (index == -1 && name.empty())
                    continue;
                if (type != (stage ? BoundaryType::SPECIFIED_STAGE : BoundaryType::SPECIFIED_FLOW))
                    return "RK2 surface signal does not match its hydraulic boundary kind.";
                const int resolved = index == -2 ? ctx.find_timeseries(name) : index;
                if (resolved < 0 || resolved >= int(ctx.tables.tables.size()) ||
                    (!name.empty() && ctx.find_timeseries(name) != resolved))
                    return "RK2 surface signal name or table index differs.";
                const auto& table = ctx.tables.tables[resolved];
                if (table.type != TableType::TIMESERIES || table.is_file_based ||
                    table.file_handle || table.num_cols != 1)
                    return "RK2 surface signals require single-column in-memory time series.";
                const double scale = stage ? stage_scale : flow_scale;
                if (!(scale > 0) || !std::isfinite(scale))
                    return "Invalid RK2 surface signal unit conversion.";
                auto signal = resolveGwSignal(ctx, 0, table.id, cache, "[2D boundary RK2]");
                for (double value : signal.ordinates())
                    if (!std::isfinite(scale * value))
                        return "Nonfinite SI surface boundary signal.";
                signals.push_back({slot, resolved, stage, scale, table.id, std::move(signal)});
                index = resolved;
            }
        }
        // Initial interpolation/conversion can fail despite finite ordinates.
        // Keep the previously configured fresh owner until every row is valid.
        for (const auto& row : signals) {
            if (boundary.runtime_original.count(row.slot))
                continue;
            double value = row.scale * row.signal.at(0);
            if (row.curve) {
                const int cell = MeshData::slot_cell(row.slot);
                double eta = 0, depth = 0;
                inertial::cellEtaDepth(*mesh_, *opts_, cell, state_->volume.at(cell), eta, depth);
                const double query = eta / row.x_scale;
                if (!std::isfinite(query))
                    return "Nonfinite initial RK2 rating coordinate.";
                value = row.scale * table_lookupEx(*row.curve, query);
            }
            if (!std::isfinite(value))
                return "Nonfinite initial RK2 boundary evaluation.";
            (row.stage ? boundary.edge_bc_head : boundary.edge_bc_flow).at(row.slot) = value;
        }
        rk_boundary_signals_ = std::move(signals);
        rk_boundary_anchor_ = ctx.options.start_date;
        *const_cast<BoundaryData*>(state_->boundary) = std::move(boundary);
        return {};
    } catch (const std::exception& e) {
        return e.what();
    }
}
void ExplicitInertialSolver::resolveRk2BoundarySignals(double time) {
    if (rk_boundary_signals_.empty())
        return;
    auto& boundary = *const_cast<BoundaryData*>(state_->boundary);
    for (const auto& row : rk_boundary_signals_) {
        if (boundary.runtime_original.count(row.slot))
            continue;
        const int index = row.curve   ? boundary.edge_bc_rating_curve.at(row.slot)
                          : row.stage ? boundary.edge_bc_tseries.at(row.slot)
                                      : boundary.edge_bc_flow_tseries.at(row.slot);
        if (index != row.table)
            throw std::runtime_error("RK2 surface signal binding differs.");
        double value = row.scale * row.signal.at(time);
        if (row.curve) {
            const int cell = MeshData::slot_cell(row.slot);
            double eta = 0, depth = 0;
            inertial::cellEtaDepth(*mesh_, *opts_, cell, state_->volume.at(cell), eta, depth);
            const double query = eta / row.x_scale;
            if (!std::isfinite(query))
                throw std::runtime_error("Nonfinite RK2 rating coordinate.");
            value = row.scale * table_lookupEx(*row.curve, query);
        }
        if (!std::isfinite(value))
            throw std::runtime_error("Nonfinite RK2 evaluated surface boundary.");
        (row.stage ? boundary.edge_bc_head : boundary.edge_bc_flow).at(row.slot) = value;
    }
}
bool ExplicitInertialSolver::surfaceLatentCooling() const noexcept {
    return gw_ && second_order_ && state_->heat.enabled && state_->heat.surface &&
           (opts_->evaporation != 0 ||
            std::any_of(state_->runtime_forcings.rows.begin(), state_->runtime_forcings.rows.end(),
                        [](const auto& row) { return row.channel == 2; }));
}
std::string ExplicitInertialSolver::rk2ThermalProfileError() const {
    const auto& h = state_->heat;
    if (!h.enabled)
        return {};
    const auto n = std::size_t(mesh_->n_cells());
    const auto& tr = state_->transport;
    if (!gw_ || !gw_->heat().active() || !opts_->coupled_heat || opts_->bed_heat_model != 2 ||
        !h.context || !h.bed || !h.ground_column || tr.n_species <= 0 || tr.temp_row < 0 ||
        tr.gained_external.size() != std::size_t(tr.n_species) ||
        tr.gained_bed.size() != std::size_t(tr.n_species) ||
        tr.row_names != gw_->transport().row_names || tr.n_species != gw_->transport().n_species ||
        tr.temp_row != gw_->transport().temp_row || tr.age_row != gw_->transport().age_row ||
        h.bed_species != tr.n_species || h.bed_temp_row != tr.temp_row ||
        gw_->heat().cells.size() != n || h.context != gw_->heat().context ||
        h.aquifer_ground != &gw_->heat() || h.aquifer_owned.size() != n || h.failures.size() != n ||
        h.ground_column_seconds.size() != n ||
        (!h.channel_footprint.empty() && h.channel_footprint.size() != n))
        return "RK2 finite heat requires matched porous GROUND_COLUMN owners.";
    for (std::size_t i = 0; i < n; ++i) {
        if (surfaceLatentCooling()) {
            if (opts_->evaporation != 0 &&
                (!std::isfinite(state_->evap_rate.at(i)) || state_->evap_rate[i] < 0))
                return "Invalid RK2 native surface evaporation demand.";
            if (state_->volume[i] > 0) {
                const double temperature =
                    tr.cell_mass[tr.idx(tr.temp_row, int(i))] / state_->volume[i];
                if (!(temperature + 273.15 > 0) ||
                    !(transport::heat::latentHeatOfVaporization(temperature) > 0))
                    return "Invalid accepted RK2 latent-cooling temperature.";
            }
        }
        if (!gw_->covers(int(i)) || !h.aquifer_owned[i] || !gw_->heat().cells[i].surface_owned ||
            h.failures[i] || !std::isfinite(h.ground_column_seconds[i]) ||
            h.ground_column_seconds[i] < 0 ||
            (!h.channel_footprint.empty() && h.channel_footprint[i] != 0) ||
            (i < gw_->heat().lid_owners.size() && !gw_->heat().lid_owners[i].empty()))
            return "RK2 finite heat requires complete aquifer coverage without channel, snow or LID thermal owners.";
    }
    for (const auto* row : {&h.atmospheric_j, &h.radiative_j, &h.sensible_j, &h.ground_j,
                            &h.water_bed_j, &h.water_j, &h.latent_j, &h.evaporated_m3}) {
        if (row->size() != n)
            return "RK2 finite thermal ledger shape differs.";
        for (double value : *row)
            if (!std::isfinite(value))
                return "RK2 finite thermal ledger is nonfinite.";
    }
    return {};
}
/**
 * @brief Advance the split coupled physical thermal operator at a held forcing sample horizon.
 * @param[in] seconds Finite positive accepted operator duration s.
 * @param[in] time Finite elapsed forcing sample s, called sample_time in the declaration.
 * @note First advances independent groundwater RK2 conduction, then each qualified
 * surface/ground finite contact with zero latent evaporation (handled by accepted water sinks),
 * publishes surface degC*m3 gain and refreshes groundwater derived views. Disabled surface heat
 * returns after ground work. Thermal flags/refusal throw std::runtime_error; allocation may
 * throw after earlier columns changed. Requires qualified shared owners and enclosing RK2
 * staging/rollback, exclusive access. Does not alter hydraulic water or engine calendar.
 */
void ExplicitInertialSolver::advanceRk2Thermal(double seconds, double time) {
    gw_->advanceRk2Heat(seconds, time);
    if (!state_->heat.enabled)
        return;
    auto& tr = state_->transport;
    auto& h = state_->heat;
    for (int i = 0; i < mesh_->n_cells(); ++i) {
        auto& mass = tr.cell_mass[tr.idx(tr.temp_row, i)];
        const double before = mass;
        mass = applyMeshHeatCell(*mesh_, *opts_, h, i, mass, state_->volume[i], 0, seconds, true);
        if (h.failures[i] || gw_->heat().cells[i].failed)
            throw std::runtime_error(gw_->heat().cells[i].error.empty()
                                         ? "RK2 finite thermal contact failed."
                                         : gw_->heat().cells[i].error);
        tr.gained_external[tr.temp_row] += mass - before;
        gw_->heat().refreshView(i, gw_->transport());
    }
}
void ExplicitInertialSolver::advanceRk2BedContact(double seconds) {
    auto& tr = state_->transport;
    auto& h = state_->heat;
    if (!h.enabled || tr.n_species == 1)
        return;
    auto& ground = gw_->transport();
    auto& cells = gw_->heat().cells;
    const auto surface0 = tr;
    const auto ground0 = ground;
    std::vector<GroundPorewaterColumn> pore0;
    for (const auto& cell : cells)
        pore0.push_back(cell.porewater);
    for (int stage = 0; stage < 2; ++stage)
        for (int i = 0; i < mesh_->n_cells(); ++i) {
            const double wet =
                inertial::cellWetArea(*mesh_, *opts_, i, state_->volume[i]) / mesh_->tri_area[i];
            for (int row = 0; row < tr.n_species; ++row)
                if (row != tr.temp_row) {
                    auto& mass = tr.cell_mass[tr.idx(row, i)];
                    const double before = mass;
                    if (wet > 0)
                        mass = applyLocalBedSpeciesEuler(*mesh_, h, i, row, mass, state_->volume[i],
                                                         seconds * wet);
                    tr.gained_bed[row] += mass - before;
                }
            // All physical increments settle into the current canonical SIGMA
            // inventory before evaluating the second stage's concentrations.
            gw_->heat().refreshView(i, ground);
        }
    const auto blend = [](double& now, double before) {
        now = before + .5 * (now - before);
        if (!std::isfinite(now))
            throw std::runtime_error("Nonfinite RK2 porewater contact blend.");
    };
    for (int row = 0; row < tr.n_species; ++row)
        if (row != tr.temp_row) {
            blend(tr.gained_bed[row], surface0.gained_bed[row]);
            for (int i = 0; i < mesh_->n_cells(); ++i) {
                blend(tr.cell_mass[tr.idx(row, i)], surface0.cell_mass[tr.idx(row, i)]);
                auto& pore = cells[i].porewater;
                const auto nl = cells[i].column.theta.size();
                for (std::size_t j = 0; j < nl; ++j) {
                    const auto k = std::size_t(row) * nl + j;
                    blend(pore.contact_mass[k], pore0[i].contact_mass[k]);
                    if (!ground.n_layers)
                        blend(pore.mass[k], pore0[i].mass[k]);
                }
                if (ground.n_layers) {
                    blend(ground.sat_mass[ground.idx(row, i)],
                          ground0.sat_mass[ground.idx(row, i)]);
                    for (int layer = 0; layer < ground.n_layers; ++layer)
                        blend(ground.layer_mass[ground.layerIdx(row, layer, i)],
                              ground0.layer_mass[ground.layerIdx(row, layer, i)]);
                    ground.refreshUnsat(row, i);
                }
            }
        }
    for (int i = 0; i < mesh_->n_cells(); ++i)
        gw_->heat().refreshView(i, ground);
    if (auto error = gw_->refreshPorewaterContactLedger(); !error.empty())
        throw std::runtime_error(error);
}

void ExplicitInertialSolver::advanceRk2Snow(double seconds, double start) {
    if (!state_->snow.active())
        return;
    auto snow = state_->snow;
    // Forcing resolution can write radiation diagnostics. Keep that scratch
    // weather private; the engine retains its own native weather calendar.
    SimulationContext weather(*state_->snow_context);
    if (auto error =
            snow.advance(weather, start, start + seconds, state_->rainfall, state_->snow_catch);
        !error.empty())
        throw std::runtime_error(error);
    auto volume = state_->volume;
    for (int cell = 0; cell < mesh_->n_cells(); ++cell)
        if (snow.owns(cell)) {
            const double liquid = snow.ledgers()[cell].liquid - state_->snow.ledgers()[cell].liquid;
            if (!std::isfinite(liquid) || liquid < 0 || !std::isfinite(volume[cell] + liquid))
                throw std::runtime_error("Nonfinite RK2 snow liquid proposal.");
            volume[cell] += liquid;
        }
    snow.useAcceptedExposure();
    state_->snow = std::move(snow);
    state_->volume = std::move(volume);
    reconstructAll();
}

// SSP-RK2 (Heun) over the global active lists: U¹ = U⁰ + Δt·L(U⁰),
// U² = U¹ + Δt·L(U¹), Uⁿ⁺¹ = ½(U⁰ + U²). The per-advance ledgers that both
// stages incremented (boundary ∫F dt, coupling ∫Q dt, infiltration applied)
// are reset to their trapezoidal value, ½·(stage-1 + stage-2) increments.
/**
 * @brief Start one accepted SSP-RK2 surface interval with the initial thermal retry depth.
 * @param[in] interval Positive finite interval duration s selected by the owning scheduler.
 * @param[in] start Finite elapsed interval start s, consistent with prepared forcing and owners.
 * @note Delegates to the thermal-depth overload with depth zero. Its coupled profile validation,
 * operator splitting, receipt blending, exception and rollback contracts apply. Requires
 * initialized exclusive marcher/state and prepared active lists; does not advance the engine
 * calendar.
 */
void ExplicitInertialSolver::runRk2Step(double interval, double start) {
    runRk2Step(interval, start, 0);
}
/**
 * @brief Advance the prepared surface and qualified groundwater through a coupled SSP-RK2
 * interval.
 * @param[in] interval Positive finite duration s, already selected by the scheduler's stability
 * bound.
 * @param[in] start Finite elapsed interval start s, consistent with forcing calendars and
 * completed owners.
 * @param[in] thermal_depth Current recursive latent-admissibility retry depth; entry normally
 * uses zero.
 * @note Requires initialized mesh/state/options, active cell/face lists, valid borrowed boundary
 * and forcing identities, exclusive mutable owners and matching native row layouts. Coupled
 * groundwater refuses unsupported runtime/thermal profiles, common-stage/channel-ground
 * ownership and live 1D node coupling. It snapshots the coupled marcher, surface, ground and
 * borrowed boundary receipts; the standalone surface path does not provide that coupled
 * rollback.
 * @details Splits at enabled runtime expiry and supported flow/quality/thermal/boundary knots
 * and sign transitions. Each part applies symmetric runtime, native contact, thermal, snow and
 * reaction operators around two surface/ground Euler stages. It blends water, momentum, native
 * inventories, infiltration/coupling/boundary/runtime receipts and thermal ledgers with the
 * part's beginning state, then resolves end-time boundary values, expiry and infiltration
 * capacity. No engine calendar or source-history publication is performed here. Accepted parts
 * add two Euler steps to march statistics.
 * @throws std::runtime_error On profile, forcing, boundary, native inventory, receipt or
 * physical thermal refusal; allocation and owner operations may also throw. Coupled failure
 * restores the saved canonical trial state and borrowed boundary values before propagation.
 * Scratch/external callback effects are not a general transaction. Latent-cooling admissibility
 * refusal restores the trial and recursively attempts two halves, at most 12 levels and only
 * while time halves remain representable; refusal or a later half failure restores the enclosing
 * coupled trial. No retry here recomputes an independent CFL limit.
 */
void ExplicitInertialSolver::runRk2Step(double interval, double start, unsigned thermal_depth) {
    std::unique_ptr<ExplicitInertialSolver> marcher0;
    std::unique_ptr<SurfaceStateData> surface0;
    std::unique_ptr<SubsurfaceSolver> ground0;
    std::map<int, RuntimeReceipt> receipts0;
    std::vector<double> boundary_head0, boundary_flow0;
    if (gw_) {
        if (auto error = gw_->rk2ProfileError(); !error.empty())
            throw std::runtime_error(error);
        if (auto error = rk2ThermalProfileError(); !error.empty())
            throw std::runtime_error(error);
        if (auto error = rk2SurfaceRuntimeProfile(*state_, gw_); !error.empty())
            throw std::runtime_error(error);

        if (common_stage_ || channel_ground_ ||
            (gw_->transport().temp_row >= 0 &&
             (state_->transport.temp_row != gw_->transport().temp_row ||
              (opts_->coupled_heat && !state_->heat.enabled))) ||
            (state_->node_coupling && !state_->node_coupling->empty()))
            throw std::runtime_error("Coupled RK2 has an unqualified surface/source owner.");
        // Capture every owned numerical field. Borrowed owners are refused
        // above; the boundary receipt map is the sole writable borrowed view.
        perf::count(perf::ProcessCounter::rk2_snapshots, 3);
        marcher0 = std::make_unique<ExplicitInertialSolver>(*this);
        surface0 = std::make_unique<SurfaceStateData>(*state_);
        ground0 = std::make_unique<SubsurfaceSolver>(*gw_);
        if (state_->boundary)
            receipts0 = state_->boundary->receipts;
        if (!rk_boundary_signals_.empty()) {
            boundary_head0 = state_->boundary->edge_bc_head;
            boundary_flow0 = state_->boundary->edge_bc_flow;
        }
    }
    const auto restore_trial = [&]() {
        if (marcher0) {
            *this = *marcher0;
            *state_ = *surface0;
            *gw_ = *ground0;
            if (state_->boundary)
                state_->boundary->receipts = receipts0;
            if (!boundary_head0.empty()) {
                auto& b = *const_cast<BoundaryData*>(state_->boundary);
                b.edge_bc_head = boundary_head0;
                b.edge_bc_flow = boundary_flow0;
            }
        }
    };
    try {
        if (gw_ && !state_->runtime_forcings.rows.empty() && state_->rk2_rain_pending.empty())
            state_->rk2_rain_pending.assign(state_->volume.size(), 0);
        // A forcing knot or sign reversal splits BOTH owners' stages. Splitting
        // only fireGwCells would compose first-order aquifer substeps inside a
        // surface Heun stage. Each Euler evaluation integrates prescribed
        // forcing with its held aquifer state; blending counts the interval once.
        std::vector<double> cuts;
        const auto add_cuts = [&](const GwSourceSignal& signal) {
            const auto& times = signal.timePoints();
            const auto& values = signal.ordinates();
            const auto first = std::upper_bound(times.begin(), times.end(), start);
            for (auto it = first; it != times.end() && *it < start + interval; ++it)
                cuts.push_back(*it);
            for (std::size_t j = std::max<std::size_t>(1, first - times.begin());
                 j < times.size() && times[j - 1] < start + interval; ++j)
                if ((values[j - 1] < 0 && values[j] > 0) || (values[j - 1] > 0 && values[j] < 0)) {
                    const double zero = times[j - 1] + (times[j] - times[j - 1]) *
                                                           (-values[j - 1]) /
                                                           (values[j] - values[j - 1]);
                    if (zero > start && zero < start + interval)
                        cuts.push_back(zero);
                }
        };
        if (gw_) {
            for (const auto& row : state_->runtime_sources.rows)
                if (row.enabled && row.until > start && row.until < start + interval)
                    cuts.push_back(row.until);
            for (const auto& row : gw_->runtimeSources().rows)
                if (row.enabled && row.until > start && row.until < start + interval)
                    cuts.push_back(row.until);
            for (const auto& row : state_->runtime_forcings.rows)
                if (row.enabled && row.until > start && row.until < start + interval)
                    cuts.push_back(row.until);
            gw_->heat().appendRk2Cuts(start, start + interval, cuts);
            for (const auto& source : gw_->sources_) {
                add_cuts(source.flow);
                for (const auto& term : source.terms)
                    add_cuts(term.signal);
            }
            for (const auto& row : rk_boundary_signals_) {
                if (row.curve || state_->boundary->runtime_original.count(row.slot))
                    continue;
                const auto& times = row.signal.timePoints();
                const auto& values = row.signal.ordinates();
                for (auto it = std::upper_bound(times.begin(), times.end(), start);
                     it != times.end() && *it < start + interval; ++it)
                    cuts.push_back(*it);
                if (!row.stage) {
                    for (std::size_t k = 1; k < times.size(); ++k)
                        if ((values[k - 1] < 0 && values[k] > 0) ||
                            (values[k - 1] > 0 && values[k] < 0)) {
                            const double scale =
                                std::max(std::abs(values[k - 1]), std::abs(values[k]));
                            const double a = values[k - 1] / scale, b = values[k] / scale;
                            const double zero =
                                times[k - 1] + (times[k] - times[k - 1]) * (-a) / (b - a);
                            if (zero > start && zero < start + interval)
                                cuts.push_back(zero);
                        }
                }
            }
            for (const auto& boundary : gw_->boundaries_) {
                for (const auto& term : boundary.terms)
                    add_cuts(term.signal);
                if (!boundary.head)
                    add_cuts(boundary.signal);
                else if (!boundary.signal.timePoints().empty()) {
                    const int cell = boundary.cell;
                    const auto& water = gw_->state();
                    const double bottom = water.z_bed[cell];
                    appendGwBoundaryCuts(boundary, start, start + interval, bottom + water.hg[cell],
                                         bottom, water.zs[cell], cuts);
                }
            }
        }
        if (!cuts.empty()) {
            cuts.push_back(start);
            cuts.push_back(start + interval);
            std::sort(cuts.begin(), cuts.end());
            cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
        }
        const std::size_t parts = cuts.empty() ? 1 : cuts.size() - 1;
        for (std::size_t part = 0; part < parts; ++part) {
            const double time = cuts.empty() ? start : cuts[part],
                         dt = cuts.empty() ? interval : cuts[part + 1] - time;
            std::unique_ptr<SubsurfaceSolver> part_ground;
            if (gw_) {
                state_->runtime_sources.expire(time);
                state_->runtime_forcings.expire(time);
                gw_->runtimeSources().expire(time);
            }
            if (gw_)
                advanceRk2Snow(.5 * dt, time);
            const auto runtime0 = state_->runtime_sources;
            const auto forcing0 = state_->runtime_forcings;
            const auto rain_pending0 = state_->rk2_rain_pending;
            if (gw_) {
                advanceRk2RuntimeSources(*state_, *opts_, .5 * dt);
                if (runtime0.active())
                    reconstructAll();
                advanceRk2BedContact(.5 * dt);
                advanceRk2Thermal(.5 * dt, time + .25 * dt);
                advanceRk2SurfaceReactions(.5 * dt);
                gw_->advanceRk2Reactions(.5 * dt);
                // Transport blends against the state after the first reaction half.
                // ground0 remains the untouched whole-call rollback owner.
                if (part > 0 || gw_->hasRk2Reactions() || gw_->heat().active())
                    part_ground = std::make_unique<SubsurfaceSolver>(*gw_);
            }
            const int nt = mesh_->n_triangles();
            // Member buffers: vector assignment reuses capacity, so no per-step
            // allocation (F6).
            rk_v0_ = state_->volume;
            std::map<int, RuntimeReceipt> boundary_receipts0;
            if (state_->boundary)
                boundary_receipts0 = state_->boundary->receipts;
            rk_qx0_ = qcx_;
            rk_qy0_ = qcy_;
            rk_bc0_ = bc_accum_;
            rk_ex0_ = exch_;
            rk_inf0_ = state_->infil_applied;
            rk_cpl0_ = state_->coupling_applied;
            rk_drawn0_ = node_drawn_;
            const auto transport0 = state_->transport;
            const auto heat_evap0 = state_->heat.evaporated_m3;
            const bool latent_cooling = surfaceLatentCooling();
            const auto heat_latent0 =
                latent_cooling ? state_->heat.latent_j : std::vector<double>{};
            const auto heat_atmosphere0 =
                latent_cooling ? state_->heat.atmospheric_j : std::vector<double>{};
            const auto heat_water0 = latent_cooling ? state_->heat.water_j : std::vector<double>{};
            const double evap0 = state_->evap_loss_total;
            for (int stage = 0; stage < 2; ++stage) {
                if (gw_ && !forcing0.rows.empty())
                    for (int i = 0; i < mesh_->n_cells(); ++i) {
                        const double area_dt = mesh_->tri_area[i] * dt,
                                     rain = std::max(0., state_->liquidRainfall(i)) * area_dt;
                        if (!std::isfinite(rain) || !std::isfinite(state_->volume[i]) ||
                            !std::isfinite(state_->volume[i] + rain) ||
                            !std::isfinite(state_->rk2_rain_pending[i] + rain) ||
                            !std::isfinite(
                                state_->runtime_forcings.rate(
                                    2, i, opts_->evaporation != 0 ? state_->evap_rate[i] : 0) *
                                area_dt) ||
                            !std::isfinite(
                                state_->runtime_forcings.rate(3, i, state_->infil_rate[i]) *
                                area_dt))
                            throw std::runtime_error(
                                "Nonfinite RK2 runtime forcing water proposal.");
                        const auto* quality = state_->runtime_forcings.get(4, i);
                        if (!quality || quality->concentrations.empty())
                            quality = state_->runtime_forcings.get(1, i);
                        const auto& concentrations = quality && !quality->concentrations.empty()
                                                         ? quality->concentrations
                                                         : state_->transport.rain_conc;
                        for (int row = 0; row < state_->transport.n_species; ++row) {
                            const double mass =
                                state_->transport.cell_mass[state_->transport.idx(row, i)];
                            const double incoming = std::size_t(row) < concentrations.size()
                                                        ? rain * concentrations[row]
                                                        : 0;
                            if (!std::isfinite(mass) || !std::isfinite(incoming) ||
                                !std::isfinite(mass + incoming))
                                throw std::runtime_error(
                                    "Nonfinite RK2 runtime rainfall quality proposal.");
                        }
                    }
                rk_transport_stage_ =
                    gw_ && (species_on_ || !forcing0.rows.empty() || !bc_cell_.empty());
                if (rk_transport_stage_) {
                    rk_stage_mass_ = state_->transport.cell_mass;
                    rk_stage_volume_ = state_->volume;
                }
                if (gw_)
                    gw_->publishRk2Infiltration(*state_, dt, time);
                resolveRk2BoundarySignals(time + stage * dt);
                fireFaces(active_faces_, dt, /*global_step=*/true);
                if (gw_)
                    gw_->fireGwFaces(0, dt);
                const auto latent_before =
                    latent_cooling ? state_->heat.latent_j : std::vector<double>{};
                fireCells(active_cells_, dt, /*tier0=*/true);
                if (latent_cooling)
                    for (int i = 0; i < mesh_->n_cells(); ++i) {
                        const auto& h = state_->heat;
                        const auto& t = state_->transport;
                        const double volume = state_->volume[i],
                                     mass = t.cell_mass[t.idx(t.temp_row, i)];
                        if (h.failures[i] || !std::isfinite(volume) || !std::isfinite(mass) ||
                            !std::isfinite(h.latent_j[i]) || !std::isfinite(h.atmospheric_j[i]) ||
                            !std::isfinite(h.water_j[i]))
                            throw std::runtime_error("Invalid RK2 surface latent energy proposal.");
                        if ((volume <= 0 && h.latent_j[i] != latent_before[i]) ||
                            (volume > 0 && !(mass / volume + 273.15 > 0)))
                            throw SurfaceLatentIntervalLimit(
                                "RK2 latent cooling requires a smaller admissible coupled interval.");
                    }
                if (gw_)
                    gw_->fireGwRk2Cells(dt, *state_, time);
                if (rk_transport_stage_ && state_->transport.age_row >= 0) {
                    auto& tr = state_->transport;
                    double generated = 0;
                    for (int i = 0; i < tr.n_cells; ++i) {
                        const double production = dt * rk_stage_volume_[i];
                        tr.cell_mass[tr.idx(tr.age_row, i)] += production;
                        generated += production;
                    }
                    tr.generated_age[tr.age_row] += generated;
                }
                rk_transport_stage_ = false;
            }
            // Parallel over every cell. An INACTIVE cell was not fired by either
            // stage, so its average is the identity (volume, q⃗ and the infiltration
            // ledger are unchanged) and only the dry-momentum reset is evaluated —
            // the same value the serial all-cell loop produced.
#pragma omp parallel for schedule(static) num_threads(opts_->num_threads)
            for (int i = 0; i < nt; ++i) {
                if (cell_active_[i]) {
                    state_->volume[i] = 0.5 * (rk_v0_[i] + state_->volume[i]);
                    qcx_[i] = 0.5 * (rk_qx0_[i] + qcx_[i]);
                    qcy_[i] = 0.5 * (rk_qy0_[i] + qcy_[i]);
                    inertial::cellEtaDepth(*mesh_, *opts_, i, state_->volume[i], state_->head[i],
                                           state_->depth[i]);
                    state_->infil_applied[i] =
                        rk_inf0_[i] + 0.5 * (state_->infil_applied[i] - rk_inf0_[i]);
                    // Same half-step average as every other accumulator the RK2
                    // predictor advanced: without it the coupling volume books the
                    // full predictor step while the volume books the average, and
                    // the per-cell series drifts from the domain totals.
                    state_->coupling_applied[i] =
                        rk_cpl0_[i] + 0.5 * (state_->coupling_applied[i] - rk_cpl0_[i]);
                }
                if (state_->depth[i] <= opts_->dry_depth) {
                    qcx_[i] = 0.0;
                    qcy_[i] = 0.0;
                }
            }
            auto& tr = state_->transport;
            for (auto member :
                 {&SurfaceTransportState::cell_mass, &SurfaceTransportState::lost_infiltration,
                  &SurfaceTransportState::lost_boundary, &SurfaceTransportState::gained_external,
                  &SurfaceTransportState::lost_external, &SurfaceTransportState::gained_bed,
                  &SurfaceTransportState::lost_coupling, &SurfaceTransportState::exch_mass,
                  &SurfaceTransportState::exch_spill, &SurfaceTransportState::gained_rainfall,
                  &SurfaceTransportState::gained_boundary, &SurfaceTransportState::gained_coupling,
                  &SurfaceTransportState::gained_exfiltration,
                  &SurfaceTransportState::gained_washoff, &SurfaceTransportState::cell_runoff_vol,
                  &SurfaceTransportState::generated_age}) {
                auto& now = tr.*member;
                const auto& before = transport0.*member;
                if (now.size() != before.size())
                    throw std::runtime_error("RK2 surface transport shape changed.");
                for (std::size_t k = 0; k < now.size(); ++k) {
                    now[k] = .5 * (before[k] + now[k]);
                    if (!std::isfinite(now[k]))
                        throw std::runtime_error("RK2 surface transport is nonfinite.");
                }
            }
            for (std::size_t i = 0; i < state_->heat.evaporated_m3.size(); ++i)
                state_->heat.evaporated_m3[i] =
                    heat_evap0[i] + .5 * (state_->heat.evaporated_m3[i] - heat_evap0[i]);
            if (latent_cooling)
                for (std::size_t i = 0; i < heat_latent0.size(); ++i) {
                    auto& h = state_->heat;
                    h.latent_j[i] = heat_latent0[i] + .5 * (h.latent_j[i] - heat_latent0[i]);
                    h.atmospheric_j[i] =
                        heat_atmosphere0[i] + .5 * (h.atmospheric_j[i] - heat_atmosphere0[i]);
                    h.water_j[i] = heat_water0[i] + .5 * (h.water_j[i] - heat_water0[i]);
                }
            for (std::size_t i = 0; i < state_->rk2_rain_pending.size(); ++i)
                state_->rk2_rain_pending[i] =
                    rain_pending0[i] + .5 * (state_->rk2_rain_pending[i] - rain_pending0[i]);
            for (std::size_t k = 0; k < forcing0.rows.size(); ++k) {
                auto& now = state_->runtime_forcings.rows[k].receipt;
                const auto& old = forcing0.rows[k].receipt;
                const auto blend = [](double& value, double before) {
                    value = before + .5 * (value - before);
                };
                blend(now.requested, old.requested);
                blend(now.applied, old.applied);
                blend(now.water_in, old.water_in);
                blend(now.water_out, old.water_out);
                blend(now.heat_requested, old.heat_requested);
                blend(now.heat_applied, old.heat_applied);
                for (std::size_t r = 0; r < now.applied_mass.size(); ++r) {
                    blend(now.requested_mass[r], old.requested_mass[r]);
                    blend(now.applied_mass[r], old.applied_mass[r]);
                }
                if (forcing0.rows[k].enabled && forcing0.rows[k].channel == 3) {
                    now.last_dt = dt;
                    now.last_flow = (now.applied - old.applied) / dt;
                }
            }
            if (state_->boundary)
                for (auto& entry : state_->boundary->receipts) {
                    const auto& before = boundary_receipts0.at(entry.first);
                    auto& now = entry.second;
                    const auto blend = [](double a, double b) { return a + 0.5 * (b - a); };
                    now.requested = blend(before.requested, now.requested);
                    now.applied = blend(before.applied, now.applied);
                    now.water_in = blend(before.water_in, now.water_in);
                    now.water_out = blend(before.water_out, now.water_out);
                    now.heat_requested = blend(before.heat_requested, now.heat_requested);
                    now.heat_applied = blend(before.heat_applied, now.heat_applied);
                    for (std::size_t r = 0; r < now.applied_mass.size(); ++r) {
                        now.requested_mass[r] =
                            blend(before.requested_mass[r], now.requested_mass[r]);
                        now.applied_mass[r] = blend(before.applied_mass[r], now.applied_mass[r]);
                    }
                    if (gw_ &&
                        state_->boundary->edge_bc_type[entry.first] != int8_t(BoundaryType::WALL)) {
                        now.last_dt = dt;
                        now.last_flow = (now.applied - before.applied) / dt;
                    }
                }
            for (std::size_t k = 0; k < bc_accum_.size(); ++k)
                bc_accum_[k] = rk_bc0_[k] + 0.5 * (bc_accum_[k] - rk_bc0_[k]);
            for (std::size_t k = 0; k < exch_.size(); ++k)
                exch_[k] = rk_ex0_[k] + 0.5 * (exch_[k] - rk_ex0_[k]);
            for (std::size_t k = 0; k < node_drawn_.size(); ++k)
                node_drawn_[k] = rk_drawn0_[k] + 0.5 * (node_drawn_[k] - rk_drawn0_[k]);
            state_->evap_loss_total = evap0 + 0.5 * (state_->evap_loss_total - evap0);
            if (gw_) {
                gw_->blendRk2Water(part_ground ? *part_ground : *ground0, time + dt);
                gw_->advanceRk2Reactions(.5 * dt);
                advanceRk2SurfaceReactions(.5 * dt);
                advanceRk2Thermal(.5 * dt, time + .75 * dt);
                advanceRk2BedContact(.5 * dt);
                advanceRk2RuntimeSources(*state_, *opts_, .5 * dt);
                if (runtime0.active())
                    reconstructAll();
                const double middle = time + .5 * dt;
                advanceRk2Snow(time + dt - middle, middle);
                for (std::size_t k = 0; k < runtime0.rows.size(); ++k)
                    if (runtime0.rows[k].enabled) {
                        auto& receipt = state_->runtime_sources.rows[k].receipt;
                        receipt.last_dt = dt;
                        receipt.last_flow =
                            (receipt.applied - runtime0.rows[k].receipt.applied) / dt;
                    }
                state_->runtime_sources.expire(time + dt);
                state_->runtime_forcings.expire(time + dt);
                gw_->runtimeSources().expire(time + dt);
                // Publish diagnostic ordinates from the accepted state, after both
                // symmetric source halves. Curve evaluation is not an extra water step.
                resolveRk2BoundarySignals(time + dt);
                if (auto error = validateRk2SurfaceBoundary(*mesh_, *state_); !error.empty())
                    throw std::runtime_error(error);
                source_phase_end_ = time + dt;
                gw_->publishRk2Infiltration(*state_, dt, time + dt);
            }
            accumulators_pending_ = false;
            substeps_run_ += 2;
            last_steps_ += 2;
        }
    } catch (const SurfaceLatentIntervalLimit&) {
        restore_trial();
        const double half = .5 * interval;
        if (thermal_depth >= 12 || !(half > 0) || start + half == start ||
            start + half == start + interval)
            throw;
        try {
            runRk2Step(half, start, thermal_depth + 1);
            runRk2Step(interval - half, start + half, thermal_depth + 1);
        } catch (...) {
            restore_trial();
            throw;
        }
    } catch (...) {
        if (marcher0) {
            *this = std::move(*marcher0);
            *state_ = std::move(*surface0);
            *gw_ = std::move(*ground0);
            if (state_->boundary)
                state_->boundary->receipts = std::move(receipts0);
            if (!boundary_head0.empty()) {
                auto& b = *const_cast<BoundaryData*>(state_->boundary);
                b.edge_bc_head = std::move(boundary_head0);
                b.edge_bc_flow = std::move(boundary_flow0);
            }
        }
        throw;
    }
}

/**
 * @brief Bind qualified surface pollutant decay and compiled BULK PIPE laws at initial coupled
 * time.
 * @param[in] ctx Prepared native pollutant/MSX layout, finite nonnegative decay 1/s and compiled
 * reaction token/term/species tables. ReactionData is borrowed on kinetics success and must
 * outlive solver uses without identity mutation.
 * @return Empty on success, otherwise fresh-owner, decay-layout/value, uncompiled/unavailable
 * BULK species or unsupported hydrologic expression dependency diagnostic.
 * @note Allows only enabled BULK/pollutant dependencies and DT/TEMP variables. Stages validation
 * then installs copied decay/row mapping and borrowed reaction system, serialized identity,
 * workspace and zeroed signed native loss/report counters. No reaction/water/heat advance.
 * Allocation/serialization can throw after some binding fields install; configure serially
 * before any advance.
 */
std::string ExplicitInertialSolver::configureRk2Reactions(const SimulationContext& ctx) {
    if (!initialized_ || !second_order_ || !gw_ || t_last_sync_ != 0)
        return "RK2 surface reactions require a fresh coupled numerical owner.";
    const auto& tr = state_->transport;
    const auto& rx = ctx.reactions;
    std::vector<double> decay(tr.n_pollut, 0);
    for (int row = 0; row < tr.n_pollut; ++row) {
        if (std::size_t(row) >= ctx.pollutants.k_decay.size())
            return "RK2 surface decay layout differs.";
        decay[row] = ctx.pollutants.k_decay[row];
        if (!std::isfinite(decay[row]) || decay[row] < 0)
            return "RK2 surface decay is inadmissible.";
    }
    const bool kinetics = tr.n_msx > 0 && std::any_of(rx.pipe_form.begin(), rx.pipe_form.end(),
                                                      [](ReactionExprForm form) {
                                                          return form != ReactionExprForm::NONE;
                                                      });
    std::vector<int> rows;
    if (kinetics) {
        if (!rx.compiled)
            return "RK2 surface reactions require compiled PIPE laws.";
        rows.resize(rx.n_species());
        for (int species = 0; species < rx.n_species(); ++species) {
            const int row = tr.rowIndex(rx.species_name[species]);
            rows[species] = row >= tr.n_pollut && row < tr.n_pollut + tr.n_msx ? row : -1;
            if (rx.pipe_form[species] != ReactionExprForm::NONE && rows[species] < 0)
                return "RK2 surface reaction selects an unavailable BULK species.";
        }
        const auto validate = [&](auto&& self, const RxExprSpan& span) -> bool {
            for (int i = span.begin; i < span.begin + span.len; ++i) {
                const auto& token = rx.token_pool[i];
                if (token.op == RxToken::PUSH_SPECIES && rows[token.idx] < 0)
                    return false;
                if (token.op == RxToken::PUSH_POLLUT && token.idx >= tr.n_pollut)
                    return false;
                if (token.op == RxToken::PUSH_TERM && !self(self, rx.term_expr[token.idx]))
                    return false;
                if (token.op == RxToken::PUSH_HYDVAR && token.idx != int(RxHydVar::DT) &&
                    token.idx != int(RxHydVar::TEMP))
                    return false;
            }
            return true;
        };
        for (int species = 0; species < rx.n_species(); ++species)
            if (rx.pipe_form[species] != ReactionExprForm::NONE &&
                !validate(validate, rx.pipe_expr[species]))
                return "RK2 surface reactions require enabled BULK/pollutant dependencies and DT/TEMP variables.";
    }
    const bool active =
        kinetics || std::any_of(decay.begin(), decay.end(), [](double k) { return k > 0; });
    rk_decay_ = std::move(decay);
    rk_reaction_rows_ = std::move(rows);
    rk_reactions_ = kinetics ? &rx : nullptr;
    rk_reaction_identity_ = kinetics ? transport::serializeReactionSystem(ctx) : std::string{};
    if (kinetics)
        rk_reaction_workspace_.init(rx);
    rk_reacted_.assign(active ? tr.n_species : 0, 0);
    rk_reacted_reported_.assign(active ? tr.n_pollut : 0, 0);
    return {};
}
void ExplicitInertialSolver::advanceRk2SurfaceReactions(double seconds) {
    perf::ProcessTimer process_timer(perf::ProcessPhase::surface_reactions);
    if (rk_reacted_.empty() || !(seconds > 0))
        return;
    auto& tr = state_->transport;
    auto next = tr.cell_mass;
    auto loss = rk_reacted_;
    const double decay_interval = rk_reactions_ ? .5 * seconds : seconds;
    std::vector<double> decay_keep(tr.n_pollut);
    for (int row = 0; row < tr.n_pollut; ++row)
        decay_keep[row] = std::exp(-rk_decay_[row] * decay_interval);
    for (int cell = 0; cell < tr.n_cells; ++cell) {
        const double volume = state_->volume[cell];
        if (!(volume > opts_->dry_depth * mesh_->tri_area[cell]))
            continue;
        const auto decay = [&]() {
            for (int row = 0; row < tr.n_pollut; ++row)
                next[tr.idx(row, cell)] *= decay_keep[row];
        };
        decay();
        if (rk_reactions_) {
            const auto& rx = *rk_reactions_;
            std::vector<double> aqueous(rx.n_species()), pollutants(tr.n_pollut);
            for (int species = 0; species < rx.n_species(); ++species)
                if (rk_reaction_rows_[species] >= 0)
                    aqueous[species] = next[tr.idx(rk_reaction_rows_[species], cell)] / volume;
            for (int row = 0; row < tr.n_pollut; ++row)
                pollutants[row] = next[tr.idx(row, cell)] / volume;
            double vars[int(RxHydVar::COUNT_)] = {};
            vars[int(RxHydVar::TEMP)] =
                tr.temp_row >= 0 ? next[tr.idx(tr.temp_row, cell)] / volume : rx.default_temp_c;
            const auto report = transport::ReactionIntegrator::stepSubcycled(
                rx, rx.pipe_form, rx.pipe_expr, seconds, aqueous.data(), vars,
                rk_reaction_workspace_, pollutants.data());
            if (!report.ok)
                throw std::runtime_error("RK2 surface reaction: " + report.error);
            for (int species = 0; species < rx.n_species(); ++species)
                if (rk_reaction_rows_[species] >= 0)
                    next[tr.idx(rk_reaction_rows_[species], cell)] = aqueous[species] * volume;
            decay();
        }
        for (int row = 0; row < tr.n_pollut + tr.n_msx; ++row) {
            const auto k = tr.idx(row, cell);
            if (!std::isfinite(next[k]) || next[k] < 0)
                throw std::runtime_error("Inadmissible RK2 surface reaction inventory.");
            loss[row] += tr.cell_mass[k] - next[k];
            if (!std::isfinite(loss[row]))
                throw std::runtime_error("Nonfinite RK2 surface reaction ledger.");
        }
    }
    tr.cell_mass = std::move(next);
    rk_reacted_ = std::move(loss);
}
/**
 * @brief Add only unreported surface reaction loss into the native engine quality balance.
 * @param[in,out] ctx Prepared matching qual_routing_reacted ledger, at least the tracked
 * pollutant count. Existing engine totals are retained.
 * @note Stages ledger, adds (rk_reacted-rk_reacted_reported)*flow_2d_to_1d to convert
 * SI-volume-native inventory to the 1D native-volume convention, checks finiteness, then
 * installs totals and advances reported counters. Throws std::runtime_error for shape/overflow
 * and may allocate; refusal preserves canonical engine totals. MSX/full surface loss remains
 * available separately through surfaceReactionLoss(). Requires exclusive report/marcher state;
 * no physical reaction replay, water change or clock advance.
 */
void ExplicitInertialSolver::syncRk2ReactionReport(SimulationContext& ctx) {
    auto next = ctx.mass_balance.qual_routing_reacted;
    if (next.size() < rk_reacted_reported_.size())
        throw std::runtime_error("RK2 surface reaction report layout differs.");
    for (std::size_t row = 0; row < rk_reacted_reported_.size(); ++row) {
        next[row] += (rk_reacted_[row] - rk_reacted_reported_[row]) * opts_->flow_2d_to_1d;
        if (!std::isfinite(next[row]))
            throw std::runtime_error("Nonfinite RK2 surface reaction report.");
    }
    ctx.mass_balance.qual_routing_reacted = std::move(next);
    for (std::size_t row = 0; row < rk_reacted_reported_.size(); ++row)
        rk_reacted_reported_[row] = rk_reacted_[row];
}

// FULL_SWE ghost-cell boundary (2D_FULL_SWE plan §2.2 item 9). The ghost
// state per type: WALL mirrors u_n; NORMAL_FLOW is transmissive
// (zero-gradient); SPECIFIED_STAGE holds η_bc with the interior velocity;
// SPECIFIED_FLOW / RATING_CURVE prescribe the per-metre discharge (outward
// positive) with the interior depth, or the critical depth of that
// discharge when the cell is dry (dry-bed inflow). The Riemann flux against
// the ghost gives the mass flux (returned inflow-positive, m³/s) and the
// momentum change, which is applied to the cell here (tier-0 cadence, after
// the parallel cell pass — the same slot the inertial law's Perot completion
// uses).
double ExplicitInertialSolver::boundaryFluxSwe(std::size_t k, int i, double dt_c,
                                               const std::array<double, 4>* held) {
    const int idx = bc_slot_[k];
    const auto bt = static_cast<BoundaryType>(state_->boundary->edge_bc_type[idx]);
    const double L = mesh_->edge_length[idx];
    if (L <= 1.0e-12)
        return 0.0;
    const double nx = mesh_->edge_nx[idx], ny = mesh_->edge_ny[idx]; // outward
    const double eta_i = held ? (*held)[0] : state_->head[i],
                 h_i = held ? (*held)[1] : state_->depth[i];
    const double z_i = eta_i - h_i;
    const double dry = opts_->dry_depth;
    const double qx_i = held ? (*held)[2] : qcx_[i], qy_i = held ? (*held)[3] : qcy_[i];
    double u_n = 0.0, u_t = 0.0;
    if (h_i > dry) {
        const double ux = qx_i / h_i, uy = qy_i / h_i;
        u_n = ux * nx + uy * ny;
        u_t = -ux * ny + uy * nx;
    }
    // Ghost (h_g, u_ng, u_tg) in the outward-normal frame.
    double h_g = h_i, un_g = u_n, ut_g = u_t, eta_g = eta_i;
    // A prescribed-discharge boundary imposes its mass flux exactly; the
    // Riemann solve below then only supplies the momentum flux.
    bool prescribed_mass = false;
    double q_prescribed = 0.0; // outward-positive, per metre of edge
    switch (bt) {
    case BoundaryType::WALL:
        un_g = -u_n;
        break;
    case BoundaryType::NORMAL_FLOW: {
        // Manning normal-flow outlet, the same law the local-inertial and
        // diffusive paths apply (SurfaceFluxCalculator::boundaryEdgeFlux):
        // per-metre outflow q = h^{5/3}·sqrt(S)/n. Under FULL_SWE this used
        // to be a bare zero-gradient ghost, i.e. the bed slope the modeller
        // set on the edge did nothing.
        // The slope is SIGNED: positive falls away from the domain and
        // drains it (unchanged), negative falls toward it and feeds it.
        // A negative slope cannot bootstrap a DRY cell — the law conveys
        // with the interior depth, and h_i = 0 carries nothing; use a
        // SPECIFIED_FLOW edge for dry-bed inflow, which reconstructs the
        // critical depth of its discharge.
        const double S = state_->boundary->edge_bed_slope[idx];
        const double n_man = mesh_->mannings_n[i];
        prescribed_mass = true;
        if (S != 0.0 && h_i > 0.0 && n_man > 0.0) {
            const double sgnS = (S > 0.0) ? 1.0 : -1.0;
            const double h53 = h_i * std::cbrt(h_i * h_i);
            q_prescribed = sgnS * h53 * std::sqrt(std::fabs(S)) / n_man; // outward +
            un_g = q_prescribed / h_i;
            ut_g = u_t;
        } else {
            // No slope, no depth, no roughness: the law conveys nothing.
            // The mirror ghost keeps the momentum consistent with that —
            // a bare zero-gradient ghost is an ABSORBING boundary and
            // drained a lake at rest through an inert outlet.
            q_prescribed = 0.0;
            un_g = -u_n;
        }
        break;
    }
    case BoundaryType::SPECIFIED_STAGE: {
        const double eta_bc = state_->boundary->edge_bc_head[idx];
        const double c_i = std::sqrt(swe::kGravity * std::max(h_i, 0.0));
        if (h_i > dry && u_n >= c_i) {
            // Supercritical OUTFLOW: no characteristic enters the domain,
            // the prescribed stage cannot act — transmissive ghost.
            break;
        }
        eta_g = eta_bc;
        h_g = (eta_bc - z_i > 0.0) ? eta_bc - z_i : 0.0;
        // Subcritical: the outgoing Riemann invariant u_n + 2c fixes the
        // ghost velocity for the prescribed depth (characteristic
        // extrapolation), instead of a bare copy of the interior velocity.
        if (h_i > dry && h_g > 0.0)
            un_g = u_n + 2.0 * (c_i - std::sqrt(swe::kGravity * h_g));
        break;
    }
    case BoundaryType::SPECIFIED_FLOW:
    case BoundaryType::RATING_CURVE: {
        // A prescribed discharge is a FLUX boundary: the mass flux is the
        // prescribed value (imposed below, as the local-inertial law does
        // in SurfaceFluxCalculator), not whatever a Riemann problem
        // against a ghost happens to deliver. The ghost still sets the
        // MOMENTUM flux and keeps the Audusse pressure balance, so its
        // depth should be the physical boundary depth:
        //   * dry cell            — critical depth (dry-bed inflow),
        //   * subcritical inflow  — from the outgoing invariant u + 2c
        //                           together with q (one characteristic
        //                           leaves, so depth is NOT free),
        //   * supercritical inflow / any outflow — the interior depth.
        const double q_out = state_->boundary->edge_bc_flow[idx]; // outward +
        if (h_i > dry) {
            h_g = h_i;
            if (q_out < 0.0) { // inflow
                const double c_i = std::sqrt(swe::kGravity * h_i);
                if (std::fabs(u_n) < c_i) // subcritical
                    h_g = swe::depthFromInvariantAndDischarge(u_n + 2.0 * c_i, q_out, h_i);
            }
            eta_g = z_i + h_g;
        } else {
            h_g = swe::criticalDepth(std::fabs(q_out));
            eta_g = z_i + h_g;
        }
        un_g = (h_g > 0.0) ? q_out / h_g : 0.0;
        ut_g = 0.0;
        prescribed_mass = true;
        q_prescribed = q_out;
        break;
    }
    }
    // Interior = L, ghost = R, normal outward; velocities re-expressed as
    // (x, y) unit discharges for the shared kernel.
    const double qxg = h_g * (un_g * nx - ut_g * ny);
    const double qyg = h_g * (un_g * ny + ut_g * nx);
    swe::FaceFlux F;
    double cLx, cLy, cRx, cRy;
    const bool wet = swe::faceFlux(eta_i, h_i, qx_i, qy_i, eta_g, h_g, qxg, qyg, nx, ny, dry, F,
                                   cLx, cLy, cRx, cRy);
    const double conv = mesh_->edge_conveyance[idx];
    // A prescribed inflow must reach a DRY cell too — faceFlux reports "not
    // wet" there (both reconstructed depths vanish), and returning 0 would
    // make a dry-bed inflow boundary inert.
    if (!wet)
        return prescribed_mass ? -q_prescribed * conv * L : 0.0;
    const double A = mesh_->tri_area[i];
    // Momentum booked now (the mass is applied by the caller with the v ≥ 0
    // clamp; a clamp rescales the momentum in the same ratio there).
    const double fmx = -F.mx * conv + cLx, fmy = -F.my * conv + cLy;
    // Remembered so the caller can rescale it in the same ratio when its
    // availability clamp shrinks the mass (the boundary loop is serial).
    swe_bc_dqx_ = fmx * L * dt_c / A;
    swe_bc_dqy_ = fmy * L * dt_c / A;
    qcx_[i] += swe_bc_dqx_;
    qcy_[i] += swe_bc_dqy_;
    // Mass: the prescribed discharge verbatim (the local-inertial law's
    // contract, SurfaceFluxCalculator::boundaryEdgeFlux) — the Riemann
    // solve's own mass flux is a wave-speed-weighted BLEND of the interior
    // and prescribed states, which under-delivered the SWASHES bump inflows
    // by 7-21 % and reversed a supercritical inlet outright.
    if (prescribed_mass)
        return -q_prescribed * conv * L;
    return -F.mass * conv * L; // inflow-positive m³/s
}

void ExplicitInertialSolver::publishMomentumVelocity() {
    if (!initialized_ || mode_ != Momentum2D::FULL_SWE) return;
#pragma omp parallel for schedule(static) num_threads(opts_->num_threads)
    for (int i=0;i<mesh_->n_cells();++i) {
        const double h=state_->depth[i];
        state_->face_vx[i]=h>opts_->dry_depth ? -qcx_[i]/h : 0;
        state_->face_vy[i]=h>opts_->dry_depth ? -qcy_[i]/h : 0;
    }
}

void ExplicitInertialSolver::applyMomentumPhysics(double seconds) {
    const auto wall = [&] {
        if (!wall_drag_slots_.empty())
            wall_drag_energy_ += MomentumPhysics2D::wallDrag(*mesh_, *state_->boundary,
                wall_drag_slots_, state_->depth, state_->head, qcx_, qcy_, opts_->dry_depth,
                .5 * seconds, opts_->cell_closure == CellClosure2D::VFR);
    };
    wall();
    const double f = opts_->coriolisParameter();
    MomentumPhysics2D::rotate(qcx_, qcy_, .5 * seconds * f);
    if (opts_->turbulence == Turbulence2D::SMAGORINSKY)
        momentum_physics_.advance(*mesh_, edges_, state_->depth, state_->head, qcx_, qcy_,
                                 opts_->dry_depth, opts_->smagorinsky_cs, seconds,
                                 opts_->eddy_viscosity_background);
    else if (opts_->turbulence == Turbulence2D::MIXING_LENGTH)
        momentum_physics_.advance(*mesh_, edges_, state_->depth, state_->head, qcx_, qcy_,
                                 opts_->dry_depth, 0, seconds, opts_->eddy_viscosity_background,
                                 opts_->mixing_length_coefficient);
    MomentumPhysics2D::rotate(qcx_, qcy_, .5 * seconds * f);
    wall();
}

double ExplicitInertialSolver::runMomentumStep(double limit, double time) {
    if (gw_ || common_stage_ || channel_ground_ || state_->transport.active() ||
        state_->heat.enabled || state_->snow.active() ||
        (state_->node_coupling && !state_->node_coupling->empty()) ||
        state_->runtime_sources.active() || !state_->runtime_forcings.rows.empty())
        throw std::runtime_error("Optional momentum physics has an unqualified coupled source owner.");
    double dt = limit;
    const double f = std::abs(opts_->coriolisParameter());
    if (f > 0) dt = std::min(dt, .25 / f);
    if (opts_->turbulence == Turbulence2D::SMAGORINSKY &&
        (opts_->smagorinsky_cs > 0 || opts_->eddy_viscosity_background > 0)) {
        std::vector<double> rx, ry;
        dt = std::min(dt, momentum_physics_.evaluate(*mesh_, edges_, state_->depth, state_->head,
                      qcx_, qcy_, opts_->dry_depth, opts_->smagorinsky_cs, rx, ry, -1,
                      opts_->eddy_viscosity_background));
    } else if (opts_->turbulence == Turbulence2D::MIXING_LENGTH &&
               (opts_->mixing_length_coefficient > 0 || opts_->eddy_viscosity_background > 0)) {
        std::vector<double> rx, ry;
        dt = std::min(dt, momentum_physics_.evaluate(*mesh_, edges_, state_->depth, state_->head,
                      qcx_, qcy_, opts_->dry_depth, 0, rx, ry, -1, opts_->eddy_viscosity_background,
                      opts_->mixing_length_coefficient));
    }
    // Only momentum has changed during this trial; sources and their receipts
    // are untouched. Tighten the hydrodynamic CFL after the first split half.
    const auto x0 = qcx_, y0 = qcy_;
    const double wall_energy0 = wall_drag_energy_;
    for (int trial = 0; ; ++trial) {
        qcx_ = x0; qcy_ = y0;
        wall_drag_energy_ = wall_energy0;
        applyMomentumPhysics(.5 * dt);
        double stable = opts_->max_timestep;
        for (int i = 0; i < mesh_->n_cells(); ++i)
            if (state_->depth[i] > opts_->dry_depth)
                stable = std::min(stable, inertial::cellCflDt(opts_->cfl_number,
                    edges_.cell_lpos[i], state_->depth[i],
                    std::hypot(qcx_[i], qcy_[i]) / state_->depth[i]));
        if (dt <= stable) break;
        if (trial == 39) {
            qcx_ = x0; qcy_ = y0;
            wall_drag_energy_ = wall_energy0;
            throw std::runtime_error("Optional momentum physics cannot satisfy the wave CFL.");
        }
        dt = std::min(.5 * dt, stable);
    }
    if (second_order_) runRk2Step(dt, time);
    else runMacroCycle(dt, 1, time);
    applyMomentumPhysics(.5 * dt);
    return dt;
}

void ExplicitInertialSolver::runMacroCycle(double dt0, int nsub, double time) {
    const int K = static_cast<int>(cells_by_tier_.size());
    static const bool dbg_invariant = [] {
        const char* e = std::getenv("OPENSWMM_2D_MARCHER_CHECK");
        return e && e[0] == '1';
    }();
    auto invariant = [&]() -> double {
        double s = 0.0;
        for (int i = 0; i < mesh_->n_triangles(); ++i)
            s += state_->volume[i];
        for (int e = 0; e < edges_.ne; ++e)
            s += face_accum_[face_accum_stride_ * (e)] + face_accum_[face_accum_stride_ * (e) + 1];
        return s;
    };

    for (int s = 0; s < nsub; ++s) {
        const double inv0 = dbg_invariant ? invariant() : 0.0;
        // Faces integrate at the start of their interval; cells publish at
        // its end, after every incident face has covered that interval.
        // Thus a complete macro-cycle leaves no unconsumed surface flux.
        for (int k = 0; k < K; ++k) {
            if (s % (1 << k))
                continue;
            if (!edges_by_tier_[k].empty())
                fireFaces(edges_by_tier_[k], (1 << k) * dt0);
            // G1: the subsurface's lateral Darcy faces ride the SAME rung of
            // the SAME ladder. Its tier lists are its own (G-A), so a GW face
            // fires here only if the groundwater's stability step put it on
            // rung k — the two domains share the cadence, not the criterion.
            if (gw_)
                gw_->fireGwFaces(k, (1 << k) * dt0);
        }
        if (dbg_invariant) {
            const double inv1 = invariant();
            if (std::fabs(inv1 - inv0) > 1.0e-9 * (std::fabs(inv0) + 1.0))
                std::fprintf(stderr,
                             "[marcher-check] FACE phase moved invariant: "
                             "s=%d d=%.6e\n",
                             s, inv1 - inv0);
        }
        for (int k = 0; k < K; ++k) {
            if ((s + 1) % (1 << k))
                continue;
            if (k == 0)
                source_phase_end_ = time + (s + 1) * dt0;
            if (!cells_by_tier_[k].empty() || k == 0)
                fireCells(cells_by_tier_[k], (1 << k) * dt0, k == 0);
            // The GW cells of this rung fire AFTER the surface cells of the
            // same rung, so a cell that infiltrated in this substep hands the
            // water down within the substep rather than a rung later.
            if (gw_)
                gw_->fireGwCells(k, (1 << k) * dt0, *state_, time + (s + 1 - (1 << k)) * dt0,
                                 common_stage_ ? source_phase_end_ : -1.0);
        }
        // Node exchange is sampled at tier-0 cadence against the batch-frozen
        // 1D heads, exactly like the surface's junction exchange, and booked
        // into the GW cells' accumulators (G-B) rather than applied directly.
        if (gw_ && state_->nodes_1d)
            gw_->sampleNodeExchange(state_->nodes_1d, dt0, time + (s + 1) * dt0);
        captureSourceResults();
        if (dbg_invariant) {
            const double inv2 = invariant();
            if (std::fabs(inv2 - inv0) > 1.0e-9 * (std::fabs(inv0) + 1.0))
                std::fprintf(stderr,
                             "[marcher-check] CELL phase moved invariant: "
                             "s=%d d=%.6e (sources excluded? rain/bc active)\n",
                             s, inv2 - inv0);
        }
        ++substeps_run_;
        ++last_steps_;
    }
}

double ExplicitInertialSolver::advance(double t_current, double t_target) {
    if (!(second_order_ && gw_))
        return advanceImpl(t_current, t_target);
    if (auto error = rk2CheckpointProfile(); !error.empty())
        throw std::runtime_error(error);
    if (!std::isfinite(t_current) || !std::isfinite(t_target) || t_current != t_last_sync_ ||
        t_target < t_current)
        throw std::invalid_argument("Coupled RK2 requires its accepted completed clock.");
    perf::count(perf::ProcessCounter::rk2_snapshots, 3);
    auto marcher = *this;
    auto surface = *state_;
    auto ground = *gw_;
    std::map<int, RuntimeReceipt> receipts;
    if (state_->boundary)
        receipts = state_->boundary->receipts;
    const auto boundary_head =
        rk_boundary_signals_.empty() ? std::vector<double>{} : state_->boundary->edge_bc_head;
    const auto boundary_flow =
        rk_boundary_signals_.empty() ? std::vector<double>{} : state_->boundary->edge_bc_flow;
    try {
        return advanceImpl(t_current, t_target);
    } catch (...) {
        *this = std::move(marcher);
        *state_ = std::move(surface);
        *gw_ = std::move(ground);
        if (state_->boundary)
            state_->boundary->receipts = std::move(receipts);
        if (!boundary_head.empty()) {
            auto& b = *const_cast<BoundaryData*>(state_->boundary);
            b.edge_bc_head = boundary_head;
            b.edge_bc_flow = boundary_flow;
        }
        throw;
    }
}

/**
 * @brief Execute the CPU macro-cycle/global-tail loop and publish one accepted window boundary.
 * @param[in] t_current Elapsed start s (declaration: current), finite and compatible with owner
 * clocks.
 * @param[in] t_target Elapsed target s (declaration: target), no earlier than t_current.
 * @return t_target after settling and publishing accepted state, or ordinary
 * uninitialized/nonforward no-op.
 * @note Common-source mode validates readiness and exact stage/source clocks then marks
 * checkpoint not ready while work is in progress. Resets per-advance boundary/node native/water
 * books, preserves rebuild cadence across windows, tightens current stability and advances
 * active surface plus independent groundwater even when the surface is dry. Full cycles use
 * binary tiers (or qualified global RK2); tails settle first and use equal global steps,
 * preserving topology/tier assignment. Only qualified numerical sub-ulp landing skips physics,
 * reanchoring coupled accepted clocks consistently. Completes lazy inactive sources,
 * reconstructs interior flux at published heads, writes applied window-mean boundary m3/s,
 * refreshes recorder checkpoint match and restores readiness. Does not write result files itself
 * or advance engine calendar. Refusal/allocation can follow partial physical, ledger,
 * stage/recorder updates; outer advance() supplies only the specific coupled RK2 rollback.
 * Requires stable prepared arrays, positive finite step bounds and exclusive outer owner access.
 */
double ExplicitInertialSolver::advanceImpl(double t_current, double t_target) {
    if (common_stage_ && (!checkpoint_ready_ || !std::isfinite(t_current) ||
                          !std::isfinite(t_target) || t_current != t_last_sync_ ||
                          t_current != common_stage_->completedEnd() || t_target < t_current))
        throw std::invalid_argument("Common source marcher requires its accepted completed clock.");
    if (!initialized_ || t_target <= t_current)
        return t_target;
    refreshConveyance();
    plain_sources_ = !second_order_ && !gw_ && !common_stage_ && !channel_ground_ &&
                     state_->transport.n_species == 0 && !state_->heat.enabled &&
                     !state_->snow.active() && state_->runtime_forcings.rows.empty() &&
                     state_->runtime_sources.rows.empty() && state_->rk2_rain_pending.empty();
    struct ResetPlainSources {
        bool& selected;
        ~ResetPlainSources() { selected = false; }
    } reset_plain_sources{plain_sources_};
    if (common_stage_)
        checkpoint_ready_ = false;

    double t = t_current;
    // The lazy-source clock persists across advances (inactive cells may owe
    // sources for spans straddling advance boundaries); re-anchor only if the
    // caller's clock went backwards (hotstart / reinit).
    if (t_last_sync_ > t_current)
        t_last_sync_ = t_current;
    std::fill(bc_accum_.begin(), bc_accum_.end(), 0.0);
    std::fill(exch_.begin(), exch_.end(), 0.0);
    std::fill(node_drawn_.begin(), node_drawn_.end(), 0.0);
    // S1: the per-point species drain resets with the per-point volume drain
    // it rides on, so S3's tuple reads one advance's worth and not a total.
    std::fill(state_->transport.exch_mass.begin(), state_->transport.exch_mass.end(), 0.0);
    std::fill(state_->transport.exch_spill.begin(), state_->transport.exch_spill.end(), 0.0);
    exch_tau_ = 0.0;
    last_steps_ = 0;
    // Rebuild cadence persists ACROSS advances: under windowless co-advance
    // the router calls advance() per ~1 s routing step, and a forced O(nt)
    // settle+rebuild per call was measured as the dominant cost at 228k
    // cells. The lazy-source clock still lands exactly (syncAndRebuild at
    // every entry whose cadence is due, plus the final landing below).
    int cycles_since_rebuild = cycles_since_rebuild_;

    while (t < t_target) {
        if (cycles_since_rebuild >= kRebuildEveryCycles ||
            (front_breach_ && cycles_since_rebuild > 0)) {
            syncAndRebuild(t);
            cycles_since_rebuild = 0;
        } else {
            refreshDt0();
        }
        if (opts_->momentumPhysics()) {
            const double dt = runMomentumStep(std::min(dt0_, t_target - t), t);
            if (!(dt > 0) || t + dt == t)
                throw std::runtime_error("Optional momentum physics cannot advance the clock.");
            t = dt == t_target - t ? t_target : t + dt;
            last_dt_ = dt;
            ++cycles_since_rebuild;
            continue;
        }
        const int K = static_cast<int>(cells_by_tier_.size());
        const int nsub_full = 1 << (K - 1);
        const double remaining = t_target - t;

        // G1-c item 2 (2026-09-19): a quiescent SURFACE no longer strides
        // the window when an aquifer is live — the groundwater's lateral
        // Darcy flow, deep loss, ET and node exchange fire on the ladder
        // whether or not any surface cell is wet, and a dry cell the aquifer
        // exfiltrates into is seeded active by syncAndRebuild's pending pass.
        // Before this the whole macro cycle was skipped, so a dry summer
        // froze the water table in place.
        if (active_cells_.empty() && !(gw_ && gw_->active())) {
            // Quiescent: stride the window; the lazy tier keeps accumulating.
            t = t_target;
            last_dt_ = remaining;
            // Lazy rain/spill can wet an inactive cell during this stride.
            // Reconsider activation at the next entry; no macro cycles ran
            // to advance the normal rebuild cadence.
            cycles_since_rebuild = kRebuildEveryCycles;
            break;
        }

        if (nsub_full * dt0_ <= remaining) {
            // Full macro cycle fits (dt0_ <= remaining here, so no clamp).
            if (second_order_)
                runRk2Step(dt0_, t); // K == 1: one global step
            else
                runMacroCycle(dt0_, nsub_full, t);
            t += nsub_full * dt0_;
            last_dt_ = dt0_;
            ++cycles_since_rebuild;
            continue;
        }

        // Tail: not enough room for a full macro cycle — split the WHOLE
        // remaining span into nt EQUAL global substeps so the window lands
        // exactly with no degenerate step. nt = ceil(remaining/dt0_) is at
        // most the substep count the un-split tail sequence took, and each
        // dt_tail = remaining/nt is in (dt0_/2, dt0_] for nt >= 2 — still
        // CFL-safe, never arbitrarily small. Settle pending transfers first:
        // a cell about to be stepped out of cadence must not carry an
        // in-flight accumulator whose cap bookkeeping assumed the tiered
        // schedule.
        //
        // This used to be expressed by collapsing every cell and face to
        // tier 0 and running a ONE-substep macro cycle at dt0 = remaining,
        // which cost an O(n_cells + n_faces) re-tiering, forced a full
        // syncAndRebuild on the next entry, and — because the leftover has
        // no floor — fired degenerate substeps (down to ~1e-13 s when the
        // macro path's `t += nsub*dt0` landed 1 ulp short of t_target).
        // Firing the union lists directly is the same arithmetic in the
        // same order (the collapsed tier-0 lists WERE these lists) and
        // leaves the tiering intact, so the rebuild keeps its own cadence.
        {
            // Sub-ulp residue (macro landing shortfall): land without
            // firing physics on a span below any numerical significance.
            const double clock_ulp = 8 * std::numeric_limits<double>::epsilon() *
                                     std::max({1., std::abs(t), std::abs(t_target)});
            if (!common_stage_ && remaining <= dt0_ * 1.0e-9 &&
                (!(second_order_ && gw_) || remaining <= clock_ulp)) {
                if (second_order_ && gw_) {
                    // Numerical landing only: no physical interval is replayed.
                    // Both owners adopt the same representable accepted clock.
                    source_phase_end_ = t_target;
                    gw_->source_firing_end_ = t_target;
                    if (state_->snow.active())
                        state_->snow.reanchorCompletedEnd(t_target);
                    for (int i = 0; i < gw_->state_.n_cells; ++i)
                        if (gw_->covers(i))
                            gw_->cell_completed_[i] = t_target;
                }
                t = t_target;
                break;
            }
            settleAccumulators();
            const int nt = std::max(1, static_cast<int>(std::ceil(remaining / dt0_)));
            const double dt_tail = remaining / nt;
            const int Kgw = static_cast<int>(cells_by_tier_.size());
            for (int s = 0; s < nt; ++s) {
                if (second_order_) {
                    runRk2Step(dt_tail, t + s * dt_tail);
                    continue;
                }
                fireFaces(active_faces_, dt_tail, /*global_step=*/true);
                // G-X1 (2026-09-19): the tail fires the aquifer too. It
                // used to fire the surface lists only, so on a deck whose
                // routing step never fits a full macro cycle (a variable
                // 1D step, or a step just under dt0) the groundwater never
                // advanced at all — no lateral flow, no node exchange.
                // Every GW tier fires once at the tail step, the same
                // collapse the surface lists get.
                if (gw_)
                    for (int k = 0; k < Kgw; ++k)
                        gw_->fireGwFaces(k, dt_tail);
                source_phase_end_ = s + 1 == nt ? t_target : t + (s + 1) * dt_tail;
                fireCells(active_cells_, dt_tail, /*tier0=*/true);
                if (gw_) {
                    for (int k = 0; k < Kgw; ++k)
                        gw_->fireGwCells(k, dt_tail, *state_, t + s * dt_tail,
                                         common_stage_ ? source_phase_end_ : -1.0);
                    if (state_->nodes_1d)
                        gw_->sampleNodeExchange(state_->nodes_1d, dt_tail, source_phase_end_);
                }
                captureSourceResults();
                ++substeps_run_;
                ++last_steps_;
            }
            accumulators_pending_ = false; // every side just gathered
            last_dt_ = dt_tail;
            ++cycles_since_rebuild;
            t = t_target; // exact landing — no 1-ulp re-entry
            break;
        }
    }

    cycles_since_rebuild_ = cycles_since_rebuild;
    settleAccumulators();
    // Final lazy-source landing: cheap when nothing is pending — the full
    // rebuild only runs on its own cadence.
    if (t_target > t_last_sync_) {
        if (cycles_since_rebuild_ >= kRebuildEveryCycles) {
            syncAndRebuild(t_target);
            cycles_since_rebuild_ = 0;
        } else {
            lazySourcesOnly(t_target);
        }
    }

    // Publish the flux picture the router's output/ledger contract reads
    // (MOMENTUM INERTIAL: no DW recompute). Interior faces re-limit q against
    // the PUBLISHED surface (the update's own clamp used the face depth it
    // saw; the subsequent cell pass moved the heads — a draining front would
    // otherwise publish a super-Froude flux inconsistent with the published
    // depths). Boundary slots carry the WINDOW-MEAN applied flux so the
    // router's −flux·dt_done booking recovers the exact ∫F_applied dt.
    perf::ProcessTimer publish_timer(perf::ProcessPhase::surface_publish);
    const int publish_threads = std::max(1, opts_->num_threads);
    const std::size_t slots = state_->edge_flux.size();
    if (publish_threads > 1 && slots >= 2048) {
        const auto count = static_cast<std::ptrdiff_t>(slots);
#pragma omp parallel for schedule(static) num_threads(publish_threads)
        for (std::ptrdiff_t k = 0; k < count; ++k)
            state_->edge_flux[static_cast<std::size_t>(k)] = 0.0;
    } else
        std::fill(state_->edge_flux.begin(), state_->edge_flux.end(), 0.0);
    const bool vfr_face = (opts_->face_reconstruction == FaceDepth2D::VFR_FACE);
    // Only the ACTIVE faces can carry flux: a face with an inactive side had
    // its q zeroed when the side deactivated, so the old full sweep spent
    // O(n_faces) recomputing a face depth and a Froude cap in order to publish
    // the zero the fill above already wrote. Identical output, and a quiescent
    // tail (active fraction reached 0.0 % in the storm run) now costs nothing.
    // Each interior edge owns two unique output slots. Boundary slots follow
    // after the team joins, retaining their window-mean publication order.
    const auto publish_face = [&](int e) {
        double qp = 0.0;
        if (mode_ == Momentum2D::LOCAL_INERTIAL) {
            const double hf =
                vfr_face ? inertial::faceFlowDepthVfr(state_->head[edges_.cL[e]],
                                                      state_->head[edges_.cR[e]], edges_.ze_lo[e],
                                                      edges_.ze_hi[e])
                         : inertial::faceFlowDepth(state_->head[edges_.cL[e]],
                                                   state_->head[edges_.cR[e]], edges_.zface[e]);
            if (hf > opts_->dry_depth)
                qp = inertial::froudeCap(q_[e], hf, opts_->froude_max) * edges_.conveyance[e];
        } else {
            qp = q_[e]; // last evaluated face mass flux (already limited)
        }
        const double F = qp * edges_.xi[e];
        state_->edge_flux[edges_.slotL[e]] = -F;
        state_->edge_flux[edges_.slotR[e]] = +F;
    };
    const int active_faces = static_cast<int>(active_faces_.size());
    if (publish_threads > 1 && active_faces >= 2048) {
#pragma omp parallel for schedule(static) num_threads(publish_threads)
        for (int k = 0; k < active_faces; ++k)
            publish_face(active_faces_[static_cast<std::size_t>(k)]);
    } else
        for (const int e : active_faces_)
            publish_face(e);
    const double span = t_target - t_current;
    if (span > 0.0)
        for (std::size_t k = 0; k < bc_cell_.size(); ++k)
            state_->edge_flux[bc_slot_[k]] = bc_accum_[k] / span;

    publish_timer.stop();
    refreshSourceResultBoundary();
    checkpoint_ready_ = true;
    return t_target;
}

void ExplicitInertialSolver::refreshSourceResultBoundary() {
    if (source_history_) {
        // Final render/face publication belongs to the accepted boundary too.
        // Update only its checkpoint match, never add a second source interval.
        const auto error = source_history_->refreshCheckpointMatch(*common_stage_);
        if (!error.empty())
            throw std::runtime_error("[2D_SOURCE_RESULTS] " + error);
    }
}

void ExplicitInertialSolver::reinitialize(double /*t0*/) {
    if (common_stage_)
        throw std::runtime_error("Common source stage restart/rollback is not qualified.");
    if (!initialized_)
        return;
    // External state edit (hot start / breach redo): volumes are authoritative;
    // face momentum and pending transfers are stale — drop them.
    std::fill(q_.begin(), q_.end(), 0.0);
    std::fill(bc_q_.begin(), bc_q_.end(), 0.0);
    std::fill(face_accum_.begin(), face_accum_.end(), 0.0);
    if (mode_ == Momentum2D::FULL_SWE) {
        std::fill(qcx_.begin(), qcx_.end(), 0.0);
        std::fill(qcy_.begin(), qcy_.end(), 0.0);
    }
    accumulators_pending_ = false;
    reconstructAll();
}

void ExplicitInertialSolver::refreshBoundaries() {
    if (!initialized_ || !state_->boundary)
        return;
    settleAccumulators();
    const auto old_slots = bc_slot_;
    const auto old_q = bc_q_;

    const auto ns = static_cast<std::size_t>(state_->transport.n_species);
    bc_cell_.clear();
    bc_slot_.clear();
    wall_ptr_.clear();
    wall_slot_.clear();
    wall_ptr_.push_back(0);
    for (int c = 0; c < mesh_->n_cells(); ++c) {
        for (int e = 0; e < mesh_->cell_vertex_count(c); ++e) {
            if (mesh_->cell_neighbour(c, e) >= 0)
                continue;
            const int slot = MeshData::slot(c, e);
            if (static_cast<BoundaryType>(state_->boundary->edge_bc_type[slot]) ==
                BoundaryType::WALL) {
                wall_slot_.push_back(slot);
                state_->edge_flux[slot] = 0.0;
            } else {
                bc_cell_.push_back(c);
                bc_slot_.push_back(slot);
                pin_t0_[static_cast<std::size_t>(c)] = 1;
            }
        }
        wall_ptr_.push_back(static_cast<int>(wall_slot_.size()));
    }
    bc_accum_.assign(bc_slot_.size(), 0.0);
    bc_q_.assign(bc_slot_.size(), 0.0);
    auto& conc = state_->transport.bc_conc;
    conc.assign(bc_slot_.size() * ns, 0.0);
    for (int slot : bc_slot_)
        state_->boundary->ensureReceipt(slot, static_cast<int>(ns));
    for (std::size_t k = 0; k < bc_slot_.size(); ++k) {
        const auto it = std::lower_bound(old_slots.begin(), old_slots.end(), bc_slot_[k]);
        if (it != old_slots.end() && *it == bc_slot_[k]) {
            const auto j = static_cast<std::size_t>(it - old_slots.begin());
            bc_q_[k] = old_q[j];
        }
        for (const auto& row : state_->transport.bc_quality_rows)
            if (row.slot == bc_slot_[k])
                conc[k * ns + row.species] = row.conc;
        const auto found = state_->transport.runtime_bc_conc.find(bc_slot_[k]);
        if (found != state_->transport.runtime_bc_conc.end())
            for (std::size_t row = 0; row < ns; ++row)
                conc[k * ns + row] = found->second[row];
    }
    resolveRk2BoundarySignals(t_last_sync_);
    cycles_since_rebuild_ = kRebuildEveryCycles;
}

void ExplicitInertialSolver::resyncFromVolumes(double /*t0*/) {
    if (common_stage_)
        throw std::runtime_error("Common source stage external volume edits are not qualified.");
    if (!initialized_)
        return;
    // Volumes already live in state_->volume; keep the face momentum (nothing
    // failed — this is a pure re-time on this path). Pending transfers were
    // booked into volumes at the last cell firing; accumulators stay.
    reconstructAll();
    cycles_since_rebuild_ = kRebuildEveryCycles;
}

void ExplicitInertialSolver::pinExternalSourceCells(const std::vector<int>& cells) {
    for (int cell : cells)
        pin_t0_.at(static_cast<std::size_t>(cell)) = 1;
    cycles_since_rebuild_ = kRebuildEveryCycles;
}

/**
 * @brief Borrow a prepared active shared channel/physical-ground owner at the initial CPU source
 * clock.
 * @param[in,out] stage External finite channel/ground mapping and canonical borrowed owners,
 * stable and alive through all marcher use. Inactive stage is ignored without attachment.
 * @throws std::invalid_argument Marcher is uninitialized, accepted source time is nonzero or a
 * common source stage is attached.
 * @note Stores pointer, sizes per-cell source/mixed-temperature scratch and pins the contact
 * cells. Allocation or invalid cell can throw after pointer/scratch changes; no whole-call
 * rollback. Does not initialize contacts, perform heat exchange or advance hydraulic/thermal
 * clocks.
 */
void ExplicitInertialSolver::setChannelGround(ChannelGroundState& stage) {
    if (!initialized_ || t_last_sync_ != 0 || common_stage_)
        throw std::invalid_argument(
            "Channel ground contacts require an initial CPU mesh source clock.");
    if (!stage.active())
        return;
    channel_ground_ = &stage;
    channel_sources_.resize(mesh_->n_cells());
    channel_source_ready_.assign(mesh_->n_cells(), 0);
    channel_temperature_mixed_ = channel_source_ready_;
    pinExternalSourceCells(stage.cells());
}

void ExplicitInertialSolver::setCommonSourceStage(CommonSourceStage& stage) {
    if (((state_ && state_->heat.enabled) || (gw_ && gw_->heat().active())) && !stage.meshOnly())
        throw std::invalid_argument(
            "Coupled heat with catchment/LID source ownership is not yet qualified.");
    if (!initialized_ || second_order_ || t_last_sync_ != 0.0 || stage.completedEnd() != 0.0 ||
        !stage.matches(mesh_, state_, gw_))
        throw std::invalid_argument(
            "Common source stage requires matching initial first-order mesh/receiver state.");
    if (common_stage_)
        throw std::invalid_argument("Common source stage already attached.");
    common_stage_ = &stage;
    pinExternalSourceCells(stage.cells());
}

void ExplicitInertialSolver::setSourceResultHistory(SourceResultHistory& history) {
    if (!common_stage_ || source_history_ || engine_source_history_ || engine_source_stream_ ||
        t_last_sync_ != 0.0 || !history.tables().empty())
        throw std::invalid_argument(
            "Source results require an empty recorder and initial common source stage.");
    const auto error = history.append(*common_stage_);
    if (!error.empty())
        throw std::invalid_argument(error);
    source_history_ = &history;
}
void ExplicitInertialSolver::captureSourceResults() {
    if (engine_source_stream_ && engine_source_stream_->detailed()) {
        const auto error = engine_source_stream_->captureSourceInterval(*common_stage_);
        if (!error.empty())
            throw std::runtime_error("[2D_SOURCE_STREAM] " + error);
    }
    if (engine_source_history_) {
        const auto error = engine_source_history_->captureSourceInterval(*common_stage_);
        if (!error.empty())
            throw std::runtime_error("[2D_SOURCE_RESULTS] " + error);
    }
    if (!source_history_)
        return;
    const auto error = source_history_->append(*common_stage_);
    if (!error.empty())
        throw std::runtime_error("[2D_SOURCE_RESULTS] " + error);
}
void ExplicitInertialSolver::resumeSourceResultHistory(SourceResultHistory& history) {
    const auto error = checkpointBoundary();
    if (!error.empty() || source_history_ || engine_source_history_ || engine_source_stream_)
        throw std::invalid_argument(error.empty() ? "Source history already attached." : error);
    const auto match = history.resume(*common_stage_);
    if (!match.empty())
        throw std::invalid_argument(match);
    source_history_ = &history;
}

void ExplicitInertialSolver::finalize() {
    if (initialized_ && !wall_drag_slots_.empty())
        std::fprintf(stderr, "[openswmm 2D] quadratic wall drag: %zu faces, energy removed/rho=%.17g m5/s2\n",
                     wall_drag_slots_.size(), wall_drag_energy_);
    if (initialized_ && opts_ && opts_->turbulence != Turbulence2D::NONE)
        std::fprintf(stderr, "[openswmm 2D] %s mixing: %llu substeps, %llu retries, "
                     "last diffusion bound %.17g s.\n", turbulenceName(opts_->turbulence),
                     static_cast<unsigned long long>(momentum_physics_.substeps),
                     static_cast<unsigned long long>(momentum_physics_.retries), momentum_physics_.last_limit);
    common_stage_ = nullptr;
    source_history_ = nullptr;
    engine_source_history_ = nullptr;
    engine_source_stream_ = nullptr;
    checkpoint_ready_ = false;
    joint_checkpoint_ = false;
    if (initialized_) {
        settleAccumulators();
        flushSourceLedgers();
    }
    if (!telemetry_path_.empty() && !telemetry_.empty()) {
        if (std::FILE* f = openswmm::io::fopen_utf8(telemetry_path_, "w")) {
            std::fprintf(f, "t_s,active_cells,active_frac\n");
            const double nt = std::max(1, mesh_ ? mesh_->n_triangles() : 1);
            for (const auto& [t, n] : telemetry_)
                std::fprintf(f, "%.3f,%d,%.6f\n", t, n, n / nt);
            std::fclose(f);
        }
    }
    q_.clear();
    qcx_.clear();
    qcy_.clear();
    face_accum_.clear();
    dw_slope_.clear();
    cell_active_.clear();
    active_cells_.clear();
    tier_.clear();
    face_tier_.clear();
    cells_by_tier_.clear();
    edges_by_tier_.clear();
    bc_cell_.clear();
    bc_slot_.clear();
    bc_accum_.clear();
    bc_q_.clear();
    telemetry_.clear();
    active_frac_min_ = 1.0e30;
    active_frac_max_ = -1.0e30;
    active_frac_sum_ = 0.0;
    active_samples_ = 0;
    initialized_ = false;
}

ISurfaceSolver::RunStats ExplicitInertialSolver::run_stats() const noexcept {
    RunStats s;
    s.nsteps = substeps_run_;
    s.nrhs = face_passes_;
    s.last_h = last_dt_;

    // Marcher telemetry for the report block: active-fraction spread over the
    // rebuild samples (folded as they are taken — see syncAndRebuild) +
    // cumulative tier-occupancy histogram. Must be read BEFORE finalize()
    // (which resets the statistics) — SurfaceRouter2D does.
    if (active_samples_ > 0) {
        s.active_frac_min = active_frac_min_;
        s.active_frac_max = active_frac_max_;
        s.active_frac_mean = active_frac_sum_ / static_cast<double>(active_samples_);
    }
    s.n_tiers = static_cast<int>(std::min(cells_by_tier_.size(), tier_occupancy_.size()));
    for (int k = 0; k < s.n_tiers; ++k)
        s.tier_cells[k] = tier_occupancy_[static_cast<std::size_t>(k)];
    return s;
}

} // namespace openswmm::twoD
