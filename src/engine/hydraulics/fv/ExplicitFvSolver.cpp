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

    cell_eta_.assign(nc, 0.0);
    cell_u_.assign(nc, 0.0);
    cell_q_int_.assign(nc, 0.0);

    node_exch_.assign(nn, 0.0);
    node_in_.assign(nn, 0.0);
    node_out_.assign(nn, 0.0);
    flood_vol_.assign(nn, 0.0);

    cell_active_.assign(nc, 1);
    active_faces_.clear();
    lists_valid_  = false;
    since_rebuild_ = 0;
    census_count_ = 0;
    dt_cache_     = 0.0;

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
}

void ExplicitFvSolver::reinitialize(double /*t0*/) {
    if (!mesh_) return;
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
    return s;
}

// ===========================================================================
// Node storage relation
// ===========================================================================

double ExplicitFvSolver::nodeVolumeFromDepth(int node, double depth) const {
    const auto un = static_cast<std::size_t>(node);
    if (depth <= 0.0) return 0.0;
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

        if (speed > 1.0e-12 && dx_ref < 1.0e29)
            dt = std::min(dt, opts_.cfl * dx_ref / speed);
    }

    // Node constraint. A coupled node behaves like an extra control volume of
    // effective length A_s/T, and with the MIN_SURFAREA floor that length is
    // often FAR shorter than Δx — a manhole is the stiff element of an
    // otherwise coarse mesh. Omitting this term is the classic way an explicit
    // 1D network solver appears stable on a single reach and rings on a real
    // network.
    const int nn = mesh_->n_nodes();
    for (int n = 0; n < nn; ++n) {
        const auto un = static_cast<std::size_t>(n);
        if (mesh_->node_kind[un] == kNodeVirtual) continue;
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
// Face reconstruction + flux
// ===========================================================================

void ExplicitFvSolver::faceSide(int face, int cell, int node, double zstar,
                                int dir, double u_interior,
                                k::FaceState& out, double& i1_raw) const {
    const auto uf = static_cast<std::size_t>(face);
    const FvGeometry* g = nullptr;
    double eta = 0.0, h_raw = 0.0, u = 0.0;

    if (cell >= 0) {
        const auto uc = static_cast<std::size_t>(cell);
        g     = &mesh_->geom[static_cast<std::size_t>(mesh_->cell_geom[uc])];
        eta   = cell_eta_[uc];
        h_raw = state_->cell_h[uc];
        u     = static_cast<double>(dir) * cell_u_[uc];
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
            eta   = state_->node_head[static_cast<std::size_t>(node)];
            h_raw = std::max(0.0, eta - mesh_->face_zb[uf]);
            u     = u_interior;
        } else {
            // Closed end (dead-end conduit, or a test wall): mirror the interior
            // state and reverse its velocity. The Riemann solver then returns
            // zero mass flux and the correct hydrostatic pressure — a reflecting
            // wall, rather than the dry-bed OUTflow a null ghost would produce.
            eta   = cell_eta_[uo];
            h_raw = state_->cell_h[uo];
            u     = -u_interior;
        }
    }

    i1_raw = (h_raw > k::kDryDepth)
                 ? k::i1OfDepth(*g, h_raw, k::areaOfDepth(*g, h_raw)) : 0.0;

    const double h_star = std::max(0.0, eta - zstar);
    if (h_star <= k::kDryDepth) {
        out = k::FaceState{};
        return;
    }
    out.a  = k::areaOfDepth(*g, h_star);
    out.u  = u;
    out.q  = out.a * u;
    out.c  = k::celerity(out.a, k::widthOfDepth(*g, h_star));
    out.i1 = k::i1OfDepth(*g, h_star, out.a);
}

void ExplicitFvSolver::computeFluxes() {
    const int n_act = static_cast<int>(active_faces_.size());

#ifdef SWMM_USE_OPENMP
#pragma omp parallel for schedule(static) if (n_act >= kOmpMinFaces)
#endif
    for (int a = 0; a < n_act; ++a) {
        const int f = active_faces_[static_cast<std::size_t>(a)];
        const auto uf = static_cast<std::size_t>(f);
        const int cl = mesh_->face_cl[uf];
        const int cr = mesh_->face_cr[uf];
        const int nd = mesh_->face_node[uf];

        const double zl = (cl >= 0) ? mesh_->cell_zb[static_cast<std::size_t>(cl)]
                                    : mesh_->face_zb[uf];
        const double zr = (cr >= 0) ? mesh_->cell_zb[static_cast<std::size_t>(cr)]
                                    : mesh_->face_zb[uf];
        const double zstar = std::max(zl, zr);

        // The ghost inherits the interior cell's velocity, expressed in the
        // face frame — resolve it first, regardless of which side the node is on.
        const int interior = (cl >= 0) ? cl : cr;
        const double u_int =
            (interior >= 0)
                ? static_cast<double>((cl >= 0) ? mesh_->face_dir_l[uf]
                                                : mesh_->face_dir_r[uf]) *
                      cell_u_[static_cast<std::size_t>(interior)]
                : 0.0;

        k::FaceState L, R;
        double i1l = 0.0, i1r = 0.0;
        faceSide(f, cl, nd, zstar, mesh_->face_dir_l[uf], u_int, L, i1l);
        faceSide(f, cr, nd, zstar, mesh_->face_dir_r[uf], u_int, R, i1r);

        const k::FaceFlux fl = k::riemannFlux(L, R);
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
    total_flux_ += n_act;
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

        double dA = 0.0, dQ = 0.0, k_loss = 0.0;
        for (int e = 0; e < 2; ++e) {
            const auto uf = static_cast<std::size_t>(faces[e]);
            const double fa = f_mass_[uf];
            const double fq = f_mom_[uf];
            if (sides[e] == 0) {
                dA -= fa;
                dQ -= static_cast<double>(mesh_->face_dir_l[uf]) * (fq + f_corr_l_[uf]);
            } else {
                dA += fa;
                dQ += static_cast<double>(mesh_->face_dir_r[uf]) * (fq + f_corr_r_[uf]);
            }
            // Entrance/exit losses apply only where the cell meets a node.
            // Which coefficient is a question of which END of the conduit this
            // is: the node sitting on the face's LEFT means this is the
            // conduit's upstream end, hence the entrance loss.
            if (mesh_->face_node[uf] >= 0)
                k_loss += (mesh_->face_cl[uf] < 0) ? g.loss_inlet : g.loss_outlet;
        }

        const double a_old = state_->cell_a[uc];
        double a_new = a_old + dt * dA * inv_dx;
        double q_new = state_->cell_q[uc] + dt * dQ * inv_dx;

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
        q_new = k::frictionUpdate(q_new, u, k::hydRadOfDepth(g, h_new), dt,
                                  g.rough_factor);
        if (k_loss > 0.0) q_new = k::localLossUpdate(q_new, u, k_loss, dx, dt);

        state_->cell_q[uc] = q_new;
        cell_u_[uc] = q_new / a_new;
        cell_q_int_[uc] += q_new * dt;
    }
}

// ===========================================================================
// Node update
// ===========================================================================

void ExplicitFvSolver::updateNodes(double dt, const FvStepForcing& forcing) {
    const int nn = mesh_->n_nodes();

    // Structure links contribute a source/sink pair. They are held constant
    // across the substeps of one routing step (D-FV3): re-evaluating the
    // structure equations here would need the engine's HydStructures code,
    // which the plugin boundary deliberately excludes, and would chatter
    // controls tuned for DW-scale steps.
    static thread_local std::vector<double> q_struct;
    q_struct.assign(static_cast<std::size_t>(nn), 0.0);
    if (forcing.structure_flow) {
        const int nstruct = static_cast<int>(mesh_->struct_link.size());
        for (int s = 0; s < nstruct; ++s) {
            const auto us = static_cast<std::size_t>(s);
            const double q = forcing.structure_flow[mesh_->struct_link[us]];
            if (q == 0.0) continue;
            q_struct[static_cast<std::size_t>(mesh_->struct_n1[us])] -= q;
            q_struct[static_cast<std::size_t>(mesh_->struct_n2[us])] += q;
        }
    }

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

        const double q_lat = (forcing.node_lateral ? forcing.node_lateral[un] : 0.0) +
                             q_struct[un];
        double vol = state_->node_volume[un] + dt * (sum_faces + q_lat);
        if (vol < 0.0) vol = 0.0;

        double depth = nodeDepthFromVolume(n, vol);
        const double full = mesh_->node_full_depth[un];
        if (full > 0.0 && depth > full) {
            const double v_full = nodeVolumeFromDepth(n, full);
            const double excess = vol - v_full;
            const double ponded = mesh_->node_ponded_area[un];
            if (ponded > 0.0) {
                depth = full + excess / ponded;      // ALLOW_PONDING
            } else {
                flood_vol_[un] += excess;
                vol   = v_full;
                depth = full;
            }
        }
        state_->node_volume[un] = vol;
        state_->node_head[un]   = mesh_->node_invert[un] + depth;
    }
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
    refreshDepths();               // cell_h / eta / u are derived
}

// ===========================================================================
// Advance
// ===========================================================================

double ExplicitFvSolver::advance(double t_current, double t_target,
                                 const FvStepForcing& forcing) {
    if (!mesh_ || t_target <= t_current) return t_target;

    // Per-routing-step bookkeeping.
    std::fill(node_exch_.begin(), node_exch_.end(), 0.0);
    std::fill(node_in_.begin(), node_in_.end(), 0.0);
    std::fill(node_out_.begin(), node_out_.end(), 0.0);
    std::fill(flood_vol_.begin(), flood_vol_.end(), 0.0);
    std::fill(cell_q_int_.begin(), cell_q_int_.end(), 0.0);

    refreshDepths();
    // Re-seed the node volume ledger from the head the engine currently holds,
    // so external edits (API writes, hot start, an outfall stage) take effect.
    // With the junction area fixed at init this is EXACT — it reproduces the
    // volume the previous step ended with rather than perturbing it.
    for (int n = 0; n < mesh_->n_nodes(); ++n) {
        const auto un = static_cast<std::size_t>(n);
        if (mesh_->node_kind[un] == kNodeVirtual) continue;
        state_->node_volume[un] = nodeVolumeFromDepth(
            n, std::max(0.0, state_->node_head[un] - mesh_->node_invert[un]));
    }

    double t = t_current;
    long steps = 0;
    lists_valid_  = false;
    census_count_ = 0;

    while (t < t_target) {
        if (!lists_valid_ || since_rebuild_ >= kRebuildInterval)
            rebuildActiveLists();

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
            computeFluxes();
            limitPositivity(dt);
            reconstructScalars(dt);   // AFTER limiting: the species must ride on
                                      // the same water the hydrodynamics moved
            updateCells(dt, forcing);
            updateNodes(dt, forcing);
            dispersionSolve(dt);

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
        ++steps;
        ++since_rebuild_;
        last_h_ = dt;
        min_h_ = (min_h_ <= 0.0) ? dt : std::min(min_h_, dt);

        // Backstop against a pathological dt collapse turning one routing step
        // into an unbounded loop. The caller sees the shortfall in the returned
        // time and can shrink the routing step.
        if (steps > 2000000L) break;
    }

    last_nsteps_ = steps;
    total_steps_ += steps;
    sim_time_ += (t - t_current);
    suggested_h_ = (dt_cache_ > 0.0 && dt_cache_ < 1.0e29) ? dt_cache_ : 0.0;
    return t;
}

} // namespace openswmm::fv
