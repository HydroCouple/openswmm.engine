/**
 * @file ExplicitKokkosSurfaceSolver.cpp
 * @brief Kokkos port of the explicit local-inertial LTS marcher.
 *
 * @details Numerics are the serial ExplicitInertialSolver's, kernel for
 *          kernel — the scalar bodies come from InertialKernels.hpp /
 *          VfrClosure.hpp compiled with OPENSWMM_KERNEL_FN =
 *          KOKKOS_INLINE_FUNCTION (single source). See the header for the
 *          loop-structure mapping and the determinism contract.
 *
 * @see ExplicitInertialSolver.cpp (serial reference), ExplicitKokkosSurfaceSolver.hpp
 * @ingroup engine_2d_gpu
 */

#include <Kokkos_Core.hpp>

// Single-source scalar kernels, device-annotated for this TU.
#define OPENSWMM_KERNEL_FN KOKKOS_INLINE_FUNCTION

#include "ExplicitKokkosSurfaceSolver.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "../data/MeshData.hpp"
#include "../data/SolverOptions2D.hpp"
#include "../data/SurfaceStateData.hpp"
#include "../data/BoundaryData.hpp"
#include "../coupling/NodeCoupling.hpp"   // CouplingPoint
#include "../solver/InertialKernels.hpp"
#include "../../data/NodeData.hpp"

namespace openswmm::twoD::gpu {

namespace {

/// Active-set / tier rebuild cadence in macro cycles (== serial marcher).
constexpr int kRebuildEveryCycles = 4;

// ---------------------------------------------------------------------------
// Host→device mirroring helpers.
// ---------------------------------------------------------------------------
DView devCopy(const char* name, const std::vector<double>& v) {
    DView d(Kokkos::view_alloc(Kokkos::WithoutInitializing, std::string(name)), v.size());
    Kokkos::View<const double*, Kokkos::HostSpace,
                 Kokkos::MemoryTraits<Kokkos::Unmanaged>>
        h(v.data(), v.size());
    Kokkos::deep_copy(d, h);
    return d;
}

IView devCopy(const char* name, const std::vector<int>& v) {
    IView d(Kokkos::view_alloc(Kokkos::WithoutInitializing, std::string(name)), v.size());
    Kokkos::View<const int*, Kokkos::HostSpace,
                 Kokkos::MemoryTraits<Kokkos::Unmanaged>>
        h(v.data(), v.size());
    Kokkos::deep_copy(d, h);
    return d;
}

/// Refresh an existing device view from a host vector (no realloc).
void devRefresh(const DView& d, const std::vector<double>& v) {
    Kokkos::View<const double*, Kokkos::HostSpace,
                 Kokkos::MemoryTraits<Kokkos::Unmanaged>>
        h(v.data(), v.size());
    Kokkos::deep_copy(d, h);
}

/// Copy a device view back into a host vector.
void hostRefresh(std::vector<double>& v, const DView& d) {
    Kokkos::View<double*, Kokkos::HostSpace,
                 Kokkos::MemoryTraits<Kokkos::Unmanaged>>
        h(v.data(), v.size());
    Kokkos::deep_copy(h, d);
}

// ---------------------------------------------------------------------------
// Device twins of the small host-only helpers (bodies copied verbatim from
// SurfaceFluxCalculator.cpp / NodeCoupling.cpp — bit-identical math).
// ---------------------------------------------------------------------------
KOKKOS_INLINE_FUNCTION double devEvapSink(double rate, double depth,
                                          double dry_depth) {
    if (rate <= 0.0 || depth <= 0.0) return 0.0;
    if (depth >= dry_depth) return rate;
    const double t = depth / dry_depth;
    return rate * t * t * (3.0 - 2.0 * t);
}

constexpr double kOrificeHEps = 0.02;   // == NodeCoupling.cpp ORIFICE_H_EPS

KOKKOS_INLINE_FUNCTION double devOrificePhi(double a) {
    if (a >= kOrificeHEps) return std::sqrt(a);
    const double inv_sqrt_e = 1.0 / std::sqrt(kOrificeHEps);
    return (1.5 * inv_sqrt_e) * a
           - (0.5 * inv_sqrt_e / kOrificeHEps) * a * a;
}

KOKKOS_INLINE_FUNCTION double devOrificeFlow(double dh, double cd,
                                             double area) {
    const double a = std::fabs(dh);
    if (a < 1.0e-12) return 0.0;
    const double sign = (dh > 0.0) ? 1.0 : -1.0;
    return sign * cd * area * std::sqrt(2.0 * inertial::kGravity)
           * devOrificePhi(a);
}

KOKKOS_INLINE_FUNCTION double devEffectiveArea(double h_max, double z_ground,
                                               double /*full_depth*/,
                                               double A_inlet,
                                               double A_manhole) {
    if (h_max < z_ground) return A_inlet;
    const double d_trans = 0.05;
    const double frac =
        (h_max - z_ground) / d_trans < 1.0 ? (h_max - z_ground) / d_trans : 1.0;
    return A_inlet + frac * (A_manhole - A_inlet);
}

KOKKOS_INLINE_FUNCTION double devWetRamp(double d, double dry_depth) {
    double t = d / dry_depth;
    t = (t < 0.0) ? 0.0 : (t > 1.0 ? 1.0 : t);
    return t * t * (3.0 - 2.0 * t);
}

} // anonymous namespace

// ===========================================================================
// initialize
// ===========================================================================

void ExplicitKokkosSurfaceSolver::initialize(MeshData& mesh,
                                             SurfaceStateData& state,
                                             SolverOptions2D& opts) {
    mesh_  = &mesh;
    state_ = &state;
    opts_  = &opts;

    const int nt = mesh.n_triangles();
    if (nt <= 0) return;

    edges_.build(mesh);
    const int ne = edges_.ne;

    // Geometry mirrors (const for the run).
    d_tri_area_ = devCopy("tri_area", mesh.tri_area);
    d_tri_cz_   = devCopy("tri_cz", mesh.tri_cz);
    d_tri_cx_   = devCopy("tri_cx", mesh.tri_cx);
    d_tri_cy_   = devCopy("tri_cy", mesh.tri_cy);
    d_vz_       = devCopy("vz", mesh.vz);
    d_vx_       = devCopy("vx", mesh.vx);
    d_vy_       = devCopy("vy", mesh.vy);
    d_tri_v0_   = devCopy("tri_v0", mesh.tri_v0);
    d_tri_v1_   = devCopy("tri_v1", mesh.tri_v1);
    d_tri_v2_   = devCopy("tri_v2", mesh.tri_v2);
    d_vs_ptr_   = devCopy("vs_ptr", mesh.vert_stencil_ptr);
    d_vs_idx_   = devCopy("vs_idx", mesh.vert_stencil_idx);
    d_vs_wt_    = devCopy("vs_wt", mesh.vert_stencil_wt);
    d_edge_length_ = devCopy("edge_length", mesh.edge_length);
    d_mannings_n_  = devCopy("mannings_n", mesh.mannings_n);

    d_cL_    = devCopy("cL", edges_.cL);
    d_cR_    = devCopy("cR", edges_.cR);
    d_slotL_ = devCopy("slotL", edges_.slotL);
    d_slotR_ = devCopy("slotR", edges_.slotR);
    d_xi_    = devCopy("xi", edges_.xi);
    d_inv_dx_ = devCopy("inv_dx_normal", edges_.inv_dx_normal);
    d_zface_ = devCopy("zface", edges_.zface);
    d_ze_lo_ = devCopy("ze_lo", edges_.ze_lo);
    d_ze_hi_ = devCopy("ze_hi", edges_.ze_hi);
    d_nx_    = devCopy("nx", edges_.nx);
    d_ny_    = devCopy("ny", edges_.ny);
    d_mx_    = devCopy("mx", edges_.mx);
    d_my_    = devCopy("my", edges_.my);
    d_n2_    = devCopy("n2_face", edges_.n2_face);
    d_lchar_ = devCopy("cell_lchar", edges_.cell_lchar);
    d_cell_ptr_  = devCopy("cell_ptr", edges_.cell_ptr);
    d_cell_edge_ = devCopy("cell_edge", edges_.cell_edge);
    d_sign_      = devCopy("cell_sign", edges_.cell_sign);

    // State + marching arrays.
    d_volume_ = devCopy("volume", state.volume);
    d_head_   = DView("head", nt);
    d_depth_  = DView("depth", nt);
    d_q_      = DView("q", ne);
    d_faccL_  = DView("faccL", ne);
    d_faccR_  = DView("faccR", ne);
    have_perot_ = (opts.theta < 1.0);
    d_qcx_ = DView("qcx", have_perot_ ? nt : 0);
    d_qcy_ = DView("qcy", have_perot_ ? nt : 0);
    d_rain_ = DView("rain", nt);
    d_coup_ = DView("coup", nt);
    d_evap_ = DView("evap", nt);
    d_edge_flux_ = DView("edge_flux", state.edge_flux.size());
    d_active_ = IView("active", nt);
    d_pin_t0_ = IView("pin_t0", nt);
    d_tier_   = IView("tier", nt);
    d_face_tier_ = IView("face_tier", ne);
    d_cells_compact_ = IView(
        Kokkos::view_alloc(Kokkos::WithoutInitializing,
                           std::string("cells_compact")), nt);
    d_edges_compact_ = IView(
        Kokkos::view_alloc(Kokkos::WithoutInitializing,
                           std::string("edges_compact")), ne);
    d_scratch_ = IView("scratch", nt);
    d_dtcell_  = DView(
        Kokkos::view_alloc(Kokkos::WithoutInitializing,
                           std::string("dtcell")), nt);

    K_ = std::clamp(opts.lts_tiers, 1, 8);

    // Non-WALL boundary entries (host scan, same rule as the serial marcher).
    bc_cell_host_.clear();
    bc_slot_host_.clear();
    if (state.boundary) {
        for (int i = 0; i < nt; ++i) {
            for (int e = 0; e < 3; ++e) {
                const int idx = i * 3 + e;
                const auto ty =
                    static_cast<BoundaryType>(state.boundary->edge_bc_type[idx]);
                const bool interior = (e == 0   ? mesh.tri_nbr0[i]
                                       : e == 1 ? mesh.tri_nbr1[i]
                                                : mesh.tri_nbr2[i]) >= 0;
                if (!interior && ty != BoundaryType::WALL) {
                    bc_cell_host_.push_back(i);
                    bc_slot_host_.push_back(idx);
                }
            }
        }
    }
    const int nbc = static_cast<int>(bc_cell_host_.size());
    d_bc_cell_ = devCopy("bc_cell", bc_cell_host_);
    d_bc_slot_ = devCopy("bc_slot", bc_slot_host_);
    d_bc_type_  = IView("bc_type", nbc);
    d_bc_accum_ = DView("bc_accum", nbc);
    d_bc_slope_ = DView("bc_slope", nbc);
    d_bc_head_  = DView("bc_head", nbc);
    d_bc_flow_  = DView("bc_flow", nbc);
    d_bc_q_     = DView("bc_q", nbc);

    // Coupling points (non-outfall; owned by the router, frozen topology).
    const std::size_t np =
        state.node_coupling ? state.node_coupling->size() : 0;
    {
        std::vector<int> cell(np), vert(np), node(np);
        std::vector<double> cd(np), area(np);
        for (std::size_t k = 0; k < np; ++k) {
            const auto& cp = (*state.node_coupling)[k];
            cell[k] = cp.cell_idx;
            vert[k] = cp.vertex_idx;
            node[k] = cp.node_idx;
            cd[k]   = cp.cd;
            area[k] = cp.area;
        }
        d_cp_cell_   = devCopy("cp_cell", cell);
        d_cp_vertex_ = devCopy("cp_vertex", vert);
        d_cp_node_   = devCopy("cp_node", node);
        d_cp_cd_     = devCopy("cp_cd", cd);
        d_cp_area_   = devCopy("cp_area", area);
    }
    d_exch_ = DView("exch", np);
    exch_host_.assign(np, 0.0);
    const std::size_t nn =
        state.nodes_1d ? state.nodes_1d->volume.size() : 0;
    d_node_drawn_     = DView("node_drawn", nn);
    d_node_head_      = DView("node_head", nn);
    d_node_depth_     = DView("node_depth", nn);
    d_node_volume_    = DView("node_volume", nn);
    d_node_invert_    = DView("node_invert", nn);
    d_node_fulldepth_ = DView("node_fulldepth", nn);
    if (nn > 0) {
        devRefresh(d_node_invert_, state.nodes_1d->invert_elev);
        devRefresh(d_node_fulldepth_, state.nodes_1d->full_depth);
    }

    // Tier-0 pins: BC cells + coupling cells (fastest-changing forcing).
    {
        auto pin = d_pin_t0_;
        Kokkos::deep_copy(pin, 0);
        auto bc_cell = d_bc_cell_;
        Kokkos::parallel_for(
            "pin_bc", Kokkos::RangePolicy<ExecSpace>(0, nbc),
            KOKKOS_LAMBDA(int k) { pin(bc_cell(k)) = 1; });
        auto cp_cell = d_cp_cell_;
        Kokkos::parallel_for(
            "pin_cp", Kokkos::RangePolicy<ExecSpace>(0, (int)np),
            KOKKOS_LAMBDA(int k) {
                const int ci = cp_cell(k);
                if (ci >= 0) pin(ci) = 1;
            });
    }

    reconstructAllDev();
    t_last_sync_ = 0.0;
    cycles_since_rebuild_ = 1000;
    substeps_run_ = face_passes_ = last_steps_ = 0;
    last_dt_ = 0.0;
    tier_occupancy_.fill(0);
    telemetry_.clear();
    if (const char* p = std::getenv("OPENSWMM_2D_MARCHER_TELEMETRY"))
        telemetry_path_ = p;
    initialized_ = true;
}

// ===========================================================================
// Elementwise passes
// ===========================================================================

void ExplicitKokkosSurfaceSolver::reconstructAllDev() {
    const int nt = mesh_->n_triangles();
    auto vol = d_volume_, head = d_head_, depth = d_depth_;
    auto area = d_tri_area_, cz = d_tri_cz_, vz = d_vz_;
    auto v0 = d_tri_v0_, v1 = d_tri_v1_, v2 = d_tri_v2_;
    const bool vfr = (opts_->cell_closure == CellClosure2D::VFR);
    const double mwf = opts_->vfr_min_wet_frac;
    Kokkos::parallel_for(
        "reconstructAll", Kokkos::RangePolicy<ExecSpace>(0, nt),
        KOKKOS_LAMBDA(int i) {
            double e, d;
            inertial::etaDepthScalar(area(i), cz(i), vz(v0(i)), vz(v1(i)),
                                     vz(v2(i)), vfr, mwf, vol(i), e, d);
            head(i) = e;
            depth(i) = d;
        });
}

void ExplicitKokkosSurfaceSolver::settleAccumulatorsDev() {
    const int nt = mesh_->n_triangles();
    auto vol = d_volume_, head = d_head_, depth = d_depth_;
    auto faccL = d_faccL_, faccR = d_faccR_;
    auto ptr = d_cell_ptr_, edge = d_cell_edge_;
    auto sign = d_sign_;
    auto area = d_tri_area_, cz = d_tri_cz_, vz = d_vz_;
    auto v0 = d_tri_v0_, v1 = d_tri_v1_, v2 = d_tri_v2_;
    const bool vfr = (opts_->cell_closure == CellClosure2D::VFR);
    const double mwf = opts_->vfr_min_wet_frac;
    Kokkos::parallel_for(
        "settle", Kokkos::RangePolicy<ExecSpace>(0, nt),
        KOKKOS_LAMBDA(int i) {
            double pending = 0.0;
            for (int p = ptr(i); p < ptr(i + 1); ++p) {
                const int e = edge(p);
                if (sign(p) > 0.0) {
                    pending += faccL(e);
                    faccL(e) = 0.0;
                } else {
                    pending += faccR(e);
                    faccR(e) = 0.0;
                }
            }
            if (pending == 0.0) return;
            double v = vol(i) + pending;
            vol(i) = (v > 0.0) ? v : 0.0;
            double e2, d2;
            inertial::etaDepthScalar(area(i), cz(i), vz(v0(i)), vz(v1(i)),
                                     vz(v2(i)), vfr, mwf, vol(i), e2, d2);
            head(i) = e2;
            depth(i) = d2;
        });
}

void ExplicitKokkosSurfaceSolver::lazySourcesDev(double t) {
    const double dt_lazy = t - t_last_sync_;
    if (dt_lazy <= 0.0) return;
    const int nt = mesh_->n_triangles();
    auto vol = d_volume_, head = d_head_, depth = d_depth_;
    auto rain = d_rain_, coup = d_coup_, evap = d_evap_;
    auto active = d_active_;
    auto area = d_tri_area_, cz = d_tri_cz_, vz = d_vz_;
    auto v0 = d_tri_v0_, v1 = d_tri_v1_, v2 = d_tri_v2_;
    const bool vfr = (opts_->cell_closure == CellClosure2D::VFR);
    const double mwf = opts_->vfr_min_wet_frac;
    const double dry = opts_->dry_depth;
    Kokkos::parallel_for(
        "lazySources", Kokkos::RangePolicy<ExecSpace>(0, nt),
        KOKKOS_LAMBDA(int i) {
            if (active(i)) return;
            const double src = rain(i) + coup(i)
                               - devEvapSink(evap(i), depth(i), dry);
            if (src == 0.0) return;
            double v = vol(i) + dt_lazy * src * area(i);
            vol(i) = (v > 0.0) ? v : 0.0;
            double e2, d2;
            inertial::etaDepthScalar(area(i), cz(i), vz(v0(i)), vz(v1(i)),
                                     vz(v2(i)), vfr, mwf, vol(i), e2, d2);
            head(i) = e2;
            depth(i) = d2;
        });
    t_last_sync_ = t;
}

// ===========================================================================
// Active set + tier rebuild (device compaction via parallel_scan)
// ===========================================================================

void ExplicitKokkosSurfaceSolver::syncAndRebuild(double t) {
    settleAccumulatorsDev();
    lazySourcesDev(t);

    const int nt = mesh_->n_triangles();
    const int ne = edges_.ne;
    const double h_on  = opts_->h_move + 0.001;
    const double h_off = std::max(0.0, opts_->h_move - 0.001);

    auto active = d_active_, seed = d_scratch_, pin = d_pin_t0_;
    auto depth = d_depth_, coup = d_coup_;
    Kokkos::parallel_for(
        "seed", Kokkos::RangePolicy<ExecSpace>(0, nt), KOKKOS_LAMBDA(int i) {
            const double thresh = active(i) ? h_off : h_on;
            seed(i) =
                (depth(i) >= thresh || coup(i) != 0.0 || pin(i)) ? 1 : 0;
        });
    Kokkos::parallel_for(
        "seed_to_active", Kokkos::RangePolicy<ExecSpace>(0, nt),
        KOKKOS_LAMBDA(int i) { active(i) = seed(i); });
    {   // one-ring halo (benign write race: constant 1)
        auto cLv = d_cL_, cRv = d_cR_;
        Kokkos::parallel_for(
            "halo", Kokkos::RangePolicy<ExecSpace>(0, ne),
            KOKKOS_LAMBDA(int e) {
                const int a = cLv(e), b = cRv(e);
                if (seed(a) && !seed(b)) active(b) = 1;
                else if (seed(b) && !seed(a)) active(a) = 1;
            });
    }

    // Per-cell CFL dt + exact min-reduce → dt0.
    auto dtcell = d_dtcell_;
    auto lchar = d_lchar_;
    auto qcx = d_qcx_, qcy = d_qcy_;
    const bool perot = have_perot_;
    const double alpha = opts_->cfl_number, dry = opts_->dry_depth;
    const double dt_max = opts_->max_timestep;
    double dt0 = 1.0e30;
    Kokkos::parallel_reduce(
        "cfl_min", Kokkos::RangePolicy<ExecSpace>(0, nt),
        KOKKOS_LAMBDA(int i, double& mn) {
            if (!active(i)) { dtcell(i) = 1.0e30; return; }
            const double h = depth(i);
            double speed = 0.0;
            if (perot && h > 1.0e-6)
                speed = std::hypot(qcx(i), qcy(i)) / h;
            double dt = (h > dry)
                            ? inertial::cellCflDt(alpha, lchar(i), h, speed)
                            : 1.0e30;
            dt = (dt < dt_max) ? dt : dt_max;
            dtcell(i) = dt;
            if (dt < mn) mn = dt;
        },
        Kokkos::Min<double>(dt0));
    if (dt0 >= 1.0e30) dt0 = opts_->max_timestep;   // fully quiescent
    dt0_ = dt0;

    // Tier assignment (active cells only; stale tiers persist like serial).
    const int K = K_;
    // (refreshDt0 below re-evaluates this reduction between rebuilds,
    //  tighten-only — growth waits for the tier reassignment here.)
    auto tier = d_tier_;
    Kokkos::parallel_for(
        "tier_assign", Kokkos::RangePolicy<ExecSpace>(0, nt),
        KOKKOS_LAMBDA(int i) {
            if (!active(i)) return;
            int tk = 0;
            if (K > 1) {
                const double ratio = dtcell(i) / dt0;
                if (ratio >= 2.0) {
                    int lg = (int)std::log2(ratio);
                    tk = (lg < K - 1) ? lg : (K - 1);
                }
                if (coup(i) != 0.0 || pin(i)) tk = 0;
            }
            tier(i) = tk;
        });

    // Face tiers (min of incident, both-active) + stale-momentum zero.
    auto ftier = d_face_tier_;
    auto qv = d_q_;
    {
        auto cLv = d_cL_, cRv = d_cR_;
        Kokkos::parallel_for(
            "face_tier", Kokkos::RangePolicy<ExecSpace>(0, ne),
            KOKKOS_LAMBDA(int e) {
                const int a = cLv(e), b = cRv(e);
                if (active(a) && active(b)) {
                    const int ta = tier(a), tb = tier(b);
                    ftier(e) = (ta < tb) ? ta : tb;
                } else {
                    ftier(e) = K;      // sentinel: not marched
                    qv(e) = 0.0;
                }
            });
    }

    // Compact per-tier lists (ascending order == serial push_back order).
    auto cells_c = d_cells_compact_;
    auto edges_c = d_edges_compact_;
    int cell_base = 0, edge_base = 0;
    n_active_ = 0;
    for (int k = 0; k < K; ++k) {
        tier_off_[k] = cell_base;
        int count = 0;
        Kokkos::parallel_scan(
            "compact_cells", Kokkos::RangePolicy<ExecSpace>(0, nt),
            KOKKOS_LAMBDA(int i, int& upd, bool fin) {
                const int f = (active(i) && tier(i) == k) ? 1 : 0;
                if (fin && f) cells_c(cell_base + upd) = i;
                upd += f;
            },
            count);
        cell_base += count;
        tier_occupancy_[static_cast<std::size_t>(k)] += count;
        n_active_ += count;

        ftier_off_[k] = edge_base;
        int fcount = 0;
        Kokkos::parallel_scan(
            "compact_edges", Kokkos::RangePolicy<ExecSpace>(0, ne),
            KOKKOS_LAMBDA(int e, int& upd, bool fin) {
                const int f = (ftier(e) == k) ? 1 : 0;
                if (fin && f) edges_c(edge_base + upd) = e;
                upd += f;
            },
            fcount);
        edge_base += fcount;
    }
    tier_off_[K] = cell_base;
    ftier_off_[K] = edge_base;

    telemetry_.emplace_back(t, n_active_);
}

void ExplicitKokkosSurfaceSolver::refreshDt0() {
    // Tighten-only dt0_ refresh between rebuilds (== serial marcher): depths
    // evolve for up to kRebuildEveryCycles macro cycles on a frozen dt0_, so
    // the realized CFL could exceed the configured bound. Tightening is safe
    // (every tier still satisfies dt_cell ≥ 2^k·dt0); growth waits for the
    // syncAndRebuild tier reassignment.
    const int nt = mesh_->n_triangles();
    if (n_active_ == 0) return;
    auto active = d_active_;
    auto depth = d_depth_;
    auto lchar = d_lchar_;
    auto qcx = d_qcx_, qcy = d_qcy_;
    const bool perot = have_perot_;
    const double alpha = opts_->cfl_number, dry = opts_->dry_depth;
    double fresh = 1.0e30;
    Kokkos::parallel_reduce(
        "cfl_refresh", Kokkos::RangePolicy<ExecSpace>(0, nt),
        KOKKOS_LAMBDA(int i, double& mn) {
            if (!active(i)) return;
            const double h = depth(i);
            if (h <= dry) return;
            double speed = 0.0;
            if (perot && h > 1.0e-6)
                speed = std::hypot(qcx(i), qcy(i)) / h;
            const double dt = inertial::cellCflDt(alpha, lchar(i), h, speed);
            if (dt < mn) mn = dt;
        },
        Kokkos::Min<double>(fresh));
    if (fresh < dt0_) dt0_ = fresh;
}

void ExplicitKokkosSurfaceSolver::collapseToGlobalDt() {
    // Tail landing: everything active drops to tier 0 (serial semantics).
    settleAccumulatorsDev();
    const int nt = mesh_->n_triangles();
    const int ne = edges_.ne;
    const int K = K_;
    auto active = d_active_;
    auto tier = d_tier_;
    Kokkos::parallel_for(
        "collapse_cells", Kokkos::RangePolicy<ExecSpace>(0, nt),
        KOKKOS_LAMBDA(int i) {
            if (active(i)) tier(i) = 0;
        });
    auto ftier = d_face_tier_;
    {
        auto cLv = d_cL_, cRv = d_cR_;
        Kokkos::parallel_for(
            "collapse_faces", Kokkos::RangePolicy<ExecSpace>(0, ne),
            KOKKOS_LAMBDA(int e) {
                if (active(cLv(e)) && active(cRv(e))) ftier(e) = 0;
                else ftier(e) = K;
            });
    }
    auto cells_c = d_cells_compact_;
    auto edges_c = d_edges_compact_;
    int count = 0;
    Kokkos::parallel_scan(
        "collapse_compact_cells", Kokkos::RangePolicy<ExecSpace>(0, nt),
        KOKKOS_LAMBDA(int i, int& upd, bool fin) {
            const int f = active(i) ? 1 : 0;
            if (fin && f) cells_c(upd) = i;
            upd += f;
        },
        count);
    int fcount = 0;
    Kokkos::parallel_scan(
        "collapse_compact_edges", Kokkos::RangePolicy<ExecSpace>(0, ne),
        KOKKOS_LAMBDA(int e, int& upd, bool fin) {
            const int f = (ftier(e) == 0) ? 1 : 0;
            if (fin && f) edges_c(upd) = e;
            upd += f;
        },
        fcount);
    for (int k = 1; k <= K; ++k) {
        tier_off_[k] = count;
        ftier_off_[k] = fcount;
    }
    tier_off_[0] = ftier_off_[0] = 0;
    n_active_ = count;
}

// ===========================================================================
// Marching kernels
// ===========================================================================

void ExplicitKokkosSurfaceSolver::fireFaces(int k, double dt_f) {
    const int lo = ftier_off_[k], hi = ftier_off_[k + 1];
    if (lo >= hi) return;
    auto list = d_edges_compact_;
    auto cLv = d_cL_, cRv = d_cR_;
    auto head = d_head_, vol = d_volume_;
    auto qv = d_q_, faccL = d_faccL_, faccR = d_faccR_;
    auto zface = d_zface_, inv_dx = d_inv_dx_, n2 = d_n2_, xi = d_xi_;
    auto ze_lo = d_ze_lo_, ze_hi = d_ze_hi_;
    auto nxv = d_nx_, nyv = d_ny_;
    auto qcx = d_qcx_, qcy = d_qcy_;
    auto tier = d_tier_, ftier = d_face_tier_;
    const bool perot = have_perot_;
    // VFR_FACE: B&S Eq. 14 at the shared edge's true crest (== serial branch).
    const bool vfr_face =
        (opts_->face_reconstruction == FaceDepth2D::VFR_FACE);
    const double theta = opts_->theta;
    const double dry = opts_->dry_depth;
    const double fr_max = opts_->froude_max;
    const double beta_share = opts_->exchange_beta / 3.0;
    Kokkos::parallel_for(
        "fireFaces", Kokkos::RangePolicy<ExecSpace>(lo, hi),
        KOKKOS_LAMBDA(int idx) {
            const int e = list(idx);
            const int a = cLv(e), b = cRv(e);
            const double hf = vfr_face
                ? inertial::faceFlowDepthVfr(head(a), head(b),
                                             ze_lo(e), ze_hi(e))
                : inertial::faceFlowDepth(head(a), head(b), zface(e));
            if (hf <= dry) {
                qv(e) = 0.0;
                return;
            }
            double qhat  = qv(e);
            double q_mag = std::fabs(qv(e));
            if (perot) {
                const double qfx = 0.5 * (qcx(a) + qcx(b));
                const double qfy = 0.5 * (qcy(a) + qcy(b));
                const double qn  = qfx * nxv(e) + qfy * nyv(e);
                qhat  = theta * qv(e) + (1.0 - theta) * qn;
                // Vector friction magnitude, floored at |q_n| (== serial).
                const double qm = std::sqrt(qfx * qfx + qfy * qfy);
                if (qm > q_mag) q_mag = qm;
            }
            double deta = head(b) - head(a);
            if (std::fabs(deta) < inertial::kEtaDeadband) deta = 0.0;
            const double slope = deta * inv_dx(e);
            double qn1 = inertial::inertialFaceUpdate(qv(e), qhat, hf, dt_f,
                                                      slope, n2(e), q_mag);
            qn1 = inertial::froudeCap(qn1, hf, fr_max);

            const int exp_cell = (qn1 > 0.0) ? a : b;
            const int refire = 1 << (tier(exp_cell) - ftier(e));
            const double vmax = vol(exp_cell) > 0.0 ? vol(exp_cell) : 0.0;
            const double budget = beta_share / refire * vmax;
            const double take = std::fabs(qn1) * xi(e) * dt_f;
            if (take > budget) qn1 *= (take > 0.0) ? budget / take : 0.0;
            qv(e) = qn1;

            const double dM = qn1 * xi(e) * dt_f;
            faccL(e) -= dM;
            faccR(e) += dM;
        });
    face_passes_ += (hi - lo);
}

void ExplicitKokkosSurfaceSolver::fireCells(int k, double dt_c) {
    const int lo = tier_off_[k], hi = tier_off_[k + 1];
    auto list = d_cells_compact_;
    auto vol = d_volume_, head = d_head_, depth = d_depth_;
    auto faccL = d_faccL_, faccR = d_faccR_;
    auto ptr = d_cell_ptr_, edge = d_cell_edge_;
    auto sign = d_sign_;
    auto rain = d_rain_, coup = d_coup_, evap = d_evap_;
    auto qv = d_q_, xi = d_xi_, mx = d_mx_, my = d_my_;
    auto qcx = d_qcx_, qcy = d_qcy_;
    auto area = d_tri_area_, cz = d_tri_cz_, cx = d_tri_cx_, cy = d_tri_cy_;
    auto vz = d_vz_;
    auto v0 = d_tri_v0_, v1 = d_tri_v1_, v2 = d_tri_v2_;
    const bool vfr = (opts_->cell_closure == CellClosure2D::VFR);
    const double mwf = opts_->vfr_min_wet_frac;
    const bool perot = have_perot_;
    const double dry = opts_->dry_depth;
    if (lo < hi) {
        Kokkos::parallel_for(
            "fireCells", Kokkos::RangePolicy<ExecSpace>(lo, hi),
            KOKKOS_LAMBDA(int idx) {
                const int i = list(idx);
                double flux_m3 = 0.0;
                for (int p = ptr(i); p < ptr(i + 1); ++p) {
                    const int e = edge(p);
                    if (sign(p) > 0.0) {
                        flux_m3 += faccL(e);
                        faccL(e) = 0.0;
                    } else {
                        flux_m3 += faccR(e);
                        faccR(e) = 0.0;
                    }
                }
                const double src = rain(i) + coup(i)
                                   - devEvapSink(evap(i), depth(i), dry);
                double v = vol(i) + flux_m3 + dt_c * src * area(i);
                vol(i) = (v > 0.0) ? v : 0.0;
                double e2, d2;
                inertial::etaDepthScalar(area(i), cz(i), vz(v0(i)), vz(v1(i)),
                                         vz(v2(i)), vfr, mwf, vol(i), e2, d2);
                head(i) = e2;
                depth(i) = d2;
                if (perot) {
                    double sx = 0.0, sy = 0.0;
                    for (int p = ptr(i); p < ptr(i + 1); ++p) {
                        const int e = edge(p);
                        const double f = sign(p) * qv(e) * xi(e);
                        sx += f * (mx(e) - cx(i));
                        sy += f * (my(e) - cy(i));
                    }
                    const double inv_a = 1.0 / area(i);
                    qcx(i) = sx * inv_a;
                    qcy(i) = sy * inv_a;
                }
            });
    }
    if (k != 0) return;

    // Boundary edges (tier-0 cadence; single-thread device kernel preserves
    // the serial per-cell ordering for multi-edge cells).
    const int nbc = static_cast<int>(bc_cell_host_.size());
    if (nbc > 0) {
        auto bc_cell = d_bc_cell_, bc_slot = d_bc_slot_, bc_type = d_bc_type_;
        auto bc_accum = d_bc_accum_, bc_slope = d_bc_slope_;
        auto bc_head = d_bc_head_, bc_flow = d_bc_flow_;
        auto bc_q = d_bc_q_;
        auto active = d_active_;
        auto tier = d_tier_;
        auto edge_len = d_edge_length_;
        auto mann = d_mannings_n_;
        auto vxv = d_vx_, vyv = d_vy_;
        auto cxv = d_tri_cx_, cyv = d_tri_cy_;
        const bool vfr_face =
            (opts_->face_reconstruction == FaceDepth2D::VFR_FACE);
        const double fr_max = opts_->froude_max;
        const int bt_wall = static_cast<int>(BoundaryType::WALL);
        const int bt_normal = static_cast<int>(BoundaryType::NORMAL_FLOW);
        const int bt_flow = static_cast<int>(BoundaryType::SPECIFIED_FLOW);
        const int bt_rating = static_cast<int>(BoundaryType::RATING_CURVE);
        const int bt_stage = static_cast<int>(BoundaryType::SPECIFIED_STAGE);
        Kokkos::parallel_for(
            "fireBc", Kokkos::RangePolicy<ExecSpace>(0, 1),
            KOKKOS_LAMBDA(int) {
                for (int kk = 0; kk < nbc; ++kk) {
                    const int i = bc_cell(kk);
                    if (tier(i) != 0 || !active(i)) continue;
                    const int idx = bc_slot(kk);
                    const int ty = bc_type(kk);
                    const double L = edge_len(idx);
                    const double n = mann(i);
                    double f = 0.0;
                    if (ty == bt_wall) {
                        f = 0.0;
                    } else if (ty == bt_normal) {
                        const double S = bc_slope(kk);
                        double h_out = depth(i);
                        if (vfr_face) {
                            const int e = idx % 3;
                            const int vv[3] = {v0(i), v1(i), v2(i)};
                            const double za = vz(vv[(e + 1) % 3]);
                            const double zb = vz(vv[(e + 2) % 3]);
                            h_out = inertial::faceDepthFromEta(
                                head(i), (za < zb) ? za : zb,
                                (za < zb) ? zb : za);
                        }
                        if (S > 0.0 && h_out > 0.0 && n > 0.0) {
                            const double h53 =
                                h_out * std::cbrt(h_out * h_out);
                            f = -(h53 * std::sqrt(S) / n) * L;
                        }
                    } else if (ty == bt_flow || ty == bt_rating) {
                        f = -bc_flow(kk) * L;
                    } else if (ty == bt_stage) {
                        // Inertial stage boundary (mirrors the serial
                        // marcher): the interior momentum law integrated
                        // against a ghost held at η_bc, zero-gradient q.
                        const double eta_bc = bc_head(kk);
                        double hf;
                        if (vfr_face) {
                            const int e = idx % 3;
                            const int vv[3] = {v0(i), v1(i), v2(i)};
                            const double za = vz(vv[(e + 1) % 3]);
                            const double zb = vz(vv[(e + 2) % 3]);
                            const double eta_hi =
                                (head(i) > eta_bc) ? head(i) : eta_bc;
                            hf = inertial::faceDepthFromEta(
                                eta_hi, (za < zb) ? za : zb,
                                (za < zb) ? zb : za);
                        } else {
                            hf = inertial::faceFlowDepth(head(i), eta_bc,
                                                         cz(i));
                        }
                        if (hf <= dry || L <= 1.0e-12) {
                            bc_q(kk) = 0.0;
                            continue;
                        }
                        double deta = head(i) - eta_bc;
                        if (std::fabs(deta) < inertial::kEtaDeadband)
                            deta = 0.0;
                        // Ghost across the edge at the centroid→edge
                        // distance 2A/(3L).
                        const double slope =
                            deta * (3.0 * L) / (2.0 * area(i));
                        double qn1 = inertial::inertialFaceUpdate(
                            bc_q(kk), bc_q(kk), hf, dt_c, slope, n * n,
                            std::fabs(bc_q(kk)));
                        qn1 = inertial::froudeCap(qn1, hf, fr_max);
                        f = qn1 * L;   // inflow-positive
                    }
                    if (f == 0.0) {
                        bc_q(kk) = 0.0;
                        continue;
                    }
                    // Volume-space clamp; booked flux re-derived from the
                    // applied change (mirrors ExplicitInertialSolver).
                    const double v_old = vol(i);
                    double v_new = v_old + dt_c * f;
                    if (ty == bt_stage) {
                        // Equilibrium clamp, kept as the tiny-cell /
                        // overshoot backstop: one substep moves the cell AT
                        // MOST to the prescribed stage.
                        const double v_eq = inertial::volumeFromEtaScalar(
                            area(i), cz(i), vz(v0(i)), vz(v1(i)), vz(v2(i)),
                            vfr, mwf, bc_head(kk));
                        if (f < 0.0) {
                            const double lo =
                                (v_old < v_eq) ? v_old : v_eq;
                            if (v_new < lo) v_new = lo;
                        } else {
                            const double hi =
                                (v_old > v_eq) ? v_old : v_eq;
                            if (v_new > hi) v_new = hi;
                        }
                    }
                    if (v_new < 0.0) v_new = 0.0;  // availability floor
                    f = (v_new - v_old) / dt_c;
                    // Momentum matches applied mass (== serial rescale).
                    bc_q(kk) = (L > 1.0e-12) ? f / L : 0.0;
                    if (f != 0.0) {
                        vol(i) = v_new;
                        bc_accum(kk) += dt_c * f;
                        double e2, d2;
                        inertial::etaDepthScalar(area(i), cz(i), vz(v0(i)),
                                                 vz(v1(i)), vz(v2(i)), vfr,
                                                 mwf, vol(i), e2, d2);
                        head(i) = e2;
                        depth(i) = d2;
                    }
                    // Perot completion: add the boundary edge's contribution
                    // the interior-only rebuild missed (== serial marcher).
                    if (perot && bc_q(kk) != 0.0) {
                        const int e = idx % 3;
                        const int vv[3] = {v0(i), v1(i), v2(i)};
                        const int va = vv[(e + 1) % 3];
                        const int vb = vv[(e + 2) % 3];
                        const double mxb = 0.5 * (vxv(va) + vxv(vb));
                        const double myb = 0.5 * (vyv(va) + vyv(vb));
                        const double fo = -f;   // outward flux (m³/s)
                        const double inv_a = 1.0 / area(i);
                        qcx(i) += fo * (mxb - cxv(i)) * inv_a;
                        qcy(i) += fo * (myb - cyv(i)) * inv_a;
                    }
                }
            });
    }

    // Live junction exchange at tier-0 cadence — single-thread device kernel
    // preserves the serial point ordering (shared-cell drains and shared-node
    // spill budgets are order-dependent by contract).
    const int np = static_cast<int>(exch_host_.size());
    if (np > 0 && state_->nodes_1d) {
        auto cp_cell = d_cp_cell_, cp_vertex = d_cp_vertex_,
             cp_node = d_cp_node_;
        auto cp_cd = d_cp_cd_, cp_area = d_cp_area_;
        auto exch = d_exch_, node_drawn = d_node_drawn_;
        auto nh = d_node_head_, nd = d_node_depth_, nv = d_node_volume_;
        auto ninv = d_node_invert_, nfd = d_node_fulldepth_;
        auto active = d_active_;
        auto vs_ptr = d_vs_ptr_, vs_idx = d_vs_idx_;
        auto vs_wt = d_vs_wt_;
        auto vzv = d_vz_;
        const double len12 = opts_->len_1d_to_2d;
        const double vol12 = opts_->vol_1d_to_2d;
        const double beta = opts_->exchange_beta;
        Kokkos::parallel_for(
            "fireExchange", Kokkos::RangePolicy<ExecSpace>(0, 1),
            KOKKOS_LAMBDA(int) {
                for (int kk = 0; kk < np; ++kk) {
                    const int ci = cp_cell(kk);
                    if (ci < 0 || !active(ci)) continue;
                    const int ni = cp_node(kk);
                    const int vtx = cp_vertex(kk);

                    // couplingHead2D: pseudo-Laplacian vertex head (FLAT) /
                    // wet-masked depth-weighted η (VFR) / cell head.
                    double h_2d;
                    if (vtx < 0) {
                        h_2d = head(ci);
                    } else if (vfr) {
                        double num = 0.0, den = 0.0;
                        for (int p = vs_ptr(vtx); p < vs_ptr(vtx + 1); ++p) {
                            const int t = vs_idx(p);
                            const double h = depth(t);
                            if (!(h >= dry)) continue;
                            num += h * head(t);
                            den += h;
                        }
                        h_2d = (den > 0.0) ? num / den : vzv(vtx);
                    } else {
                        double h = 0.0;
                        for (int p = vs_ptr(vtx); p < vs_ptr(vtx + 1); ++p)
                            h += vs_wt(p) * head(vs_idx(p));
                        h_2d = h;
                    }
                    const double h_1d = nh(ni) * len12;

                    double depth_2d_avail;
                    if (vtx >= 0) {
                        depth_2d_avail = 0.0;
                        for (int p = vs_ptr(vtx); p < vs_ptr(vtx + 1); ++p) {
                            const double dpt = depth(vs_idx(p));
                            if (dpt > depth_2d_avail) depth_2d_avail = dpt;
                        }
                    } else {
                        depth_2d_avail = depth(ci);
                    }
                    const double depth_1d_avail = nd(ni) * len12;

                    const double crown = (ninv(ni) + nfd(ni)) * len12;
                    const double h_max = (h_1d > h_2d) ? h_1d : h_2d;
                    const double A_eff = devEffectiveArea(
                        h_max, crown, nfd(ni), cp_area(kk),
                        cp_area(kk) * 2.0);

                    double Q = devOrificeFlow(h_2d - h_1d, cp_cd(kk), A_eff);

                    const double CAP_BAND = 0.05;
                    double ct = (h_max - crown) / CAP_BAND;
                    ct = (ct < 0.0) ? 0.0 : (ct > 1.0 ? 1.0 : ct);
                    Q *= ct * ct * (3.0 - 2.0 * ct);

                    Q *= (Q > 0.0) ? devWetRamp(depth_2d_avail, dry)
                                   : devWetRamp(depth_1d_avail, dry);
                    if (Q == 0.0) continue;

                    if (Q > 0.0) {   // 2D → 1D drain: availability share
                        const double vmax = vol(ci) > 0.0 ? vol(ci) : 0.0;
                        const double cap = beta * vmax / dt_c;
                        if (Q > cap) Q = cap;
                    } else {         // 1D → 2D spill: node stored budget
                        const double avail0 = nv(ni) * vol12 - node_drawn(ni);
                        const double avail = (avail0 > 0.0) ? avail0 : 0.0;
                        if (avail <= 0.0) continue;
                        const double want = -Q * dt_c;
                        const double take = (want < avail) ? want : avail;
                        node_drawn(ni) += take;
                        Q = -take / dt_c;
                    }
                    vol(ci) -= Q * dt_c;
                    if (vol(ci) < 0.0) vol(ci) = 0.0;
                    exch(kk) += Q * dt_c;
                    double e2, d2;
                    inertial::etaDepthScalar(area(ci), cz(ci), vz(v0(ci)),
                                             vz(v1(ci)), vz(v2(ci)), vfr, mwf,
                                             vol(ci), e2, d2);
                    head(ci) = e2;
                    depth(ci) = d2;
                }
            });
    }
}

void ExplicitKokkosSurfaceSolver::runMacroCycle(double dt0, int nsub) {
    const int K = K_;
    for (int s = 0; s < nsub; ++s) {
        for (int k = 0; k < K; ++k) {
            if (s % (1 << k)) continue;
            fireFaces(k, (1 << k) * dt0);
        }
        for (int k = 0; k < K; ++k) {
            if (s % (1 << k)) continue;
            fireCells(k, (1 << k) * dt0);
        }
        ++substeps_run_;
        ++last_steps_;
    }
}

// ===========================================================================
// advance
// ===========================================================================

void ExplicitKokkosSurfaceSolver::pushForcings() {
    devRefresh(d_rain_, state_->rainfall);
    devRefresh(d_coup_, state_->coupling_flux);
    devRefresh(d_evap_, state_->evap_rate);

    // Boundary values were resolved host-side for this batch.
    const int nbc = static_cast<int>(bc_cell_host_.size());
    if (nbc > 0 && state_->boundary) {
        std::vector<int> ty(nbc);
        std::vector<double> sl(nbc), hd(nbc), fl(nbc);
        const auto& b = *state_->boundary;
        for (int k = 0; k < nbc; ++k) {
            const int idx = bc_slot_host_[k];
            ty[k] = static_cast<int>(b.edge_bc_type[idx]);
            sl[k] = b.edge_bed_slope[idx];
            hd[k] = b.edge_bc_head[idx];
            fl[k] = b.edge_bc_flow[idx];
        }
        Kokkos::View<const int*, Kokkos::HostSpace,
                     Kokkos::MemoryTraits<Kokkos::Unmanaged>>
            hty(ty.data(), ty.size());
        Kokkos::deep_copy(d_bc_type_, hty);
        devRefresh(d_bc_slope_, sl);
        devRefresh(d_bc_head_, hd);
        devRefresh(d_bc_flow_, fl);
    }
}

void ExplicitKokkosSurfaceSolver::pushNodeState() {
    if (!state_->nodes_1d || d_node_head_.extent(0) == 0) return;
    devRefresh(d_node_head_, state_->nodes_1d->head);
    devRefresh(d_node_depth_, state_->nodes_1d->depth);
    devRefresh(d_node_volume_, state_->nodes_1d->volume);
}

void ExplicitKokkosSurfaceSolver::publishAndCopyBack(double t_current,
                                                     double t_target) {
    const int ne = edges_.ne;
    Kokkos::deep_copy(d_edge_flux_, 0.0);
    {
        auto ef = d_edge_flux_;
        auto cLv = d_cL_, cRv = d_cR_, slotL = d_slotL_, slotR = d_slotR_;
        auto head = d_head_, zface = d_zface_, qv = d_q_, xi = d_xi_;
        auto ze_lo = d_ze_lo_, ze_hi = d_ze_hi_;
        const bool vfr_face =
            (opts_->face_reconstruction == FaceDepth2D::VFR_FACE);
        const double dry = opts_->dry_depth;
        const double fr_max = opts_->froude_max;
        Kokkos::parallel_for(
            "publish_flux", Kokkos::RangePolicy<ExecSpace>(0, ne),
            KOKKOS_LAMBDA(int e) {
                const double hf = vfr_face
                    ? inertial::faceFlowDepthVfr(head(cLv(e)), head(cRv(e)),
                                                 ze_lo(e), ze_hi(e))
                    : inertial::faceFlowDepth(head(cLv(e)), head(cRv(e)),
                                              zface(e));
                double qp = 0.0;
                if (hf > dry) qp = inertial::froudeCap(qv(e), hf, fr_max);
                const double F = qp * xi(e);
                ef(slotL(e)) = -F;
                ef(slotR(e)) = +F;
            });
    }
    const double span = t_target - t_current;
    const int nbc = static_cast<int>(bc_cell_host_.size());
    if (span > 0.0 && nbc > 0) {
        auto ef = d_edge_flux_;
        auto bc_slot = d_bc_slot_;
        auto bc_accum = d_bc_accum_;
        Kokkos::parallel_for(
            "publish_bc", Kokkos::RangePolicy<ExecSpace>(0, nbc),
            KOKKOS_LAMBDA(int k) { ef(bc_slot(k)) = bc_accum(k) / span; });
    }
    Kokkos::fence();

    hostRefresh(state_->volume, d_volume_);
    hostRefresh(state_->head, d_head_);
    hostRefresh(state_->depth, d_depth_);
    hostRefresh(state_->edge_flux, d_edge_flux_);
    if (!exch_host_.empty()) hostRefresh(exch_host_, d_exch_);
}

double ExplicitKokkosSurfaceSolver::advance(double t_current,
                                            double t_target) {
    if (!initialized_ || t_target <= t_current) return t_target;

    double t = t_current;
    if (t_last_sync_ > t_current) t_last_sync_ = t_current;
    pushForcings();
    pushNodeState();
    Kokkos::deep_copy(d_bc_accum_, 0.0);
    Kokkos::deep_copy(d_exch_, 0.0);
    Kokkos::deep_copy(d_node_drawn_, 0.0);
    last_steps_ = 0;
    int cycles_since_rebuild = cycles_since_rebuild_;

    while (t < t_target) {
        if (cycles_since_rebuild >= kRebuildEveryCycles) {
            syncAndRebuild(t);
            cycles_since_rebuild = 0;
        } else {
            refreshDt0();
        }
        const int nsub_full = 1 << (K_ - 1);
        const double remaining = t_target - t;

        if (n_active_ == 0) {
            t = t_target;
            last_dt_ = remaining;
            break;
        }

        double dt0 = std::min(dt0_, remaining);
        int nsub = nsub_full;
        if (nsub_full * dt0_ > remaining) {
            collapseToGlobalDt();
            nsub = 1;
            cycles_since_rebuild = kRebuildEveryCycles;   // rebuild after tail
        }

        runMacroCycle(dt0, nsub);
        t += nsub * dt0;
        last_dt_ = dt0;
        ++cycles_since_rebuild;
    }

    cycles_since_rebuild_ = cycles_since_rebuild;
    if (t_target > t_last_sync_) {
        if (cycles_since_rebuild_ >= kRebuildEveryCycles) {
            syncAndRebuild(t_target);
            cycles_since_rebuild_ = 0;
        } else {
            lazySourcesDev(t_target);
        }
    }

    publishAndCopyBack(t_current, t_target);
    return t_target;
}

// ===========================================================================
// reinitialize / resync / finalize / stats
// ===========================================================================

void ExplicitKokkosSurfaceSolver::reinitialize(double /*t0*/) {
    if (!initialized_) return;
    // External state edit: volumes authoritative; momentum + pending stale.
    devRefresh(d_volume_, state_->volume);
    Kokkos::deep_copy(d_q_, 0.0);
    Kokkos::deep_copy(d_bc_q_, 0.0);
    Kokkos::deep_copy(d_faccL_, 0.0);
    Kokkos::deep_copy(d_faccR_, 0.0);
    reconstructAllDev();
    cycles_since_rebuild_ = 1000;
}

void ExplicitKokkosSurfaceSolver::resyncFromVolumes(double /*t0*/) {
    if (!initialized_) return;
    devRefresh(d_volume_, state_->volume);
    reconstructAllDev();
}

void ExplicitKokkosSurfaceSolver::finalize() {
    if (!initialized_) return;
    if (!telemetry_path_.empty() && !telemetry_.empty()) {
        if (std::FILE* f = std::fopen(telemetry_path_.c_str(), "w")) {
            std::fprintf(f, "t_s,active_cells,active_frac\n");
            const double nt = std::max(1, mesh_ ? mesh_->n_triangles() : 1);
            for (const auto& [t, n] : telemetry_)
                std::fprintf(f, "%.3f,%d,%.6f\n", t, n, n / nt);
            std::fclose(f);
        }
    }
    telemetry_.clear();
    initialized_ = false;
}

ISurfaceSolver::RunStats ExplicitKokkosSurfaceSolver::run_stats()
    const noexcept {
    RunStats s;
    s.nsteps = substeps_run_;
    s.nrhs   = face_passes_;
    s.last_h = last_dt_;
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
    s.n_tiers = K_;
    for (int k = 0; k < s.n_tiers; ++k)
        s.tier_cells[k] = tier_occupancy_[static_cast<std::size_t>(k)];
    return s;
}

} // namespace openswmm::twoD::gpu
