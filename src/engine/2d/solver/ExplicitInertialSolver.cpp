/**
 * @file ExplicitInertialSolver.cpp
 * @brief Implementation of the explicit local-inertial FV marcher with
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
 */

#include "ExplicitInertialSolver.hpp"

#include <algorithm>
#include <cmath>
#if defined(SWMM_USE_OPENMP)
#include <omp.h>
#endif
#include <cstdio>
#include <cstdlib>
#include <stdexcept>   // std::runtime_error ([2D_BOUNDARY_QUALITY] resolve)
#include <string>      // std::to_string

#include "../data/MeshData.hpp"
#include "../data/SolverOptions2D.hpp"
#include "../data/SurfaceStateData.hpp"
#include "../data/BoundaryData.hpp"
#include "InertialKernels.hpp"
#include "SweKernels.hpp"
#include "DiffusiveKernels.hpp"
#include "SurfaceFluxCalculator.hpp"   // evapSink, computeBoundaryEdgeFlux
#include "../coupling/NodeCoupling.hpp"  // computeNodeCouplingQ (live exchange)
#include "../../data/NodeData.hpp"

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
}  // namespace

void ExplicitInertialSolver::initialize(MeshData& mesh, SurfaceStateData& state,
                                        SolverOptions2D& opts) {
    mesh_  = &mesh;
    state_ = &state;
    opts_  = &opts;

    const int nt = mesh.n_triangles();
    if (nt <= 0) return;

    edges_.build(mesh);
    const auto ne = static_cast<std::size_t>(edges_.ne);
    q_.assign(ne, 0.0);
    facc_L_.assign(ne, 0.0);
    facc_R_.assign(ne, 0.0);
    face_tier_.assign(ne, 0);

    // Momentum closure (MOMENTUM_EQUATION). FULL_SWE carries cell momentum
    // accumulators and a positivity-safe CFL; DIFFUSIVE_WAVE carries the
    // per-cell slope its Δx² step bound needs.
    mode_ = opts.momentum;
    second_order_ = false;
    macc_x_L_.clear(); macc_x_R_.clear(); macc_y_L_.clear(); macc_y_R_.clear();
    dw_slope_.clear();
    if (mode_ == Momentum2D::FULL_SWE) {
        macc_x_L_.assign(ne, 0.0); macc_x_R_.assign(ne, 0.0);
        macc_y_L_.assign(ne, 0.0); macc_y_R_.assign(ne, 0.0);
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
                "LTS_TIERS %d reduced to 1.\n", opts.lts_tiers);
            opts.lts_tiers = 1;
        }
        second_order_ = (opts.reconstruction_order == 2);
        if (second_order_ && state.transport.active()) {
            std::fprintf(stderr,
                "[openswmm 2D] RECONSTRUCTION_ORDER 2 with overland transport "
                "is not supported yet; using first order.\n");
            second_order_ = false;
        }
        if (second_order_) {
            const auto un = static_cast<std::size_t>(nt);
            gex_.assign(un, 0.0); gey_.assign(un, 0.0);
            gux_.assign(un, 0.0); guy_.assign(un, 0.0);
            gvx_.assign(un, 0.0); gvy_.assign(un, 0.0);
        } else {
            gex_.clear(); gey_.clear(); gux_.clear(); guy_.clear(); gvx_.clear(); gvy_.clear();
        }
    } else if (mode_ == Momentum2D::DIFFUSIVE_WAVE) {
        dw_slope_.assign(static_cast<std::size_t>(nt), 0.0);
    }
    // FRONT_REBUILD AUTO: on for FULL_SWE only. A Godunov front travels
    // ~2√(gh) and can cross the 1-ring halo inside the 4-cycle cadence; a
    // diffusive-wave front cannot (its Δx²-bounded step moves it a fraction
    // of a cell per cycle), so DIFFUSIVE_WAVE keeps LI's cheap cadence
    // (2D_PERF_REGRESSION_DIAGNOSIS §4 fix 3).
    front_rebuild_ = (opts.front_rebuild < 0)
        ? (mode_ == Momentum2D::FULL_SWE)
        : (opts.front_rebuild != 0);
    frontier_.assign(front_rebuild_ ? static_cast<std::size_t>(nt) : 0, 0);
    front_breach_ = false;
    // Resolved once: MeshData::n_quads() is an O(n_cells) scan (F1).
    has_quads_ = mesh.n_quads() > 0;
    // FULL_SWE: WALL boundary slots per cell, CSR (F5). Slot order within a
    // cell is ascending k, the order the per-cell scan used to visit them.
    wall_ptr_.clear(); wall_slot_.clear();
    if (mode_ == Momentum2D::FULL_SWE) {
        wall_ptr_.reserve(static_cast<std::size_t>(nt) + 1);
        wall_ptr_.push_back(0);
        for (int i = 0; i < nt; ++i) {
            const int nvc = mesh.cell_vertex_count(i);
            for (int k2 = 0; k2 < nvc; ++k2) {
                if (mesh.cell_neighbour(i, k2) >= 0) continue;
                const int slot = MeshData::slot(i, k2);
                if (state.boundary &&
                    static_cast<BoundaryType>(state.boundary->edge_bc_type[slot]) !=
                        BoundaryType::WALL)
                    continue;
                wall_slot_.push_back(slot);
            }
            wall_ptr_.push_back(static_cast<int>(wall_slot_.size()));
        }
    }
    // S1: species accumulators, sized only when transport is live so a
    // hydrodynamics-only model allocates nothing here.
    if (state.transport.active()) {
        const auto ns = static_cast<std::size_t>(state.transport.n_species);
        sacc_L_.assign(ns * ne, 0.0);
        sacc_R_.assign(ns * ne, 0.0);
    } else {
        sacc_L_.clear();
        sacc_R_.clear();
    }
    // The Perot cell vectors serve the θ-blend AND the convective term, so
    // ADVECTION forces them on even at θ = 1. FULL_SWE uses the same arrays
    // as its prognostic cell momentum.
    qcx_.clear(); qcy_.clear();
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
                const auto ty =
                    static_cast<BoundaryType>(state.boundary->edge_bc_type[idx]);
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
                if (bc_slot_[k] == row.slot) { found = true; break; }
            if (!found)
                throw std::runtime_error(
                    "[2D_BOUNDARY_QUALITY] edge slot " + std::to_string(row.slot) +
                    " is not a non-WALL boundary edge, so its concentration "
                    "could never enter the mesh.");
            if (row.species < 0 || static_cast<std::size_t>(row.species) >= ns)
                throw std::runtime_error(
                    "[2D_BOUNDARY_QUALITY] species index out of range.");
            tr.bc_conc[k * ns + static_cast<std::size_t>(row.species)] = row.conc;
        }
    }

    // Live junction exchange (windowless coupling): one ∫Q dt accumulator per
    // point; spill budget tracked per 1D node. Their cells pin to tier 0 (the
    // exchange forcing changes at the fastest cadence), as do BC cells.
    exch_.assign(state.node_coupling ? state.node_coupling->size() : 0, 0.0);
    node_drawn_.assign(state.nodes_1d ? state.nodes_1d->volume.size() : 0, 0.0);
    pin_t0_.assign(static_cast<std::size_t>(nt), 0);
    for (int i : bc_cell_) pin_t0_[static_cast<std::size_t>(i)] = 1;
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
                q_[e] = 0.5 * ((qax + qbx) * edges_.nx[e] +
                               (qay + qby) * edges_.ny[e]);
            }
            // Perot gather so the θ-blend and CFL speed see the seeded
            // momentum from the very first firing (same stencil as fireCells).
            if (!qcx_.empty()) {
                const auto& ed = edges_;
                for (int i = 0; i < nt; ++i) {
                    double sx = 0.0, sy = 0.0;
                    for (int p = ed.cell_ptr[i]; p < ed.cell_ptr[i + 1]; ++p) {
                        const int    e = ed.cell_edge[p];
                        const double fq = static_cast<double>(ed.cell_sign[p]) * q_[e] * ed.xi[e];
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
    if (const char* p = std::getenv("OPENSWMM_2D_MARCHER_TELEMETRY"))
        telemetry_path_ = p;
    initialized_ = true;
}

double ExplicitInertialSolver::donorConc(int s, int cell) const noexcept {
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
    if (!(v > 0.0)) return 0.0;
    return tr.cell_mass[tr.idx(s, cell)] / v;
}

void ExplicitInertialSolver::sinkMassAtCellConc(int i, double dv_m3,
                                                std::vector<double>& ledger,
                                                double* per_point_ledger)
    noexcept {
    if (!(dv_m3 > 0.0)) return;
    auto& tr = state_->transport;
    if (!tr.active()) return;
    // Concentration is read BEFORE the volume was reduced (callers pass the
    // pre-sink volume through state_->volume unchanged until after this
    // call), so the mass removed is exactly dv * c_old — the mass the water
    // that left was carrying.
    for (int s = 0; s < tr.n_species; ++s) {
        const double c  = donorConc(s, i);
        if (c == 0.0) continue;
        const double dm = dv_m3 * c;
        double& m = tr.cell_mass[tr.idx(s, i)];
        // Cannot remove more than is there (the volume clamps identically);
        // the residual is a rounding artefact, not a modelling event. The
        // SIGNED temperature row (S4) has no floor: °C·m³ below zero is a
        // state, and "min(dm, m)" on two negatives would empty the row.
        const double taken = tr.signedRow(s) ? dm : ((dm < m) ? dm : m);
        m -= taken;
        ledger[static_cast<std::size_t>(s)] += taken;
        if (per_point_ledger) per_point_ledger[s] += taken;
    }
}

void ExplicitInertialSolver::addRainMass(int i, double rain_m3) noexcept {
    if (!(rain_m3 > 0.0)) return;
    auto& tr = state_->transport;
    if (!tr.active() || tr.rain_conc.empty()) return;
    for (int s = 0; s < tr.n_species; ++s) {
        const auto us = static_cast<std::size_t>(s);
        if (us >= tr.rain_conc.size() || tr.rain_conc[us] == 0.0) continue;
        const double g = rain_m3 * tr.rain_conc[us];
        tr.cell_mass[tr.idx(s, i)] += g;
        tr.gained_rainfall[us] += g;
    }
}

void ExplicitInertialSolver::sinkTemperatureWithEvap(int i, double evap_m3) noexcept {
    // Evaporation removes WATER and no solute (S1: concentrations rise), but
    // it removes water AT the water's temperature: the temperature row is a
    // temperature-volume, so leaving it untouched while the volume falls would
    // heat the cell by evaporating it. Sink the row at the cell's own
    // temperature; no ledger — the enthalpy left with the vapour and the 1D
    // engines book nothing for it either (H1's convention).
    if (!(evap_m3 > 0.0)) return;
    auto& tr = state_->transport;
    if (!tr.active() || tr.temp_row < 0) return;
    const double v = state_->volume[i];
    if (!(v > 0.0)) return;
    double& m = tr.cell_mass[tr.idx(tr.temp_row, i)];
    const double dv = (evap_m3 < v) ? evap_m3 : v;
    m -= dv * (m / v);
}

void ExplicitInertialSolver::addCouplingSourceMass(int i, double area_dt) noexcept {
    if (!(state_->coupling_flux[i] > 0.0) || !(area_dt > 0.0)) return;
    auto& tr = state_->transport;
    if (!tr.active() || tr.coupling_src.empty()) return;
    for (int s = 0; s < tr.n_species; ++s) {
        const double g = tr.coupling_src[tr.idx(s, i)] * area_dt;
        if (g == 0.0) continue;
        tr.cell_mass[tr.idx(s, i)] += g;
        tr.gained_coupling[static_cast<std::size_t>(s)] += g;
    }
}

void ExplicitInertialSolver::reconstructAll() {
    const int nt = mesh_->n_triangles();
#pragma omp parallel for schedule(static) num_threads(opts_->num_threads)
    for (int i = 0; i < nt; ++i) {
        inertial::cellEtaDepth(*mesh_, *opts_, i, state_->volume[i],
                               state_->head[i], state_->depth[i]);
    }
}

void ExplicitInertialSolver::settleAccumulators() {
    if (!accumulators_pending_) return;
    accumulators_pending_ = false;
    const int nt = mesh_->n_triangles();
    const auto& ed = edges_;
#pragma omp parallel for schedule(static) num_threads(opts_->num_threads)
    for (int i = 0; i < nt; ++i) {
        double pending = 0.0;
        for (int p = ed.cell_ptr[i]; p < ed.cell_ptr[i + 1]; ++p) {
            const int e = ed.cell_edge[p];
            if (ed.cell_sign[p] > 0) {
                pending += facc_L_[e];
                facc_L_[e] = 0.0;
            } else {
                pending += facc_R_[e];
                facc_R_[e] = 0.0;
            }
        }
        // S1: settle the species side in the SAME sweep. A cell re-tiered
        // or deactivated with a pending species accumulator would strand
        // mass exactly as the header warns for volume.
        if (!sacc_L_.empty()) {
            auto& tr = state_->transport;
            const auto ns  = static_cast<std::size_t>(tr.n_species);
            const auto nef = static_cast<std::size_t>(ed.ne);
            for (std::size_t s = 0; s < ns; ++s) {
                double dm = 0.0;
                for (int p = ed.cell_ptr[i]; p < ed.cell_ptr[i + 1]; ++p) {
                    const auto e = static_cast<std::size_t>(ed.cell_edge[p]);
                    if (ed.cell_sign[p] > 0) {
                        dm += sacc_L_[s * nef + e]; sacc_L_[s * nef + e] = 0.0;
                    } else {
                        dm += sacc_R_[s * nef + e]; sacc_R_[s * nef + e] = 0.0;
                    }
                }
                if (dm != 0.0) {
                    double& m = tr.cell_mass[tr.idx(static_cast<int>(s), i)];
                    m += dm;
                    // Backstop is for nonnegative rows only: the SIGNED
                    // temperature row holds °C·m³ below zero as a state.
                    if (m < 0.0 && !tr.signedRow(static_cast<int>(s))) m = 0.0;
                }
            }
        }
        // FULL_SWE: settle the MOMENTUM accumulators in the same sweep, for
        // the same reason as the species rows above — a cell re-tiered or
        // deactivated between its firings would otherwise strand the momentum
        // of mass that has already been applied, and pick it up stale on
        // reactivation.
        if (!macc_x_L_.empty()) {
            double dmx = 0.0, dmy = 0.0;
            for (int p = ed.cell_ptr[i]; p < ed.cell_ptr[i + 1]; ++p) {
                const int e = ed.cell_edge[p];
                if (ed.cell_sign[p] > 0) {
                    dmx += macc_x_L_[e]; macc_x_L_[e] = 0.0;
                    dmy += macc_y_L_[e]; macc_y_L_[e] = 0.0;
                } else {
                    dmx += macc_x_R_[e]; macc_x_R_[e] = 0.0;
                    dmy += macc_y_R_[e]; macc_y_R_[e] = 0.0;
                }
            }
            if (dmx != 0.0 || dmy != 0.0) {
                const double A = mesh_->tri_area[i];
                qcx_[i] += dmx / A;      // same normalisation as fireCells
                qcy_[i] += dmy / A;
            }
        }
        if (pending == 0.0) continue;
        double v = state_->volume[i] + pending;
        state_->volume[i] = (v > 0.0) ? v : 0.0;
        inertial::cellEtaDepth(*mesh_, *opts_, i, state_->volume[i],
                               state_->head[i], state_->depth[i]);
    }
}

void ExplicitInertialSolver::lazySourcesOnly(double t) {
    const int nt = mesh_->n_triangles();
    const double dt_lazy = t - t_last_sync_;
    if (dt_lazy <= 0.0) return;
#pragma omp parallel for schedule(static) num_threads(opts_->num_threads)
    for (int i = 0; i < nt; ++i) {
        if (cell_active_[i]) continue;
        const double infil = infilSink(state_->infil_rate[i], state_->depth[i],
                                       opts_->dry_depth);
        // Book before the early-out: rain exactly cancelling the sink leaves
        // src == 0, but water still infiltrated and the ledger counts the rain.
        state_->infil_applied[i] += infil * dt_lazy;
        const double evap =
            evapSink(state_->evap_rate[i], state_->depth[i], opts_->dry_depth);
        const double src =
            state_->rainfall[i] + state_->coupling_flux[i] - evap - infil;
        if (src == 0.0) continue;
        if (!sacc_L_.empty()) {   // S1: see syncAndRebuild's lazy pass
            auto& tr = state_->transport;
            const double area = mesh_->tri_area[i];
            sinkTemperatureWithEvap(i, evap * dt_lazy * area);          // S4
            sinkMassAtCellConc(i, infil * dt_lazy * area, tr.lost_infiltration);
            if (state_->coupling_flux[i] < 0.0)
                sinkMassAtCellConc(i, -state_->coupling_flux[i] * dt_lazy *
                                          area, tr.lost_coupling);
            addRainMass(i, state_->rainfall[i] * dt_lazy * area);   // S2
            addCouplingSourceMass(i, dt_lazy * area);                // S3
        }
        double v = state_->volume[i] + dt_lazy * src * mesh_->tri_area[i];
        state_->volume[i] = (v > 0.0) ? v : 0.0;
        inertial::cellEtaDepth(*mesh_, *opts_, i, state_->volume[i],
                               state_->head[i], state_->depth[i]);
    }
    t_last_sync_ = t;
}

void ExplicitInertialSolver::syncAndRebuild(double t) {
    settleAccumulators();
    const int nt = mesh_->n_triangles();
    const double dt_lazy = t - t_last_sync_;

    // 1. Lazy source integration on INACTIVE cells: rain + held coupling flux
    //    accumulate as pure storage (no face flux by construction).
    if (dt_lazy > 0.0) {
#pragma omp parallel for schedule(static) num_threads(opts_->num_threads)
        for (int i = 0; i < nt; ++i) {
            if (cell_active_[i]) continue;
            const double infil = infilSink(state_->infil_rate[i],
                                           state_->depth[i], opts_->dry_depth);
            state_->infil_applied[i] += infil * dt_lazy;
            const double evap = evapSink(state_->evap_rate[i],
                                         state_->depth[i], opts_->dry_depth);
            const double src =
                state_->rainfall[i] + state_->coupling_flux[i] - evap - infil;
            if (src == 0.0) continue;
            // S1: the lazy tier moves water without faces; the sinks still
            // carry the cell's species out (same rule as fireCells).
            if (!sacc_L_.empty()) {
                auto& tr = state_->transport;
                const double area = mesh_->tri_area[i];
                sinkTemperatureWithEvap(i, evap * dt_lazy * area);      // S4
                sinkMassAtCellConc(i, infil * dt_lazy * area,
                                   tr.lost_infiltration);
                if (state_->coupling_flux[i] < 0.0)
                    sinkMassAtCellConc(i, -state_->coupling_flux[i] * dt_lazy *
                                              area, tr.lost_coupling);
                addRainMass(i, state_->rainfall[i] * dt_lazy * area);   // S2
                addCouplingSourceMass(i, dt_lazy * area);                // S3
            }
            double v = state_->volume[i] + dt_lazy * src * mesh_->tri_area[i];
            state_->volume[i] = (v > 0.0) ? v : 0.0;
            inertial::cellEtaDepth(*mesh_, *opts_, i, state_->volume[i],
                                   state_->head[i], state_->depth[i]);
        }
    }
    t_last_sync_ = t;

    // 2. Seed: hysteretic depth threshold (entering cells need h_on, active
    //    cells stay until h_off), plus concentrated sources (held coupling)
    //    and non-wall boundary cells. Rain alone does NOT activate — that is
    //    the point of the lazy tier.
    //    The hysteresis band scales with H_MOVE (capped at the historical
    //    ±1 mm): a fixed ±1 mm band made H_MOVE 1e-4 require 1.1 mm to
    //    activate — 10× the requested threshold — freezing wetting/drying
    //    fronts on shallow benchmarks (Thacker). Bit-identical at the
    //    default h_move = 0.003 (band = 1 mm either way).
    const double band  = std::min(0.001, 0.5 * opts_->h_move);
    const double h_on  = opts_->h_move + band;
    h_on_front_ = h_on;
    const double h_off = std::max(0.0, opts_->h_move - band);
    rebuild_seed_.assign(static_cast<std::size_t>(nt), 0);
    std::vector<uint8_t>& next = rebuild_seed_;
#pragma omp parallel for schedule(static) num_threads(opts_->num_threads)
    for (int i = 0; i < nt; ++i) {
        const double thresh = cell_active_[i] ? h_off : h_on;
        if (state_->depth[i] >= thresh || state_->coupling_flux[i] != 0.0 ||
            pin_t0_[i])
            next[i] = 1;
    }

    // 3. One-ring halo so fronts can enter their neighbours within a rebuild
    //    period, then compact the work lists. A face flows only when BOTH
    //    incident cells are active — a one-sided face would export volume into
    //    a cell whose update never runs (measured as an 18 % basin loss); the
    //    halo guarantees the front always has an active receiving cell.
    cell_active_ = next;
    for (int e = 0; e < edges_.ne; ++e) {
        const int a = edges_.cL[e], b = edges_.cR[e];
        if (next[a] && !next[b]) cell_active_[b] = 1;
        else if (next[b] && !next[a]) cell_active_[a] = 1;
    }
    // FRONT_REBUILD: a dry-bed front travels ~2√(gh) — about one cell per
    // base substep at CFL ½ — so between two macro cycles it can cross
    // several rings. Widen the halo so the front never runs out of active
    // receiving cells before the breach-triggered rebuild lands.
    if (front_rebuild_) {
        // Cell-parallel Jacobi rings (each thread writes only its own cell,
        // reading the previous ring's copy): the same set as the serial
        // edge walk produced, without the O(n_edges) serial passes.
        for (int ring = 1; ring < kFrontHaloRings; ++ring) {
            rebuild_seed_ = cell_active_;
#pragma omp parallel for schedule(static) num_threads(opts_->num_threads)
            for (int i = 0; i < nt; ++i) {
                if (rebuild_seed_[i]) continue;
                const int nvc = mesh_->cell_vertex_count(i);
                for (int k2 = 0; k2 < nvc; ++k2) {
                    const int j = mesh_->cell_neighbour(i, k2);
                    if (j >= 0 && rebuild_seed_[j]) { cell_active_[i] = 1; break; }
                }
            }
        }
    }
    active_cells_.clear();
    for (int i = 0; i < nt; ++i)
        if (cell_active_[i]) active_cells_.push_back(i);
    if (front_rebuild_) {
        // Outer ring of the halo: an active cell with an inactive neighbour.
#pragma omp parallel for schedule(static) num_threads(opts_->num_threads)
        for (int i = 0; i < nt; ++i) {
            uint8_t f = 0;
            if (cell_active_[i]) {
                const int nvc = mesh_->cell_vertex_count(i);
                for (int k2 = 0; k2 < nvc; ++k2) {
                    const int j = mesh_->cell_neighbour(i, k2);
                    if (j >= 0 && !cell_active_[j]) { f = 1; break; }
                }
            }
            frontier_[i] = f;
        }
        front_breach_ = false;
    }

    // 4. Tier assignment from the local CFL step. Pinned to tier 0: cells with
    //    concentrated sources (coupling points) and boundary cells — their
    //    forcing changes fastest. dt0_ = the finest active requirement.
    const int K  = static_cast<int>(cells_by_tier_.size());
    const int na = static_cast<int>(active_cells_.size());
    if (mode_ == Momentum2D::DIFFUSIVE_WAVE) refreshDiffusiveSlopes();
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
            double dt = (h > opts_->dry_depth)
                            ? inertial::cellCflDt(opts_->cfl_number,
                                                  (mode_ == Momentum2D::FULL_SWE)
                                                      ? edges_.cell_lpos[i]
                                                      : edges_.cell_lchar[i],
                                                  h, speed)
                            : 1.0e30;
            if (mode_ == Momentum2D::DIFFUSIVE_WAVE && h > opts_->dry_depth)
                dt = diffusive::cellDiffusiveDt(
                    opts_->cfl_number, edges_.cell_lchar[i], h,
                    mesh_->mannings_n[i], dw_slope_[i],
                    opts_->flux_dh_eps / std::max(edges_.cell_lchar[i], 1.0e-9));
            dt = std::min(dt, opts_->max_timestep);
            dt_cell[static_cast<std::size_t>(k)] = dt;
            if (dt < local) local = dt;
        }
        rebuild_dt_partial_[static_cast<std::size_t>(tid)] = local;
    }
    for (double v : rebuild_dt_partial_) dt0_ = std::min(dt0_, v);
    if (dt0_ >= 1.0e30) dt0_ = opts_->max_timestep;   // fully quiescent

    for (auto& v : cells_by_tier_) v.clear();
    for (auto& v : edges_by_tier_) v.clear();
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
            if (state_->coupling_flux[i] != 0.0 || pin_t0_[i]) tk = 0;
        }
        tier_[i] = static_cast<uint8_t>(tk);
        if (!front_rebuild_)
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
                if (!cell_active_[a] || !cell_active_[b]) continue;
                const bool dry_a = state_->depth[a] <= opts_->dry_depth;
                const bool dry_b = state_->depth[b] <= opts_->dry_depth;
                if (dry_a && tier_[a] > tier_[b]) { tier_[a] = tier_[b]; changed = true; }
                if (dry_b && tier_[b] > tier_[a]) { tier_[b] = tier_[a]; changed = true; }
            }
            if (!changed) break;
        }
        for (const int i : active_cells_)
            cells_by_tier_[static_cast<std::size_t>(tier_[i])].push_back(i);
    }

    active_faces_.clear();
    for (int e = 0; e < edges_.ne; ++e) {
        const int a = edges_.cL[e], b = edges_.cR[e];
        if (cell_active_[a] && cell_active_[b]) {
            const auto ft = std::min(tier_[a], tier_[b]);
            face_tier_[e] = ft;
            edges_by_tier_[ft].push_back(e);
            active_faces_.push_back(e);
        } else {
            q_[e] = 0.0;   // walled faces carry no stale momentum
        }
    }

    for (std::size_t tk = 0;
         tk < cells_by_tier_.size() && tk < tier_occupancy_.size(); ++tk)
        tier_occupancy_[tk] += static_cast<long>(cells_by_tier_[tk].size());

    telemetry_.emplace_back(t, static_cast<int>(active_cells_.size()));
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
    if (na == 0) return;
    if (mode_ == Momentum2D::DIFFUSIVE_WAVE) refreshDiffusiveSlopes();
    double fresh = 1.0e30;
#pragma omp parallel num_threads(opts_->num_threads)
    {
        // Manual min-reduction: MSVC's default /openmp is OpenMP 2.0, which
        // lacks reduction(min:).
        double local = 1.0e30;
#pragma omp for schedule(static) nowait
        for (int k = 0; k < na; ++k) {
            const int i = active_cells_[static_cast<std::size_t>(k)];
            const double h = state_->depth[i];
            if (h <= opts_->dry_depth) continue;
            double speed = 0.0;
            if (!qcx_.empty() && h > 1.0e-6)
                speed = inertial::qMagnitude(qcx_[i], qcy_[i]) / h;
            double dt = inertial::cellCflDt(opts_->cfl_number,
                                            (mode_ == Momentum2D::FULL_SWE)
                                                ? edges_.cell_lpos[i]
                                                : edges_.cell_lchar[i],
                                            h, speed);
            if (mode_ == Momentum2D::DIFFUSIVE_WAVE)
                dt = diffusive::cellDiffusiveDt(
                    opts_->cfl_number, edges_.cell_lchar[i], h,
                    mesh_->mannings_n[i], dw_slope_[i],
                    opts_->flux_dh_eps / std::max(edges_.cell_lchar[i], 1.0e-9));
            if (dt < local) local = dt;
        }
#pragma omp critical
        {
            if (local < fresh) fresh = local;
        }
    }
    if (fresh < dt0_) dt0_ = fresh;
}

void ExplicitInertialSolver::fireFaces(const std::vector<int>& faces,
                                       double dt_f, bool global_step) {
    switch (mode_) {
        case Momentum2D::FULL_SWE:       fireFacesSwe(faces, dt_f, global_step); return;
        case Momentum2D::DIFFUSIVE_WAVE: fireFacesDiffusive(faces, dt_f, global_step); return;
        case Momentum2D::LOCAL_INERTIAL: break;
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
            const double sl = std::fabs((state_->head[ed.cR[e]] - state_->head[ed.cL[e]]) *
                                        ed.inv_dx_normal[e]);
            if (sl > smax) smax = sl;
        }
        dw_slope_[i] = smax;
    }
}

void ExplicitInertialSolver::fireFacesInertial(const std::vector<int>& faces,
                                               double dt_f, bool global_step) {
    const auto& ed = edges_;
    const int   na = static_cast<int>(faces.size());
    const double theta = opts_->theta;
    // Positivity share per face: a cell's ≤ nv outgoing faces may take at most
    // β·V in total, so each takes β/nv of its EXPORTER's volume. Triangle
    // meshes keep the historical β/3 exactly.
    const double beta3 = opts_->exchange_beta / 3.0;
    const double beta4 = opts_->exchange_beta / 4.0;
    // All-triangle meshes skip the per-face cell_nv load entirely (resolved
    // once in initialize(): the scan is O(n_cells)).
    const bool   has_quads = has_quads_;
    const bool   species   = !sacc_L_.empty();
    // VFR_FACE: block/convey at the shared edge's TRUE crest via the B&S
    // Eq. 14 wetted-edge depth; MEAN keeps the centroid zface bit-identical.
    const bool vfr_face =
        (opts_->face_reconstruction == FaceDepth2D::VFR_FACE);

#pragma omp parallel for schedule(static) num_threads(opts_->num_threads)
    for (int k = 0; k < na; ++k) {
        const int e = faces[static_cast<std::size_t>(k)];
        const int a = ed.cL[e], b = ed.cR[e];
        const double hf = vfr_face
            ? inertial::faceFlowDepthVfr(state_->head[a], state_->head[b],
                                         ed.ze_lo[e], ed.ze_hi[e])
            : inertial::faceFlowDepth(state_->head[a],
                                      state_->head[b], ed.zface[e]);
        if (hf <= opts_->dry_depth) {
            q_[e] = 0.0;
            continue;
        }
        double qhat  = q_[e];
        double q_mag = std::fabs(q_[e]);
        if (!qcx_.empty()) {
            const double qfx = 0.5 * (qcx_[a] + qcx_[b]);
            const double qfy = 0.5 * (qcy_[a] + qcy_[b]);
            const double qn  = qfx * ed.nx[e] + qfy * ed.ny[e];
            qhat  = theta * q_[e] + (1.0 - theta) * qn;
            // Friction magnitude: the face flow VECTOR, floored at |q_n| so
            // a face whose reconstruction lags its own discharge (front
            // arrival, first firing after activation) never under-damps.
            q_mag = std::max(q_mag, inertial::qMagnitude(qfx, qfy));
        }
        double deta = state_->head[b] - state_->head[a];
        if (std::fabs(deta) < inertial::kEtaDeadband) deta = 0.0;
        const double slope = deta * ed.inv_dx_normal[e];
        // Convective momentum flux (ADVECTION, opt-in): both cells must be
        // wet — a wet/dry front keeps the pure local-inertial law, whose
        // robustness there is the reason this scheme exists.
        double adv = 0.0;
        if (opts_->advection && !qcx_.empty()) {
            const double hL = state_->depth[a], hR = state_->depth[b];
            if (hL > opts_->dry_depth && hR > opts_->dry_depth) {
                const double unL = (qcx_[a] * ed.nx[e] + qcy_[a] * ed.ny[e]) / hL;
                const double unR = (qcx_[b] * ed.nx[e] + qcy_[b] * ed.ny[e]) / hR;
                adv = inertial::inertialAdvection(q_[e], unL, hL, unR, hR,
                                                  ed.inv_dx_normal[e]);
            }
        }
        double qn1 = inertial::inertialFaceUpdate(q_[e], qhat, hf, dt_f, slope,
                                                 ed.n2_face[e], q_mag, adv);
        qn1 = inertial::froudeCap(qn1, hf, opts_->froude_max);

        // Positivity at face cadence: this face may take at most a β/3 share
        // of its exporting cell's volume over the exporter's WHOLE cell cycle.
        // The exporter republishes V only at its own firings, and a finer face
        // fires 2^(k_exp − t_face) times in between — divide the share by that
        // ratio or the repeated takes drain the cell into the backstop
        // (measured: a dam-break basin discarded to exactly zero).
        const int    exp_cell = (qn1 > 0.0) ? a : b;
        const int    refire   =
            global_step ? 1 : (1 << (tier_[exp_cell] - face_tier_[e]));
        const double beta_share =
            (has_quads && mesh_->cell_vertex_count(exp_cell) == 4) ? beta4 : beta3;
        const double budget   = beta_share / refire *
                                std::max(state_->volume[exp_cell], 0.0);
        const double take = std::fabs(qn1) * ed.xi[e] * dt_f;
        if (take > budget)
            qn1 *= (take > 0.0) ? budget / take : 0.0;
        q_[e] = qn1;

        // Book the identical ±ΔM on both sides (single writer per face).
        const double dM = qn1 * ed.xi[e] * dt_f;   // positive = cL→cR
        facc_L_[e] -= dM;
        facc_R_[e] += dM;

        if (species) bookFaceSpecies(e, a, b, dM, hf, dt_f, global_step);
    }
    face_passes_ += na;
    accumulators_pending_ = true;
}



// ---------------------------------------------------------------------------
// MOMENTUM_EQUATION FULL_SWE — Godunov face flux (SweKernels.hpp).
// ---------------------------------------------------------------------------
void ExplicitInertialSolver::fireFacesSwe(const std::vector<int>& faces,
                                          double dt_f, bool global_step) {
    const auto& ed = edges_;
    const int   na = static_cast<int>(faces.size());
    const double beta3 = opts_->exchange_beta / 3.0;
    const double beta4 = opts_->exchange_beta / 4.0;
    const double dry   = opts_->dry_depth;

    static const bool muscl_off = std::getenv("OPENSWMM_2D_MUSCL_OFF") != nullptr;
    if (second_order_) computeLimitedGradientsSwe();
    const bool so = second_order_ && !muscl_off;
    const bool species = !sacc_L_.empty();
    const bool has_quads = has_quads_;

#pragma omp parallel for schedule(static) num_threads(opts_->num_threads)
    for (int k = 0; k < na; ++k) {
        const int e = faces[static_cast<std::size_t>(k)];
        const int a = ed.cL[e], b = ed.cR[e];
        swe::FaceFlux F;
        double cLx, cLy, cRx, cRy;
        bool wet;
        if (so) {
            // MUSCL: extrapolate (η, u, v) from each centroid to the face
            // midpoint along the precomputed Perot arms; the bed stays
            // piecewise constant per cell.
            const double ha = state_->depth[a], hb = state_->depth[b];
            const double za = state_->head[a] - ha, zb = state_->head[b] - hb;
            double axa = 0.0, aya = 0.0, axb = 0.0, ayb = 0.0;
            for (int p = ed.cell_ptr[a]; p < ed.cell_ptr[a + 1]; ++p)
                if (ed.cell_edge[p] == e) { axa = ed.cell_arm_x[p]; aya = ed.cell_arm_y[p]; break; }
            for (int p = ed.cell_ptr[b]; p < ed.cell_ptr[b + 1]; ++p)
                if (ed.cell_edge[p] == e) { axb = ed.cell_arm_x[p]; ayb = ed.cell_arm_y[p]; break; }
            const double ua = (ha > dry) ? qcx_[a] / ha : 0.0, va = (ha > dry) ? qcy_[a] / ha : 0.0;
            const double ub = (hb > dry) ? qcx_[b] / hb : 0.0, vb = (hb > dry) ? qcy_[b] / hb : 0.0;
            const double etaLf = state_->head[a] + gex_[a] * axa + gey_[a] * aya;
            const double etaRf = state_->head[b] + gex_[b] * axb + gey_[b] * ayb;
            const double uLf = ua + gux_[a] * axa + guy_[a] * aya;
            const double vLf = va + gvx_[a] * axa + gvy_[a] * aya;
            const double uRf = ub + gux_[b] * axb + guy_[b] * ayb;
            const double vRf = vb + gvx_[b] * axb + gvy_[b] * ayb;
            wet = swe::faceFluxRecon(etaLf, uLf, vLf, za, ha,
                                     etaRf, uRf, vRf, zb, hb,
                                     ed.nx[e], ed.ny[e], dry, F, cLx, cLy, cRx, cRy);
        } else {
            wet = swe::faceFlux(
                state_->head[a], state_->depth[a], qcx_[a], qcy_[a],
                state_->head[b], state_->depth[b], qcx_[b], qcy_[b],
                ed.nx[e], ed.ny[e], dry, F, cLx, cLy, cRx, cRy);
        }
        // Dry-dry face: no flux, no correction (faceFlux zeroes both), so
        // nothing to book — the accumulator read-modify-writes of zero and
        // the conveyance gather are skipped (F4). Species dispersion still
        // gets its call.
        if (!wet) {
            q_[e] = 0.0;
            if (species) {
                const double hf0 = std::max(state_->head[a], state_->head[b]) - ed.zface[e];
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
            const int    exp_cell = (fh > 0.0) ? a : b;
            const int    refire   =
                global_step ? 1 : (1 << (tier_[exp_cell] - face_tier_[e]));
            const double beta_share =
                (has_quads && mesh_->cell_vertex_count(exp_cell) == 4) ? beta4 : beta3;
            const double budget = beta_share / refire *
                                  std::max(state_->volume[exp_cell], 0.0);
            const double take = std::fabs(fh) * ed.xi[e] * dt_f;
            if (take > budget) {
                const double lam = (take > 0.0) ? budget / take : 0.0;
                fh *= lam; fmx *= lam; fmy *= lam;
            }
        }
        q_[e] = fh;
        const double xdt = ed.xi[e] * dt_f;
        const double dM  = fh * xdt;                       // m³, + = cL→cR
        facc_L_[e] -= dM;
        facc_R_[e] += dM;
        // Momentum: −flux + bed-slope correction on the exporter side, +flux
        // + correction on the receiver side (units m⁴/s).
        macc_x_L_[e] += (-fmx + cLx) * xdt;
        macc_y_L_[e] += (-fmy + cLy) * xdt;
        macc_x_R_[e] += ( fmx + cRx) * xdt;
        macc_y_R_[e] += ( fmy + cRy) * xdt;

        // Species ride the mass flux at the face depth the inertial law
        // would have used (dispersion conductance only).
        if (species) {
            const double hf = std::max(state_->head[a], state_->head[b]) - ed.zface[e];
            bookFaceSpecies(e, a, b, dM, (hf > 0.0) ? hf : 0.0, dt_f, global_step);
        }
    }
    face_passes_ += na;
    accumulators_pending_ = true;
}

// ---------------------------------------------------------------------------
// MOMENTUM_EQUATION DIFFUSIVE_WAVE — Manning quasi-steady face flux.
// ---------------------------------------------------------------------------
void ExplicitInertialSolver::fireFacesDiffusive(const std::vector<int>& faces,
                                                double dt_f, bool global_step) {
    const auto& ed = edges_;
    const int   na = static_cast<int>(faces.size());
    const double beta3 = opts_->exchange_beta / 3.0;
    const double beta4 = opts_->exchange_beta / 4.0;
    const bool vfr_face =
        (opts_->face_reconstruction == FaceDepth2D::VFR_FACE);
    const bool species   = !sacc_L_.empty();
    const bool has_quads = has_quads_;

#pragma omp parallel for schedule(static) num_threads(opts_->num_threads)
    for (int k = 0; k < na; ++k) {
        const int e = faces[static_cast<std::size_t>(k)];
        const int a = ed.cL[e], b = ed.cR[e];
        const double hf = vfr_face
            ? inertial::faceFlowDepthVfr(state_->head[a], state_->head[b],
                                         ed.ze_lo[e], ed.ze_hi[e])
            : inertial::faceFlowDepth(state_->head[a], state_->head[b], ed.zface[e]);
        if (hf <= opts_->dry_depth) { q_[e] = 0.0; continue; }
        double deta = state_->head[b] - state_->head[a];
        if (std::fabs(deta) < inertial::kEtaDeadband) deta = 0.0;
        const double slope = deta * ed.inv_dx_normal[e];
        const double s_eps = opts_->flux_dh_eps * ed.inv_dx_normal[e];
        // n_face = sqrt(n2_face) precomputed per face (F7).
        double qn1 = diffusive::faceDischarge(hf, slope, ed.n_face[e], s_eps);
        qn1 *= mesh_->edge_conveyance[ed.slotL[e]];

        // Same positivity share as the other closures.
        const int    exp_cell = (qn1 > 0.0) ? a : b;
        const int    refire   =
            global_step ? 1 : (1 << (tier_[exp_cell] - face_tier_[e]));
        const double beta_share =
            (has_quads && mesh_->cell_vertex_count(exp_cell) == 4) ? beta4 : beta3;
        const double budget = beta_share / refire *
                              std::max(state_->volume[exp_cell], 0.0);
        const double take = std::fabs(qn1) * ed.xi[e] * dt_f;
        if (take > budget) qn1 *= (take > 0.0) ? budget / take : 0.0;
        q_[e] = qn1;
        const double dM = qn1 * ed.xi[e] * dt_f;
        facc_L_[e] -= dM;
        facc_R_[e] += dM;
        if (species) bookFaceSpecies(e, a, b, dM, hf, dt_f, global_step);
    }
    face_passes_ += na;
    accumulators_pending_ = true;
}

void ExplicitInertialSolver::bookFaceSpecies(int e, int a, int b, double dM,
                                             double hf, double dt_f,
                                             bool global_step) noexcept {
    // Hydrodynamics-only models (no species) have nothing to book: return
    // before the divisions below — this runs once per face evaluation.
    if (sacc_L_.empty()) return;
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
    if (!sacc_L_.empty() && dM != 0.0) {
        const int   donor = (dM > 0.0) ? a : b;
        const auto  ns    = static_cast<std::size_t>(
            state_->transport.n_species);
        const auto  ue    = static_cast<std::size_t>(e);
        const auto  nef   = static_cast<std::size_t>(ed.ne);
        for (std::size_t s = 0; s < ns; ++s) {
            const double c = donorConc(static_cast<int>(s), donor);
            if (c == 0.0) continue;
            const double dMs = dM * c;
            sacc_L_[s * nef + ue] -= dMs;
            sacc_R_[s * nef + ue] += dMs;
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
    if (!sacc_L_.empty() && opts_->dispersion > 0.0) {
        const double va = state_->volume[a], vb = state_->volume[b];
        if (va > 0.0 && vb > 0.0) {
            const double cond =
                opts_->dispersion * hf * ed.xi[e] * ed.inv_dx_normal[e] *
                dt_f;                                  // m³ exchanged
            const auto  ns  = static_cast<std::size_t>(
                state_->transport.n_species);
            const auto  ue  = static_cast<std::size_t>(e);
            const auto  nef = static_cast<std::size_t>(ed.ne);
            auto& tr = state_->transport;
            for (std::size_t s = 0; s < ns; ++s) {
                const double ma = tr.cell_mass[tr.idx(static_cast<int>(s), a)];
                const double mb = tr.cell_mass[tr.idx(static_cast<int>(s), b)];
                const double ca = ma / va, cb = mb / vb;
                double dMd = cond * (ca - cb);          // + = a → b
                if (dMd == 0.0) continue;
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
                const int    giver    = (dMd > 0.0) ? a : b;
                const int    receiver = (dMd > 0.0) ? b : a;
                const double eq = (ca - cb) * va * vb / (va + vb) /
                    ((mesh_->cell_vertex_count(receiver) == 4) ? 4.0 : 3.0);
                // β share of the GIVER's mass, divided by the refire
                // ratio exactly as the volume share is.
                const int    refire = global_step
                    ? 1 : (1 << (tier_[giver] - face_tier_[e]));
                // The giver-mass share is a POSITIVITY guard: give at
                // most a β share of what is there. The SIGNED temperature
                // row (S4) has no positivity to guard — its mass crosses
                // zero at a warm/cold front, where a mass share would
                // vanish and bind every smoothing exchange. For it the
                // equalisation bound alone is the cap: it is what
                // enforces the pairwise max principle in both directions.
                const double share  =
                    ((mesh_->cell_vertex_count(giver) == 4) ? beta4 : beta3) /
                    refire * std::fabs((dMd > 0.0) ? ma : mb);
                const double cap = tr.signedRow(static_cast<int>(s))
                    ? std::fabs(eq)
                    : std::min(std::fabs(eq), share);
                if (std::fabs(dMd) > cap) {
                    dMd = (dMd > 0.0) ? cap : -cap;
                    #pragma omp atomic
                    ++tr.dispersion_limiter_binds;
                }
                sacc_L_[s * nef + ue] -= dMd;
                sacc_R_[s * nef + ue] += dMd;
            }
        }
    }
}

// The cell kernel is a template on the closure so the LOCAL_INERTIAL /
// DIFFUSIVE_WAVE instantiation contains none of the FULL_SWE momentum code
// (measured: the merged body cost the LI cell pass +17 % — register pressure
// and dead branches inside the OpenMP-outlined loop; the LI instantiation is
// the pre-FULL_SWE kernel again). Dispatch is one branch per firing.
void ExplicitInertialSolver::fireCells(const std::vector<int>& cells,
                                       double dt_c, bool tier0) {
    if (mode_ == Momentum2D::FULL_SWE) fireCellsImpl<true>(cells, dt_c, tier0);
    else                               fireCellsImpl<false>(cells, dt_c, tier0);
}

template <bool kSwe>
void ExplicitInertialSolver::fireCellsImpl(const std::vector<int>& cells,
                                           double dt_c, bool tier0) {
    const auto& ed = edges_;
    const int   nc = static_cast<int>(cells.size());
    // Loop-invariant closure flags, resolved outside the parallel region so
    // the outlined body carries them as firstprivate constants instead of
    // re-reading members through `this` on every cell.
    constexpr bool swe = kSwe;
    const bool   perot   = !qcx_.empty() && !swe;
    const bool   species = !sacc_L_.empty();
    const bool   front   = front_rebuild_;
    const double dry     = opts_->dry_depth;

#pragma omp parallel for schedule(static) num_threads(opts_->num_threads)
    for (int k = 0; k < nc; ++k) {
        const int i = cells[static_cast<std::size_t>(k)];
        // ONE walk of this cell's CSR row: gather + clear its side of every
        // incident face accumulator, and (when the Perot reconstruction is
        // live) accumulate its discharge vector from the same face ids. The
        // two loops used to load cell_edge/cell_sign twice for every incident
        // face; the arms (m_e - c_i) are precomputed per CSR entry so the
        // midpoint gather and the two subtractions are gone as well.
        double flux_m3 = 0.0, sx = 0.0, sy = 0.0;
        double dmx = 0.0, dmy = 0.0;          // FULL_SWE momentum gather (m⁴/s)
        for (int p = ed.cell_ptr[i]; p < ed.cell_ptr[i + 1]; ++p) {
            const int e = ed.cell_edge[p];
            if (ed.cell_sign[p] > 0) {
                flux_m3 += facc_L_[e];
                facc_L_[e] = 0.0;
                if constexpr (swe) {
                    dmx += macc_x_L_[e]; macc_x_L_[e] = 0.0;
                    dmy += macc_y_L_[e]; macc_y_L_[e] = 0.0;
                }
            } else {
                flux_m3 += facc_R_[e];
                facc_R_[e] = 0.0;
                if constexpr (swe) {
                    dmx += macc_x_R_[e]; macc_x_R_[e] = 0.0;
                    dmy += macc_y_R_[e]; macc_y_R_[e] = 0.0;
                }
            }
            if (perot) {
                const double f =
                    static_cast<double>(ed.cell_sign[p]) * q_[e] * ed.xi[e];
                sx += f * ed.cell_arm_x[p];
                sy += f * ed.cell_arm_y[p];
            }
        }
        const double infil =
            infilSink(state_->infil_rate[i], state_->depth[i], dry);
        state_->infil_applied[i] += infil * dt_c;
        const double evap =
            evapSink(state_->evap_rate[i], state_->depth[i], dry);
        const double src =
            state_->rainfall[i] + state_->coupling_flux[i] - evap - infil;

        // S1 species. ORDER MATTERS and is the same as the faces': sinks
        // read this cell's concentration against its PUBLISHED volume
        // (before the update below), then the face gather lands, then the
        // volume moves. Infiltration and a negative (2D→1D) held coupling
        // flux leave at the cell's concentration; rainfall and a positive
        // coupling flux arrive at ZERO concentration in S1 (S2/S3 own their
        // concentrations); evaporation removes no mass at all, so the
        // concentration rises — the up-concentration the plan's §2.3 wants
        // right from the start.
        if (species) {
            auto& tr = state_->transport;
            const double area = mesh_->tri_area[i];
            sinkTemperatureWithEvap(i, evap * dt_c * area);              // S4
            sinkMassAtCellConc(i, infil * dt_c * area, tr.lost_infiltration);
            if (state_->coupling_flux[i] < 0.0)
                sinkMassAtCellConc(i, -state_->coupling_flux[i] * dt_c * area,
                                   tr.lost_coupling);
            // S2: rainfall arrives at the [POLLUTANTS] rain concentration.
            // Volume × concentration, booked to the gained ledger so the
            // continuity statement (S1 total − sources) still closes.
            const double rain_m3 = state_->rainfall[i] * dt_c * area;
            const auto ns  = static_cast<std::size_t>(tr.n_species);
            const auto nef = static_cast<std::size_t>(ed.ne);
            for (std::size_t s = 0; s < ns; ++s) {
                double dm = 0.0;
                if (rain_m3 > 0.0 && s < tr.rain_conc.size() &&
                    tr.rain_conc[s] != 0.0) {
                    const double gained = rain_m3 * tr.rain_conc[s];
                    dm += gained;
                    tr.gained_rainfall[s] += gained;
                }
                // S3: outfall discharge (positive coupling_flux) carries the
                // outfall's concentration as a mass-rate density.
                if (!tr.coupling_src.empty() && state_->coupling_flux[i] > 0.0) {
                    const double g = tr.coupling_src[tr.idx(static_cast<int>(s), i)] *
                                     dt_c * area;
                    if (g != 0.0) { dm += g; tr.gained_coupling[s] += g; }
                }
                for (int p = ed.cell_ptr[i]; p < ed.cell_ptr[i + 1]; ++p) {
                    const auto e = static_cast<std::size_t>(ed.cell_edge[p]);
                    if (ed.cell_sign[p] > 0) {
                        dm += sacc_L_[s * nef + e];
                        sacc_L_[s * nef + e] = 0.0;
                    } else {
                        dm += sacc_R_[s * nef + e];
                        sacc_R_[s * nef + e] = 0.0;
                    }
                }
                double& m = tr.cell_mass[tr.idx(static_cast<int>(s), i)];
                m += dm;
                // The same backstop the volume has, for the same reason: the
                // face caps make a deficit ~impossible, and a −1 ulp of mass
                // must not become a negative concentration in a report.
                // Nonnegative rows only — the SIGNED temperature row holds
                // °C·m³ below zero as a state, not an artefact.
                if (m < 0.0 && !tr.signedRow(static_cast<int>(s))) m = 0.0;
            }
        }

        double v = state_->volume[i] + flux_m3 +
                   dt_c * src * mesh_->tri_area[i];
#ifndef NDEBUG
        if (v < -1.0e-12) {
            static thread_local bool warned = false;
            if (!warned && std::getenv("OPENSWMM_2D_MARCHER_CHECK")) {
                std::fprintf(stderr,
                             "[marcher-check] cell %d tier %d clamped v=%.6e "
                             "(V=%.6e flux=%.6e)\n",
                             i, static_cast<int>(tier_[i]), v,
                             state_->volume[i], flux_m3);
                warned = true;
            }
        }
#endif
        state_->volume[i] = (v > 0.0) ? v : 0.0;   // backstop; the face caps
                                                   // make deficits ~impossible
        inertial::cellEtaDepth(*mesh_, *opts_, i, state_->volume[i],
                               state_->head[i], state_->depth[i]);
        // FRONT_REBUILD: the wetting front reached the halo's outer ring.
        if (front && !front_breach_ && frontier_[i] &&
            state_->depth[i] >= h_on_front_)
            front_breach_ = true;   // benign race: any writer sets true
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
            // WALL boundary faces (every boundary slot without a non-WALL
            // type) close the pressure balance: the mirror-state Riemann
            // flux carries no mass but ½g·h² of normal momentum, without
            // which a cell at rest beside a wall would accelerate away from
            // it. Booked here at the cell's own cadence (the non-WALL types
            // fire at tier 0 through boundaryFluxSwe).
            if (h_new > dry) {
                // WALL slots come from the CSR built in initialize() (F5):
                // interior cells have an empty row instead of a neighbour
                // + boundary-type scan on every firing.
                for (int w = wall_ptr_[i]; w < wall_ptr_[i + 1]; ++w) {
                    const int slot = wall_slot_[static_cast<std::size_t>(w)];
                    const double nx = mesh_->edge_nx[slot], ny = mesh_->edge_ny[slot];
                    const double ux = qcx_[i] / h_new, uy = qcy_[i] / h_new;
                    const double un = ux * nx + uy * ny, ut = -ux * ny + uy * nx;
                    // Mirror ghost: same depth, reversed normal velocity.
                    const double qxg = h_new * (-un * nx - ut * ny);
                    const double qyg = h_new * (-un * ny + ut * nx);
                    swe::FaceFlux F;
                    double cLx, cLy, cRx, cRy;
                    if (swe::faceFlux(state_->head[i], h_new, qcx_[i], qcy_[i],
                                      state_->head[i], h_new, qxg, qyg,
                                      nx, ny, dry, F,
                                      cLx, cLy, cRx, cRy)) {
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
                qcx_[i] = qx; qcy_[i] = qy;
            } else {
                qcx_[i] = 0.0; qcy_[i] = 0.0;
            }
        }
    }

    // Boundary edges owned by cells of this firing (serial: perimeter-sized).
    const bool vfr_face_bc =
        (opts_->face_reconstruction == FaceDepth2D::VFR_FACE);
    for (std::size_t k = 0; k < bc_cell_.size(); ++k) {
        const int i = bc_cell_[k];
        if (tier_[i] != 0 || !cell_active_[i]) continue;
        // BC cells are pinned to tier 0, so they fire with every tier-0 list;
        // guard against double-firing when called for other tiers.
        if (!tier0) continue;
        const int    idx = bc_slot_[k];
        const auto   bt  = static_cast<BoundaryType>(
            state_->boundary->edge_bc_type[idx]);
        const double L = mesh_->edge_length[idx];
        double f;
        double f_requested_swe = 0.0;
        if (mode_ == Momentum2D::FULL_SWE) {
            // Ghost-cell Riemann boundary: mass AND momentum, no clamp
            // beyond v ≥ 0 (the Riemann flux is bounded by the states).
            swe_bc_dqx_ = swe_bc_dqy_ = 0.0;
            f = boundaryFluxSwe(k, i, dt_c);
            f_requested_swe = f;
        } else if (mode_ == Momentum2D::DIFFUSIVE_WAVE) {
            // The diffusive-wave boundary law for every type (collapsed
            // Manning conductance against the prescribed stage).
            f = computeBoundaryEdgeFlux(*mesh_, *state_, *opts_,
                                        opts_->flux_dh_eps, i, idx);
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
                const double za  = mesh_->vz[va_e];
                const double zb  = mesh_->vz[vb_e];
                hf = inertial::faceDepthFromEta(
                    std::max(state_->head[i], eta_bc),
                    std::min(za, zb), std::max(za, zb));
            } else {
                hf = inertial::faceFlowDepth(state_->head[i], eta_bc,
                                             mesh_->tri_cz[i]);
            }
            if (hf <= opts_->dry_depth) {
                bc_q_[k] = 0.0;
                continue;
            }
            double deta = state_->head[i] - eta_bc;
            if (std::fabs(deta) < inertial::kEtaDeadband) deta = 0.0;
            // The ghost sits across the boundary edge at the centroid→edge
            // normal distance: 2A/(3L) for a triangle (expression retained
            // verbatim for bit-identity), edge_dist_c for a quad.
            const double slope = (mesh_->cell_vertex_count(i) == 3)
                ? deta * (3.0 * L) / (2.0 * mesh_->tri_area[i])
                : deta / mesh_->edge_dist_c[idx];
            const double n     = mesh_->mannings_n[i];
            double qn1 = inertial::inertialFaceUpdate(
                bc_q_[k], bc_q_[k], hf, dt_c, slope, n * n,
                std::fabs(bc_q_[k]));
            qn1 = inertial::froudeCap(qn1, hf, opts_->froude_max);
            f = qn1 * L;   // inflow-positive
        } else {
            f = computeBoundaryEdgeFlux(*mesh_, *state_, *opts_,
                                        opts_->flux_dh_eps, i, idx);
        }
        if (f == 0.0) {
            bc_q_[k] = 0.0;
            continue;
        }
        // Clamp the exchange in VOLUME space and re-derive the booked flux
        // from the applied change, so booking matches application exactly
        // (no −1 ulp volume dust from the flux-space clamp).
        const double v_old = state_->volume[i];
        double v_new = v_old + dt_c * f;
        if (bt == BoundaryType::SPECIFIED_STAGE && mode_ != Momentum2D::FULL_SWE) {
            // Equilibrium clamp, kept as the tiny-cell / overshoot backstop:
            // one substep moves the cell AT MOST to the prescribed stage. At
            // the inertial law's gravity-scale takes it almost never binds.
            const double v_eq = inertial::cellVolumeFromEta(
                *mesh_, *opts_, i, state_->boundary->edge_bc_head[idx]);
            if (f < 0.0) v_new = std::max(v_new, std::min(v_old, v_eq));
            else         v_new = std::min(v_new, std::max(v_old, v_eq));
        }
        if (v_new < 0.0) v_new = 0.0;   // availability clamp (exact floor)
        f = (v_new - v_old) / dt_c;
        // FULL_SWE booked its momentum inside boundaryFluxSwe against the
        // REQUESTED mass; when the clamp above shrank that mass, take the
        // same fraction of the momentum back (what the comment below has
        // always promised, and what the interior positivity cap does).
        if (mode_ == Momentum2D::FULL_SWE && f != f_requested_swe &&
            (swe_bc_dqx_ != 0.0 || swe_bc_dqy_ != 0.0))
        {
            const double lam = (f_requested_swe != 0.0) ? f / f_requested_swe : 0.0;
            qcx_[i] += swe_bc_dqx_ * (lam - 1.0);
            qcy_[i] += swe_bc_dqy_ * (lam - 1.0);
        }
        // Momentum matches applied mass (mirrors the interior positivity-cap
        // rescale of qn1) — the prescribed-flux types record theirs here too.
        bc_q_[k] = (L > 1.0e-12) ? f / L : 0.0;
        if (f != 0.0) {
            // S1: an OUTFLOW boundary carries the cell's concentration out;
            // an inflow boundary brings water at zero concentration until S2
            // gives edges a species column. Applied volume, not requested,
            // so the species booking matches the water booking exactly.
            if (!sacc_L_.empty()) {
                auto& tr = state_->transport;
                if (v_new < v_old) {
                    sinkMassAtCellConc(i, v_old - v_new, tr.lost_boundary);
                } else {
                    // S2: INFLOW carries the edge's [2D_BOUNDARY_QUALITY]
                    // concentration (0 when none was given — clean water).
                    const auto ns = static_cast<std::size_t>(tr.n_species);
                    if (k * ns + ns <= tr.bc_conc.size()) {
                        const double dv = v_new - v_old;
                        for (std::size_t s = 0; s < ns; ++s) {
                            const double c = tr.bc_conc[k * ns + s];
                            if (c == 0.0) continue;
                            const double g = dv * c;
                            tr.cell_mass[tr.idx(static_cast<int>(s), i)] += g;
                            tr.gained_boundary[s] += g;
                        }
                    }
                }
            }
            state_->volume[i] = v_new;
            bc_accum_[k] += dt_c * f;
            inertial::cellEtaDepth(*mesh_, *opts_, i, state_->volume[i],
                                   state_->head[i], state_->depth[i]);
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
            const double fo    = -f;   // outward volumetric flux (m³/s)
            const double inv_a = 1.0 / mesh_->tri_area[i];
            qcx_[i] += fo * (mxb - mesh_->tri_cx[i]) * inv_a;
            qcy_[i] += fo * (myb - mesh_->tri_cy[i]) * inv_a;
        }
    }

    // Live junction exchange at tier-0 cadence (windowless coupling): the
    // orifice law against LIVE 2D heads and the routing step's 1D heads.
    // Drains cap at the exchange-β share of the source cell; spills cap at
    // the node's stored volume for the whole advance (node_drawn_ ledger) —
    // the same water cannot spill twice within a routing step.
    if (!exch_.empty() && tier0 &&
        state_->node_coupling && state_->nodes_1d) {
        const auto& pts = *state_->node_coupling;
        for (std::size_t k = 0; k < pts.size(); ++k) {
            const auto& cp = pts[k];
            const int   ci = cp.cell_idx;
            if (ci < 0 || !cell_active_[ci]) continue;
            const double h_off = (k < exch_head_slope_.size())
                                     ? exch_head_slope_[k] * exch_tau_
                                     : 0.0;
            double Q = computeNodeCouplingQ(cp, *mesh_, *state_,
                                            *state_->nodes_1d, *opts_,
                                            nullptr, h_off);
            if (Q == 0.0) continue;
            if (Q > 0.0) {   // 2D → 1D drain: availability share of the cell
                Q = std::min(Q, opts_->exchange_beta *
                                    std::max(state_->volume[ci], 0.0) / dt_c);
            } else {         // 1D → 2D spill: node stored-volume budget
                const auto ni = static_cast<std::size_t>(cp.node_idx);
                const double avail =
                    std::max(0.0, state_->nodes_1d->volume[ni] *
                                      opts_->vol_1d_to_2d) -
                    node_drawn_[ni];
                if (avail <= 0.0) continue;
                const double want = -Q * dt_c;
                const double take = std::min(want, avail);
                node_drawn_[ni] += take;
                Q = -take / dt_c;
            }
            // S1: a 2D→1D drain (Q > 0) leaves at the cell's concentration
            // and is booked per coupling point so S3's tuple can hand it to
            // the node's qual_mass_in; a 1D→2D spill arrives at zero
            // concentration until S3 carries the node's published value.
            if (!sacc_L_.empty() && Q > 0.0) {
                auto& tr = state_->transport;
                const auto ns = static_cast<std::size_t>(tr.n_species);
                double* pt = (k * ns < tr.exch_mass.size())
                                 ? &tr.exch_mass[k * ns] : nullptr;
                sinkMassAtCellConc(ci, Q * dt_c, tr.lost_coupling, pt);
            } else if (!sacc_L_.empty() && Q < 0.0) {
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
                    for (std::size_t s = 0; s < ns; ++s) {
                        const double c = conc[ni * ns + s];
                        if (c == 0.0) continue;
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
            state_->volume[ci] -= Q * dt_c;
            if (state_->volume[ci] < 0.0) state_->volume[ci] = 0.0;
            exch_[k] += Q * dt_c;
            inertial::cellEtaDepth(*mesh_, *opts_, ci, state_->volume[ci],
                                   state_->head[ci], state_->depth[ci]);
        }
    }
    // Head-ramp clock: tier-0 fires once per finest substep, so dt_c here is
    // exactly the wall the batch has advanced since the last exchange pass.
    if (tier0) exch_tau_ += dt_c;
}



// RECONSTRUCTION_ORDER 2: Green-Gauss gradients of (η, u, v) over the active
// cells (face value = mean of the two cells; boundary faces zero-gradient),
// limited per cell and variable with Barth–Jespersen against the min/max of
// the cell and its face neighbours. Cells that are dry, thin (< 10·h_dry) or
// touch a dry cell fall back to first order (φ = 0) — the wet/dry front keeps
// the robust monotone update.
void ExplicitInertialSolver::computeLimitedGradientsSwe() {
    const auto& ed = edges_;
    const int na = static_cast<int>(active_cells_.size());
    const double dry = opts_->dry_depth;
#pragma omp parallel for schedule(static) num_threads(opts_->num_threads)
    for (int k = 0; k < na; ++k) {
        const int i = active_cells_[static_cast<std::size_t>(k)];
        gex_[i] = gey_[i] = gux_[i] = guy_[i] = gvx_[i] = gvy_[i] = 0.0;
        const double hi = state_->depth[i];
        if (hi <= 10.0 * dry) continue;
        const double ei = state_->head[i];
        const double ui = qcx_[i] / hi, vi = qcy_[i] / hi;
        double gx[3] = {0, 0, 0}, gy[3] = {0, 0, 0};
        double wmin[3] = {ei, ui, vi}, wmax[3] = {ei, ui, vi};
        bool ok = true;
        int nfaces = 0;
        for (int p = ed.cell_ptr[i]; p < ed.cell_ptr[i + 1]; ++p) {
            const int e = ed.cell_edge[p];
            const int j = (ed.cL[e] == i) ? ed.cR[e] : ed.cL[e];
            const double hj = state_->depth[j];
            if (hj <= dry || !cell_active_[j]) { ok = false; break; }
            const double ej = state_->head[j], uj = qcx_[j] / hj, vj = qcy_[j] / hj;
            const double sgn = static_cast<double>(ed.cell_sign[p]);   // outward normal sign
            const double nx = sgn * ed.nx[e] * ed.xi[e], ny = sgn * ed.ny[e] * ed.xi[e];
            const double w[3] = {0.5 * (ei + ej), 0.5 * (ui + uj), 0.5 * (vi + vj)};
            const double wj[3] = {ej, uj, vj};
            for (int m = 0; m < 3; ++m) {
                gx[m] += w[m] * nx; gy[m] += w[m] * ny;
                wmin[m] = std::min(wmin[m], wj[m]); wmax[m] = std::max(wmax[m], wj[m]);
            }
            ++nfaces;
        }
        // Boundary faces (no CSR entry): zero-gradient contribution w_i·n̂·ξ.
        const int nvc = mesh_->cell_vertex_count(i);
        if (ok && nfaces < nvc) {
            for (int kk = 0; kk < nvc; ++kk) {
                if (mesh_->cell_neighbour(i, kk) >= 0) continue;
                const int slot = MeshData::slot(i, kk);
                const double nx = mesh_->edge_nx[slot] * mesh_->edge_length[slot];
                const double ny = mesh_->edge_ny[slot] * mesh_->edge_length[slot];
                gx[0] += ei * nx; gy[0] += ei * ny;
                gx[1] += ui * nx; gy[1] += ui * ny;
                gx[2] += vi * nx; gy[2] += vi * ny;
            }
        }
        if (!ok || nfaces == 0) continue;
        const double inv_a = 1.0 / mesh_->tri_area[i];
        const double wi[3] = {ei, ui, vi};
        double phi[3] = {1.0, 1.0, 1.0};
        for (int m = 0; m < 3; ++m) { gx[m] *= inv_a; gy[m] *= inv_a; }
        for (int p = ed.cell_ptr[i]; p < ed.cell_ptr[i + 1]; ++p) {
            const double ax = ed.cell_arm_x[p], ay = ed.cell_arm_y[p];
            for (int m = 0; m < 3; ++m) {
                const double wf = wi[m] + gx[m] * ax + gy[m] * ay;
                phi[m] = std::min(phi[m], swe::bjLimiter(wi[m], wf, wmin[m], wmax[m]));
            }
        }
        gex_[i] = phi[0] * gx[0]; gey_[i] = phi[0] * gy[0];
        gux_[i] = phi[1] * gx[1]; guy_[i] = phi[1] * gy[1];
        gvx_[i] = phi[2] * gx[2]; gvy_[i] = phi[2] * gy[2];
    }
}

// SSP-RK2 (Heun) over the global active lists: U¹ = U⁰ + Δt·L(U⁰),
// U² = U¹ + Δt·L(U¹), Uⁿ⁺¹ = ½(U⁰ + U²). The per-advance ledgers that both
// stages incremented (boundary ∫F dt, coupling ∫Q dt, infiltration applied)
// are reset to their trapezoidal value, ½·(stage-1 + stage-2) increments.
void ExplicitInertialSolver::runRk2Step(double dt) {
    const int nt = mesh_->n_triangles();
    // Member buffers: vector assignment reuses capacity, so no per-step
    // allocation (F6).
    rk_v0_  = state_->volume;
    rk_qx0_ = qcx_;
    rk_qy0_ = qcy_;
    rk_bc0_ = bc_accum_; rk_ex0_ = exch_; rk_inf0_ = state_->infil_applied;
    rk_drawn0_ = node_drawn_;
    for (int stage = 0; stage < 2; ++stage) {
        fireFaces(active_faces_, dt, /*global_step=*/true);
        fireCells(active_cells_, dt, /*tier0=*/true);
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
            inertial::cellEtaDepth(*mesh_, *opts_, i, state_->volume[i],
                                   state_->head[i], state_->depth[i]);
            state_->infil_applied[i] =
                rk_inf0_[i] + 0.5 * (state_->infil_applied[i] - rk_inf0_[i]);
        }
        if (state_->depth[i] <= opts_->dry_depth) { qcx_[i] = 0.0; qcy_[i] = 0.0; }
    }
    for (std::size_t k = 0; k < bc_accum_.size(); ++k)
        bc_accum_[k] = rk_bc0_[k] + 0.5 * (bc_accum_[k] - rk_bc0_[k]);
    for (std::size_t k = 0; k < exch_.size(); ++k)
        exch_[k] = rk_ex0_[k] + 0.5 * (exch_[k] - rk_ex0_[k]);
    for (std::size_t k = 0; k < node_drawn_.size(); ++k)
        node_drawn_[k] = rk_drawn0_[k] + 0.5 * (node_drawn_[k] - rk_drawn0_[k]);
    accumulators_pending_ = false;
    substeps_run_ += 2;
    last_steps_   += 2;
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
double ExplicitInertialSolver::boundaryFluxSwe(std::size_t k, int i, double dt_c) {
    const int    idx = bc_slot_[k];
    const auto   bt  = static_cast<BoundaryType>(state_->boundary->edge_bc_type[idx]);
    const double L   = mesh_->edge_length[idx];
    if (L <= 1.0e-12) return 0.0;
    const double nx = mesh_->edge_nx[idx], ny = mesh_->edge_ny[idx];   // outward
    const double eta_i = state_->head[i], h_i = state_->depth[i];
    const double z_i   = eta_i - h_i;
    const double dry   = opts_->dry_depth;
    const double qx_i  = qcx_[i], qy_i = qcy_[i];
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
    bool   prescribed_mass = false;
    double q_prescribed    = 0.0;      // outward-positive, per metre of edge
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
            const double S = state_->boundary->edge_bed_slope[idx];
            const double n_man = mesh_->mannings_n[i];
            prescribed_mass = true;
            if (S > 0.0 && h_i > 0.0 && n_man > 0.0) {
                const double h53 = h_i * std::cbrt(h_i * h_i);
                q_prescribed = h53 * std::sqrt(S) / n_man;       // outward +
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
            h_g   = (eta_bc - z_i > 0.0) ? eta_bc - z_i : 0.0;
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
            const double q_out = state_->boundary->edge_bc_flow[idx];   // outward +
            if (h_i > dry) {
                h_g = h_i;
                if (q_out < 0.0) {                       // inflow
                    const double c_i = std::sqrt(swe::kGravity * h_i);
                    if (std::fabs(u_n) < c_i)            // subcritical
                        h_g = swe::depthFromInvariantAndDischarge(
                            u_n + 2.0 * c_i, q_out, h_i);
                }
                eta_g = z_i + h_g;
            } else {
                h_g  = swe::criticalDepth(std::fabs(q_out));
                eta_g = z_i + h_g;
            }
            un_g = (h_g > 0.0) ? q_out / h_g : 0.0;
            ut_g = 0.0;
            prescribed_mass = true;
            q_prescribed    = q_out;
            break;
        }
    }
    // Interior = L, ghost = R, normal outward; velocities re-expressed as
    // (x, y) unit discharges for the shared kernel.
    const double qxg = h_g * (un_g * nx - ut_g * ny);
    const double qyg = h_g * (un_g * ny + ut_g * nx);
    swe::FaceFlux F;
    double cLx, cLy, cRx, cRy;
    const bool wet = swe::faceFlux(eta_i, h_i, qx_i, qy_i,
                                   eta_g, h_g, qxg, qyg,
                                   nx, ny, dry, F, cLx, cLy, cRx, cRy);
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
    return -F.mass * conv * L;                            // inflow-positive m³/s
}

void ExplicitInertialSolver::runMacroCycle(double dt0, int nsub) {
    const int K = static_cast<int>(cells_by_tier_.size());
    static const bool dbg_invariant = [] {
        const char* e = std::getenv("OPENSWMM_2D_MARCHER_CHECK");
        return e && e[0] == '1';
    }();
    auto invariant = [&]() -> double {
        double s = 0.0;
        for (int i = 0; i < mesh_->n_triangles(); ++i) s += state_->volume[i];
        for (int e = 0; e < edges_.ne; ++e) s += facc_L_[e] + facc_R_[e];
        return s;
    };

    for (int s = 0; s < nsub; ++s) {
        const double inv0 = dbg_invariant ? invariant() : 0.0;
        // Fire every tier due at this base substep: faces first (they read the
        // incident cells' published surfaces), then the due cells.
        for (int k = 0; k < K; ++k) {
            if (s % (1 << k)) continue;
            if (!edges_by_tier_[k].empty())
                fireFaces(edges_by_tier_[k], (1 << k) * dt0);
        }
        if (dbg_invariant) {
            const double inv1 = invariant();
            if (std::fabs(inv1 - inv0) > 1.0e-9 * (std::fabs(inv0) + 1.0))
                std::fprintf(stderr,
                             "[marcher-check] FACE phase moved invariant: "
                             "s=%d d=%.6e\n", s, inv1 - inv0);
        }
        for (int k = 0; k < K; ++k) {
            if (s % (1 << k)) continue;
            if (!cells_by_tier_[k].empty() || k == 0)
                fireCells(cells_by_tier_[k], (1 << k) * dt0, k == 0);
        }
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
    if (!initialized_ || t_target <= t_current) return t_target;

    double t = t_current;
    // The lazy-source clock persists across advances (inactive cells may owe
    // sources for spans straddling advance boundaries); re-anchor only if the
    // caller's clock went backwards (hotstart / reinit).
    if (t_last_sync_ > t_current) t_last_sync_ = t_current;
    std::fill(bc_accum_.begin(), bc_accum_.end(), 0.0);
    std::fill(exch_.begin(), exch_.end(), 0.0);
    std::fill(node_drawn_.begin(), node_drawn_.end(), 0.0);
    // S1: the per-point species drain resets with the per-point volume drain
    // it rides on, so S3's tuple reads one advance's worth and not a total.
    std::fill(state_->transport.exch_mass.begin(),
              state_->transport.exch_mass.end(), 0.0);
    std::fill(state_->transport.exch_spill.begin(),
              state_->transport.exch_spill.end(), 0.0);
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
        const int K = static_cast<int>(cells_by_tier_.size());
        const int nsub_full = 1 << (K - 1);
        const double remaining = t_target - t;

        if (active_cells_.empty()) {
            // Quiescent: stride the window; the lazy tier keeps accumulating.
            t = t_target;
            last_dt_ = remaining;
            break;
        }

        if (nsub_full * dt0_ <= remaining) {
            // Full macro cycle fits (dt0_ <= remaining here, so no clamp).
            if (second_order_) runRk2Step(dt0_);      // K == 1: one global step
            else               runMacroCycle(dt0_, nsub_full);
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
            if (remaining <= dt0_ * 1.0e-9) {
                t = t_target;
                break;
            }
            settleAccumulators();
            const int nt = std::max(
                1, static_cast<int>(std::ceil(remaining / dt0_)));
            const double dt_tail = remaining / nt;
            for (int s = 0; s < nt; ++s) {
                if (second_order_) { runRk2Step(dt_tail); continue; }
                fireFaces(active_faces_, dt_tail, /*global_step=*/true);
                fireCells(active_cells_, dt_tail, /*tier0=*/true);
                ++substeps_run_;
                ++last_steps_;
            }
            accumulators_pending_ = false;   // every side just gathered
            last_dt_ = dt_tail;
            ++cycles_since_rebuild;
            t = t_target;   // exact landing — no 1-ulp re-entry
            break;
        }
    }

    cycles_since_rebuild_ = cycles_since_rebuild;
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
    std::fill(state_->edge_flux.begin(), state_->edge_flux.end(), 0.0);
    const bool vfr_face =
        (opts_->face_reconstruction == FaceDepth2D::VFR_FACE);
    // Only the ACTIVE faces can carry flux: a face with an inactive side had
    // its q zeroed when the side deactivated, so the old full sweep spent
    // O(n_faces) recomputing a face depth and a Froude cap in order to publish
    // the zero the fill above already wrote. Identical output, and a quiescent
    // tail (active fraction reached 0.0 % in the storm run) now costs nothing.
    for (const int e : active_faces_) {
        double qp = 0.0;
        if (mode_ == Momentum2D::LOCAL_INERTIAL) {
            const double hf = vfr_face
                ? inertial::faceFlowDepthVfr(state_->head[edges_.cL[e]],
                                             state_->head[edges_.cR[e]],
                                             edges_.ze_lo[e], edges_.ze_hi[e])
                : inertial::faceFlowDepth(state_->head[edges_.cL[e]],
                                          state_->head[edges_.cR[e]],
                                          edges_.zface[e]);
            if (hf > opts_->dry_depth)
                qp = inertial::froudeCap(q_[e], hf, opts_->froude_max);
        } else {
            qp = q_[e];   // last evaluated face mass flux (already limited)
        }
        const double F = qp * edges_.xi[e];
        state_->edge_flux[edges_.slotL[e]] = -F;
        state_->edge_flux[edges_.slotR[e]] = +F;
    }
    const double span = t_target - t_current;
    if (span > 0.0)
        for (std::size_t k = 0; k < bc_cell_.size(); ++k)
            state_->edge_flux[bc_slot_[k]] = bc_accum_[k] / span;

    return t_target;
}

void ExplicitInertialSolver::reinitialize(double /*t0*/) {
    if (!initialized_) return;
    // External state edit (hot start / breach redo): volumes are authoritative;
    // face momentum and pending transfers are stale — drop them.
    std::fill(q_.begin(), q_.end(), 0.0);
    std::fill(bc_q_.begin(), bc_q_.end(), 0.0);
    std::fill(facc_L_.begin(), facc_L_.end(), 0.0);
    std::fill(facc_R_.begin(), facc_R_.end(), 0.0);
    std::fill(macc_x_L_.begin(), macc_x_L_.end(), 0.0);
    std::fill(macc_x_R_.begin(), macc_x_R_.end(), 0.0);
    std::fill(macc_y_L_.begin(), macc_y_L_.end(), 0.0);
    std::fill(macc_y_R_.begin(), macc_y_R_.end(), 0.0);
    if (mode_ == Momentum2D::FULL_SWE) {
        std::fill(qcx_.begin(), qcx_.end(), 0.0);
        std::fill(qcy_.begin(), qcy_.end(), 0.0);
    }
    accumulators_pending_ = false;
    reconstructAll();
}

void ExplicitInertialSolver::resyncFromVolumes(double /*t0*/) {
    if (!initialized_) return;
    // Volumes already live in state_->volume; keep the face momentum (nothing
    // failed — this is a pure re-time on this path). Pending transfers were
    // booked into volumes at the last cell firing; accumulators stay.
    reconstructAll();
}

void ExplicitInertialSolver::finalize() {
    if (!telemetry_path_.empty() && !telemetry_.empty()) {
        if (std::FILE* f = std::fopen(telemetry_path_.c_str(), "w")) {
            std::fprintf(f, "t_s,active_cells,active_frac\n");
            const double nt = std::max(1, mesh_ ? mesh_->n_triangles() : 1);
            for (const auto& [t, n] : telemetry_)
                std::fprintf(f, "%.3f,%d,%.6f\n", t, n, n / nt);
            std::fclose(f);
        }
    }
    q_.clear(); qcx_.clear(); qcy_.clear();
    facc_L_.clear(); facc_R_.clear();
    macc_x_L_.clear(); macc_x_R_.clear(); macc_y_L_.clear(); macc_y_R_.clear();
    dw_slope_.clear();
    cell_active_.clear(); active_cells_.clear();
    tier_.clear(); face_tier_.clear();
    cells_by_tier_.clear(); edges_by_tier_.clear();
    bc_cell_.clear(); bc_slot_.clear(); bc_accum_.clear(); bc_q_.clear();
    telemetry_.clear();
    initialized_ = false;
}

ISurfaceSolver::RunStats ExplicitInertialSolver::run_stats() const noexcept {
    RunStats s;
    s.nsteps = substeps_run_;
    s.nrhs   = face_passes_;
    s.last_h = last_dt_;

    // Marcher telemetry for the report block: active-fraction spread over the
    // rebuild samples + cumulative tier-occupancy histogram. Must be read
    // BEFORE finalize() (which clears telemetry_) — SurfaceRouter2D does.
    if (!telemetry_.empty() && mesh_ && mesh_->n_triangles() > 0) {
        const double nt = static_cast<double>(mesh_->n_triangles());
        double mn = 1.0e30, mx = -1.0e30, sum = 0.0;
        for (const auto& [t, n] : telemetry_) {
            const double frac = n / nt;
            mn = std::min(mn, frac);
            mx = std::max(mx, frac);
            sum += frac;
        }
        s.active_frac_min  = mn;
        s.active_frac_max  = mx;
        s.active_frac_mean = sum / static_cast<double>(telemetry_.size());
    }
    s.n_tiers = static_cast<int>(
        std::min(cells_by_tier_.size(), tier_occupancy_.size()));
    for (int k = 0; k < s.n_tiers; ++k)
        s.tier_cells[k] = tier_occupancy_[static_cast<std::size_t>(k)];
    return s;
}

} // namespace openswmm::twoD
