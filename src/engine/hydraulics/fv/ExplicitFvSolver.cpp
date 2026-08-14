/**
 * @file ExplicitFvSolver.cpp
 * @brief Implementation of the CPU explicit finite-volume 1D network solver.
 *
 * @see ExplicitFvSolver.hpp
 * @ingroup engine_fv
 */

#include "ExplicitFvSolver.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include "FvKernels.hpp"
#include "../HydClosureKernels.hpp"
#include "../../core/Constants.hpp"

#ifdef SWMM_USE_OPENMP
#include <omp.h>
#endif

namespace openswmm::fv {

namespace k = kernels;

namespace {

/// Below this many faces the OpenMP fork/join costs more than the flux loop.
constexpr int kOmpMinFaces = 4096;

/// Slope limiters for the second-order scalar reconstruction. All three are
/// TVD; superbee is the sharpest and can artificially steepen smooth profiles,
/// minmod the most diffusive and most robust.
inline double limitSlope(double a, double b, Limiter lim) {
    if (a * b <= 0.0) return 0.0;                  // extremum ⇒ no slope
    switch (lim) {
        case Limiter::VANLEER:
            return 2.0 * a * b / (a + b);
        case Limiter::SUPERBEE: {
            const double s = (a > 0.0) ? 1.0 : -1.0;
            return s * std::max(std::min(2.0 * std::fabs(a), std::fabs(b)),
                                std::min(std::fabs(a), 2.0 * std::fabs(b)));
        }
        case Limiter::MINMOD:
        default:
            return (std::fabs(a) < std::fabs(b)) ? a : b;
    }
}

} // namespace

// ===========================================================================
// Lifecycle
// ===========================================================================

void ExplicitFvSolver::initialize(NetworkMeshData& mesh, NetworkStateData& state,
                                  const FvOptions& opts) {
    mesh_  = &mesh;
    state_ = &state;
    opts_  = opts;

    const auto nf = static_cast<std::size_t>(mesh.n_faces());
    const auto nc = static_cast<std::size_t>(mesh.n_cells());
    const auto nn = static_cast<std::size_t>(mesh.n_nodes());
    const auto ns = static_cast<std::size_t>(std::max(0, state.n_species));

    // Node capacity attributes are optional on a hand-built mesh (the solver
    // unit tests construct NetworkMeshData directly). Default them rather than
    // indexing short vectors: no ponding, no surcharge depth.
    if (mesh.node_sur_depth.size() < nn) mesh.node_sur_depth.resize(nn, 0.0);
    if (mesh.node_can_pond.size() < nn)  mesh.node_can_pond.resize(nn, 0);
    // Same reason: faceSide indexes face_geom unconditionally, so a mesh that
    // never went through NetworkMeshBuilder must get the per-cell fallback
    // here rather than reading an empty vector. No-op for a built mesh.
    mesh.deriveFaceGeom();
    if (mesh.face_gate.size() < nf)      mesh.face_gate.resize(nf, 0);
    if (mesh.face_culvert.size() < nf)   mesh.face_culvert.resize(nf, -1);

    f_mass_.assign(nf, 0.0);
    f_mom_.assign(nf, 0.0);
    f_sstar_.assign(nf, 0.0);
    f_corr_l_.assign(nf, 0.0);
    f_corr_r_.assign(nf, 0.0);
    f_scale_.assign(nf, 1.0);
    f_phi_l_.assign(ns * nf, 0.0);
    f_phi_r_.assign(ns * nf, 0.0);
    f_phi_flux_.assign(ns * nf, 0.0);
    f_state_l_.assign(nf, k::FaceState{});
    f_state_r_.assign(nf, k::FaceState{});
    f_flux_.assign(nf, k::FaceFlux{});
    hllc_ = (opts.riemann == RiemannSolver::HLLC);
    cell_slope_.assign(nc, 0.0);
    cell_eta_slope_.assign(nc, 0.0);
    cell_u_slope_.assign(nc, 0.0);
    cell_ho2_.assign(nc, 0);

    cell_eta_.assign(nc, 0.0);
    cell_u_.assign(nc, 0.0);
    cell_q_int_.assign(nc, 0.0);

    node_exch_.assign(nn, 0.0);
    node_in_.assign(nn, 0.0);
    node_out_.assign(nn, 0.0);
    flood_vol_.assign(nn, 0.0);
    inlet_control_.assign(static_cast<std::size_t>(mesh.n_conduits()), 0);

    cell_tier_.assign(nc, 0);
    node_tier_.assign(nn, 0);
    face_tier_.assign(nf, 0);
    acc_a_.clear();
    acc_q_.clear();
    acc_nvol_.clear();
    cells_by_tier_.clear();
    faces_by_tier_.clear();
    nodes_by_tier_.clear();
    lts_tiers_ = 1;
    tier_occupancy_.fill(0);

    cell_active_.assign(nc, 1);
    active_faces_.clear();
    lists_valid_  = false;
    since_rebuild_ = 0;
    census_count_ = 0;
    dt_cache_     = 0.0;
    lts_valid_    = false;
    lts_countdown_ = 0;
    lts_dt0_      = 0.0;

    total_steps_ = 0;
    total_flux_  = 0;
    min_h_       = 0.0;
    sim_time_    = 0.0;
    active_sum_  = 0.0;
    active_min_  = -1.0;
    active_max_  = -1.0;
    active_n_    = 0;

    refreshDepths();
    refreshNodeAreas();

    // Algebraic-junction eligibility. Static for the run: plain junctions
    // (no storage table) with at least one incident face. A node with only
    // structure links has no face flux to balance — its residual would carry
    // no head dependence — so it keeps the bucket machinery. Outfalls are
    // excluded by kind; virtual junctions never reach the node paths at all.
    node_alg_.assign(nn, 0);
    node_carry_.assign(nn, 0.0);
    node_vfull_.assign(nn, 0.0);
    node_pass_static_.assign(nn, 0);
    node_pass_.assign(nn, 0);
    for (std::size_t un = 0; un < nn; ++un) {
        node_vfull_[un] = nodeVolumeFromDepth(static_cast<int>(un),
                                              mesh_->node_full_depth[un]);
        node_alg_[un] =
            (mesh_->node_kind[un] == kNodeJunction &&
             mesh_->node_vol_off[un] < 0 &&
             mesh_->node_face_ptr[un + 1] > mesh_->node_face_ptr[un])
                ? 1 : 0;
        // Static half of the pass-through test: exactly two incident conduit
        // faces, neither carrying a culvert inlet or a flap gate (those need
        // their head- and direction-dependent laws applied against a solved
        // node state, not bypassed by a direct face).
        if (node_alg_[un] &&
            mesh_->node_face_ptr[un + 1] - mesh_->node_face_ptr[un] == 2) {
            bool clean = true;
            for (int p = mesh_->node_face_ptr[un];
                 p < mesh_->node_face_ptr[un + 1]; ++p) {
                const auto uf = static_cast<std::size_t>(
                    mesh_->node_face_idx[static_cast<std::size_t>(p)]);
                if (mesh_->face_culvert[uf] >= 0 || mesh_->face_gate[uf] != 0)
                    clean = false;
            }
            node_pass_static_[un] = clean ? 1 : 0;
        }
    }
}

void ExplicitFvSolver::reinitialize(double /*t0*/) {
    if (!mesh_) return;
    lts_valid_    = false;
    lists_valid_  = false;
    census_count_ = 0;
    dt_cache_     = 0.0;
    refreshDepths();
    refreshNodeAreas();
}

void ExplicitFvSolver::finalize() {
    mesh_  = nullptr;
    state_ = nullptr;
}

INetworkSolver::RunStats ExplicitFvSolver::run_stats() const noexcept {
    RunStats s;
    s.nsteps = total_steps_;
    s.nflux  = total_flux_;
    s.last_h = last_h_;
    s.min_h  = min_h_;
    s.avg_h  = (total_steps_ > 0) ? sim_time_ / static_cast<double>(total_steps_) : 0.0;
    if (active_n_ > 0) {
        s.active_frac_mean = active_sum_ / static_cast<double>(active_n_);
        s.active_frac_min  = active_min_;
        s.active_frac_max  = active_max_;
    }
    // LTS tier histogram (§6.12): cells counted once per re-tier, so the
    // distribution is what the report needs to show tiering was active — a run
    // that quietly collapsed to one tier reads as n_tiers == 1 here rather than
    // as a silently ordinary run.
    s.n_tiers = std::min(lts_tiers_, static_cast<int>(kMaxLtsTiers));
    for (int k = 0; k < s.n_tiers; ++k)
        s.tier_cells[k] = tier_occupancy_[static_cast<std::size_t>(k)];
    return s;
}

// ===========================================================================
// Node storage relation
// ===========================================================================

// Above the rim a ponding node's storage IS its ponded area, and BOTH
// conversions have to say so. The solver re-seeds each node's volume from the
// head it is holding at the top of every routing step; with the ponding tail
// missing from these two functions that re-seed re-derived a 4 ft-deep pond as
// MIN_SURFAREA x 4 ft and threw the pond away every step — the surface never
// climbed past the rim and the water left the mass balance entirely.
double ExplicitFvSolver::nodeVolumeFromDepth(int node, double depth) const {
    const auto un = static_cast<std::size_t>(node);
    if (depth <= 0.0) return 0.0;
    const double full = mesh_->node_full_depth[un];
    if (mesh_->node_can_pond[un] && full > 0.0 && depth > full)
        return nodeVolumeFromDepth(node, full) +
               (depth - full) * mesh_->node_ponded_area[un];
    const int off = mesh_->node_vol_off[un];
    if (off < 0)                                   // junction: linear in depth
        return state_->node_surf_area[un] * depth;

    const double dmax = mesh_->node_vol_dmax[un];
    const int n = kNodeVolSamples;
    const double vmax = mesh_->node_vol_tbl[static_cast<std::size_t>(off + n - 1)];
    if (depth >= dmax) return vmax + mesh_->node_vol_atop[un] * (depth - dmax);

    const double dd = dmax / static_cast<double>(n - 1);
    int i = static_cast<int>(depth / dd);
    if (i > n - 2) i = n - 2;
    if (i < 0) i = 0;
    const double t = (depth - static_cast<double>(i) * dd) / dd;
    const double v0 = mesh_->node_vol_tbl[static_cast<std::size_t>(off + i)];
    const double v1 = mesh_->node_vol_tbl[static_cast<std::size_t>(off + i + 1)];
    return v0 + (v1 - v0) * t;
}

double ExplicitFvSolver::nodeDepthFromVolume(int node, double volume) const {
    const auto un = static_cast<std::size_t>(node);
    if (volume <= 0.0) return 0.0;
    const double full = mesh_->node_full_depth[un];
    if (mesh_->node_can_pond[un] && full > 0.0) {
        const double v_full = nodeVolumeFromDepth(node, full);
        if (volume > v_full)
            return full + (volume - v_full) / mesh_->node_ponded_area[un];
    }
    const int off = mesh_->node_vol_off[un];
    if (off < 0) {
        const double a = state_->node_surf_area[un];
        return (a > 0.0) ? volume / a : 0.0;
    }
    const int n = kNodeVolSamples;
    const double dmax = mesh_->node_vol_dmax[un];
    const double dd = dmax / static_cast<double>(n - 1);
    const double vmax = mesh_->node_vol_tbl[static_cast<std::size_t>(off + n - 1)];
    if (volume >= vmax)
        return dmax + (volume - vmax) / mesh_->node_vol_atop[un];

    int lo = 0, hi = n - 1;
    while (hi - lo > 1) {
        const int mid = (lo + hi) / 2;
        if (mesh_->node_vol_tbl[static_cast<std::size_t>(off + mid)] <= volume) lo = mid;
        else                                                                    hi = mid;
    }
    const double v0 = mesh_->node_vol_tbl[static_cast<std::size_t>(off + lo)];
    const double v1 = mesh_->node_vol_tbl[static_cast<std::size_t>(off + lo + 1)];
    const double t = (v1 > v0) ? (volume - v0) / (v1 - v0) : 0.0;
    return (static_cast<double>(lo) + t) * dd;
}

// ===========================================================================
// State refresh
// ===========================================================================

void ExplicitFvSolver::refreshDepths() {
    const int nc = mesh_->n_cells();
    for (int c = 0; c < nc; ++c) {
        const auto uc = static_cast<std::size_t>(c);
        const FvGeometry& g = mesh_->geom[static_cast<std::size_t>(mesh_->cell_geom[uc])];
        double a = state_->cell_a[uc];
        if (a < 0.0) a = 0.0;
        const double h = k::depthOfArea(g, a);
        state_->cell_a[uc] = a;
        state_->cell_h[uc] = h;
        cell_eta_[uc] = mesh_->cell_zb[uc] + h;
        if (h <= k::kDryDepth) {
            state_->cell_q[uc] = 0.0;
            cell_u_[uc]        = 0.0;
        } else {
            cell_u_[uc] = state_->cell_q[uc] / a;
        }
    }
}

void ExplicitFvSolver::refreshNodeAreas() {
    const int nn = mesh_->n_nodes();
    for (int n = 0; n < nn; ++n) {
        const auto un = static_cast<std::size_t>(n);
        if (mesh_->node_kind[un] == kNodeVirtual) {
            state_->node_surf_area[un] = 0.0;
            continue;
        }
        if (mesh_->node_vol_off[un] >= 0) {
            // Storage node: the flattened table carries the geometry. The area
            // is reported, not used to advance, so read it off the local slope.
            const double d = std::max(0.0, state_->node_head[un] - mesh_->node_invert[un]);
            const double dd = mesh_->node_vol_dmax[un] /
                              static_cast<double>(kNodeVolSamples - 1);
            const double v0 = nodeVolumeFromDepth(n, d);
            const double v1 = nodeVolumeFromDepth(n, d + dd);
            state_->node_surf_area[un] = std::max((v1 - v0) / dd, constants::MIN_SURFAREA);
            continue;
        }

        // Junction / outfall / divider: the ENGINE's own storage convention,
        // fixed for the run (NetworkMeshData::node_area). Two properties fall
        // out, and both are load-bearing:
        //
        //   * V = A_s*depth is a genuine state relation because A_s is
        //     constant, so re-seeding the ledger from the head between routing
        //     steps is exact instead of creating (A_s_new - A_s_old)*depth of
        //     water out of nothing;
        //   * the volume the solver holds is the SAME function of depth the
        //     mass balance reports, so no water is stored where continuity
        //     cannot see it.
        //
        // Tracking the live conduit top width instead — DW's convention, and
        // the obvious first guess — violates both, and cost ~0.3 % of routing
        // continuity on Example1.
        state_->node_surf_area[un] = mesh_->node_area[un];
    }
}

// ===========================================================================
// Work-list compaction (plan §5.2.1)
// ===========================================================================

void ExplicitFvSolver::rebuildActiveLists() {
    const int nc = mesh_->n_cells();
    const int nf = mesh_->n_faces();

    // Both exits invalidate the tier cache. Doing it only on the compacted
    // path would make the re-tier CADENCE depend on whether compaction is on,
    // and the cadence is part of the integration schedule — §6.10 would fail
    // for a reason that has nothing to do with which faces were skipped.
    lts_valid_ = false;

    // Faces left out of the list keep whatever they last held, and the node
    // update reads every one of its faces — so clear the flux arrays here, at
    // the one point where the list changes.
    std::fill(f_mass_.begin(), f_mass_.end(), 0.0);
    std::fill(f_mom_.begin(), f_mom_.end(), 0.0);
    std::fill(f_corr_l_.begin(), f_corr_l_.end(), 0.0);
    std::fill(f_corr_r_.begin(), f_corr_r_.end(), 0.0);

    if (!opts_.compaction) {
        std::fill(cell_active_.begin(), cell_active_.end(), char{1});
        active_faces_.resize(static_cast<std::size_t>(nf));
        for (int f = 0; f < nf; ++f) active_faces_[static_cast<std::size_t>(f)] = f;
        lists_valid_ = true;
        since_rebuild_ = 0;
        return;
    }

    // Seed: any cell holding water or momentum, plus any cell fed by a node
    // that stands above the conduit invert — a dry pipe hanging off a full
    // manhole must not be skipped.
    for (int c = 0; c < nc; ++c) {
        const auto uc = static_cast<std::size_t>(c);
        cell_active_[uc] = (state_->cell_h[uc] > k::kDryDepth ||
                            state_->cell_q[uc] != 0.0) ? char{1} : char{0};
    }
    for (int f = 0; f < nf; ++f) {
        const auto uf = static_cast<std::size_t>(f);
        const int nd = mesh_->face_node[uf];
        if (nd < 0) continue;
        const auto un = static_cast<std::size_t>(nd);
        if (state_->node_head[un] - mesh_->face_zb[uf] <= k::kDryDepth) continue;
        const int cell = (mesh_->face_cl[uf] >= 0) ? mesh_->face_cl[uf]
                                                   : mesh_->face_cr[uf];
        if (cell >= 0) cell_active_[static_cast<std::size_t>(cell)] = 1;
    }

    // Halo. A wet front advances at most CFL ≤ 1 cell per substep, so a halo of
    // kRebuildInterval cells keeps a stale list conservative for exactly that
    // many substeps. Every face the non-compacted run would give a NON-ZERO
    // flux is therefore in the list; the ones left out have two dry sides and
    // riemannFlux returns exactly 0.0 for those — which is what makes
    // compaction results-TRANSPARENT rather than merely close (§6.10).
    //
    // The first level also guarantees both sides of every active face are
    // active, so no active face can dump flux into a cell that is never
    // updated (which would lose mass).
    //
    // Double-buffered on purpose. Growing the set IN PLACE lets a single sweep
    // carry the flag along the whole face array in index order, so the "halo"
    // silently becomes the entire downstream reach and compaction stops
    // compacting anything. Each level must expand by exactly one cell.
    halo_prev_.assign(cell_active_.begin(), cell_active_.end());
    for (int level = 0; level < kRebuildInterval; ++level) {
        bool grew = false;
        for (int f = 0; f < nf; ++f) {
            const auto uf = static_cast<std::size_t>(f);
            const int cl = mesh_->face_cl[uf];
            const int cr = mesh_->face_cr[uf];
            if (cl < 0 || cr < 0) continue;
            const auto ul = static_cast<std::size_t>(cl);
            const auto ur = static_cast<std::size_t>(cr);
            if (halo_prev_[ul] && !cell_active_[ur]) { cell_active_[ur] = 1; grew = true; }
            if (halo_prev_[ur] && !cell_active_[ul]) { cell_active_[ul] = 1; grew = true; }
        }
        if (!grew) break;
        halo_prev_.assign(cell_active_.begin(), cell_active_.end());
    }

    active_faces_.clear();
    for (int f = 0; f < nf; ++f) {
        const auto uf = static_cast<std::size_t>(f);
        const int cl = mesh_->face_cl[uf];
        const int cr = mesh_->face_cr[uf];
        const bool la = (cl >= 0) && cell_active_[static_cast<std::size_t>(cl)];
        const bool ra = (cr >= 0) && cell_active_[static_cast<std::size_t>(cr)];
        if (la || ra) active_faces_.push_back(f);
    }

    const double frac = (nf > 0) ? static_cast<double>(active_faces_.size()) /
                                   static_cast<double>(nf) : 1.0;
    active_sum_ += frac;
    ++active_n_;
    active_min_ = (active_min_ < 0.0) ? frac : std::min(active_min_, frac);
    active_max_ = (active_max_ < 0.0) ? frac : std::max(active_max_, frac);

    lists_valid_   = true;
    since_rebuild_ = 0;
}

// ===========================================================================
// CFL census
// ===========================================================================

double ExplicitFvSolver::censusDt() const {
    double dt = std::numeric_limits<double>::max();
    static const bool kDtTrace = [] {
        const char* e = std::getenv("OPENSWMM_FV_DT_TRACE");
        return e && e[0] == '1';
    }();
    int    trace_face = -1;
    double trace_dx = 0.0, trace_speed = 0.0;

    // FACE-based, not cell-based. A cell-only census misses the wave speed a
    // BOUNDARY GHOST carries: a surcharged manhole standing above the crown
    // presents a pressurized ghost whose celerity is the slot celerity (~100
    // ft/s) to a part-full pipe whose own cells report ~5 ft/s. That is the
    // single most common configuration in a real sewer, and stepping on the
    // cell speed over-runs the true CFL limit by more than an order of
    // magnitude — the pipe empties instead of filling.
    for (const int f : active_faces_) {
        const auto uf = static_cast<std::size_t>(f);
        const int cl = mesh_->face_cl[uf];
        const int cr = mesh_->face_cr[uf];
        const int nd = mesh_->face_node[uf];

        double speed = 0.0;
        double dx_ref = 1.0e30;

        auto consider_cell = [&](int cell) {
            const auto uc = static_cast<std::size_t>(cell);
            dx_ref = std::min(dx_ref, mesh_->cell_dx[uc]);
            const double h = state_->cell_h[uc];
            if (h <= k::kDryDepth) return;
            const FvGeometry& g =
                mesh_->geom[static_cast<std::size_t>(mesh_->cell_geom[uc])];
            speed = std::max(speed,
                             std::fabs(cell_u_[uc]) +
                                 k::celerity(state_->cell_a[uc],
                                             k::widthOfDepth(g, h)));
        };
        if (cl >= 0) consider_cell(cl);
        if (cr >= 0) consider_cell(cr);

        if (nd >= 0) {
            const int other = (cl >= 0) ? cl : cr;
            if (other >= 0) {
                const auto uo = static_cast<std::size_t>(other);
                const FvGeometry& g =
                    mesh_->geom[static_cast<std::size_t>(mesh_->cell_geom[uo])];
                const double hg = state_->node_head[static_cast<std::size_t>(nd)] -
                                  mesh_->face_zb[uf];
                if (hg > k::kDryDepth) {
                    const double ag = k::areaOfDepth(g, hg);
                    speed = std::max(speed,
                                     std::fabs(cell_u_[uo]) +
                                         k::celerity(ag, k::widthOfDepth(g, hg)));
                }
            }
        }

        if (speed > 1.0e-12 && dx_ref < 1.0e29) {
            const double dt_f = opts_.cfl * dx_ref / speed;
            if (dt_f < dt) { dt = dt_f; trace_face = f; trace_dx = dx_ref;
                             trace_speed = speed; }
        }
    }

    // Diagnostic: report which face is setting the step, and what its dx and
    // wave speed are. Explaining a substep count needs the argmin, not the min.
    if (kDtTrace) {
        static long calls = 0;
        if ((calls++ % 2000) == 0 && trace_face >= 0) {
            const auto uf = static_cast<std::size_t>(trace_face);
            const int cl = mesh_->face_cl[uf], cr = mesh_->face_cr[uf];
            const int cc = (cl >= 0) ? cl : cr;
            const auto uc = static_cast<std::size_t>(cc);
            const FvGeometry& g =
                mesh_->geom[static_cast<std::size_t>(mesh_->cell_geom[uc])];
            const double h = state_->cell_h[uc];
            std::fprintf(stderr,
                "[fv-dt] dt=%.4f face=%d node=%d dx=%.2f speed=%.2f "
                "(cell %d conduit %d h=%.3f y_full=%.2f u=%.2f c=%.2f T=%.4f)\n",
                dt, trace_face, mesh_->face_node[uf], trace_dx, trace_speed,
                cc, mesh_->cell_conduit[uc], h, g.y_full, cell_u_[uc],
                k::celerity(state_->cell_a[uc], k::widthOfDepth(g, h)),
                k::widthOfDepth(g, h));
        }
    }

    // Node constraint. A coupled node behaves like an extra control volume of
    // effective length A_s/T, and with the MIN_SURFAREA floor that length is
    // often FAR shorter than Δx — a manhole is the stiff element of an
    // otherwise coarse mesh. Omitting this term under EXPLICIT coupling is the
    // classic way an explicit 1D network solver appears stable on a single
    // reach and rings on a real network.
    //
    const int nn = mesh_->n_nodes();
    // Semi-implicit coupling removes the node's STABILITY limit — the damped
    // response cannot ring — but not its ACCURACY requirement: a manhole whose
    // head swings within a step is still under-resolved, however stable it is.
    //
    // Tiering is what supplies that resolution cheaply. With FV_LTS on the node
    // gets its own fine tier from nodeStableDt() and fires at its own rate
    // while the conduit cells stay coarse, so the global step is free of it.
    // With tiering off there is nowhere to put the requirement but the global
    // step, and it has to be honoured — measured on the reference model at
    // Δx = 20 ft, dropping it without tiering moved mean peak-flow agreement
    // from 7.2 % to 18.8 % and the worst conduit from 18.7 % to 128 %.
    //
    // The skip is gated on tiering being ELIGIBLE TO RUN, not on the FV_LTS
    // option alone. An earlier gate tested `opts_.lts` — but the option being
    // on does not mean tiering runs: RK2 disables the LTS path outright, and
    // a water-quality run (species FCT needs a synchronous sweep) degenerates
    // assignTiers() to K = 1. Both fall through to the global path, and
    // skipping the node term there silently reproduced FV_NODE_DT NONE —
    // with its measured 10-50× accuracy loss — while the option still read
    // STABILITY.
    //
    // FV_NODE_DT NONE is honoured here as well, so the option means the same
    // thing on both paths — and only under SEMI_IMPLICIT coupling, because
    // for explicitly coupled nodes the bound buys stability, not accuracy,
    // and there is no opting out of it.
    //
    // The relaxation was swept rather than assumed: with tiering on, 1×, 4×,
    // 16×, 64× and unbounded gave 14.7 / 7.3 / 5.1 / 5.1 / 5.0 s at 7.2 / 7.2 /
    // 7.1 / 7.2 / 7.2 % mean agreement.
    if (opts_.node_coupling == NodeCoupling::SEMI_IMPLICIT &&
        (ltsEligible() || opts_.node_dt_limit == NodeDtLimit::NONE)) {
        if (dt >= std::numeric_limits<double>::max() * 0.5) dt = 1.0e30;
        return dt;
    }
    for (int n = 0; n < nn; ++n) {
        const auto un = static_cast<std::size_t>(n);
        if (mesh_->node_kind[un] == kNodeVirtual) continue;
        // Algebraic junctions have no volume state and hence no stability or
        // accuracy bound of their own — their head is solved, not integrated.
        if (!node_alg_.empty() && node_alg_[un]) continue;
        // Outfalls: head imposed, volume never integrated — nothing to bound
        // (mirrors nodeStableDt's skip, same rationale).
        if (mesh_->node_kind[un] == kNodeOutfall) continue;
        const double as = state_->node_surf_area[un];
        if (!(as > 0.0)) continue;
        const int b = mesh_->node_face_ptr[un];
        const int e = mesh_->node_face_ptr[un + 1];
        for (int p = b; p < e; ++p) {
            const int f = mesh_->node_face_idx[static_cast<std::size_t>(p)];
            const auto uf = static_cast<std::size_t>(f);
            const int cell = (mesh_->face_cl[uf] >= 0) ? mesh_->face_cl[uf]
                                                       : mesh_->face_cr[uf];
            if (cell < 0) continue;
            const auto uc = static_cast<std::size_t>(cell);
            if (!cell_active_[uc]) continue;
            const double h = state_->cell_h[uc];
            if (h <= k::kDryDepth) continue;
            const FvGeometry& g =
                mesh_->geom[static_cast<std::size_t>(mesh_->cell_geom[uc])];
            const double t = k::widthOfDepth(g, h);
            if (t <= 0.0) continue;
            const double c_wave = k::celerity(state_->cell_a[uc], t);
            dt = std::min(dt, k::faceCflDt(opts_.cfl, as / t, cell_u_[uc], c_wave));
        }
    }

    if (dt >= std::numeric_limits<double>::max() * 0.5) dt = 1.0e30;
    return dt;
}

// ===========================================================================
// Scalar reconstruction — the anti-diffusion layer (plan §3.2)
// ===========================================================================

void ExplicitFvSolver::reconstructScalars(double dt) {
    const int ns = state_->n_species;
    if (ns <= 0) return;

    const int nc = mesh_->n_cells();
    const int nf = mesh_->n_faces();

    for (int s = 0; s < ns; ++s) {
        const auto sbase = static_cast<std::size_t>(s) * static_cast<std::size_t>(nc);
        const auto fbase = static_cast<std::size_t>(s) * static_cast<std::size_t>(nf);

        // ---- per-cell limited slope along the chain ------------------------
        if (opts_.scalar_scheme == ScalarScheme::UPWIND) {
            std::fill(cell_slope_.begin(), cell_slope_.end(), 0.0);
        } else {
            for (int ch = 0; ch < mesh_->n_chains(); ++ch) {
                const int b = mesh_->chain_ptr[static_cast<std::size_t>(ch)];
                const int e = mesh_->chain_ptr[static_cast<std::size_t>(ch) + 1];
                for (int i = b; i < e; ++i) {
                    const int c = mesh_->chain_cells[static_cast<std::size_t>(i)];
                    const auto uc = static_cast<std::size_t>(c);
                    if (i == b || i == e - 1) { cell_slope_[uc] = 0.0; continue; }
                    const int cm = mesh_->chain_cells[static_cast<std::size_t>(i - 1)];
                    const int cp = mesh_->chain_cells[static_cast<std::size_t>(i + 1)];
                    const auto um = static_cast<std::size_t>(cm);
                    const auto up = static_cast<std::size_t>(cp);
                    const double dm = 0.5 * (mesh_->cell_dx[um] + mesh_->cell_dx[uc]);
                    const double dp = 0.5 * (mesh_->cell_dx[uc] + mesh_->cell_dx[up]);
                    const double gm = (state_->cell_phi[sbase + uc] -
                                       state_->cell_phi[sbase + um]) / dm;
                    const double gp = (state_->cell_phi[sbase + up] -
                                       state_->cell_phi[sbase + uc]) / dp;
                    // Chain-space slope → the cell's OWN axis.
                    cell_slope_[uc] =
                        limitSlope(gm, gp, opts_.limiter) *
                        static_cast<double>(mesh_->chain_dir[static_cast<std::size_t>(i)]);
                }
            }
        }

        // ---- face values ----------------------------------------------------
        for (const int f : active_faces_) {
            const auto uf = static_cast<std::size_t>(f);
            const int cl = mesh_->face_cl[uf];
            const int cr = mesh_->face_cr[uf];
            // A node ghost presents the interior cell's own value (zero
            // gradient). That keeps a uniform field uniform under inflow, which
            // is the §6.11(a) gate, without inventing a node concentration this
            // solver does not track.
            const int el = (cl >= 0) ? cl : cr;
            const int er = (cr >= 0) ? cr : cl;
            const auto ul = static_cast<std::size_t>(el);
            const auto ur = static_cast<std::size_t>(er);

            double phil = state_->cell_phi[sbase + ul];
            double phir = state_->cell_phi[sbase + ur];
            if (cl >= 0) {
                const double sign = static_cast<double>(mesh_->face_dir_l[uf]);
                phil += sign * cell_slope_[ul] * 0.5 * mesh_->cell_dx[ul];
            }
            if (cr >= 0) {
                const double sign = -static_cast<double>(mesh_->face_dir_r[uf]);
                phir += sign * cell_slope_[ur] * 0.5 * mesh_->cell_dx[ur];
            }

            if (opts_.scalar_scheme == ScalarScheme::QUICKEST_ULTIMATE) {
                // QUICKEST needs TWO upstream cells. Where the stencil exists
                // it is 3rd order; where it does not — the first cell below a
                // manhole with several inflowing pipes, or a short conduit —
                // it degrades, and the MUSCL value computed above is the
                // fallback. In COARSE mode a sewer spends much of its length in
                // that degraded regime, which is exactly what the §6.11(d)
                // junction-density sweep is there to quantify.
                const bool fwd = (f_sstar_[uf] >= 0.0);
                const int cu = fwd ? cl : cr;         // upwind cell
                const int cd = fwd ? cr : cl;         // downwind cell
                if (cu >= 0 && cd >= 0) {
                    const auto uu = static_cast<std::size_t>(cu);
                    const int chn = mesh_->cell_chain[uu];
                    const int pos = mesh_->cell_chain_pos[uu];
                    const int cb = mesh_->chain_ptr[static_cast<std::size_t>(chn)];
                    const int ce = mesh_->chain_ptr[static_cast<std::size_t>(chn) + 1];
                    // Step one further upstream ALONG THE FLOW, which is the
                    // downwind cell's opposite neighbour in the chain.
                    const int dpos = mesh_->cell_chain_pos[static_cast<std::size_t>(cd)];
                    const int upos = pos + (pos - dpos);   // one further upstream
                    if (upos >= 0 && upos < ce - cb) {
                        const int cuu = mesh_->chain_cells[
                            static_cast<std::size_t>(cb + upos)];
                        const auto uuu = static_cast<std::size_t>(cuu);
                        const double pu  = state_->cell_phi[sbase + uu];
                        const double pd  = state_->cell_phi[sbase + static_cast<std::size_t>(cd)];
                        const double puu = state_->cell_phi[sbase + uuu];
                        const double dx  = mesh_->cell_dx[uu];
                        const double cr_no = std::min(
                            1.0, std::fabs(cell_u_[uu]) * dt / std::max(dx, 1.0e-12));
                        const double q = pd - 2.0 * pu + puu;
                        double pf = 0.5 * (pd + pu) - 0.5 * cr_no * (pd - pu) -
                                    (1.0 - cr_no * cr_no) / 6.0 * q;
                        // ULTIMATE monotonicity limiter (Leonard 1991), applied
                        // in normalized variables.
                        const double den = pd - puu;
                        if (std::fabs(den) > 1.0e-30) {
                            const double un_ = (pu - puu) / den;
                            if (un_ <= 0.0 || un_ >= 1.0) {
                                pf = pu;                       // non-monotone ⇒ upwind
                            } else {
                                double fn = (pf - puu) / den;
                                const double hi = std::min(1.0, un_ / std::max(cr_no, 1.0e-12));
                                fn = std::clamp(fn, un_, hi);
                                pf = puu + fn * den;
                            }
                        } else {
                            pf = pu;
                        }
                        if (fwd) phil = pf; else phir = pf;
                    }
                }
            }

            // Local-extremum clamp — the last line of defence for the "no new
            // extrema, no negative concentrations" contract (§6.11b). Both
            // higher-order reconstructions are TVD for PURE advection on a
            // fixed grid; here the cell areas are evolving underneath them
            // (reflections, wetting, drying), and QUICKEST's normalized-variable
            // limiter measurably loses monotonicity in that regime. Clamping the
            // face value into the bracket its own two cells span costs nothing
            // where the scheme is already monotone and restores the guarantee
            // where it is not.
            if (opts_.scalar_scheme != ScalarScheme::UPWIND) {
                const double a = state_->cell_phi[sbase + ul];
                const double b = state_->cell_phi[sbase + ur];
                const double lo = std::min(a, b);
                const double hi = std::max(a, b);
                phil = std::clamp(phil, lo, hi);
                phir = std::clamp(phir, lo, hi);
            }

            f_phi_l_[fbase + uf] = phil;
            f_phi_r_[fbase + uf] = phir;
        }

        // ---- flux assembly + Zalesak limiting ------------------------------
        limitSpeciesFluxes(s, dt);
    }
}

/**
 * @brief Assemble the face species fluxes and limit them (Zalesak FCT).
 *
 * @details Bracketing the reconstructed FACE value between its two cells is
 *          necessary but NOT sufficient for the discrete maximum principle:
 *          under reversing, unsteady flow with the cell areas evolving
 *          underneath the scalar, a face value legitimately inside its bracket
 *          can still drain more solute than its donor cell holds. Measured:
 *          QUICKEST-ULTIMATE produced −1.3e-3 on a step-function advection case
 *          with wall reflections, which is not acceptable in a water-quality
 *          model.
 *
 *          The fix has to limit the FLUX, not the result. Clipping the updated
 *          concentration would enforce the bound but destroy exact solute
 *          conservation (§6.11c) — the two properties trade off, and Zalesak
 *          (1979) is the construction that keeps both: blend each face's
 *          high-order flux back toward first-order upwind by the largest factor
 *          that no incident cell's bound rejects. The SAME limited flux updates
 *          both incident cells, so conservation is untouched.
 */
void ExplicitFvSolver::limitSpeciesFluxes(int species, double dt) {
    const int nc = mesh_->n_cells();
    const int nf = mesh_->n_faces();
    const auto cbase = static_cast<std::size_t>(species) *
                       static_cast<std::size_t>(nc);
    const auto fbase = static_cast<std::size_t>(species) *
                       static_cast<std::size_t>(nf);

    lo_flux_.assign(static_cast<std::size_t>(nf), 0.0);
    anti_flux_.assign(static_cast<std::size_t>(nf), 0.0);

    // Low-order (first-order upwind on the sign of the mass flux) and the
    // antidiffusive remainder.
    for (const int f : active_faces_) {
        const auto uf = static_cast<std::size_t>(f);
        const int cl = mesh_->face_cl[uf];
        const int cr = mesh_->face_cr[uf];
        const auto ul = static_cast<std::size_t>((cl >= 0) ? cl : cr);
        const auto ur = static_cast<std::size_t>((cr >= 0) ? cr : cl);
        const double fm = f_mass_[uf];
        const double lo = fm * ((fm >= 0.0) ? state_->cell_phi[cbase + ul]
                                            : state_->cell_phi[cbase + ur]);
        const double hi = k::speciesFlux(f_state_l_[uf], f_state_r_[uf],
                                         adjustedFlux(f), f_phi_l_[fbase + uf],
                                         f_phi_r_[fbase + uf], hllc_);
        lo_flux_[uf]   = lo;
        anti_flux_[uf] = hi - lo;
    }

    // Transported-diffused state under the low-order flux alone, plus the
    // local bounds it and its neighbours span.
    td_.assign(static_cast<std::size_t>(nc), 0.0);
    anew_.assign(static_cast<std::size_t>(nc), 0.0);
    for (int c = 0; c < nc; ++c) {
        const auto uc = static_cast<std::size_t>(c);
        if (!cell_active_[uc]) { td_[uc] = state_->cell_phi[cbase + uc];
                                 anew_[uc] = state_->cell_a[uc]; continue; }
        const int faces[2]    = {mesh_->cell_face0[uc], mesh_->cell_face1[uc]};
        const int8_t sides[2] = {mesh_->cell_side0[uc], mesh_->cell_side1[uc]};
        double dA = 0.0, dm = 0.0;
        for (int e = 0; e < 2; ++e) {
            const auto uf = static_cast<std::size_t>(faces[e]);
            const double sg = (sides[e] == 0) ? -1.0 : 1.0;
            dA += sg * f_mass_[uf];
            dm += sg * lo_flux_[uf];
        }
        const double inv_dx = 1.0 / mesh_->cell_dx[uc];
        const double a_new = std::max(0.0, state_->cell_a[uc] + dt * dA * inv_dx);
        anew_[uc] = a_new;
        const double m = state_->cell_a[uc] * state_->cell_phi[cbase + uc] +
                         dt * dm * inv_dx;
        td_[uc] = (a_new > k::kDryArea) ? m / a_new : state_->cell_phi[cbase + uc];
    }

    rplus_.assign(static_cast<std::size_t>(nc), 1.0);
    rminus_.assign(static_cast<std::size_t>(nc), 1.0);
    for (int c = 0; c < nc; ++c) {
        const auto uc = static_cast<std::size_t>(c);
        if (!cell_active_[uc]) continue;
        const int faces[2]    = {mesh_->cell_face0[uc], mesh_->cell_face1[uc]};
        const int8_t sides[2] = {mesh_->cell_side0[uc], mesh_->cell_side1[uc]};

        double pmax = std::max(state_->cell_phi[cbase + uc], td_[uc]);
        double pmin = std::min(state_->cell_phi[cbase + uc], td_[uc]);
        double pplus = 0.0, pminus = 0.0;
        for (int e = 0; e < 2; ++e) {
            const auto uf = static_cast<std::size_t>(faces[e]);
            const int cl = mesh_->face_cl[uf];
            const int cr = mesh_->face_cr[uf];
            const int nb = (cl == c) ? cr : cl;
            if (nb >= 0) {
                const auto un = static_cast<std::size_t>(nb);
                pmax = std::max({pmax, state_->cell_phi[cbase + un], td_[un]});
                pmin = std::min({pmin, state_->cell_phi[cbase + un], td_[un]});
            }
            const double sg = (sides[e] == 0) ? -1.0 : 1.0;
            const double a = sg * anti_flux_[uf];
            if (a > 0.0) pplus += a; else pminus -= a;
        }
        const double cap = anew_[uc] * mesh_->cell_dx[uc] / dt;
        if (pplus  > 0.0) rplus_[uc]  = std::min(1.0, (pmax - td_[uc]) * cap / pplus);
        if (pminus > 0.0) rminus_[uc] = std::min(1.0, (td_[uc] - pmin) * cap / pminus);
        rplus_[uc]  = std::max(0.0, rplus_[uc]);
        rminus_[uc] = std::max(0.0, rminus_[uc]);
    }

    for (const int f : active_faces_) {
        const auto uf = static_cast<std::size_t>(f);
        const double a = anti_flux_[uf];
        double coef = 1.0;
        if (a != 0.0) {
            const int cl = mesh_->face_cl[uf];
            const int cr = mesh_->face_cr[uf];
            // The face contributes −a to its LEFT cell and +a to its RIGHT one.
            const double from_r = (cr >= 0)
                ? ((a > 0.0) ? rplus_[static_cast<std::size_t>(cr)]
                             : rminus_[static_cast<std::size_t>(cr)])
                : 1.0;
            const double from_l = (cl >= 0)
                ? ((a > 0.0) ? rminus_[static_cast<std::size_t>(cl)]
                             : rplus_[static_cast<std::size_t>(cl)])
                : 1.0;
            coef = std::min(from_r, from_l);
        }
        f_phi_flux_[fbase + uf] = lo_flux_[uf] + coef * a;
    }
}

/// The flux record with the positivity-scaled mass flux substituted in, so the
/// species rides on exactly the water the hydrodynamic update moved.
kernels::FaceFlux ExplicitFvSolver::adjustedFlux(int face) const {
    kernels::FaceFlux f = f_flux_[static_cast<std::size_t>(face)];
    f.mass = f_mass_[static_cast<std::size_t>(face)];
    return f;
}

// ===========================================================================
// Second-order state reconstruction (FV_ORDER 2)
// ===========================================================================

/**
 * @brief Limited linear slopes of the free surface and velocity per cell.
 *
 * @details MUSCL on (η, u) rather than on (A, Q). Two reasons, both structural:
 *
 *   * **Well-balancedness survives.** A lake at rest has η constant, so every
 *     slope is exactly zero and the second-order scheme degenerates to the
 *     first-order one — which §6.1 already proves is exact. Reconstructing A or
 *     h instead would give every cell on a sloping bed a non-zero slope at
 *     rest, and lake-at-rest would hold only to truncation error.
 *   * **The bed is not limited.** It is linear within a conduit, so its
 *     gradient is exact (`cell_dzdx`); depth at a face is then the difference
 *     of two separately reconstructed quantities, η and z, which is what keeps
 *     the two consistent.
 *
 * The companion piece is the centred bed source added in updateCells: at second
 * order the two faces of a cell see DIFFERENT reconstructed depths, so the
 * hydrostatic flux difference no longer vanishes at rest on its own.
 */
void ExplicitFvSolver::reconstructState() {
    const int nc = mesh_->n_cells();
    if (opts_.order < 2) {
        std::fill(cell_eta_slope_.begin(), cell_eta_slope_.end(), 0.0);
        std::fill(cell_u_slope_.begin(), cell_u_slope_.end(), 0.0);
        std::fill(cell_ho2_.begin(), cell_ho2_.end(), char{0});
        return;
    }

    // Admissibility. Linear reconstruction is only meaningful where the cell is
    // small compared with the scales it is reconstructing. The binding one here
    // is the BED: a cell whose ends differ in elevation by an appreciable
    // fraction of the water depth cannot carry a linear free surface across
    // itself, and extrapolating the bed to its faces gives a negative depth
    // upstream. Measured on a 6000 ft conduit meshed as ONE cell (COARSE mode,
    // 12 ft fall, ~2 ft deep): unguarded second order drove the steady
    // discharge from 300 cfs to 0. Cells that fail the test fall back to the
    // first-order path, which is exactly what the flag gates.
    for (int c = 0; c < nc; ++c) {
        const auto uc = static_cast<std::size_t>(c);
        const double h = state_->cell_h[uc];
        const double fall = std::fabs(mesh_->cell_dzdx[uc]) * mesh_->cell_dx[uc];
        cell_ho2_[uc] = (h > k::kDryDepth && fall < 0.5 * h) ? char{1} : char{0};
    }

    for (int ch = 0; ch < mesh_->n_chains(); ++ch) {
        const int b = mesh_->chain_ptr[static_cast<std::size_t>(ch)];
        const int e = mesh_->chain_ptr[static_cast<std::size_t>(ch) + 1];
        for (int i = b; i < e; ++i) {
            const int c = mesh_->chain_cells[static_cast<std::size_t>(i)];
            const auto uc = static_cast<std::size_t>(c);
            if (i == b || i == e - 1 || !cell_active_[uc] || !cell_ho2_[uc]) {
                cell_eta_slope_[uc] = 0.0;
                cell_u_slope_[uc]   = 0.0;
                continue;
            }
            const int cm = mesh_->chain_cells[static_cast<std::size_t>(i - 1)];
            const int cp = mesh_->chain_cells[static_cast<std::size_t>(i + 1)];
            const auto um = static_cast<std::size_t>(cm);
            const auto up = static_cast<std::size_t>(cp);
            // A dry neighbour carries no usable state; falling back to first
            // order at the wet/dry front is what keeps the front positive.
            if (state_->cell_h[um] <= k::kDryDepth ||
                state_->cell_h[up] <= k::kDryDepth ||
                state_->cell_h[uc] <= k::kDryDepth) {
                cell_eta_slope_[uc] = 0.0;
                cell_u_slope_[uc]   = 0.0;
                continue;
            }
            const double dm = 0.5 * (mesh_->cell_dx[um] + mesh_->cell_dx[uc]);
            const double dp = 0.5 * (mesh_->cell_dx[uc] + mesh_->cell_dx[up]);
            const double dirc =
                static_cast<double>(mesh_->chain_dir[static_cast<std::size_t>(i)]);
            const double dirm =
                static_cast<double>(mesh_->chain_dir[static_cast<std::size_t>(i - 1)]);
            const double dirp =
                static_cast<double>(mesh_->chain_dir[static_cast<std::size_t>(i + 1)]);

            // Deadband on the free-surface difference. eta is reconstructed
            // from the stored area, so a lake at rest carries ulp-level noise
            // in eta; without this the limiter turns that noise into a real
            // slope (superbee doubles it), the momentum equation integrates the
            // resulting face imbalance, and machine-precision lake-at-rest
            // decays over a long run. Same reasoning — and the same 1e-12 ft
            // threshold — as the 2D marcher's kEtaDeadband.
            const double d_eta_m = cell_eta_[uc] - cell_eta_[um];
            const double d_eta_p = cell_eta_[up] - cell_eta_[uc];
            if (std::fabs(d_eta_m) < k::kEtaDeadband &&
                std::fabs(d_eta_p) < k::kEtaDeadband) {
                cell_eta_slope_[uc] = 0.0;
            } else {
                cell_eta_slope_[uc] =
                    limitSlope(d_eta_m / dm, d_eta_p / dp, opts_.limiter) * dirc;
            }

            // u is ODD under an axis flip, so neighbours are brought into this
            // cell's frame before differencing.
            const double um_axis = cell_u_[um] * dirm * dirc;
            const double up_axis = cell_u_[up] * dirp * dirc;
            const double gm_u = (cell_u_[uc] - um_axis) / dm;
            const double gp_u = (up_axis - cell_u_[uc]) / dp;
            cell_u_slope_[uc] = limitSlope(gm_u, gp_u, opts_.limiter);
        }
    }
}

// ===========================================================================
// Face reconstruction + flux
// ===========================================================================

void ExplicitFvSolver::faceSide(int face, int cell, int node, double zstar,
                                int dir, double u_interior,
                                k::FaceState& out, double& i1_raw,
                                double& z_side, bool measure_only) const {
    const auto uf = static_cast<std::size_t>(face);
    const FvGeometry* g = nullptr;
    double eta = 0.0, h_raw = 0.0, u = 0.0;
    z_side = 0.0;

    // The section this FACE reconstructs in — the same one for both sides, so
    // the Riemann problem is posed between states in a single geometry. It is
    // the cells' own section everywhere except at a width step between two
    // conduits (mesh.face_geom), where using each side's own section would
    // leave the step's wall exerting no force on the flow.
    const FvGeometry* gf =
        &mesh_->geom[static_cast<std::size_t>(mesh_->face_geom[uf])];

    if (cell >= 0) {
        const auto uc = static_cast<std::size_t>(cell);
        g     = &mesh_->geom[static_cast<std::size_t>(mesh_->cell_geom[uc])];
        if (opts_.order < 2 || !cell_ho2_[uc]) {
            // First order: the cell-centred state, taken from the stored depth
            // rather than recomputed as eta - z_b. Those differ by an ulp, and
            // lake-at-rest is asserted at machine precision — recomputing it
            // here is enough to break §6.1.
            eta    = cell_eta_[uc];
            h_raw  = state_->cell_h[uc];
            u      = static_cast<double>(dir) * cell_u_[uc];
            z_side = mesh_->cell_zb[uc];
        } else {
            // Extrapolate to the face along the cell's own axis. `half` is
            // signed: positive toward the cell's downstream end. The BED is
            // extrapolated with its exact gradient and the free surface with
            // its limited one, and the depth is their difference — which is
            // what keeps a lake at rest exactly balanced (zero eta slope leaves
            // h varying with the bed alone, as it must).
            const bool is_left = (mesh_->face_cl[uf] == cell);
            const double sgn = is_left ? static_cast<double>(mesh_->face_dir_l[uf])
                                       : -static_cast<double>(mesh_->face_dir_r[uf]);
            const double half = sgn * 0.5 * mesh_->cell_dx[uc];
            z_side = mesh_->cell_zb[uc] + mesh_->cell_dzdx[uc] * half;
            eta    = cell_eta_[uc] + cell_eta_slope_[uc] * half;
            h_raw  = std::max(0.0, eta - z_side);
            u      = static_cast<double>(dir) *
                     (cell_u_[uc] + cell_u_slope_[uc] * half);
        }
    } else {
        const int other = (mesh_->face_cl[uf] >= 0) ? mesh_->face_cl[uf]
                                                    : mesh_->face_cr[uf];
        const auto uo = static_cast<std::size_t>(other);
        g = &mesh_->geom[static_cast<std::size_t>(mesh_->cell_geom[uo])];
        if (node >= 0) {
            // Node ghost: depth from the node head over the conduit invert at
            // this end, velocity extrapolated from the interior cell. Sending it
            // through the SAME Riemann solver as an interior face is what gives
            // wave reflection off the node, choking and supercritical approach
            // flow for free — instead of a dQ/dH linearization (plan §3.4).
            const auto und = static_cast<std::size_t>(node);
            eta   = state_->node_head[und];
            h_raw = std::max(0.0, eta - mesh_->face_zb[uf]);
            u     = u_interior;
            z_side = mesh_->face_zb[uf];
            // PASS-THROUGH junction: the ghost presents the FAR cell's full
            // centred state — surface, depth, bed — so together with the far
            // velocity (computeFaceFlux) the two node faces reproduce the
            // direct spliced face this pair replaces. Presenting the solved
            // node head at the node invert instead loses ~1 mm of head per
            // junction to split-Riemann dissipation, which a 199-junction
            // subcritical chain integrates into a 0.23 m backwater
            // (macdonald-long-sub L1 0.228 → 0.0064 with this treatment).
            if (!node_pass_.empty() && node_pass_[und] &&
                algebraicActive(node)) {
                const int nb2 = mesh_->node_face_ptr[und];
                const int ne2 = mesh_->node_face_ptr[und + 1];
                for (int p2 = nb2; p2 < ne2; ++p2) {
                    const int of = mesh_->node_face_idx[
                        static_cast<std::size_t>(p2)];
                    if (of == face) continue;
                    const auto uof = static_cast<std::size_t>(of);
                    const int oc = (mesh_->face_cl[uof] >= 0)
                                       ? mesh_->face_cl[uof]
                                       : mesh_->face_cr[uof];
                    if (oc >= 0) {
                        const auto uoc = static_cast<std::size_t>(oc);
                        eta    = cell_eta_[uoc];
                        h_raw  = state_->cell_h[uoc];
                        z_side = mesh_->cell_zb[uoc];
                    }
                }
            }
        } else {
            // Closed end (dead-end conduit, or a test wall): mirror the interior
            // state and reverse its velocity. The Riemann solver then returns
            // zero mass flux and the correct hydrostatic pressure — a reflecting
            // wall, rather than the dry-bed OUTflow a null ghost would produce.
            eta   = cell_eta_[uo];
            h_raw = state_->cell_h[uo];
            u     = -u_interior;
            z_side = mesh_->face_zb[uf];
        }
    }

    // measure_only asks for the reconstructed BED only — it is pass 1 of
    // computeFaceFlux, whose sole output is z* = max(z_L, z_R). Every z_side
    // branch above is arithmetic on stored bed values (cell_zb, the second-order
    // dzdx extrapolation, face_zb, or the pass-through far cell's bed), so the
    // geometry below is not needed: leaving it here cost two virtual
    // getAofY/getRofY-class evaluations per face side whose result the probe
    // pass discarded (its i1 out-param is written twice and never read).
    if (measure_only) { i1_raw = 0.0; out = k::FaceState{}; return; }

    // i1_raw stays in the CELL's own section — it is the cell's true
    // hydrostatic moment, and the difference from the reconstructed one is the
    // correction that puts the balance back (computeFaceFlux).
    i1_raw = (h_raw > k::kDryDepth)
                 ? k::i1OfDepth(*g, h_raw, k::areaOfDepth(*g, h_raw)) : 0.0;

    const double h_star = std::max(0.0, eta - zstar);
    if (h_star <= k::kDryDepth) {
        out = k::FaceState{};
        return;
    }
    // …while the reconstructed state is evaluated in the FACE's section, which
    // both sides share. Where they are the same section (every prismatic face)
    // this is the identical computation.
    out.a  = k::areaOfDepth(*gf, h_star);
    out.u  = u;
    out.q  = out.a * u;
    out.c  = k::celerity(out.a, k::widthOfDepth(*gf, h_star));
    out.i1 = k::i1OfDepth(*gf, h_star, out.a);
}

void ExplicitFvSolver::computeFluxes() {
    const int n_act = static_cast<int>(active_faces_.size());

#ifdef SWMM_USE_OPENMP
#pragma omp parallel for schedule(static) if (n_act >= kOmpMinFaces)
#endif
    for (int a = 0; a < n_act; ++a)
        computeFaceFlux(active_faces_[static_cast<std::size_t>(a)]);
    total_flux_ += n_act;
}

void ExplicitFvSolver::computeFaceFlux(int f) {
    {
        const auto uf = static_cast<std::size_t>(f);
        const int cl = mesh_->face_cl[uf];
        const int cr = mesh_->face_cr[uf];
        const int nd = mesh_->face_node[uf];

        // Pass 1: resolve each side's reconstructed bed, so the hydrostatic
        // z* = max(z_L, z_R) is built from the values the flux will actually
        // use. At first order these are the cell-centred beds and this is a
        // no-op; at second order taking z* from the cell CENTRES instead would
        // reintroduce the very imbalance the reconstruction removes.
        k::FaceState probe;
        double zl = 0.0, zr = 0.0, i1probe = 0.0;
        faceSide(f, cl, nd, 0.0, mesh_->face_dir_l[uf], 0.0, probe, i1probe, zl, true);
        faceSide(f, cr, nd, 0.0, mesh_->face_dir_r[uf], 0.0, probe, i1probe, zr, true);
        const double zstar = std::max(zl, zr);

        // The ghost inherits the interior cell's velocity, expressed in the
        // face frame — resolve it first, regardless of which side the node is on.
        const int interior = (cl >= 0) ? cl : cr;
        double u_int =
            (interior >= 0)
                ? static_cast<double>((cl >= 0) ? mesh_->face_dir_l[uf]
                                                : mesh_->face_dir_r[uf]) *
                      cell_u_[static_cast<std::size_t>(interior)]
                : 0.0;

        // PASS-THROUGH junction: the ghost carries the FAR cell's velocity —
        // the water that actually arrives through the node — instead of its
        // own side's, which never transmitted momentum across the interface
        // (bump-subcritical L1 0.105 → 0.008 with this treatment; see the
        // matching depth/bed presentation in faceSide).
        if (nd >= 0 && !node_pass_.empty() &&
            node_pass_[static_cast<std::size_t>(nd)] && algebraicActive(nd)) {
            const auto und2 = static_cast<std::size_t>(nd);
            const int nb2 = mesh_->node_face_ptr[und2];
            const int ne2 = mesh_->node_face_ptr[und2 + 1];
            double s_this = 0.0, s_far = 0.0;
            int oc = -1;
            std::size_t uof = 0;
            for (int p2 = nb2; p2 < ne2; ++p2) {
                const auto up2 = static_cast<std::size_t>(p2);
                const int of = mesh_->node_face_idx[up2];
                if (of == f) { s_this = mesh_->node_face_sign[up2]; continue; }
                uof   = static_cast<std::size_t>(of);
                s_far = mesh_->node_face_sign[up2];
                oc    = (mesh_->face_cl[uof] >= 0) ? mesh_->face_cl[uof]
                                                   : mesh_->face_cr[uof];
            }
            if (oc >= 0 && s_this != 0.0 && s_far != 0.0) {
                // Through-flow continuity: inflow at the far face is outflow
                // here; map the far cell's velocity from its face frame into
                // this one.
                const double odir = (mesh_->face_cl[uof] >= 0)
                    ? static_cast<double>(mesh_->face_dir_l[uof])
                    : static_cast<double>(mesh_->face_dir_r[uof]);
                u_int = -s_this * s_far * odir *
                        cell_u_[static_cast<std::size_t>(oc)];
            }
        }

        k::FaceState L, R;
        double i1l = 0.0, i1r = 0.0, zdummy = 0.0;
        faceSide(f, cl, nd, zstar, mesh_->face_dir_l[uf], u_int, L, i1l, zdummy, false);
        faceSide(f, cr, nd, zstar, mesh_->face_dir_r[uf], u_int, R, i1r, zdummy, false);

        k::FaceFlux fl = k::riemannFlux(L, R);

        // Non-prismatic pass-through junction — RESOLVED 2026-08-13 by giving
        // the face its own section (mesh.face_geom, see faceSide). Both sides
        // of a width step are now reconstructed in ONE shared section, so the
        // pair poses a single well-posed Riemann problem and the per-side
        // hydrostatic correction below IS the wall-pressure term.
        //
        // The earlier additive closures failed because they were bolted onto a
        // flux that was ill-posed to begin with: each side was reconstructed in
        // its OWN geometry, so no added term could fix it and each one
        // double-counted (p2d-sub-short nx=20: splice-only l1 0.186, plus
        // convective ½Q̂²Δ(1/A) → 0.313, plus wall-pressure ½gΔI1(h̄) → 0.286).
        // With the shared face section instead, the SWASHES §3.5 family goes
        // from l1 5.4-16.5% to 0.17-0.60% and the jump on p2d-jump-long lands
        // within half a cell (7.50 m → 0.50 m). Prismatic decks are
        // bit-identical: there the face section IS the cells' section.

        // Flap gate. A check valve, not a wall: the face stays open while the
        // flux runs the permitted way and closes the instant it would reverse.
        // Closing it means MIRRORING the interior state across the face — equal
        // depth, reversed velocity — which is the standard reflective boundary:
        // it returns exactly zero mass flux and leaves the interior side its
        // own hydrostatic pressure, so the gate holds water back without
        // inventing momentum. Zeroing f_mass alone would leave a momentum flux
        // the closed gate never transmits.
        const uint8_t gate = mesh_->face_gate[uf];
        if (gate != 0 && ((fl.mass > 0.0 && (gate & 1u)) ||
                          (fl.mass < 0.0 && (gate & 2u)))) {
            if (cl < 0) { L = R; L.u = -R.u; L.q = -R.q; }
            else        { R = L; R.u = -L.u; R.q = -L.q; }
            fl = k::riemannFlux(L, R);
            fl.mass = 0.0;   // exact, not merely symmetric to rounding
        }

        // Culvert inlet control (FHWA HEC-5). Applied HERE, as a cap on the
        // inflow crossing the culvert's upstream face, rather than by
        // overwriting links.flow after the fact: publishFv books the node
        // ledger from these very fluxes, so a post-hoc rewrite left the
        // reported flow and the continuity balance describing different runs.
        //
        // a_full is the PER-BARREL area while the flux is the aggregate
        // section, which reproduces legacy culvert.c exactly — it likewise
        // compares Link.newFlow against a per-barrel capacity.
        const int cvr = mesh_->face_culvert[uf];
        if (cvr >= 0 && fl.mass > 0.0 && nd >= 0) {
            const FvGeometry& gc = mesh_->geom[static_cast<std::size_t>(cvr)];
            const double head =
                state_->node_head[static_cast<std::size_t>(nd)] - mesh_->face_zb[uf];
            double dqdh = 0.0;
            const double q_cap = hydkernels::culvertInflow(
                fl.mass, head, gc.y_full, gc.xs.a_full, gc.slope,
                gc.culvert_curve, gc.culvert_mitered != 0, dqdh);
            if (q_cap < fl.mass) {
                // Inlet control means the INLET is the control section, so the
                // face becomes a prescribed-discharge boundary and its flux is
                // the physical flux at that discharge, F = [Q, Q²/A + g·I₁],
                // evaluated on the upwind (node-side) state. Scaling the
                // Riemann flux instead is not self-consistent: scaling
                // momentum with mass strips the pressure that resists the
                // flow (a 58 ft/s startup spike), and leaving momentum alone
                // strips nothing and gives 151 ft/s.
                const k::FaceState& up = (cl < 0) ? L : R;
                const double a_up = (up.a > k::kDryArea) ? up.a : k::kDryArea;
                fl.mass = q_cap;
                fl.mom  = q_cap * q_cap / a_up + k::kGravity * up.i1;
                inlet_control_[static_cast<std::size_t>(cvr)] = 1;
            }
        }

        f_mass_[uf]  = fl.mass;
        f_mom_[uf]   = fl.mom;
        f_sstar_[uf] = fl.sstar;
        f_state_l_[uf] = L;
        f_state_r_[uf] = R;
        f_flux_[uf]    = fl;
        // Audusse well-balanced correction — a CELL source, so it exists only
        // on sides that are cells.
        f_corr_l_[uf] = (cl >= 0) ? k::kGravity * (i1l - L.i1) : 0.0;
        f_corr_r_[uf] = (cr >= 0) ? k::kGravity * (i1r - R.i1) : 0.0;
        f_scale_[uf]  = 1.0;
    }
}

// ===========================================================================
// Positivity limiting
// ===========================================================================

void ExplicitFvSolver::limitPositivity(double dt) {
    const int nc = mesh_->n_cells();
    const int nn = mesh_->n_nodes();
    static thread_local std::vector<double> out_cell, out_node;
    out_cell.assign(static_cast<std::size_t>(nc), 0.0);
    out_node.assign(static_cast<std::size_t>(nn), 0.0);

    for (const int f : active_faces_) {
        const auto uf = static_cast<std::size_t>(f);
        const double fa = f_mass_[uf];
        if (fa == 0.0) continue;
        if (fa > 0.0) {                     // exporting side is L
            if (mesh_->face_cl[uf] >= 0)
                out_cell[static_cast<std::size_t>(mesh_->face_cl[uf])] += fa;
            else if (mesh_->face_node[uf] >= 0)
                out_node[static_cast<std::size_t>(mesh_->face_node[uf])] += fa;
        } else {                            // exporting side is R
            if (mesh_->face_cr[uf] >= 0)
                out_cell[static_cast<std::size_t>(mesh_->face_cr[uf])] -= fa;
            else if (mesh_->face_node[uf] >= 0)
                out_node[static_cast<std::size_t>(mesh_->face_node[uf])] -= fa;
        }
    }

    for (int c = 0; c < nc; ++c) {
        const auto uc = static_cast<std::size_t>(c);
        if (out_cell[uc] <= 0.0) { out_cell[uc] = 1.0; continue; }
        out_cell[uc] = k::positivityScale(state_->cell_a[uc] * mesh_->cell_dx[uc],
                                          out_cell[uc], dt);
    }
    for (int n = 0; n < nn; ++n) {
        const auto un = static_cast<std::size_t>(n);
        if (out_node[un] <= 0.0) { out_node[un] = 1.0; continue; }
        // An algebraic junction is an interface: the water its faces carry
        // lives in the cells (whose own positivity still guards it), not in a
        // node bucket. Capping the draw at the bucket's volume is exactly the
        // artificial transshipment bound this mode exists to remove.
        if (algebraicActive(n)) { out_node[un] = 1.0; continue; }
        out_node[un] = k::positivityScale(state_->node_volume[un], out_node[un], dt);
    }

    for (const int f : active_faces_) {
        const auto uf = static_cast<std::size_t>(f);
        const double fa = f_mass_[uf];
        if (fa == 0.0) continue;
        double s;
        if (fa > 0.0) {
            s = (mesh_->face_cl[uf] >= 0)
                    ? out_cell[static_cast<std::size_t>(mesh_->face_cl[uf])]
                    : ((mesh_->face_node[uf] >= 0)
                           ? out_node[static_cast<std::size_t>(mesh_->face_node[uf])]
                           : 1.0);
        } else {
            s = (mesh_->face_cr[uf] >= 0)
                    ? out_cell[static_cast<std::size_t>(mesh_->face_cr[uf])]
                    : ((mesh_->face_node[uf] >= 0)
                           ? out_node[static_cast<std::size_t>(mesh_->face_node[uf])]
                           : 1.0);
        }
        if (s < 1.0) {
            // The IDENTICAL scaled flux updates both incident volumes, so
            // conservation is untouched by the limiter.
            f_mass_[uf] *= s;
            f_mom_[uf]  *= s;
            f_scale_[uf] = s;
        }
    }
}

// ===========================================================================
// Semi-implicit node coupling (plan §7B.1)
// ===========================================================================

void ExplicitFvSolver::relaxNodeFluxes(double dt, const FvStepForcing& forcing) {
    const bool semi = (opts_.node_coupling == NodeCoupling::SEMI_IMPLICIT);
    all_faces_live_ = true;      // global path: every face was just computed
    const int nn = mesh_->n_nodes();
    for (int n = 0; n < nn; ++n) {
        // The algebraic solve is not a relaxation — it applies under either
        // coupling mode. Storage nodes (and demoted ponding junctions) keep
        // the semi-implicit correction.
        if (algebraicActive(n))  solveAlgebraicNode(n, dt, forcing);
        else if (semi)           relaxOneNode(n, dt, forcing);
    }
}

// dV/dH at a given depth — the storage response the correction is damped
// against. Mirrors refreshNodeAreas' three cases, but evaluated at an
// ARBITRARY depth rather than at the node's current one, because that is the
// whole point of iterating: past the first sweep the head has moved.
double ExplicitFvSolver::nodeStorageSlope(int node, double depth) const {
    const auto un = static_cast<std::size_t>(node);
    const double full = mesh_->node_full_depth[un];
    if (mesh_->node_can_pond[un] && full > 0.0 && depth > full)
        return mesh_->node_ponded_area[un];
    if (mesh_->node_vol_off[un] < 0)          // junction: V = A_s·depth, A_s fixed
        return state_->node_surf_area[un];
    const double dd = mesh_->node_vol_dmax[un] /
                      static_cast<double>(kNodeVolSamples - 1);
    const double d = std::max(0.0, depth);
    return std::max((nodeVolumeFromDepth(node, d + dd) -
                     nodeVolumeFromDepth(node, d)) / dd,
                    constants::MIN_SURFAREA);
}

void ExplicitFvSolver::relaxOneNode(int n, double dt,
                                    const FvStepForcing& forcing) {
    {
        const auto un = static_cast<std::size_t>(n);
        if (mesh_->node_kind[un] == kNodeVirtual) return;
        // A prescribed head has no continuity equation to damp — the stage is
        // imposed and the exchange is whatever the Riemann solver produced.
        if (forcing.node_fixed_head && std::isfinite(forcing.node_fixed_head[un]))
            return;
        if (!(state_->node_surf_area[un] > 0.0)) return;

        const int b = mesh_->node_face_ptr[un];
        const int e = mesh_->node_face_ptr[un + 1];

        const double q_lat = nodeLateral(forcing, un) + node_qstruct_[un];
        const double invert  = mesh_->node_invert[un];
        const double h_start = state_->node_head[un];
        const double v_start =
            nodeVolumeFromDepth(n, std::max(0.0, h_start - invert));

        // One sweep is the original scheme, and stays bit-identical to it: the
        // residual is the raw face sum, and the resistance, the storage area
        // and the fluxes are all the ones the substep started with. Additional
        // sweeps re-evaluate all three at the head the previous sweep landed
        // on, so the node converges on its own continuity equation rather than
        // on a tangent taken at the start of the step (plan §7B.8).
        const int sweeps = std::max(1, opts_.node_picard_sweeps);
        double h_k = h_start;

        for (int it = 0; it < sweeps; ++it) {
            // Σ ∂F/∂H over the incident faces, from the characteristic relation
            // for a simple wave: |dQ/dH| = gA/c = √(g·A·T) at the ghost state.
            // Always a RESISTANCE — raising the head drives more out and lets
            // less in, on either side of the face — so the denominator below
            // can only grow, and the correction can only damp.
            double sum_f = 0.0, resist = 0.0;
            for (int p = b; p < e; ++p) {
                const auto up = static_cast<std::size_t>(p);
                const int f = mesh_->node_face_idx[up];
                const auto uf = static_cast<std::size_t>(f);
                sum_f += mesh_->node_face_sign[up] * f_mass_[uf];

                const int cell = (mesh_->face_cl[uf] >= 0) ? mesh_->face_cl[uf]
                                                           : mesh_->face_cr[uf];
                if (cell < 0) continue;
                const auto uc = static_cast<std::size_t>(cell);
                const FvGeometry& g =
                    mesh_->geom[static_cast<std::size_t>(mesh_->cell_geom[uc])];
                const double hg = h_k - mesh_->face_zb[uf];
                if (hg <= k::kDryDepth) continue;
                const double ag = k::areaOfDepth(g, hg);
                const double tg = k::widthOfDepth(g, hg);
                if (ag <= k::kDryArea || tg <= 0.0) continue;
                resist += std::sqrt(k::kGravity * ag * tg);
            }
            if (resist <= 0.0) break;

            // Residual of the node's own continuity equation at h_k. On the
            // first sweep h_k == h_start, so the storage term is identically
            // zero and this is the original expression.
            const double as = (it == 0)
                                  ? state_->node_surf_area[un]
                                  : nodeStorageSlope(n, h_k - invert);
            const double dv =
                (it == 0) ? 0.0
                          : nodeVolumeFromDepth(n, std::max(0.0, h_k - invert)) -
                                v_start;
            const double dh =
                dt * (sum_f + q_lat - dv / dt) / (as + dt * resist);
            if (dh == 0.0) break;

            const bool last = (it + 1 >= sweeps) ||
                              (std::fabs(dh) <= opts_.node_picard_tol);
            if (!last) {
                // Re-solve the incident faces against the corrected head. This
                // REPLACES the linear extrapolation with the flux the Riemann
                // solver actually returns there, which is the part a single
                // tangent gets wrong at a large Δt.
                //
                // Two gates, and they are different claims. cell_active_ keeps
                // work-list compaction results-transparent (§6.10): only faces
                // the flux pass itself would have computed are touched.
                // faceIsLive keeps LOCAL TIME STEPPING correct: under LTS a
                // node's incident faces can sit in different tiers, and a face
                // that is not firing on this base step is holding the flux it
                // will book over its own 2^k·dt₀ window. Re-solving it here
                // would re-time that flux against the wrong Δt and break the
                // macro cycle's face-open/volume-close contract — the very
                // property that makes a tiered step conservative in TIME as
                // well as in mass.
                h_k += dh;
                state_->node_head[un] = h_k;
                for (int p = b; p < e; ++p) {
                    const auto up = static_cast<std::size_t>(p);
                    const int f = mesh_->node_face_idx[up];
                    const auto uf = static_cast<std::size_t>(f);
                    if (!faceIsLive(f)) continue;
                    const int cl = mesh_->face_cl[uf];
                    const int cr = mesh_->face_cr[uf];
                    const bool la = (cl >= 0) &&
                                    cell_active_[static_cast<std::size_t>(cl)];
                    const bool ra = (cr >= 0) &&
                                    cell_active_[static_cast<std::size_t>(cr)];
                    if (la || ra) computeFaceFlux(f);
                }
                continue;
            }

            // Write the correction into f_mass_ — the ONE array both the cell
            // update and the node update read. That is what keeps mass
            // conservation exact: whatever the correction does, the two sides
            // of every face see the same number.
            for (int p = b; p < e; ++p) {
                const auto up = static_cast<std::size_t>(p);
                const int f = mesh_->node_face_idx[up];
                const auto uf = static_cast<std::size_t>(f);
                const int cell = (mesh_->face_cl[uf] >= 0) ? mesh_->face_cl[uf]
                                                           : mesh_->face_cr[uf];
                if (cell < 0) continue;
                const auto uc = static_cast<std::size_t>(cell);
                const FvGeometry& g =
                    mesh_->geom[static_cast<std::size_t>(mesh_->cell_geom[uc])];
                const double hg = h_k - mesh_->face_zb[uf];
                if (hg <= k::kDryDepth) continue;
                const double ag = k::areaOfDepth(g, hg);
                const double tg = k::widthOfDepth(g, hg);
                if (ag <= k::kDryArea || tg <= 0.0) continue;
                // β = −sign·√(gAT): the node on a face's LEFT exports on a
                // positive flux, the node on its RIGHT imports, and raising the
                // head opposes the import in both cases.
                const double a = std::sqrt(k::kGravity * ag * tg);
                f_mass_[uf] += -mesh_->node_face_sign[up] * a * dh;
            }
            break;
        }

        // The head is the node update's to set, from the volume ledger. Any
        // provisional value above was scaffolding for the flux re-solve.
        state_->node_head[un] = h_start;
    }
}

// ===========================================================================
// Cell update
// ===========================================================================

void ExplicitFvSolver::updateCells(double dt, const FvStepForcing& forcing) {
    const int nc = mesh_->n_cells();
    const int nf = mesh_->n_faces();
    const int ns = state_->n_species;

    for (int c = 0; c < nc; ++c) {
        const auto uc = static_cast<std::size_t>(c);
        if (!cell_active_[uc]) continue;

        const FvGeometry& g = mesh_->geom[static_cast<std::size_t>(mesh_->cell_geom[uc])];
        const double dx = mesh_->cell_dx[uc];
        const double inv_dx = 1.0 / dx;

        const int faces[2]    = {mesh_->cell_face0[uc], mesh_->cell_face1[uc]};
        const int8_t sides[2] = {mesh_->cell_side0[uc], mesh_->cell_side1[uc]};

        double dA = 0.0, dQ = 0.0, k_loss = 0.0, q_thru = 0.0;
        for (int e = 0; e < 2; ++e) {
            const auto uf = static_cast<std::size_t>(faces[e]);
            const double fa = f_mass_[uf];
            const double fq = f_mom_[uf];
            if (sides[e] == 0) {
                dA -= fa;
                dQ -= static_cast<double>(mesh_->face_dir_l[uf]) * (fq + f_corr_l_[uf]);
                q_thru += static_cast<double>(mesh_->face_dir_l[uf]) * fa;
            } else {
                dA += fa;
                dQ += static_cast<double>(mesh_->face_dir_r[uf]) * (fq + f_corr_r_[uf]);
                q_thru += static_cast<double>(mesh_->face_dir_r[uf]) * fa;
            }
            // Entrance/exit losses apply only where the cell meets a node.
            // Which coefficient is a question of which END of the conduit this
            // is: the node sitting on the face's LEFT means this is the
            // conduit's upstream end, hence the entrance loss.
            if (mesh_->face_node[uf] >= 0)
                k_loss += (mesh_->face_cl[uf] < 0) ? g.loss_inlet : g.loss_outlet;
        }
        // Reported DISCHARGE is the mass actually moving through the cell —
        // the mean of its two face fluxes in the conduit frame — not the
        // momentum state q. The two agree in open channels, but in a
        // pressurized slot part of the steady throughput rides the Riemann
        // solver's pressure-difference term: a force main delivering 25 cfs
        // (confirmed by the outfall ledger) held cell q at 18.4, and the
        // report understated the flow it was simultaneously balancing.
        cell_q_int_[uc] += 0.5 * q_thru * dt;

        // Second-order centred bed source. At first order the two faces of a
        // cell see the same reconstructed depth and this is exactly zero; at
        // second order they do not, so the hydrostatic flux difference no
        // longer cancels at rest on its own and this term is what restores the
        // C-property (Audusse & Bristeau 2005). Assembled from I₁ at each
        // face's own-cell reconstructed depth, picking the face at the cell's
        // DOWNSTREAM end as the "+" side.
        if (opts_.order >= 2 && cell_ho2_[uc]) {
            // Centred bed source. At second order the two ends of a cell see
            // different reconstructed depths, so the hydrostatic flux
            // difference no longer cancels at rest by itself.
            //
            // The term is the BED's contribution alone: I₁ evaluated at the two
            // end beds holding the cell's OWN free surface fixed. That is what
            // makes it satisfy both requirements simultaneously —
            //   * flat bed  ⇒ z⁺ = z⁻ ⇒ exactly zero, so it cannot pollute a
            //     dam break (using the reconstructed depths instead adds a
            //     spurious source wherever the SURFACE has a slope, which
            //     measurably wrecked the Ritter profile: L1 0.030 → 0.401);
            //   * lake at rest ⇒ the reconstructed depths ARE η − z^±, so it
            //     cancels the flux difference exactly rather than to O(Δx³) as
            //     the conventional −g·Ā·Δz form does.
            const double half = 0.5 * mesh_->cell_dx[uc];
            const double zp = mesh_->cell_zb[uc] + mesh_->cell_dzdx[uc] * half;
            const double zm = mesh_->cell_zb[uc] - mesh_->cell_dzdx[uc] * half;
            const double eta_c = cell_eta_[uc];
            const double hp = std::max(0.0, eta_c - zp);
            const double hm = std::max(0.0, eta_c - zm);
            const double i1p = (hp > 0.0) ? k::i1OfDepth(g, hp, k::areaOfDepth(g, hp)) : 0.0;
            const double i1m = (hm > 0.0) ? k::i1OfDepth(g, hm, k::areaOfDepth(g, hm)) : 0.0;
            dQ += k::kGravity * (i1p - i1m);
        }

        const double a_old = state_->cell_a[uc];
        double a_new = a_old + dt * dA * inv_dx;
        double q_new = state_->cell_q[uc] + dt * dQ * inv_dx;

        // Diverted junction lateral inflow (refreshStructFlows): mass only,
        // zero momentum, added BEFORE the scalar divide so clean inflow
        // dilutes the advected species.
        if (!cell_qlat_.empty() && cell_qlat_[uc] != 0.0)
            a_new += dt * cell_qlat_[uc] * inv_dx;

        // ---- advected scalars, on the SAME divergence -----------------------
        // Updating m = A·φ with the same face MASS fluxes and dividing by the
        // hydrodynamic a_new is what makes a uniform field exactly invariant:
        // m_new = φ₀(A_old + Δt·divA) = φ₀·a_new. Computing the species flux
        // from a separately-evaluated velocity — the usual trap when transport
        // is bolted onto a hydraulic solver — breaks that identity and produces
        // spurious extrema (plan §3.2).
        if (ns > 0) {
            for (int s = 0; s < ns; ++s) {
                const auto cbase = static_cast<std::size_t>(s) *
                                   static_cast<std::size_t>(nc);
                const auto fbase = static_cast<std::size_t>(s) *
                                   static_cast<std::size_t>(nf);
                double dm = 0.0;
                for (int e = 0; e < 2; ++e) {
                    const auto uf = static_cast<std::size_t>(faces[e]);
                    const double fphi = f_phi_flux_[fbase + uf];
                    if (sides[e] == 0) dm -= fphi;
                    else               dm += fphi;
                }
                const double m = a_old * state_->cell_phi[cbase + uc] +
                                 dt * dm * inv_dx;
                state_->cell_phi[cbase + uc] =
                    (a_new > k::kDryArea) ? m / a_new : 0.0;
            }
        }

        // ---- distributed conduit losses (evaporation / seepage) ------------
        // Applied AFTER the scalar divide so removing water at ambient
        // concentration leaves φ unchanged, which is the right closure for
        // seepage. (Evaporative concentration is a quality-module concern.)
        if (forcing.conduit_loss) {
            const int cr = mesh_->cell_conduit[uc];
            if (cr >= 0) a_new -= dt * forcing.conduit_loss[cr];
        }
        if (a_new < 0.0) a_new = 0.0;

        // ---- friction + local losses, both semi-implicit --------------------
        const double h_new = k::depthOfArea(g, a_new);
        state_->cell_a[uc] = a_new;
        state_->cell_h[uc] = h_new;
        cell_eta_[uc] = mesh_->cell_zb[uc] + h_new;
        if (h_new <= k::kDryDepth) {
            state_->cell_q[uc] = 0.0;
            cell_u_[uc]        = 0.0;
            continue;
        }
        const double u = q_new / a_new;
        q_new = frictionFor(g, q_new, u, h_new, dt);
        if (k_loss > 0.0) q_new = k::localLossUpdate(q_new, u, k_loss, dx, dt);

        state_->cell_q[uc] = q_new;
        cell_u_[uc] = q_new / a_new;
    }
}

// ===========================================================================
// Node update
// ===========================================================================

// ===========================================================================
// Algebraic junctions
// ===========================================================================

namespace {
/// Flux-balance residual below this is "at rest" — the head is left alone so
/// a bracketing probe cannot disturb lake-at-rest by the solve tolerance.
constexpr double kAlgQEps     = 1.0e-12;
constexpr int    kAlgMaxIters = 16;
// The EXPANSION loop gets its own, larger budget: its first step is the
// quasi-Newton scale |R|/resist, which below a pipe crown (large open-channel
// resistance) can start near the tolerance floor — 16 doublings from 1e-6 ft
// reach only 0.07 ft and can never carry the bracket across the crown into
// the Preissmann slot, which is exactly how a force main got stuck passing
// 15.9 of 25 cfs. Doubling is geometric, so 64 steps reach any physical
// bracket; each step costs a handful of flux evaluations.
constexpr int    kAlgMaxExpand = 64;
} // namespace

// A junction with no storage is an INTERFACE, not a state (mirrors legacy
// DYNWAVE, where node_getSurfArea is zero for non-storage nodes and all
// working area belongs to the conduits — here, the cells). The head is the
// root of the monotone-decreasing balance
//     R(h) = Σ sign·F(h) + q_lat + q_struct + carry/dt
// with the incident faces re-solved by the ghost-Riemann machinery at each
// trial head. Value-based iteration only — the FvKernels closure note (why
// Newton was rejected) applies: W and A are independent tabulations, so the
// characteristic √(gAT) serves as a STEP ESTIMATE, bracket-guarded, never as
// a trusted derivative. On exit the head is written permanently and f_mass_
// AND f_mom_ hold the fluxes of the balancing head — mass and momentum stay
// jointly consistent, which is precisely what a Δf_mass-only correction
// cannot offer.
//
// NOTE (2026-08-11): balancing ENERGY head here (ghost stage = E − u_f²/2g)
// was implemented and measured, and REVERTED: at a uniform pass-through the
// velocity heads cancel and the solve degenerates to this stage balance
// (macdonald unchanged), while where velocities differ across the junction
// the Bernoulli shift double-counts what the cells' momentum fluxes already
// carry (bumps regressed 3-8×). The macdonald backwater is split-Riemann
// dissipation at the interface — the two node faces do not exchange momentum
// (a virtual-junction splice of the same deck scores 0.0255 vs 0.2276) — not
// an energy-accounting defect.
void ExplicitFvSolver::solveAlgebraicNode(int n, double dt,
                                          const FvStepForcing& forcing) {
    const auto un = static_cast<std::size_t>(n);
    if (forcing.node_fixed_head && std::isfinite(forcing.node_fixed_head[un]))
        return;
    // PASS-THROUGH junction: the fluxes present the neighbouring cells to
    // each other directly (faceSide/computeFaceFlux) and do not depend on the
    // node head at all, so there is no balance to solve — the head is purely
    // diagnostic. Report the mean adjacent stage; leaving it frozen instead
    // makes every link depth graded through node heads read dry.
    if (!node_pass_.empty() && node_pass_[un]) {
        const int b0 = mesh_->node_face_ptr[un];
        const int e0 = mesh_->node_face_ptr[un + 1];
        // Average the WET neighbours only. A dry cell's eta is not a free
        // surface — it is its bed — so averaging it in reports a stage that is
        // part water and part bare ground.
        //
        // It shows up wherever a pass-through node meets an OFFSET conduit,
        // which is exactly how the analytic decks seal an outlet: COUT runs
        // from InOffset 0.015 down to OutOffset 0, so its one cell's bed is the
        // midpoint 0.0075 and it is permanently dry. The old mean then reported
        // (0.001 + 0.0075)/2 = 0.00425 at the stoker dam break's last node
        // against a true 0.001, and (3.6e-5 + 0.0075)/2 = 0.00377 at ritter's
        // against a true zero — a 4x and 100x spike sitting on the end of every
        // dam-break profile, entirely manufactured by the dry side.
        //
        // Diagnostic only, by this branch's own contract: the neighbouring
        // cells see each other directly and no flux depends on this head. The
        // metrics barely moved (stoker L1 0.01420 vs 0.01422 with the station
        // dropped) because the affected station is a single point; the profile
        // and every figure drawn from it were wrong all the same.
        double s = 0.0, lowest = 0.0;
        int m = 0;
        bool any = false;
        for (int p = b0; p < e0; ++p) {
            const auto uf0 = static_cast<std::size_t>(
                mesh_->node_face_idx[static_cast<std::size_t>(p)]);
            const int c = (mesh_->face_cl[uf0] >= 0) ? mesh_->face_cl[uf0]
                                                     : mesh_->face_cr[uf0];
            if (c < 0) continue;
            const auto uc = static_cast<std::size_t>(c);
            const double eta = cell_eta_[uc];
            if (!any || eta < lowest) { lowest = eta; any = true; }
            if (state_->cell_h[uc] <= k::kDryDepth) continue;
            s += eta;
            ++m;
        }
        if (m > 0)        state_->node_head[un] = s / m;
        else if (any)     state_->node_head[un] = lowest;  // all dry: sit on the
                                                           // lowest adjacent bed
                                                           // rather than freeze
                                                           // a stale wet head
        return;
    }
    const int b = mesh_->node_face_ptr[un];
    const int e = mesh_->node_face_ptr[un + 1];
    const double invert = mesh_->node_invert[un];
    const double q_ext =
        nodeLateral(forcing, un) + node_qstruct_[un] + node_carry_[un] / dt;

    // Fallback for a degree-1 node whose head solve clamps at the ceiling
    // with inflow still unpassed: continuity fixes the answer — the one face
    // must carry exactly q_ext — so PRESCRIBE the discharge (same reasoning
    // as the culvert inlet-control treatment). This is the pressurized-main
    // pathology: past the crown dF/dh is the slot width, the residual is a
    // plateau, and the solve rides to the ceiling and floods the shortfall —
    // measured 15.9 of 25 cfs delivered on the force-main gate. It stays a
    // FALLBACK because the solve handles most slot flow fine (the interior
    // velocity carries the flux response as the pressure gradient
    // accelerates it — a culvert approach passes 120 cfs on the solve
    // alone), and prescribing pre-emptively backed those flows up. Mass
    // only, against the interior's own hydrostatic pressure: a lateral
    // inflow carries NO directed momentum in SWMM's convention (a manhole
    // pour, not a jet), and injecting q²/A drove a 66 ft/s filling bore.
    // Only while the interior stage is below the node rim — a genuine
    // overload lets the clamp stand and books flooding.
    auto prescribeDegree1 = [&]() -> bool {
        if (e - b != 1 || q_ext == 0.0) return false;
        const auto up0 = static_cast<std::size_t>(b);
        const int f0   = mesh_->node_face_idx[up0];
        const auto uf0 = static_cast<std::size_t>(f0);
        const int cell = (mesh_->face_cl[uf0] >= 0) ? mesh_->face_cl[uf0]
                                                    : mesh_->face_cr[uf0];
        if (cell < 0 || !faceIsLive(f0) ||
            !cell_active_[static_cast<std::size_t>(cell)] ||
            mesh_->face_culvert[uf0] >= 0 || mesh_->face_gate[uf0] != 0)
            return false;
        const double eta_c = cell_eta_[static_cast<std::size_t>(cell)];
        const double rim   = invert + mesh_->node_full_depth[un] +
                             (mesh_->node_can_pond[un]
                                  ? 0.0 : mesh_->node_sur_depth[un]);
        if (mesh_->node_full_depth[un] > 0.0 && eta_c >= rim) return false;
        const double sgn = mesh_->node_face_sign[up0];
        const k::FaceState& in = (mesh_->face_cl[uf0] >= 0) ? f_state_l_[uf0]
                                                            : f_state_r_[uf0];
        f_mass_[uf0] = -sgn * q_ext;                 // sign·F + q_ext = 0
        f_mom_[uf0]  = (in.a > k::kDryArea) ? k::kGravity * in.i1 : 0.0;
        state_->node_head[un] = std::max(invert, eta_c);
        return true;
    };

    // Re-solve the LIVE incident faces at a trial head; held faces contribute
    // their held flux as a constant. Under the tier pinning in assignTiers
    // every incident face shares one tier, so whenever this runs they are all
    // live and the balance is complete.
    double h_best = 0.0, r_best = std::numeric_limits<double>::infinity();
    auto residual = [&](double h) -> double {
        state_->node_head[un] = h;
        double r = q_ext;
        for (int p = b; p < e; ++p) {
            const auto up = static_cast<std::size_t>(p);
            const int f = mesh_->node_face_idx[up];
            const auto uf = static_cast<std::size_t>(f);
            if (faceIsLive(f)) {
                const int cl = mesh_->face_cl[uf];
                const int cr = mesh_->face_cr[uf];
                const bool la =
                    (cl >= 0) && cell_active_[static_cast<std::size_t>(cl)];
                const bool ra =
                    (cr >= 0) && cell_active_[static_cast<std::size_t>(cr)];
                if (la || ra) computeFaceFlux(f);
            }
            r += mesh_->node_face_sign[up] * f_mass_[uf];
        }
        if (std::fabs(r) < std::fabs(r_best)) { h_best = h; r_best = r; }
        return r;
    };
    // Σ|dQ/dH| at the ghost states — the quasi-Newton step estimate.
    auto resistAt = [&](double h) -> double {
        double resist = 0.0;
        for (int p = b; p < e; ++p) {
            const auto up = static_cast<std::size_t>(p);
            const auto uf =
                static_cast<std::size_t>(mesh_->node_face_idx[up]);
            const int cell = (mesh_->face_cl[uf] >= 0) ? mesh_->face_cl[uf]
                                                       : mesh_->face_cr[uf];
            if (cell < 0) continue;
            const FvGeometry& g = mesh_->geom[static_cast<std::size_t>(
                mesh_->cell_geom[static_cast<std::size_t>(cell)])];
            const double hg = h - mesh_->face_zb[uf];
            if (hg <= k::kDryDepth) continue;
            const double ag = k::areaOfDepth(g, hg);
            const double tg = k::widthOfDepth(g, hg);
            if (ag <= k::kDryArea || tg <= 0.0) continue;
            resist += std::sqrt(k::kGravity * ag * tg);
        }
        return resist;
    };

    // Bracket: invert (all node-side ghosts dry) up to the sealed ceiling
    // y_max — or the rim when the node can pond, since above the rim it holds
    // real water on its ponded area and updateNodes demotes it to the bucket
    // path. A junction with no rim on record gets a generous ceiling from its
    // incident conduits' crowns.
    const double full = mesh_->node_full_depth[un];
    double top;
    if (full > 0.0) {
        top = mesh_->node_can_pond[un] ? full
                                       : full + mesh_->node_sur_depth[un];
    } else {
        top = 1.0;
        for (int p = b; p < e; ++p) {
            const auto uf =
                static_cast<std::size_t>(mesh_->node_face_idx[
                    static_cast<std::size_t>(p)]);
            const int cell = (mesh_->face_cl[uf] >= 0) ? mesh_->face_cl[uf]
                                                       : mesh_->face_cr[uf];
            if (cell < 0) continue;
            const FvGeometry& g = mesh_->geom[static_cast<std::size_t>(
                mesh_->cell_geom[static_cast<std::size_t>(cell)])];
            top = std::max(top, (mesh_->face_zb[uf] - invert) + 2.0 * g.y_full);
        }
    }
    double lo = invert;
    double hi = invert + top;

    const double h0 = state_->node_head[un];
    double h = std::min(std::max(h0, lo), hi);
    double r = (h == h0) ? residual(h0) : residual(h);
    // Already balanced (a lake at rest is the load-bearing case): leave the
    // head exactly where it is.
    if (std::fabs(r) <= kAlgQEps) { state_->node_head[un] = h0; return; }

    // Expanding bracket from the CURRENT head, never a probe of the domain
    // ends: the ceiling can be arbitrarily high (a junction with no rim gets
    // an open-ended full depth), and a bisection budget spent walking back
    // from it leaves the head — and the ghost fluxes — stranded far from the
    // root. Geometric expansion bounds the work by the distance to the root
    // instead: start at the quasi-Newton scale |R|/resist, double until the
    // residual changes sign. Hitting the floor or ceiling with no sign change
    // IS the clamp case (dry junction / flooding), with the fluxes already
    // evaluated at the clamped head.
    const double dir  = (r > 0.0) ? 1.0 : -1.0;
    const double res0 = resistAt(h);
    double step = std::max(opts_.node_picard_tol,
                           (res0 > 0.0) ? std::fabs(r) / res0
                                        : opts_.node_picard_tol);
    double h_a = h, r_a = r;
    bool bracketed = false;
    for (int it = 0; it < kAlgMaxExpand; ++it) {
        double h_b = h_a + dir * step;
        if (h_b <= lo) h_b = lo;
        if (h_b >= hi) h_b = hi;
        const double r_b = residual(h_b);
        if (std::fabs(r_b) <= kAlgQEps) return;      // landed on the root
        if ((r_a > 0.0) != (r_b > 0.0)) {            // sign change: bracketed
            if (dir > 0.0) { lo = h_a; hi = h_b; }
            else           { lo = h_b; hi = h_a; }
            h = h_b; r = r_b;
            bracketed = true;
            break;
        }
        if (h_b == lo || h_b == hi) {                // clamped at an end
            // Ceiling with inflow still unpassed = the slot plateau; hand
            // the face to the prescribed-discharge fallback if it applies.
            if (h_b == hi && r_b > 0.0 && prescribeDegree1()) return;
            return;
        }
        h_a = h_b; r_a = r_b;
        step *= 2.0;
    }
    if (!bracketed) {
        // Budget spent walking: settle on the best head seen.
        if (h_best != h_a) residual(h_best);
        return;
    }

    // Bracketed quasi-Newton: dh = R/resist when it lands inside the bracket,
    // bisection otherwise. R is monotone decreasing, so sign(R) picks the
    // sub-bracket unambiguously; flat stretches (supercritical through-flow)
    // fall to bisection.
    for (int it = 0; it < kAlgMaxIters; ++it) {
        const double resist = resistAt(h);
        double h_new = (resist > 0.0) ? h + r / resist : 0.5 * (lo + hi);
        if (!(h_new > lo && h_new < hi)) h_new = 0.5 * (lo + hi);
        const double dh = h_new - h;
        h = h_new;
        r = residual(h);
        if (r > 0.0) lo = h; else hi = h;
        if (std::fabs(dh) <= opts_.node_picard_tol ||
            std::fabs(r) <= kAlgQEps)
            break;
    }
    // Finish at the BEST head evaluated, not merely the last probe: when the
    // root sits within roundoff of the starting head, the |Δh| ≤ tol exit
    // would otherwise park the head a full tolerance away and bleed
    // tolerance-scale noise into a system at rest.
    if (std::fabs(r_best) < std::fabs(r)) residual(h_best);
    // The final residual() evaluation left both the head and the incident
    // fluxes at the accepted root — the solve owns the head; updateNodes will
    // not overwrite it for this node.
}

// Structure links contribute a source/sink pair. They are held constant
// across the substeps of one routing step (D-FV3): re-evaluating the
// structure equations per substep would need the engine's HydStructures code,
// which the plugin boundary deliberately excludes, and would chatter
// controls tuned for DW-scale steps. Scattered once per forcing refresh.
void ExplicitFvSolver::refreshStructFlows(const FvStepForcing& forcing) {
    const auto nn = static_cast<std::size_t>(mesh_->n_nodes());
    node_qstruct_.assign(nn, 0.0);
    if (forcing.structure_flow) {
        const int nstruct = static_cast<int>(mesh_->struct_link.size());
        for (int s = 0; s < nstruct; ++s) {
            const auto us = static_cast<std::size_t>(s);
            const double q = forcing.structure_flow[mesh_->struct_link[us]];
            if (q == 0.0) continue;
            node_qstruct_[static_cast<std::size_t>(mesh_->struct_n1[us])] -= q;
            node_qstruct_[static_cast<std::size_t>(mesh_->struct_n2[us])] += q;
        }
    }
    // Dynamic half of the pass-through test, refreshed with the forcing: a
    // degree-2 junction is a pure interface only while nothing is injected at
    // it. A structure flow (or unbled carry) needs a head the fluxes respond
    // to, so the node falls back to the flux-balance solve for that step.
    //
    // A LATERAL inflow does not: it carries no directed momentum (a manhole
    // pour), so a clean degree-2 junction keeps the pass-through splice and
    // the inflow is DIVERTED into the two incident cells, half each, as a
    // zero-momentum area source integrated by updateCells/fireCells.
    // Dropping to the head solve instead costs the split-Riemann ~1 mm of
    // head per junction (see faceSide) — with rain on every junction of a
    // 400-conduit channel that integrated into an 18-25% deep bias with
    // exact q (SWASHES §3.3 benchmark finding, 2026-08-12).
    if (cell_qlat_.size() != static_cast<std::size_t>(mesh_->n_cells()))
        cell_qlat_.assign(static_cast<std::size_t>(mesh_->n_cells()), 0.0);
    else
        std::fill(cell_qlat_.begin(), cell_qlat_.end(), 0.0);
    if (node_lat_div_.size() != nn) node_lat_div_.assign(nn, 0);
    else std::fill(node_lat_div_.begin(), node_lat_div_.end(), 0);
    for (std::size_t un = 0; un < nn; ++un) {
        const bool clean = node_pass_static_[un] &&
                           node_qstruct_[un] == 0.0 && node_carry_[un] == 0.0;
        const double lat =
            forcing.node_lateral ? forcing.node_lateral[un] : 0.0;
        if (clean && lat != 0.0) {
            node_lat_div_[un] = 1;
            for (int p = mesh_->node_face_ptr[un];
                 p < mesh_->node_face_ptr[un + 1]; ++p) {
                const auto uf = static_cast<std::size_t>(
                    mesh_->node_face_idx[static_cast<std::size_t>(p)]);
                const int c = (mesh_->face_cl[uf] >= 0) ? mesh_->face_cl[uf]
                                                        : mesh_->face_cr[uf];
                if (c >= 0)
                    cell_qlat_[static_cast<std::size_t>(c)] += 0.5 * lat;
            }
        }
        node_pass_[un] = (clean && (lat == 0.0 || node_lat_div_[un])) ? 1 : 0;
    }
}

void ExplicitFvSolver::updateNodes(double dt, const FvStepForcing& forcing) {
    const int nn = mesh_->n_nodes();

    for (int n = 0; n < nn; ++n) {
        const auto un = static_cast<std::size_t>(n);
        if (mesh_->node_kind[un] == kNodeVirtual) continue;

        double sum_faces = 0.0;
        const int b = mesh_->node_face_ptr[un];
        const int e = mesh_->node_face_ptr[un + 1];
        for (int p = b; p < e; ++p) {
            const auto up = static_cast<std::size_t>(p);
            sum_faces += mesh_->node_face_sign[up] *
                         f_mass_[static_cast<std::size_t>(mesh_->node_face_idx[up])];
        }
        node_exch_[un] += sum_faces * dt;
        for (int p2 = b; p2 < e; ++p2) {
            const auto up2 = static_cast<std::size_t>(p2);
            const double q = mesh_->node_face_sign[up2] *
                f_mass_[static_cast<std::size_t>(mesh_->node_face_idx[up2])];
            if (q > 0.0) node_in_[un]  += q * dt;
            else         node_out_[un] -= q * dt;
        }

        if (forcing.node_fixed_head && std::isfinite(forcing.node_fixed_head[un])) {
            // Stage boundary (outfalls, tide/time-series). The head is imposed;
            // the conduit exchange is whatever the Riemann solver produced
            // against the ghost, and the volume ledger is not ours to keep.
            state_->node_head[un] = forcing.node_fixed_head[un];
            continue;
        }

        const double q_lat = nodeLateral(forcing, un) + node_qstruct_[un];

        if (algebraicActive(n)) {
            settleAlgebraicNode(n, node_carry_[un] + dt * (sum_faces + q_lat));
            continue;
        }

        const double v_prev = state_->node_volume[un];
        double vol = v_prev + dt * (sum_faces + q_lat);
        if (vol < 0.0) vol = 0.0;

        double depth = nodeDepthFromVolume(n, vol);
        applyNodeCapacity(n, v_prev, vol, depth);
        state_->node_volume[un] = vol;
        state_->node_head[un]   = mesh_->node_invert[un] + depth;
    }
}

// Dispose of an algebraic junction's window residual — the balance the root
// solve did not close exactly (solve tolerance, cell-side limiter scaling).
// The head was set by the solve and is NOT recomputed here; volume is a
// derived diagnostic. Three destinations, in order:
//   flooding  — head clamped at the sealed ceiling and still net inflow;
//   ponding   — head at the rim of a pondable node: the surplus becomes REAL
//               water on the ponded area, and algebraicActive() demotes the
//               node to the bucket path until it drains back below v_full;
//   carry     — everything else (positive or negative), bled into the next
//               solve's forcing. Exact conservation, no storage timescale.
void ExplicitFvSolver::settleAlgebraicNode(int n, double carry) {
    const auto un = static_cast<std::size_t>(n);
    const double invert = mesh_->node_invert[un];
    const double depth  = std::max(0.0, state_->node_head[un] - invert);
    const double full   = mesh_->node_full_depth[un];
    // A node re-promoting from a demoted (ponded) spell arrives with the
    // bucket-integrated volume it held below the rim still published; fold it
    // into the carry so the transition cannot orphan water. In normal
    // operation the published volume IS the carry and this is a no-op.
    const double published = state_->node_volume[un];
    const double ledger    = std::max(0.0, node_carry_[un]);
    if (published > ledger) carry += published - ledger;
    if (carry > 0.0 && full > 0.0) {
        const double eps = 1.0e-9;
        if (mesh_->node_can_pond[un]) {
            if (depth >= full - eps) {
                state_->node_volume[un] = node_vfull_[un] + carry;
                state_->node_head[un] =
                    invert + nodeDepthFromVolume(n, state_->node_volume[un]);
                node_carry_[un] = 0.0;
                return;
            }
        } else if (depth >= full + mesh_->node_sur_depth[un] - eps) {
            flood_vol_[un] += carry;
            node_carry_[un] = 0.0;
            state_->node_volume[un] = 0.0;
            return;
        }
    }
    // PASS-THROUGH node: no solve will ever bleed this carry back, and a
    // nonzero carry REVOKES the splice at the next forcing refresh (the
    // clean test) — which is exactly how every width-varying junction
    // silently fell back to the lossy head solve at steady state (the two
    // prismatic views of a spliced non-prismatic face legitimately disagree
    // by O(ΔB) in mass flux each step; SWASHES §3.5 finding, 2026-08-13).
    // Dispose the residual as VOLUME in the incident cells instead — they
    // are where an interface junction's water physically stands — split
    // evenly, zero momentum. Anything a near-dry cell cannot absorb stays
    // in the carry and demotes the node next step (conservative fallback).
    if (!node_pass_.empty() && node_pass_[un] && carry != 0.0) {
        double left = carry;
        const int b = mesh_->node_face_ptr[un];
        const int e = mesh_->node_face_ptr[un + 1];
        const int deg = (e > b) ? (e - b) : 1;
        for (int p = b; p < e; ++p) {
            const auto uf = static_cast<std::size_t>(
                mesh_->node_face_idx[static_cast<std::size_t>(p)]);
            const int c = (mesh_->face_cl[uf] >= 0) ? mesh_->face_cl[uf]
                                                    : mesh_->face_cr[uf];
            if (c < 0) continue;
            const auto uc = static_cast<std::size_t>(c);
            const double dx = mesh_->cell_dx[uc];
            double a_new = state_->cell_a[uc] +
                           (carry / static_cast<double>(deg)) / dx;
            if (a_new < 0.0) a_new = 0.0;
            left -= (a_new - state_->cell_a[uc]) * dx;
            const FvGeometry& g = mesh_->geom[static_cast<std::size_t>(
                mesh_->cell_geom[uc])];
            const double h_new = k::depthOfArea(g, a_new);
            state_->cell_a[uc] = a_new;
            state_->cell_h[uc] = h_new;
            cell_eta_[uc] = mesh_->cell_zb[uc] + h_new;
            if (h_new <= k::kDryDepth) {
                state_->cell_q[uc] = 0.0;
                cell_u_[uc]        = 0.0;
            } else {
                cell_u_[uc] = state_->cell_q[uc] / a_new;
            }
        }
        node_carry_[un] = (std::fabs(left) > 1.0e-12) ? left : 0.0;
        state_->node_volume[un] = std::max(0.0, node_carry_[un]);
        return;
    }
    // Published volume is the REAL water the node holds — the carry ledger
    // and nothing else. An algebraic junction is an interface: the water at
    // its head stands in the incident cells, and publishing V(depth) on top
    // books the same water twice. Measured: a 120-junction chain read a
    // -0.6 % routing continuity error purely from that phantom storage
    // (legacy DW keeps the same convention — junction storage is excluded
    // from its balance).
    node_carry_[un] = carry;
    state_->node_volume[un] = std::max(0.0, carry);
}

// ===========================================================================
// frictionFor — Manning, or the force main's own pressurized law
// ===========================================================================

// A FORCE_MAIN carries a Hazen-Williams C or a Darcy-Weisbach roughness height,
// not a Manning n. PostParseResolver substitutes an equivalent n so the free-
// surface reaches of the same model still march (and it applies that
// substitution to FV already), but once the main is FULL the equivalent n is
// the wrong law: DW switches to forcemain::getFricSlope_HW/_DW there, and
// without this FV kept using Manning at all depths and got the surcharge head
// loss wrong.
//
// Both forms are the same semi-implicit update — Manning's
// 1 + Δt·g·(n/φ)²·|u|/R^(4/3) IS 1 + Δt·g·S_f/|u| — so only S_f differs.
// HW vs DW is chosen exactly as DW chooses it (DynamicWave.cpp:2134): a
// Darcy-Weisbach roughness height is a small length, a Hazen-Williams C is
// order 100.
//
// The friction laws come from HydClosureKernels.hpp — the same bodies
// ForceMain.cpp's public entry points forward to — so this compiles unchanged
// for the device backend (plan §5.1).
double ExplicitFvSolver::frictionFor(const FvGeometry& g, double q, double u,
                                     double h, double dt) const {
    const double r = k::hydRadOfDepth(g, h);
    if (g.xs.type == static_cast<int>(XSectShape::FORCE_MAIN) && h >= g.y_full) {
        const double absu = std::fabs(u);
        if (absu <= 0.0) return q;
        const double sf = (g.roughness < 1.0)
                              ? hydkernels::fricSlopeDW(u, r, g.roughness)
                              : hydkernels::fricSlopeHW(u, r, g.roughness);
        return q / (1.0 + dt * k::kGravity * sf / absu);
    }
    return k::frictionUpdate(q, u, r, dt, g.rough_factor);
}

// ===========================================================================
// applyNodeCapacity — ponding, surcharge depth, flooding (DW parity)
// ===========================================================================

void ExplicitFvSolver::applyNodeCapacity(int node, double v_prev,
                                         double& vol, double& depth) {
    const auto un = static_cast<std::size_t>(node);
    const double full = mesh_->node_full_depth[un];
    if (full <= 0.0 || depth <= full) return;

    const double v_full = nodeVolumeFromDepth(node, full);

    if (mesh_->node_can_pond[un]) {
        // Ponded: the water is not lost — the depth already carries the ponded
        // tail, because the volume/depth conversions know about it. What is
        // missing is the REPORT: the rate crossing the rim is still a flooding
        // rate under DW, and the routing mass balance leaves it out on its own
        // (it books flooding only while volume <= full_volume). Skipping it
        // here left the Node Flooding Summary empty however deep the pond got.
        flood_vol_[un] += std::max(vol - std::max(v_prev, v_full), 0.0);
        return;
    }

    // Sealed: the head may rise SURCHARGE_DEPTH above the rim before the node
    // spills, exactly as a bolted cover holds pressure under DW. Capping at the
    // rim instead made every surcharged junction flood early — and left the
    // Node Surcharge Summary permanently empty.
    const double y_max = full + mesh_->node_sur_depth[un];
    if (depth <= y_max) return;

    const double v_max = nodeVolumeFromDepth(node, y_max);
    flood_vol_[un] += vol - v_max;
    vol   = v_max;
    depth = y_max;
}

// ===========================================================================
// Implicit dispersion (D-FV1)
// ===========================================================================

void ExplicitFvSolver::dispersionSolve(double dt) {
    const int ns = state_->n_species;
    if (ns <= 0 || opts_.dispersion <= 0.0) return;

    const int nc = mesh_->n_cells();
    static thread_local std::vector<double> aa, bb, ccv, rr, xx;

    // One tridiagonal system per chain (Thomas) — cheap, unconditionally
    // stable, and it removes the Δx²/(2·D_L) explicit constraint entirely,
    // which at fine Δx is MORE restrictive than CFL (plan §3.2, D-FV1). Chains
    // span virtual junctions, so a spliced pair disperses as one conduit.
    for (int ch = 0; ch < mesh_->n_chains(); ++ch) {
        const int b = mesh_->chain_ptr[static_cast<std::size_t>(ch)];
        const int e = mesh_->chain_ptr[static_cast<std::size_t>(ch) + 1];
        const int m = e - b;
        if (m < 2) continue;
        aa.assign(static_cast<std::size_t>(m), 0.0);
        bb.assign(static_cast<std::size_t>(m), 0.0);
        ccv.assign(static_cast<std::size_t>(m), 0.0);
        rr.assign(static_cast<std::size_t>(m), 0.0);
        xx.assign(static_cast<std::size_t>(m), 0.0);

        for (int s = 0; s < ns; ++s) {
            const auto base = static_cast<std::size_t>(s) *
                              static_cast<std::size_t>(nc);
            for (int i = 0; i < m; ++i) {
                const int c = mesh_->chain_cells[static_cast<std::size_t>(b + i)];
                const auto uc = static_cast<std::size_t>(c);
                const double dx = mesh_->cell_dx[uc];
                const double a  = std::max(state_->cell_a[uc], k::kDryArea);
                double lo = 0.0, hi = 0.0;
                if (i > 0) {
                    const int cm = mesh_->chain_cells[static_cast<std::size_t>(b + i - 1)];
                    const double am = std::max(
                        state_->cell_a[static_cast<std::size_t>(cm)], k::kDryArea);
                    lo = opts_.dispersion * 0.5 * (a + am) * dt / (dx * dx * a);
                }
                if (i < m - 1) {
                    const int cp = mesh_->chain_cells[static_cast<std::size_t>(b + i + 1)];
                    const double ap = std::max(
                        state_->cell_a[static_cast<std::size_t>(cp)], k::kDryArea);
                    hi = opts_.dispersion * 0.5 * (a + ap) * dt / (dx * dx * a);
                }
                aa[static_cast<std::size_t>(i)]  = -lo;
                ccv[static_cast<std::size_t>(i)] = -hi;
                bb[static_cast<std::size_t>(i)]  = 1.0 + lo + hi;
                rr[static_cast<std::size_t>(i)]  = state_->cell_phi[base + uc];
            }
            for (int i = 1; i < m; ++i) {
                const auto ui = static_cast<std::size_t>(i);
                const double w = aa[ui] / bb[ui - 1];
                bb[ui] -= w * ccv[ui - 1];
                rr[ui] -= w * rr[ui - 1];
            }
            const auto ulast = static_cast<std::size_t>(m - 1);
            xx[ulast] = rr[ulast] / bb[ulast];
            for (int i = m - 2; i >= 0; --i) {
                const auto ui = static_cast<std::size_t>(i);
                xx[ui] = (rr[ui] - ccv[ui] * xx[ui + 1]) / bb[ui];
            }
            for (int i = 0; i < m; ++i) {
                const int c = mesh_->chain_cells[static_cast<std::size_t>(b + i)];
                state_->cell_phi[base + static_cast<std::size_t>(c)] =
                    xx[static_cast<std::size_t>(i)];
            }
        }
    }
}

// ===========================================================================
// Step-rejection snapshot
// ===========================================================================

void ExplicitFvSolver::saveState() {
    save_cell_a_    = state_->cell_a;
    save_cell_q_    = state_->cell_q;
    save_cell_phi_  = state_->cell_phi;
    save_node_vol_  = state_->node_volume;
    save_node_head_ = state_->node_head;
    save_exch_      = node_exch_;
    save_in_        = node_in_;
    save_out_       = node_out_;
    save_flood_     = flood_vol_;
    save_qint_      = cell_q_int_;
    save_carry_     = node_carry_;
}

void ExplicitFvSolver::restoreState() {
    state_->cell_a      = save_cell_a_;
    state_->cell_q      = save_cell_q_;
    state_->cell_phi    = save_cell_phi_;
    state_->node_volume = save_node_vol_;
    state_->node_head   = save_node_head_;
    node_exch_          = save_exch_;
    node_in_            = save_in_;
    node_out_           = save_out_;
    flood_vol_          = save_flood_;
    cell_q_int_         = save_qint_;
    node_carry_         = save_carry_;
    refreshDepths();               // cell_h / eta / u are derived
}

// ===========================================================================
// Substep and time integration
// ===========================================================================

void ExplicitFvSolver::takeSubstep(double dt, const FvStepForcing& forcing) {
    reconstructState();
    computeFluxes();
    relaxNodeFluxes(dt, forcing);   // BEFORE limiting: the limiter must bound
                                    // the flux that is actually applied
    limitPositivity(dt);
    reconstructScalars(dt);   // AFTER limiting: the species must ride on the
                              // same water the hydrodynamics moved
    updateCells(dt, forcing);
    updateNodes(dt, forcing);
    dispersionSolve(dt);
}

void ExplicitFvSolver::rkSave() {
    rk_cell_a_    = state_->cell_a;
    rk_cell_q_    = state_->cell_q;
    rk_cell_phi_  = state_->cell_phi;
    rk_node_vol_  = state_->node_volume;
    rk_node_head_ = state_->node_head;
    rk_exch_      = node_exch_;
    rk_in_        = node_in_;
    rk_out_       = node_out_;
    rk_flood_     = flood_vol_;
    rk_qint_      = cell_q_int_;
    rk_carry_     = node_carry_;
}

void ExplicitFvSolver::rkAverage(const FvStepForcing& forcing) {
    auto avg = [](std::vector<double>& cur, const std::vector<double>& old) {
        for (std::size_t i = 0; i < cur.size(); ++i)
            cur[i] = 0.5 * (old[i] + cur[i]);
    };
    avg(state_->cell_a,   rk_cell_a_);
    avg(state_->cell_q,   rk_cell_q_);
    avg(state_->cell_phi, rk_cell_phi_);

    // Node depth is averaged through the VOLUME, not the head: volume is the
    // conserved variable, and averaging head instead would move water in and
    // out of a storage node's non-linear curve. Fixed-head nodes are unaffected
    // — both stages imposed the same head.
    for (int n = 0; n < mesh_->n_nodes(); ++n) {
        const auto un = static_cast<std::size_t>(n);
        if (mesh_->node_kind[un] == kNodeVirtual) continue;
        if (forcing.node_fixed_head &&
            std::isfinite(forcing.node_fixed_head[un])) {
            state_->node_head[un] = 0.5 * (rk_node_head_[un] + state_->node_head[un]);
            continue;
        }
        if (algebraicActive(n)) {
            // Head is the solved variable here; the published volume is the
            // carry ledger (the node's only real water), which avg_delta below
            // averages on its own.
            state_->node_head[un] =
                0.5 * (rk_node_head_[un] + state_->node_head[un]);
            state_->node_volume[un] = std::max(0.0, node_carry_[un]);
            continue;
        }
        const double vol = 0.5 * (rk_node_vol_[un] + state_->node_volume[un]);
        state_->node_volume[un] = vol;
        state_->node_head[un]   = mesh_->node_invert[un] + nodeDepthFromVolume(n, vol);
    }

    // Ledger DELTAS, not totals: the two stages each booked a full Δt of flux,
    // and the step transported the average of the two.
    auto avg_delta = [](std::vector<double>& cur, const std::vector<double>& base) {
        for (std::size_t i = 0; i < cur.size(); ++i)
            cur[i] = base[i] + 0.5 * (cur[i] - base[i]);
    };
    avg_delta(node_exch_,  rk_exch_);
    avg_delta(node_in_,    rk_in_);
    avg_delta(node_out_,   rk_out_);
    avg_delta(flood_vol_,  rk_flood_);
    avg_delta(cell_q_int_, rk_qint_);
    avg_delta(node_carry_, rk_carry_);

    refreshDepths();
}

// ===========================================================================
// Local time stepping (plan §3.3)
// ===========================================================================

double ExplicitFvSolver::cellStableDt(int c) const {
    const auto uc = static_cast<std::size_t>(c);
    const FvGeometry& g = mesh_->geom[static_cast<std::size_t>(mesh_->cell_geom[uc])];
    const double dx = mesh_->cell_dx[uc];

    double speed = 0.0;
    const double h = state_->cell_h[uc];
    if (h > k::kDryDepth)
        speed = std::fabs(cell_u_[uc]) +
                k::celerity(state_->cell_a[uc], k::widthOfDepth(g, h));

    // The ghost a boundary face presents counts against THIS cell's step, for
    // the same reason the global census is face-based: a surcharged manhole
    // hands a part-full pipe a pressurized ghost running at the slot celerity,
    // and tiering the cell on its own free-surface speed would place it in a
    // coarse tier that cannot resolve what its own boundary is doing.
    const int faces[2] = {mesh_->cell_face0[uc], mesh_->cell_face1[uc]};
    for (const int f : faces) {
        const auto uf = static_cast<std::size_t>(f);
        const int nd = mesh_->face_node[uf];
        if (nd < 0) continue;
        const double hg = state_->node_head[static_cast<std::size_t>(nd)] -
                          mesh_->face_zb[uf];
        if (hg <= k::kDryDepth) continue;
        speed = std::max(speed, std::fabs(cell_u_[uc]) +
                                    k::celerity(k::areaOfDepth(g, hg),
                                                k::widthOfDepth(g, hg)));
    }

    return (speed > 1.0e-12) ? opts_.cfl * dx / speed : 1.0e30;
}

double ExplicitFvSolver::nodeStableDt(int n) const {
    const auto un = static_cast<std::size_t>(n);
    if (mesh_->node_kind[un] == kNodeVirtual) return 1.0e30;
    // An algebraic junction has no volume state to bound; its tier is pinned
    // to its incident cells in assignTiers so its faces fire together.
    if (!node_alg_.empty() && node_alg_[un]) return 1.0e30;
    // An outfall's head is IMPOSED (forcing.node_fixed_head) and its volume
    // ledger is never integrated — there is nothing for a bound to protect.
    // Legacy DW's node criterion skips outfalls for the same reason. Without
    // this skip the outfall's MIN_SURFAREA bucket sets a millisecond dt for
    // the whole network exactly as the junction buckets did.
    if (mesh_->node_kind[un] == kNodeOutfall) return 1.0e30;
    const double as = state_->node_surf_area[un];
    if (!(as > 0.0)) return 1.0e30;

    double dt = 1.0e30;
    const int b = mesh_->node_face_ptr[un];
    const int e = mesh_->node_face_ptr[un + 1];
    for (int p = b; p < e; ++p) {
        const int f = mesh_->node_face_idx[static_cast<std::size_t>(p)];
        const auto uf = static_cast<std::size_t>(f);
        const int cell = (mesh_->face_cl[uf] >= 0) ? mesh_->face_cl[uf]
                                                   : mesh_->face_cr[uf];
        if (cell < 0) continue;
        const auto uc = static_cast<std::size_t>(cell);
        // Deliberately NOT gated on cell_active_: the tier schedule has to be
        // the same whether or not compaction is on (§6.10), and a dry cell is
        // excluded by its own depth test in either case.
        const double h = state_->cell_h[uc];
        if (h <= k::kDryDepth) continue;
        const FvGeometry& g =
            mesh_->geom[static_cast<std::size_t>(mesh_->cell_geom[uc])];
        const double t = k::widthOfDepth(g, h);
        if (t <= 0.0) continue;
        dt = std::min(dt, k::faceCflDt(opts_.cfl, as / t, cell_u_[uc],
                                       k::celerity(state_->cell_a[uc], t)));
    }
    return dt;
}

int ExplicitFvSolver::assignTiers(double& dt0) {
    const int nc = mesh_->n_cells();
    const int nf = mesh_->n_faces();
    const int nn = mesh_->n_nodes();

    // Transport stays on the global path. The Zalesak limiter (§6.11b) bounds
    // a cell's update against the extrema of its whole neighbourhood in one
    // synchronous sweep; under tiering the neighbours are at different times
    // and the antidiffusive correction has no single state to be bounded
    // against, so a tiered FCT would be a new scheme, not a scheduling change.
    if (state_->n_species > 0) return 1;

    static thread_local std::vector<double> dt_cell, dt_node;
    dt_cell.assign(static_cast<std::size_t>(nc), 1.0e30);
    dt_node.assign(static_cast<std::size_t>(nn), 1.0e30);

    // Every cell is tiered, active or not. Restricting this to the active set
    // would make the macro-cycle length depend on which cells compaction chose
    // to skip, and the schedule is what every volume's Δt is derived from —
    // the compacted run would then not merely skip work but integrate on a
    // different clock, breaking the §6.10 transparency contract outright.
    double dt_min = 1.0e30;
    for (int c = 0; c < nc; ++c) {
        const auto uc = static_cast<std::size_t>(c);
        dt_cell[uc] = cellStableDt(c);
        dt_min = std::min(dt_min, dt_cell[uc]);
    }
    // FV_NODE_DT NONE: the node's term is an EXPLICIT stability bound on an
    // unconditionally-stable semi-implicit update (see NodeDtLimit). Dropping
    // it from dt0 lets the CELL Courant limit set the base step — which is what
    // the non-LTS path has always used — while tiering stays intact. Nodes are
    // still TIERED (dt_node feeds tier_of below), just not allowed to drag the
    // global base step down with them.
    // As in censusDt(): NONE is a semi-implicit privilege — an explicitly
    // coupled node's bound is a genuine stability limit and always applies.
    const bool node_sets_dt0 =
        (opts_.node_dt_limit == NodeDtLimit::STABILITY) ||
        (opts_.node_coupling != NodeCoupling::SEMI_IMPLICIT);
    for (int n = 0; n < nn; ++n) {
        const auto un = static_cast<std::size_t>(n);
        dt_node[un] = nodeStableDt(n);
        if (node_sets_dt0) dt_min = std::min(dt_min, dt_node[un]);
    }
    if (!(dt_min > 0.0) || dt_min >= 1.0e29) return 1;
    dt0 = dt_min;

    const int k_cap = std::min(std::max(opts_.lts_max_tiers, 1), kMaxLtsTiers);
    auto tier_of = [&](double dt) -> int {
        if (dt >= 1.0e29) return k_cap - 1;
        const int k = static_cast<int>(std::floor(std::log2(dt / dt_min)));
        return std::min(std::max(k, 0), k_cap - 1);
    };

    cell_tier_.assign(static_cast<std::size_t>(nc), 0);
    node_tier_.assign(static_cast<std::size_t>(nn), 0);
    face_tier_.assign(static_cast<std::size_t>(nf), 0);

    for (int c = 0; c < nc; ++c) {
        const auto uc = static_cast<std::size_t>(c);
        cell_tier_[uc] = static_cast<std::uint8_t>(tier_of(dt_cell[uc]));
    }
    for (int n = 0; n < nn; ++n) {
        const auto un = static_cast<std::size_t>(n);
        node_tier_[un] = static_cast<std::uint8_t>(tier_of(dt_node[un]));
    }

    // A structure link is a single flow applied as a source at one node and a
    // sink at the other. Let the two ends sit in different tiers and the same
    // discharge is integrated over different durations at each — the link
    // would create or destroy water in proportion to the tier gap. Pinning
    // both ends to tier 0 costs nothing (structures are a handful of links)
    // and keeps the pair exact.
    const int nstruct = static_cast<int>(mesh_->struct_link.size());
    for (int s = 0; s < nstruct; ++s) {
        const auto us = static_cast<std::size_t>(s);
        node_tier_[static_cast<std::size_t>(mesh_->struct_n1[us])] = 0;
        node_tier_[static_cast<std::size_t>(mesh_->struct_n2[us])] = 0;
    }

    // Grade the tiers: no face may span more than one level. Without this the
    // assignment is stable cell-by-cell but not as a scheme — a coarse cell
    // sitting directly against a much finer one holds a frozen state for its
    // whole window while the neighbour resolves a front trying to cross into
    // it, and the flux booked against that frozen state over 2^k substeps
    // overshoots. Measured on the pressurize/depressurize cycling gate: a 3 ft
    // pipe filling from both ends settled cleanly at a 2×, 4× and 8× spread
    // and not at all at 32×, because the open-channel cells ahead of the
    // filling bore sat five tiers above the pressurized cells behind it.
    // One-level grading is the standard admissibility condition for LTS on an
    // unstructured mesh, and it is cheap: the sweep converges in at most K
    // passes because every pass either lowers a tier or terminates.
    for (int pass = 0; pass < k_cap; ++pass) {
        bool changed = false;
        for (int f = 0; f < nf; ++f) {
            const auto uf = static_cast<std::size_t>(f);
            const int cl = mesh_->face_cl[uf];
            const int cr = mesh_->face_cr[uf];
            const int nd = mesh_->face_node[uf];
            std::uint8_t* a = (cl >= 0) ? &cell_tier_[static_cast<std::size_t>(cl)]
                                        : nullptr;
            std::uint8_t* b = (cr >= 0) ? &cell_tier_[static_cast<std::size_t>(cr)]
                                        : nullptr;
            if (!a && nd >= 0) a = &node_tier_[static_cast<std::size_t>(nd)];
            if (!b && nd >= 0) b = &node_tier_[static_cast<std::size_t>(nd)];
            if (!a || !b) continue;
            if (*a > *b + 1) { *a = static_cast<std::uint8_t>(*b + 1); changed = true; }
            if (*b > *a + 1) { *b = static_cast<std::uint8_t>(*a + 1); changed = true; }
        }
        if (!changed) break;
    }

    // A volume with no wave speed at all — dry and still — carries the cap
    // tier so grading cannot use it to drag a wet neighbour down, but it must
    // not SET the cycle length either: one dry pipe in a 10,000-pipe model
    // would otherwise pin every run to the maximum tier count.
    int k_max = 0;
    for (int c = 0; c < nc; ++c) {
        const auto uc = static_cast<std::size_t>(c);
        if (dt_cell[uc] < 1.0e29)
            k_max = std::max<int>(k_max, cell_tier_[uc]);
    }
    for (int n = 0; n < nn; ++n) {
        const auto un = static_cast<std::size_t>(n);
        if (dt_node[un] < 1.0e29)
            k_max = std::max<int>(k_max, node_tier_[un]);
    }
    const int K = k_max + 1;
    if (K <= 1) return 1;

    // Dry volumes sit above k_max by construction; bring them into range so
    // the per-tier lists are addressable. They carry no flux either way.
    for (auto& t : cell_tier_) t = std::min<std::uint8_t>(t, static_cast<std::uint8_t>(K - 1));
    for (auto& t : node_tier_) t = std::min<std::uint8_t>(t, static_cast<std::uint8_t>(K - 1));

    // Algebraic junctions: pin the node to its FINEST incident cell so that
    // (via the face-tier min below) every incident face fires in one tier and
    // the flux-balance solve at firing time always sees all of them live. The
    // node itself is re-solved at every firing, so it never advances a frozen
    // state — the one-level-grading overshoot argument does not apply to it.
    {
        for (int n = 0; n < nn; ++n) {
            const auto un = static_cast<std::size_t>(n);
            if (!node_alg_[un]) continue;
            std::uint8_t t = static_cast<std::uint8_t>(K - 1);
            const int nb = mesh_->node_face_ptr[un];
            const int ne = mesh_->node_face_ptr[un + 1];
            for (int p = nb; p < ne; ++p) {
                const auto uf = static_cast<std::size_t>(
                    mesh_->node_face_idx[static_cast<std::size_t>(p)]);
                const int cell = (mesh_->face_cl[uf] >= 0) ? mesh_->face_cl[uf]
                                                           : mesh_->face_cr[uf];
                if (cell >= 0)
                    t = std::min(t, cell_tier_[static_cast<std::size_t>(cell)]);
            }
            node_tier_[un] = t;
        }
    }

    // A face fires at the FINER of its two sides. That is what makes the
    // interface well defined: the coarse side never advances past a flux it
    // has not been handed, and the fine side never waits on one.
    for (int f = 0; f < nf; ++f) {
        const auto uf = static_cast<std::size_t>(f);
        int k = k_cap - 1;
        const int cl = mesh_->face_cl[uf];
        const int cr = mesh_->face_cr[uf];
        const int nd = mesh_->face_node[uf];
        if (cl >= 0) k = std::min<int>(k, cell_tier_[static_cast<std::size_t>(cl)]);
        if (cr >= 0) k = std::min<int>(k, cell_tier_[static_cast<std::size_t>(cr)]);
        if (nd >= 0) k = std::min<int>(k, node_tier_[static_cast<std::size_t>(nd)]);
        face_tier_[uf] = static_cast<std::uint8_t>(k);
    }

    cells_by_tier_.assign(static_cast<std::size_t>(K), {});
    faces_by_tier_.assign(static_cast<std::size_t>(K), {});
    nodes_by_tier_.assign(static_cast<std::size_t>(K), {});
    for (int c = 0; c < nc; ++c) {
        const auto uc = static_cast<std::size_t>(c);
        if (!cell_active_[uc]) continue;
        cells_by_tier_[cell_tier_[uc]].push_back(c);
        ++tier_occupancy_[cell_tier_[uc]];
    }
    for (const int f : active_faces_)
        faces_by_tier_[face_tier_[static_cast<std::size_t>(f)]].push_back(f);
    for (int n = 0; n < nn; ++n) {
        const auto un = static_cast<std::size_t>(n);
        if (mesh_->node_kind[un] == kNodeVirtual) continue;
        nodes_by_tier_[node_tier_[un]].push_back(n);
    }

    // Nested due-sets, built once here rather than per substep.
    due_f_upto_.assign(static_cast<std::size_t>(K), {});
    due_c_upto_.assign(static_cast<std::size_t>(K), {});
    due_n_upto_.assign(static_cast<std::size_t>(K), {});
    for (int j = 0; j < K; ++j) {
        const auto uj = static_cast<std::size_t>(j);
        if (j > 0) {
            due_f_upto_[uj] = due_f_upto_[uj - 1];
            due_c_upto_[uj] = due_c_upto_[uj - 1];
            due_n_upto_[uj] = due_n_upto_[uj - 1];
        }
        due_f_upto_[uj].insert(due_f_upto_[uj].end(), faces_by_tier_[uj].begin(),
                               faces_by_tier_[uj].end());
        due_c_upto_[uj].insert(due_c_upto_[uj].end(), cells_by_tier_[uj].begin(),
                               cells_by_tier_[uj].end());
        due_n_upto_[uj].insert(due_n_upto_[uj].end(), nodes_by_tier_[uj].begin(),
                               nodes_by_tier_[uj].end());
    }

    acc_a_.assign(static_cast<std::size_t>(nc), 0.0);
    acc_q_.assign(static_cast<std::size_t>(nc), 0.0);
    acc_nvol_.assign(static_cast<std::size_t>(nn), 0.0);
    return K;
}

void ExplicitFvSolver::fireFaces(const std::vector<int>& faces, double dt0) {
    const int n = static_cast<int>(faces.size());
    if (n == 0) return;

    // Mark this base step's due set live, so the node relaxation below knows
    // which incident fluxes it may re-solve and which belong to another tier's
    // window (see faceIsLive).
    if (live_stamp_.size() != static_cast<std::size_t>(mesh_->n_faces()))
        live_stamp_.assign(static_cast<std::size_t>(mesh_->n_faces()), 0);
    all_faces_live_ = false;                    // only the due set is live
    ++live_gen_;
    if (live_gen_ == 0) {                       // wrapped: no stale match
        std::fill(live_stamp_.begin(), live_stamp_.end(), 0);
        live_gen_ = 1;
    }
    for (int i = 0; i < n; ++i)
        live_stamp_[static_cast<std::size_t>(faces[static_cast<std::size_t>(i)])] =
            live_gen_;

#ifdef SWMM_USE_OPENMP
#pragma omp parallel for schedule(static) if (n >= kOmpMinFaces)
#endif
    for (int i = 0; i < n; ++i)
        computeFaceFlux(faces[static_cast<std::size_t>(i)]);
    total_flux_ += n;

    // Same semi-implicit node coupling as the global path, applied at each
    // node's OWN tier step. Conservation is untouched for the same reason: the
    // correction lands in f_mass_, which is what gets booked into both incident
    // accumulators below.
    const bool semi = (opts_.node_coupling == NodeCoupling::SEMI_IMPLICIT);
    if (forcing_) {
        static thread_local std::vector<char> touched;
        touched.assign(acc_nvol_.size(), 0);
        for (const int f : faces) {
            const int nd = mesh_->face_node[static_cast<std::size_t>(f)];
            if (nd >= 0) touched[static_cast<std::size_t>(nd)] = 1;
        }
        for (std::size_t nd = 0; nd < touched.size(); ++nd) {
            if (!touched[nd]) continue;
            const int n = static_cast<int>(nd);
            const double dtn = static_cast<double>(1 << node_tier_[nd]) * dt0;
            // Algebraic junctions share one tier across their incident faces
            // (assignTiers pinning), so a touched node's faces are all in this
            // due set and the flux-balance solve is complete.
            if (algebraicActive(n))  solveAlgebraicNode(n, dtn, *forcing_);
            else if (semi)           relaxOneNode(n, dtn, *forcing_);
        }
    }

    // Positivity, in VOLUME rather than rate: the faces in this set fire with
    // different Δt, so the draws are only commensurable once each is
    // multiplied by its own step. The volume a control volume can supply is
    // what it has published PLUS what has already accumulated for it — the
    // published value alone is stale by up to one coarse step.
    static thread_local std::vector<double> out_cell, out_node;
    out_cell.assign(acc_a_.size(), 0.0);
    out_node.assign(acc_nvol_.size(), 0.0);

    for (const int f : faces) {
        const auto uf = static_cast<std::size_t>(f);
        const double fa = f_mass_[uf];
        if (fa == 0.0) continue;
        const double vol = fa * (static_cast<double>(1 << face_tier_[uf]) * dt0);
        if (vol > 0.0) {                       // exporting side is L
            if (mesh_->face_cl[uf] >= 0)
                out_cell[static_cast<std::size_t>(mesh_->face_cl[uf])] += vol;
            else if (mesh_->face_node[uf] >= 0)
                out_node[static_cast<std::size_t>(mesh_->face_node[uf])] += vol;
        } else {                               // exporting side is R
            if (mesh_->face_cr[uf] >= 0)
                out_cell[static_cast<std::size_t>(mesh_->face_cr[uf])] -= vol;
            else if (mesh_->face_node[uf] >= 0)
                out_node[static_cast<std::size_t>(mesh_->face_node[uf])] -= vol;
        }
    }
    for (std::size_t c = 0; c < out_cell.size(); ++c) {
        if (out_cell[c] <= 0.0) { out_cell[c] = 1.0; continue; }
        out_cell[c] = k::positivityScale(
            state_->cell_a[c] * mesh_->cell_dx[c] + acc_a_[c], out_cell[c], 1.0);
    }
    for (std::size_t nn = 0; nn < out_node.size(); ++nn) {
        if (out_node[nn] <= 0.0) { out_node[nn] = 1.0; continue; }
        // Interface, not bucket — see limitPositivity.
        if (algebraicActive(static_cast<int>(nn))) { out_node[nn] = 1.0; continue; }
        out_node[nn] = k::positivityScale(
            state_->node_volume[nn] + acc_nvol_[nn], out_node[nn], 1.0);
    }

    // Book. The IDENTICAL scaled flux enters both incident accumulators, which
    // is what makes the tier interface conserve exactly.
    for (const int f : faces) {
        const auto uf = static_cast<std::size_t>(f);
        const int cl = mesh_->face_cl[uf];
        const int cr = mesh_->face_cr[uf];
        const int nd = mesh_->face_node[uf];
        const double dt = static_cast<double>(1 << face_tier_[uf]) * dt0;

        double fa = f_mass_[uf];
        if (fa != 0.0) {
            const double s =
                (fa > 0.0)
                    ? ((cl >= 0) ? out_cell[static_cast<std::size_t>(cl)]
                                 : ((nd >= 0) ? out_node[static_cast<std::size_t>(nd)]
                                              : 1.0))
                    : ((cr >= 0) ? out_cell[static_cast<std::size_t>(cr)]
                                 : ((nd >= 0) ? out_node[static_cast<std::size_t>(nd)]
                                              : 1.0));
            if (s < 1.0) {
                f_mass_[uf] *= s;
                f_mom_[uf]  *= s;
                f_scale_[uf] = s;
                fa = f_mass_[uf];
            }
        }
        const double fq = f_mom_[uf];

        if (cl >= 0) {
            const auto ul = static_cast<std::size_t>(cl);
            acc_a_[ul] -= fa * dt;
            acc_q_[ul] -= static_cast<double>(mesh_->face_dir_l[uf]) *
                          (fq + f_corr_l_[uf]) * dt;
            // Same face-flux discharge integral as the global path, booked at
            // face-firing time where the flux and its window are known.
            cell_q_int_[ul] += 0.5 *
                static_cast<double>(mesh_->face_dir_l[uf]) * fa * dt;
        }
        if (cr >= 0) {
            const auto ur = static_cast<std::size_t>(cr);
            acc_a_[ur] += fa * dt;
            acc_q_[ur] += static_cast<double>(mesh_->face_dir_r[uf]) *
                          (fq + f_corr_r_[uf]) * dt;
            cell_q_int_[ur] += 0.5 *
                static_cast<double>(mesh_->face_dir_r[uf]) * fa * dt;
        }
        if (nd >= 0) {
            // Sign convention from the mesh builder: the node on a face's LEFT
            // exports on a positive flux, the node on its RIGHT imports.
            const double sign = (cl < 0) ? -1.0 : 1.0;
            const auto un = static_cast<std::size_t>(nd);
            const double v = sign * fa * dt;
            acc_nvol_[un] += v;
            node_exch_[un] += v;
            if (v > 0.0) node_in_[un]  += v;
            else         node_out_[un] -= v;
        }
    }
}

void ExplicitFvSolver::fireCells(const std::vector<int>& cells, double dt0,
                                 const FvStepForcing& forcing) {
    for (const int c : cells) {
        const auto uc = static_cast<std::size_t>(c);
        const double dt = static_cast<double>(1 << cell_tier_[uc]) * dt0;
        const FvGeometry& g =
            mesh_->geom[static_cast<std::size_t>(mesh_->cell_geom[uc])];
        const double dx = mesh_->cell_dx[uc];
        const double inv_dx = 1.0 / dx;

        double dQ_src = 0.0;
        if (opts_.order >= 2 && cell_ho2_[uc]) {
            // Same centred bed source as the global path — a CELL term, so it
            // is applied at the cell's own cadence, not the faces'.
            const double half = 0.5 * dx;
            const double zp = mesh_->cell_zb[uc] + mesh_->cell_dzdx[uc] * half;
            const double zm = mesh_->cell_zb[uc] - mesh_->cell_dzdx[uc] * half;
            const double eta_c = cell_eta_[uc];
            const double hp = std::max(0.0, eta_c - zp);
            const double hm = std::max(0.0, eta_c - zm);
            const double i1p = (hp > 0.0) ? k::i1OfDepth(g, hp, k::areaOfDepth(g, hp)) : 0.0;
            const double i1m = (hm > 0.0) ? k::i1OfDepth(g, hm, k::areaOfDepth(g, hm)) : 0.0;
            dQ_src = k::kGravity * (i1p - i1m) * dt;
        }

        double k_loss = 0.0;
        const int faces[2] = {mesh_->cell_face0[uc], mesh_->cell_face1[uc]};
        for (const int f : faces) {
            const auto uf = static_cast<std::size_t>(f);
            if (mesh_->face_node[uf] >= 0)
                k_loss += (mesh_->face_cl[uf] < 0) ? g.loss_inlet : g.loss_outlet;
        }

        // Drain. Zeroing here — not at the top of the cycle — is what makes the
        // accumulator a ledger rather than a buffer: every flux booked since
        // this cell last fired is applied exactly once.
        double a_new = state_->cell_a[uc] + acc_a_[uc] * inv_dx;
        double q_new = state_->cell_q[uc] + (acc_q_[uc] + dQ_src) * inv_dx;
        acc_a_[uc] = 0.0;
        acc_q_[uc] = 0.0;

        // Diverted junction lateral inflow over this cell's LTS window
        // (mirrors updateCells; dt here is the tier window span).
        if (!cell_qlat_.empty() && cell_qlat_[uc] != 0.0)
            a_new += dt * cell_qlat_[uc] * inv_dx;

        if (forcing.conduit_loss) {
            const int cr = mesh_->cell_conduit[uc];
            if (cr >= 0) a_new -= dt * forcing.conduit_loss[cr];
        }
        if (a_new < 0.0) a_new = 0.0;

        const double h_new = k::depthOfArea(g, a_new);
        state_->cell_a[uc] = a_new;
        state_->cell_h[uc] = h_new;
        cell_eta_[uc] = mesh_->cell_zb[uc] + h_new;
        if (h_new <= k::kDryDepth) {
            state_->cell_q[uc] = 0.0;
            cell_u_[uc]        = 0.0;
            continue;
        }
        const double u = q_new / a_new;
        q_new = frictionFor(g, q_new, u, h_new, dt);
        if (k_loss > 0.0) q_new = k::localLossUpdate(q_new, u, k_loss, dx, dt);

        state_->cell_q[uc] = q_new;
        cell_u_[uc] = q_new / a_new;
    }
}

void ExplicitFvSolver::fireNodes(const std::vector<int>& nodes, double dt0,
                                 const FvStepForcing& forcing) {
    for (const int n : nodes) {
        const auto un = static_cast<std::size_t>(n);
        const double dt = static_cast<double>(1 << node_tier_[un]) * dt0;

        const double drained = acc_nvol_[un];
        acc_nvol_[un] = 0.0;

        if (forcing.node_fixed_head && std::isfinite(forcing.node_fixed_head[un])) {
            state_->node_head[un] = forcing.node_fixed_head[un];
            continue;
        }

        const double q_lat = nodeLateral(forcing, un) + node_qstruct_[un];

        if (algebraicActive(n)) {
            // Head was set by the flux-balance solve at face-firing time; the
            // drained window volume is the residual the solve did not close.
            settleAlgebraicNode(n, node_carry_[un] + drained + dt * q_lat);
            continue;
        }

        const double v_prev = state_->node_volume[un];
        double vol = v_prev + drained + dt * q_lat;
        if (vol < 0.0) vol = 0.0;

        double depth = nodeDepthFromVolume(n, vol);
        applyNodeCapacity(n, v_prev, vol, depth);
        state_->node_volume[un] = vol;
        state_->node_head[un]   = mesh_->node_invert[un] + depth;
    }
}

void ExplicitFvSolver::runMacroCycle(double dt0, int nsub,
                                     const FvStepForcing& forcing) {
    const int K = static_cast<int>(cells_by_tier_.size());

    // A tier-k FACE opens its window at s ≡ 0 (mod 2^k); a tier-k VOLUME closes
    // its window at s ≡ 2^k−1. The offset is the whole point: by the time a
    // volume fires, every face bounding it has booked exactly 2^k·dt₀ worth of
    // flux, so the flux it drains and the sources it integrates cover the SAME
    // span. Firing volumes at the window's start instead — the obvious first
    // guess — hands a tier-3 node eight base-steps of lateral inflow against
    // one base-step of drained outflow, and the resulting sawtooth in a small
    // manhole volume drove the boundary ghost hard enough to vary energy head
    // by 3.5 ft across a 2 ft deep channel.
    auto trailing_zeros = [](int v) { int j = 0; while (((v >> j) & 1) == 0) ++j; return j; };

    for (int s = 0; s < nsub; ++s) {
        const auto jf = static_cast<std::size_t>(
            std::min((s == 0) ? K - 1 : trailing_zeros(s), K - 1));
        const auto jv = static_cast<std::size_t>(
            std::min(trailing_zeros(s + 1), K - 1));

        if (opts_.order >= 2) reconstructState();
        fireFaces(due_f_upto_[jf], dt0);
        fireCells(due_c_upto_[jv], dt0, forcing);
        fireNodes(due_n_upto_[jv], dt0, forcing);
    }

    // Nothing is left pending: the final substep closes EVERY tier's window
    // (nsub = 2^(K−1) is divisible by every 2^k in play), so each accumulator
    // is drained by the volume that owns it. settleAccumulators() at the cycle
    // boundary is therefore a no-op in the normal case, and exists for the one
    // that is not — a rejected cycle, or a tail that lands mid-window.
}

void ExplicitFvSolver::settleAccumulators() {
    if (acc_a_.empty()) return;
    const int nc = mesh_->n_cells();
    const int nn = mesh_->n_nodes();

    // A pure transfer: volume and momentum already booked are moved into the
    // state with NO time advance — no friction, no source, no contribution to
    // the flow integral. Re-tiering after this is safe because nothing is owed.
    for (int c = 0; c < nc; ++c) {
        const auto uc = static_cast<std::size_t>(c);
        if (acc_a_[uc] == 0.0 && acc_q_[uc] == 0.0) continue;
        const double inv_dx = 1.0 / mesh_->cell_dx[uc];
        double a = state_->cell_a[uc] + acc_a_[uc] * inv_dx;
        if (a < 0.0) a = 0.0;
        state_->cell_a[uc] = a;
        state_->cell_q[uc] += acc_q_[uc] * inv_dx;
        acc_a_[uc] = 0.0;
        acc_q_[uc] = 0.0;
    }
    for (int n = 0; n < nn; ++n) {
        const auto un = static_cast<std::size_t>(n);
        if (acc_nvol_[un] == 0.0) continue;
        if (algebraicActive(n)) {
            // No volume to settle into — the mid-window residual joins the
            // carry and is bled back through the next flux-balance solve.
            node_carry_[un] += acc_nvol_[un];
            acc_nvol_[un] = 0.0;
            continue;
        }
        double vol = state_->node_volume[un] + acc_nvol_[un];
        if (vol < 0.0) vol = 0.0;
        acc_nvol_[un] = 0.0;
        state_->node_volume[un] = vol;
        state_->node_head[un] =
            mesh_->node_invert[un] + nodeDepthFromVolume(n, vol);
    }
    refreshDepths();
}

// ===========================================================================
// Advance
// ===========================================================================

double ExplicitFvSolver::advance(double t_current, double t_target,
                                 const FvStepForcing& forcing) {
    if (!mesh_ || t_target <= t_current) return t_target;
    forcing_ = &forcing;

    // Per-routing-step bookkeeping.
    std::fill(node_exch_.begin(), node_exch_.end(), 0.0);
    std::fill(node_in_.begin(), node_in_.end(), 0.0);
    std::fill(node_out_.begin(), node_out_.end(), 0.0);
    std::fill(flood_vol_.begin(), flood_vol_.end(), 0.0);
    std::fill(inlet_control_.begin(), inlet_control_.end(), uint8_t{0});
    std::fill(cell_q_int_.begin(), cell_q_int_.end(), 0.0);

    refreshDepths();
    // Re-seed the node volume ledger from the head the engine currently holds,
    // so external edits (API writes, hot start, an outfall stage) take effect.
    // With the junction area fixed at init this is EXACT — it reproduces the
    // volume the previous step ended with rather than perturbing it.
    for (int n = 0; n < mesh_->n_nodes(); ++n) {
        const auto un = static_cast<std::size_t>(n);
        if (mesh_->node_kind[un] == kNodeVirtual) continue;
        // Algebraic junctions hold no derived storage: their published volume
        // is the carry ledger alone (settleAlgebraicNode), and reseeding
        // V(head) here would re-book the incident cells' water as node
        // storage every routing step. A demoted ponding junction is REAL
        // storage and reseeds like any other node.
        if (!node_alg_.empty() && algebraicActive(n)) continue;
        state_->node_volume[un] = nodeVolumeFromDepth(
            n, std::max(0.0, state_->node_head[un] - mesh_->node_invert[un]));
    }
    refreshStructFlows(forcing);

    double t = t_current;
    long steps = 0;
    lists_valid_  = false;
    census_count_ = 0;

    while (t < t_target) {
        // FV_STRUCTURE_COUPLING SUBSTEP: let the caller re-evaluate the
        // head-dependent boundary flows against where the solver has actually
        // got to. A routing step spans many substeps, across which a pump's
        // wet-well depth and a weir's head difference move while a frozen
        // discharge does not — the same reason the outfall stage is refreshed.
        //
        // At the TOP of a cycle, never inside one: the accumulators are
        // settled here and a mid-cycle change would leave a face's booked flux
        // and the volume that drains it describing different forcing. The
        // first pass is skipped because the caller has just computed them.
        if (steps > 0 && forcing.refresh &&
            opts_.structure_coupling == StructureCoupling::SUBSTEP) {
            forcing.refresh(forcing.refresh_user, t - t_current);
            refreshStructFlows(forcing);
        }

        if (!lists_valid_ || since_rebuild_ >= kRebuildInterval)
            rebuildActiveLists();

        // ---- local time stepping (plan §3.3) -------------------------------
        // Tiers are reassigned only HERE, at a macro-cycle boundary with the
        // ledger settled — never mid-cycle, where a re-tiered volume would
        // either skip a flux it is owed or drain one at the wrong Δt.
        // RK2 is a property of the STEP; tiering means different volumes take
        // different steps, so the two stages would be averaging states that
        // never shared a Δt. They are mutually exclusive, and the integrator
        // wins because it is the more specific request.
        if (ltsEligible()) {
            // Re-tiering costs a pass over every cell, face and node plus the
            // grading sweep. Doing it every cycle is what made tiering SLOWER
            // than global stepping on the reference model — the assignment is
            // stable over many cycles, so it is cached and refreshed on a
            // countdown, on a rejected cycle, or when the active list changes.
            //
            // Between re-tiers dt₀ may only SHRINK. A cached tier k promises
            // that 2^k·dt₀ is admissible for its volume; letting dt₀ grow under
            // it would silently break that promise, which is exactly why the
            // plan puts re-tiering at synchronisation points only.
            // The countdown is the floor, not the whole rule. A cycle also
            // re-tiers when the model's stiffness has MOVED: a pipe crossing
            // the crown takes its celerity up twenty-fold, and a cached tier
            // that was admissible before is not after. Letting dt₀ recover
            // matters just as much — pinning it at an old small value for the
            // whole countdown was measurably slower than not tiering at all.
            // The census is already needed to bound dt₀, so the test is free.
            const double now = (lts_valid_ && lts_tiers_ > 1) ? censusDt() : 0.0;
            const bool stiffness_moved =
                lts_valid_ && lts_tiers_ > 1 &&
                (now < 0.9 * lts_dt0_ || now > 2.0 * lts_dt0_);
            if (!lts_valid_ || lts_countdown_ <= 0 || stiffness_moved) {
                settleAccumulators();
                lts_tiers_ = assignTiers(lts_dt0_);
                lts_valid_ = true;
                lts_countdown_ = kRetierEveryCycles;
            }
            const int K = lts_tiers_;
            double dt0 = lts_dt0_;
            --lts_countdown_;
            const int nsub = 1 << (K - 1);
            const double span = static_cast<double>(nsub) * dt0;
            if (K > 1 && span <= t_target - t) {
                if (opts_.order < 2) reconstructState();
                saveState();
                runMacroCycle(dt0, nsub, forcing);

                // A cell can cross the crown inside a cycle exactly as it can
                // inside a global substep (D-FV6), and the cycle is longer, so
                // the same post-step rejection applies — with the accumulators
                // cleared on rollback, since the fluxes that produced them are
                // being discarded with the state that produced them.
                if (censusDt() < kStepAcceptRatio * dt0) {
                    restoreState();
                    lts_valid_ = false;      // the state that produced these
                                             // tiers is being discarded
                    std::fill(acc_a_.begin(), acc_a_.end(), 0.0);
                    std::fill(acc_q_.begin(), acc_q_.end(), 0.0);
                    std::fill(acc_nvol_.begin(), acc_nvol_.end(), 0.0);
                    dt_cache_ = censusDt();
                    census_count_ = 0;
                    // Fall through to the global path, which carries the
                    // shrink-and-retry loop.
                } else {
                    t += span;
                    steps += nsub;
                    since_rebuild_ += nsub;
                    last_h_ = dt0;
                    min_h_ = (min_h_ <= 0.0) ? dt0 : std::min(min_h_, dt0);
                    dt_cache_ = dt0;
                    census_count_ = 0;
                    continue;
                }
            }
        }

        if (census_count_ <= 0) {
            dt_cache_ = censusDt();
            census_count_ = std::max(1, opts_.cfl_census_interval);
        }
        --census_count_;

        const double remaining = t_target - t;
        double dt = std::min(dt_cache_, remaining);
        if (!(dt > 0.0)) dt = remaining;
        if (dt < constants::MIN_TIMESTEP)
            dt = std::min(constants::MIN_TIMESTEP, remaining);

        // Take the substep, then re-census the state it produced. If a cell
        // crossed the crown (or wetted, or pressurized a node) the stable step
        // collapses, and the step just taken was not admissible — roll back and
        // retry. Every input to the decision is the solver state, so the retry
        // sequence is deterministic and identical on every backend, which is
        // what keeps compaction transparency (§6.10) intact.
        for (int attempt = 0;; ++attempt) {
            saveState();
            if (opts_.time_integration == TimeIntegration::RK2) {
                // Heun / SSP-RK2 applied to the WHOLE operator, friction
                // included. Two forward steps at the same Δt, averaged:
                //   U⁽¹⁾ = S(Uⁿ),  U⁽²⁾ = S(U⁽¹⁾),  Uⁿ⁺¹ = ½(Uⁿ + U⁽²⁾).
                // Averaging the operator rather than splitting it keeps the
                // semi-implicit friction and the positivity limiter inside each
                // stage, where their stability arguments hold; a classical
                // two-stage form that applied them once would be neither.
                rkSave();
                takeSubstep(dt, forcing);
                takeSubstep(dt, forcing);
                rkAverage(forcing);
            } else {
                takeSubstep(dt, forcing);
            }

            if (attempt >= kMaxStepRetries || dt <= constants::MIN_TIMESTEP) break;
            const double dt_post = censusDt();
            if (dt_post >= kStepAcceptRatio * dt) break;          // admissible
            restoreState();
            dt = std::max(0.9 * dt_post, constants::MIN_TIMESTEP);
            if (dt > remaining) dt = remaining;
        }
        dt_cache_ = dt;                 // next step starts from what worked
        census_count_ = 0;

        t += dt;
        // RK2 is two full operator evaluations per accepted step; count both,
        // or the reported substeps-per-step halves the actual work done.
        steps += (opts_.time_integration == TimeIntegration::RK2) ? 2 : 1;
        ++since_rebuild_;
        last_h_ = dt;
        min_h_ = (min_h_ <= 0.0) ? dt : std::min(min_h_, dt);

        // Backstop against a pathological dt collapse turning one routing step
        // into an unbounded loop. The caller sees the shortfall in the returned
        // time and can shrink the routing step.
        if (steps > 2000000L) break;
    }

    // Publish a complete state: any flux still booked but not yet drained is
    // water the engine would otherwise never see.
    settleAccumulators();

    last_nsteps_ = steps;
    total_steps_ += steps;
    sim_time_ += (t - t_current);
    suggested_h_ = (dt_cache_ > 0.0 && dt_cache_ < 1.0e29) ? dt_cache_ : 0.0;
    return t;
}

} // namespace openswmm::fv
