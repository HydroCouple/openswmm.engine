/**
 * @file      ExplicitKokkosSurfaceSolver.cpp
 * @brief     Kokkos port of the explicit local-inertial LTS marcher.
 *
 * @details Numerics are the serial ExplicitInertialSolver's, kernel for
 *          kernel — the scalar bodies come from InertialKernels.hpp /
 *          VfrClosure.hpp compiled with OPENSWMM_KERNEL_FN =
 *          KOKKOS_INLINE_FUNCTION (single source). See the header for the
 *          loop-structure mapping and the determinism contract.
 *
 * @see ExplicitInertialSolver.cpp (serial reference), ExplicitKokkosSurfaceSolver.hpp
 * @ingroup engine_2d_gpu
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include <Kokkos_Core.hpp>

// Single-source scalar kernels, device-annotated for this TU.
#define OPENSWMM_KERNEL_FN KOKKOS_INLINE_FUNCTION

#include "ExplicitKokkosSurfaceSolver.hpp"
// Relative, not "core/FileIO.hpp": this TU is also compiled into the STANDALONE
// GPU plugin targets (openswmm_gpu_omp / _cuda), whose include path is only
// ../solver and ../data — src/engine is not on it. Matches how every other
// engine header is reached from this file (../data/MeshData.hpp, etc.).
#include "../../core/FileIO.hpp" // issue #7: UTF-8 paths on Windows

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <numeric>

#include "KokkosPerfCounters.hpp"
#include "KokkosSourceKernels.hpp"
#include <stdexcept>
#include "../data/MeshData.hpp"
#include "../data/SolverOptions2D.hpp"
#include "../data/SurfaceStateData.hpp"
#include "../data/BoundaryData.hpp"
#include "../coupling/NodeCoupling.hpp" // CouplingPoint
#include "../coupling/PondModel.hpp"
#include "../solver/InertialKernels.hpp"
#include "../solver/DiffusiveKernels.hpp"
#include "../solver/SweKernels.hpp"
#include "../solver/SurfaceCapabilities.hpp"
#include "../../data/NodeData.hpp"

namespace openswmm::twoD::gpu {

/// The cell volume ↔ free-surface closure on the device: the triangle relation, or the quad
/// two-plane relation for a four-vertex cell under VFR. Same dispatch as inertial::cellEtaDepth
/// and inertial::cellVolumeFromEta.
struct CellClosure {
    DView area, cz, vz, quad_z, quad_a;
    IView cvv, nv;
    bool vfr, quads;
    double mwf;

    /// Volume → (η, depth) for cell i.
    KOKKOS_INLINE_FUNCTION void etaDepth(int i, double V, double& eta, double& depth) const {
        if (vfr && quads && nv(i) == 4) {
            inertial::etaDepthQuadScalar(area(i), cz(i), &quad_z(i * kQuadVfrZ), quad_a(i * 2),
                                         quad_a(i * 2 + 1), true, mwf, V, eta, depth);
            return;
        }
        inertial::etaDepthScalar(area(i), cz(i), vz(cvv(i * kMaxCellVerts + 0)),
                                 vz(cvv(i * kMaxCellVerts + 1)), vz(cvv(i * kMaxCellVerts + 2)),
                                 vfr, mwf, V, eta, depth);
    }
    /// Free surface → volume for cell i.
    KOKKOS_INLINE_FUNCTION double volumeFromEta(int i, double eta) const {
        if (quads && nv(i) == 4)
            return inertial::volumeFromEtaQuadScalar(area(i), cz(i), &quad_z(i * kQuadVfrZ),
                                                     quad_a(i * 2), quad_a(i * 2 + 1), vfr, mwf,
                                                     eta);
        return inertial::volumeFromEtaScalar(area(i), cz(i), vz(cvv(i * kMaxCellVerts + 0)),
                                             vz(cvv(i * kMaxCellVerts + 1)),
                                             vz(cvv(i * kMaxCellVerts + 2)), vfr, mwf, eta);
    }
};

CellClosure ExplicitKokkosSurfaceSolver::closure() const {
    return CellClosure{d_tri_area_, d_tri_cz_,
                       d_vz_,       d_quad_z_,
                       d_quad_a_,   d_cell_v_,
                       d_nv_,       opts_->cell_closure == CellClosure2D::VFR,
                       has_quads_,  opts_->vfr_min_wet_frac};
}

namespace {

/// Active-set / tier rebuild cadence in macro cycles (== serial marcher).
constexpr int kRebuildEveryCycles = 4;
/// Halo depth (rings) under FRONT_REBUILD; == the serial marcher's constant.
constexpr int kFrontHaloRings = 5;

// ---------------------------------------------------------------------------
// Host→device mirroring helpers.
// ---------------------------------------------------------------------------
/**
 * @brief Allocate and populate one execution-space double view from a host vector.
 * @param[in] name Nonnull terminated Kokkos allocation label, copied into std::string.
 * @param[in] v Borrowed contiguous host double values; stable through synchronous deep_copy.
 * @return Ref-counted device view owning v.size() elements with identical values. Empty input
 * yields an empty view; no host pointer is retained. Kokkos must be initialized and not
 * finalizing.
 * @note Allocations/deep-copy failures propagate; this does not synchronize unrelated owner
 * state.
 */
DView devCopy(const char* name, const std::vector<double>& v) {
    DView d(Kokkos::view_alloc(Kokkos::WithoutInitializing, std::string(name)), v.size());
    Kokkos::View<const double*, Kokkos::HostSpace, Kokkos::MemoryTraits<Kokkos::Unmanaged>> h(
        v.data(), v.size());
    Kokkos::deep_copy(d, h);
    return d;
}

/**
 * @brief Allocate and populate one execution-space int view from a host vector.
 * @param[in] name Nonnull terminated Kokkos allocation label, copied into std::string.
 * @param[in] v Borrowed contiguous host int values; stable through synchronous deep_copy.
 * @return Ref-counted device view owning v.size() elements with identical values. Empty input
 * yields an empty view; no host pointer is retained. Kokkos must be initialized and not
 * finalizing.
 * @note Allocations/deep-copy failures propagate; this does not synchronize unrelated owner
 * state.
 */
IView devCopy(const char* name, const std::vector<int>& v) {
    IView d(Kokkos::view_alloc(Kokkos::WithoutInitializing, std::string(name)), v.size());
    Kokkos::View<const int*, Kokkos::HostSpace, Kokkos::MemoryTraits<Kokkos::Unmanaged>> h(
        v.data(), v.size());
    Kokkos::deep_copy(d, h);
    return d;
}

/**
 * @brief Refresh an existing device view from a host vector (no realloc).
 *
 * @param[in] d Existing writable execution-space double view, despite const handle reference;
 * its extent must exactly match v.size(). Ownership/shape is unchanged.
 * @param[in] v Borrowed host values, stable through synchronous deep_copy; not retained.
 * @note Replaces device contents, without allocation of the destination or shape validation
 * here. Requires initialized Kokkos and serialized view access; Kokkos failure propagates/aborts
 * per runtime.
 */
void devRefresh(const DView& d, const std::vector<double>& v) {
    Kokkos::View<const double*, Kokkos::HostSpace, Kokkos::MemoryTraits<Kokkos::Unmanaged>> h(
        v.data(), v.size());
    Kokkos::deep_copy(d, h);
}

/**
 * @brief Copy a device view back into a host vector.
 *
 * @param[in,out] v Existing writable host vector, sized exactly to d.extent(0), stable during
 * copy.
 * @param[in] d Readable execution-space double view, not modified.
 * @note Copies synchronously through an unmanaged HostSpace view; never resizes v or retains its
 * address. Caller serializes device/host owner access. Kokkos failure propagates/aborts per
 * runtime.
 */
void hostRefresh(std::vector<double>& v, const DView& d) {
    Kokkos::View<double*, Kokkos::HostSpace, Kokkos::MemoryTraits<Kokkos::Unmanaged>> h(v.data(),
                                                                                        v.size());
    Kokkos::deep_copy(h, d);
}

// ---------------------------------------------------------------------------
// Device twins of the small host-only helpers (bodies copied verbatim from
// SurfaceFluxCalculator.cpp / NodeCoupling.cpp — bit-identical math).
// ---------------------------------------------------------------------------
constexpr double kOrificeHEps = 0.02; // == NodeCoupling.cpp ORIFICE_H_EPS

/**
 * @brief Evaluate the device regularized square-root head law.
 * @param[in] a Finite nonnegative absolute head difference m.
 * @return sqrt(a) above 0.02 m, otherwise a C1 quadratic matching the value/slope at that
 * threshold. No validation, allocation or owner mutation; negative/nonfinite inputs are outside
 * the contract.
 */
KOKKOS_INLINE_FUNCTION double devOrificePhi(double a) {
    if (a >= kOrificeHEps)
        return std::sqrt(a);
    const double inv_sqrt_e = 1.0 / std::sqrt(kOrificeHEps);
    return (1.5 * inv_sqrt_e) * a - (0.5 * inv_sqrt_e / kOrificeHEps) * a * a;
}

/**
 * @brief Evaluate signed device orifice flow using the regularized root.
 * @param[in] dh Finite signed surface-minus-node head m.
 * @param[in] cd Finite nonnegative discharge coefficient.
 * @param[in] area Finite nonnegative effective aperture m2.
 * @return Signed m3/s, positive along dh; zero for |dh|<1e-12 m. No crown/wetness/source-budget
 * validation or reservation occurs. Allocation-free device helper.
 */
KOKKOS_INLINE_FUNCTION double devOrificeFlow(double dh, double cd, double area) {
    const double a = std::fabs(dh);
    if (a < 1.0e-12)
        return 0.0;
    const double sign = (dh > 0.0) ? 1.0 : -1.0;
    return sign * cd * area * std::sqrt(2.0 * inertial::kGravity) * devOrificePhi(a);
}

/**
 * @brief Interpolate the device opening area across the crown transition.
 * @param[in] h_max Higher driving head m.
 * @param[in] z_ground Crown transition elevation m in the same datum.
 * @param[in] A_inlet Finite inlet area m2 below the transition.
 * @param[in] A_manhole Finite full manhole area m2 at/above z_ground+0.05 m.
 * @return Linear area ramp across 0.05 m, with fixed endpoint areas. The unnamed full_depth
 * argument is unused. No validation, allocation or owner change occurs.
 */
KOKKOS_INLINE_FUNCTION double devEffectiveArea(double h_max, double z_ground, double /*full_depth*/,
                                               double A_inlet, double A_manhole) {
    if (h_max < z_ground)
        return A_inlet;
    const double d_trans = 0.05;
    const double frac = (h_max - z_ground) / d_trans < 1.0 ? (h_max - z_ground) / d_trans : 1.0;
    return A_inlet + frac * (A_manhole - A_inlet);
}

/**
 * @brief Apply the device source-side Hermite wetness ramp.
 * @param[in] d Finite source water depth m.
 * @param[in] dry_depth Finite positive ramp width m.
 * @return smoothstep(clamp(d/dry_depth,0,1)), dimensionless. No zero/finiteness guards or
 * mutation.
 */
KOKKOS_INLINE_FUNCTION double devWetRamp(double d, double dry_depth) {
    double t = d / dry_depth;
    t = (t < 0.0) ? 0.0 : (t > 1.0 ? 1.0 : t);
    return t * t * (3.0 - 2.0 * t);
}

} // anonymous namespace

// ===========================================================================
// initialize
// ===========================================================================

void ExplicitKokkosSurfaceSolver::initialize(MeshData& mesh, SurfaceStateData& state,
                                             SolverOptions2D& opts) {
    kperf::Scope performance_scope(perf_counters_);
    // Direct plugin users receive the same restriction as the factory: one
    // capability set (SurfaceCapabilities.hpp) decides both.
    bool has_quads = false;
    for (int i = 0; i < mesh.n_cells() && !has_quads; ++i)
        has_quads = mesh.cell_vertex_count(i) != 3;
    if (const std::uint64_t lacking =
            requiredSurfaceCapabilities(opts, has_quads, state.transport.active()) &
            ~kKokkosSurfaceCapabilities)
        throw std::invalid_argument("Kokkos marcher does not implement: " +
                                    surfaceCapabilityNames(lacking));
    mesh_ = &mesh;
    state_ = &state;
    opts_ = &opts;

    // Perf review Phase A2: launch/fence/deep_copy counters (OPENSWMM_PERF).
    // Installed after Kokkos::initialize (plugin construction), before the
    // devCopy mirroring below so initialize's uploads are counted too.
    kperf::install();
    kperf::reset();

    const int nt = mesh.n_triangles();
    if (nt <= 0)
        return;

    edges_.build(mesh);
    const int ne = edges_.ne;

    // Geometry mirrors (const for the run).
    d_tri_area_ = devCopy("tri_area", mesh.tri_area);
    d_tri_cz_ = devCopy("tri_cz", mesh.tri_cz);
    d_tri_cx_ = devCopy("tri_cx", mesh.tri_cx);
    d_tri_cy_ = devCopy("tri_cy", mesh.tri_cy);
    d_vz_ = devCopy("vz", mesh.vz);
    d_vx_ = devCopy("vx", mesh.vx);
    d_vy_ = devCopy("vy", mesh.vy);
    // Padded [cell*kMaxCellVerts + k] connectivity (MeshData); a triangle's
    // fourth slot is padding and is never read.
    d_cell_v_ = devCopy("cell_v", mesh.cell_v);
    has_quads_ = has_quads;
    d_nv_ = devCopy("cell_nv", std::vector<int>(mesh.cell_nv.begin(), mesh.cell_nv.end()));
    // Quad two-plane closure tables and centroid-to-edge distances: read by
    // four-vertex cells only.
    d_quad_z_ = has_quads_ ? devCopy("quad_vfr_z", mesh.quad_vfr_z) : DView();
    d_quad_a_ = has_quads_ ? devCopy("quad_vfr_a", mesh.quad_vfr_a) : DView();
    d_slot_dist_ = has_quads_ ? devCopy("slot_dist", mesh.edge_dist_c_values()) : DView();
    d_vs_ptr_ = devCopy("vs_ptr", mesh.vert_stencil_ptr);
    d_vs_idx_ = devCopy("vs_idx", mesh.vert_stencil_idx);
    d_vs_wt_ = devCopy("vs_wt", mesh.vert_stencil_wt);
    d_edge_length_ = devCopy("edge_length", mesh.edge_length);
    d_mannings_n_ = devCopy("mannings_n", mesh.mannings_n);

    d_cL_ = devCopy("cL", edges_.cL);
    d_cR_ = devCopy("cR", edges_.cR);
    d_slotL_ = devCopy("slotL", edges_.slotL);
    d_slotR_ = devCopy("slotR", edges_.slotR);
    d_xi_ = devCopy("xi", edges_.xi);
    d_conveyance_ = devCopy("conveyance", edges_.conveyance);
    d_inv_dx_ = devCopy("inv_dx_normal", edges_.inv_dx_normal);
    d_zface_ = devCopy("zface", edges_.zface);
    d_ze_lo_ = devCopy("ze_lo", edges_.ze_lo);
    d_ze_hi_ = devCopy("ze_hi", edges_.ze_hi);
    d_nx_ = devCopy("nx", edges_.nx);
    d_ny_ = devCopy("ny", edges_.ny);
    d_mx_ = devCopy("mx", edges_.mx);
    d_my_ = devCopy("my", edges_.my);
    d_n2_ = devCopy("n2_face", edges_.n2_face);
    d_lchar_ = devCopy("cell_lchar", edges_.cell_lchar);
    diffusive_ = opts.momentum == Momentum2D::DIFFUSIVE_WAVE;
    swe_ = opts.momentum == Momentum2D::FULL_SWE;
    if (swe_ && opts.cfl_number > 0.5) {
        // Positivity bound of the first-order Godunov update (== serial marcher).
        if (opts.cfl_number != SolverOptions2D{}.cfl_number)
            std::fprintf(stderr,
                         "[openswmm 2D] MOMENTUM_EQUATION FULL_SWE: CFL_NUMBER %.3g "
                         "reduced to 0.5 (positivity bound of the Godunov update).\n",
                         opts.cfl_number);
        opts.cfl_number = 0.5;
    }
    second_order_ = false;
    if (swe_ && opts.reconstruction_order == 2) {
        if (opts.lts_tiers > 1) {
            std::fprintf(stderr,
                         "[openswmm 2D] RECONSTRUCTION_ORDER 2 runs in global-dt mode: "
                         "LTS_TIERS %d reduced to 1.\n",
                         opts.lts_tiers);
            opts.lts_tiers = 1;
        }
        // The plugin cannot build the vertex stencils (that code lives in the
        // core); the router and the CPU marcher build them before this point.
        if (mesh.vert_stencil_ptr.size() != std::size_t(mesh.n_vertices() + 1))
            throw std::invalid_argument(
                "Kokkos marcher: RECONSTRUCTION_ORDER 2 requires built vertex stencils");
        second_order_ = true;
    }
    muscl_ = second_order_ && std::getenv("OPENSWMM_2D_MUSCL_OFF") == nullptr;
    {
        const int nv2 = second_order_ ? mesh.n_vertices() : 0;
        const int nt2 = second_order_ ? nt : 0;
        d_gex_ = DView("gex", 2 * nt2);
        d_gey_ = DView("gey", 2 * nt2);
        d_gux_ = DView("gux", nt2);
        d_guy_ = DView("guy", nt2);
        d_gvx_ = DView("gvx", nt2);
        d_gvy_ = DView("gvy", nt2);
        d_vbounds_ = DView("vertex_bounds", 4 * nv2);
        d_arm_x_ = second_order_ ? devCopy("cell_arm_x", edges_.cell_arm_x) : DView();
        d_arm_y_ = second_order_ ? devCopy("cell_arm_y", edges_.cell_arm_y) : DView();
        d_slot_mx_ = second_order_ ? devCopy("slot_mx", mesh.edge_mid_values(0)) : DView();
        d_slot_my_ = second_order_ ? devCopy("slot_my", mesh.edge_mid_values(1)) : DView();
        d_rk_v0_ = DView("rk_v0", nt2);
        d_rk_qx0_ = DView("rk_qx0", nt2);
        d_rk_qy0_ = DView("rk_qy0", nt2);
        d_rk_inf0_ = DView("rk_inf0", nt2);
        d_rk_cpl0_ = DView("rk_cpl0", nt2);
        d_rk_evap0_ = DView("rk_evap0", nt2);
    }
    // Optional momentum physics: the wall-drag slots, the option checks and
    // the work arrays (== serial marcher's initialize).
    {
        std::vector<int> wd_slot, wd_group(1, 0);
        std::vector<double> wd_cw;
        if (state.boundary) {
            const auto& b = *state.boundary;
            // Only the edges with a drag are stored; visit them in slot order.
            for (const auto& [stored_slot, cw] : b.edge_wall_drag.entries()) {
                const int slot = int(stored_slot);
                if (!std::isfinite(cw) || cw < 0)
                    throw std::invalid_argument("WALL_DRAG requires finite nonnegative Cw.");
                if (cw == 0)
                    continue;
                const int i = MeshData::slot_cell(slot), e = MeshData::slot_local(slot);
                if (i >= mesh.n_cells() || e >= mesh.cell_vertex_count(i) ||
                    mesh.cell_neighbour(i, e) >= 0 ||
                    b.edge_bc_type[slot] != int8_t(BoundaryType::WALL))
                    throw std::invalid_argument("WALL_DRAG must name an exterior WALL edge.");
                wd_slot.push_back(slot);
                wd_cw.push_back(cw);
            }
        }
        opts.wall_drag_enabled = !wd_slot.empty();
        if (!std::isfinite(opts.eddy_viscosity_background) || opts.eddy_viscosity_background < 0)
            throw std::invalid_argument(
                "EDDY_VISCOSITY_BACKGROUND must be finite and nonnegative.");
        physics_ = opts.momentumPhysics();
        wall_drag_energy_ = 0.0;
        ph_substeps_ = ph_retries_ = 0;
        ph_last_limit_ = std::numeric_limits<double>::infinity();
        if (physics_) {
            if (!swe_ || opts.lts_tiers != 1)
                throw std::invalid_argument(
                    "Coriolis/Smagorinsky/wall drag require FULL_SWE and LTS_TIERS 1.");
            if (opts.coriolis == Coriolis2D::LATITUDE && !opts.coriolis_latitude_set)
                throw std::invalid_argument("CORIOLIS LATITUDE requires CORIOLIS_LATITUDE.");
            if (opts.coriolis == Coriolis2D::CONSTANT_F && !opts.coriolis_f_set)
                throw std::invalid_argument("CORIOLIS CONSTANT_F requires CORIOLIS_F.");
            if (!std::isfinite(opts.coriolisParameter()) || !std::isfinite(opts.smagorinsky_cs) ||
                !std::isfinite(opts.mixing_length_coefficient) ||
                opts.mixing_length_coefficient < 0 || opts.smagorinsky_cs < 0 ||
                !std::isfinite(opts.coriolis_latitude) || std::abs(opts.coriolis_latitude) > 90)
                throw std::invalid_argument("Invalid Coriolis/Smagorinsky parameters.");
            if (state.heat.enabled || state.snow.active() ||
                (state.node_coupling && !state.node_coupling->empty()))
                throw std::invalid_argument("Coriolis/Smagorinsky/wall drag currently require "
                                            "uncoupled surface hydraulics.");
            std::fprintf(stderr,
                         "[openswmm 2D] optional momentum physics: f=%.17g s^-1, "
                         "turbulence=%s Cs=%.17g, mixing-length c=%.17g, background nu=%.17g "
                         "m2/s, Kokkos global stepping.\n",
                         opts.coriolisParameter(), turbulenceName(opts.turbulence),
                         opts.smagorinsky_cs, opts.mixing_length_coefficient,
                         opts.eddy_viscosity_background);
        }
        // Slots of one cell are consecutive: one group per cell.
        for (std::size_t k = 1; k <= wd_slot.size(); ++k)
            if (k == wd_slot.size() ||
                MeshData::slot_cell(wd_slot[k]) != MeshData::slot_cell(wd_slot[k - 1]))
                wd_group.push_back(static_cast<int>(k));
        n_wall_drag_ = static_cast<int>(wd_slot.size());
        n_wall_groups_ = static_cast<int>(wd_group.size()) - 1;
        d_wd_slot_ = devCopy("wall_drag_slot", wd_slot);
        d_wd_cw_ = devCopy("wall_drag_cw", wd_cw);
        d_wd_group_ = devCopy("wall_drag_group", wd_group);
        d_wd_removed_ = DView("wall_drag_removed", 2 * n_wall_drag_);
        const int ntp = physics_ ? nt : 0, nep = physics_ ? ne : 0;
        for (DView* v : {&d_ph_rx_, &d_ph_ry_, &d_ph_sx_, &d_ph_sy_, &d_ph_tx_, &d_ph_ty_,
                         &d_ph_x0_, &d_ph_y0_, &d_ph_oldx_, &d_ph_oldy_, &d_ph_visc_, &d_ph_term_,
                         &d_held_vol_, &d_held_qx_, &d_held_qy_})
            *v = DView("physics_cell", ntp);
        for (DView* v : {&d_ph_etx_, &d_ph_ety_, &d_ph_ebx_, &d_ph_eby_})
            *v = DView("physics_face", nep);
        d_ph_stgx_ = DView("stencil_gx", 5 * ntp);
        d_ph_stgy_ = DView("stencil_gy", 5 * ntp);
        d_ph_stid_ = IView("stencil_id", 5 * ntp);
        d_ph_stn_ = IView("stencil_count", ntp);
        d_ph_single_ = IView("stencil_single", ntp);
        d_ph_conn_ = IView("face_connected", nep);
        std::vector<int> sorted_edge;
        std::vector<double> wall_distance;
        if (physics_) {
            // Each cell's faces in ascending face order, the order in which
            // the CPU's face loop adds stress to the cell.
            sorted_edge = edges_.cell_edge;
            for (int i = 0; i < nt; ++i)
                std::sort(sorted_edge.begin() + edges_.cell_ptr[i],
                          sorted_edge.begin() + edges_.cell_ptr[i + 1]);
            if (opts.turbulence == Turbulence2D::MIXING_LENGTH &&
                opts.mixing_length_coefficient > 0) {
                // Distance from each centroid to the nearest mesh boundary edge
                // (== MomentumPhysics2D::computeWallDistance).
                std::vector<std::array<double, 4>> segments;
                for (int c = 0; c < mesh.n_cells(); ++c)
                    for (int k = 0; k < mesh.cell_vertex_count(c); ++k)
                        if (mesh.cell_neighbour(c, k) < 0) {
                            int a, b;
                            mesh.cell_edge_vertices(c, k, a, b);
                            segments.push_back({mesh.vx[a], mesh.vy[a], mesh.vx[b], mesh.vy[b]});
                        }
                wall_distance.assign(static_cast<std::size_t>(nt),
                                     std::numeric_limits<double>::infinity());
                for (int c = 0; c < nt; ++c) {
                    const double px = mesh.tri_cx[c], py = mesh.tri_cy[c];
                    double best = std::numeric_limits<double>::infinity();
                    for (const auto& g : segments) {
                        const double dx = g[2] - g[0], dy = g[3] - g[1], len2 = dx * dx + dy * dy;
                        const double t =
                            len2 > 0
                                ? std::clamp(((px - g[0]) * dx + (py - g[1]) * dy) / len2, 0.0, 1.0)
                                : 0.0;
                        best = std::min(best, std::hypot(px - g[0] - t * dx, py - g[1] - t * dy));
                    }
                    wall_distance[c] = best;
                }
            }
        }
        d_ph_sedge_ = devCopy("sorted_cell_edge", sorted_edge);
        d_ph_walld_ = devCopy("wall_distance", wall_distance);
    }
    // FRONT_REBUILD AUTO is on for FULL_SWE only (== serial marcher).
    front_rebuild_ = (opts.front_rebuild < 0) ? swe_ : (opts.front_rebuild != 0);
    front_breach_ = false;
    d_frontier_ = IView("frontier", front_rebuild_ ? nt : 0);
    d_fmxL_ = DView("fmxL", swe_ ? ne : 0);
    d_fmxR_ = DView("fmxR", swe_ ? ne : 0);
    d_fmyL_ = DView("fmyL", swe_ ? ne : 0);
    d_fmyR_ = DView("fmyR", swe_ ? ne : 0);
    d_lpos_ = swe_ ? devCopy("cell_lpos", edges_.cell_lpos) : DView();
    d_slot_nx_ = swe_ ? devCopy("slot_nx", mesh.edge_nx) : DView();
    d_slot_ny_ = swe_ ? devCopy("slot_ny", mesh.edge_ny) : DView();
    {
        // WALL boundary slots per cell, CSR, ascending slot order (== serial marcher).
        std::vector<int> wall_ptr(1, 0), wall_slot;
        if (swe_) {
            wall_ptr.reserve(static_cast<std::size_t>(nt) + 1);
            for (int i = 0; i < nt; ++i) {
                const int nvc = mesh.cell_vertex_count(i);
                for (int k2 = 0; k2 < nvc; ++k2) {
                    if (mesh.cell_neighbour(i, k2) >= 0)
                        continue;
                    const int slot = MeshData::slot(i, k2);
                    if (state.boundary &&
                        static_cast<BoundaryType>(state.boundary->edge_bc_type[slot]) !=
                            BoundaryType::WALL)
                        continue;
                    wall_slot.push_back(slot);
                }
                wall_ptr.push_back(static_cast<int>(wall_slot.size()));
            }
        }
        d_wall_ptr_ = devCopy("wall_ptr", wall_ptr);
        d_wall_slot_ = devCopy("wall_slot", wall_slot);
    }
    d_n_face_ = diffusive_ ? devCopy("n_face", edges_.n_face) : DView();
    d_dw_slope_ = DView("dw_slope", diffusive_ ? nt : 0);
    d_cell_ptr_ = devCopy("cell_ptr", edges_.cell_ptr);
    d_cell_edge_ = devCopy("cell_edge", edges_.cell_edge);
    {
        // edges_.cell_sign is int8 on the host (gather-traffic diet in the CPU
        // marcher); the device kernels keep it as doubles they multiply by.
        std::vector<double> sgn(edges_.cell_sign.begin(), edges_.cell_sign.end());
        d_sign_ = devCopy("cell_sign", sgn);
    }

    // State + marching arrays.
    d_volume_ = devCopy("volume", state.volume);
    d_head_ = DView("head", nt);
    d_depth_ = DView("depth", nt);
    d_q_ = DView("q", ne);
    d_faccL_ = DView("faccL", ne);
    d_faccR_ = DView("faccR", ne);
    // Perot vectors serve the θ-blend AND the convective term (== serial).
    // FULL_SWE uses the same arrays as its prognostic cell momentum and has
    // no Perot reconstruction.
    have_perot_ = !swe_ && (opts.theta < 1.0 || opts.advection);
    d_qcx_ = DView("qcx", (have_perot_ || swe_) ? nt : 0);
    d_qcy_ = DView("qcy", (have_perot_ || swe_) ? nt : 0);
    d_rain_ = DView("rain", nt);
    d_coup_ = DView("coup", nt);
    d_evap_ = DView("evap", nt);
    d_infil_ = DView("infil", nt);
    d_evap_applied_ = DView("evap_applied", nt);
    evap_applied_host_.assign(static_cast<std::size_t>(nt), 0.0);
    d_infil_applied_ = DView("infil_applied", nt);
    infil_applied_host_.assign(static_cast<std::size_t>(nt), 0.0);
    d_coupling_applied_ = DView("coupling_applied", nt);
    coupling_applied_host_.assign(static_cast<std::size_t>(nt), 0.0);
    d_edge_flux_ = DView("edge_flux", state.edge_flux.size());
    d_active_ = IView("active", nt);
    d_pin_t0_ = IView("pin_t0", nt);
    d_tier_ = IView("tier", nt);
    d_face_tier_ = IView("face_tier", ne);
    d_cells_compact_ =
        IView(Kokkos::view_alloc(Kokkos::WithoutInitializing, std::string("cells_compact")), nt);
    d_edges_compact_ =
        IView(Kokkos::view_alloc(Kokkos::WithoutInitializing, std::string("edges_compact")), ne);
    d_scratch_ = IView("scratch", nt);
    d_dtcell_ = DView(Kokkos::view_alloc(Kokkos::WithoutInitializing, std::string("dtcell")), nt);

    K_ = std::clamp(opts.lts_tiers, 1, 8);

    // Non-WALL boundary entries (host scan, same rule as the serial marcher).
    bc_cell_host_.clear();
    bc_slot_host_.clear();
    if (state.boundary) {
        for (int i = 0; i < nt; ++i) {
            const int nvc = mesh.cell_vertex_count(i);
            for (int e = 0; e < nvc; ++e) {
                const int idx = MeshData::slot(i, e);
                const auto ty = static_cast<BoundaryType>(state.boundary->edge_bc_type[idx]);
                const bool interior = mesh.cell_neighbour(i, e) >= 0;
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
    // The scan above lists boundary edges cell by cell, so each cell's edges
    // form one contiguous run. Runs touch disjoint cells and fire concurrently;
    // the edges inside a run keep their serial order.
    {
        std::vector<int> run_ptr;
        for (int k = 0; k < nbc; ++k)
            if (k == 0 || bc_cell_host_[k] != bc_cell_host_[k - 1])
                run_ptr.push_back(k);
        n_bc_runs_ = static_cast<int>(run_ptr.size());
        run_ptr.push_back(nbc);
        bc_run_max_ = 0;
        for (int r = 0; r < n_bc_runs_; ++r)
            bc_run_max_ = std::max(bc_run_max_, run_ptr[r + 1] - run_ptr[r]);
        d_bc_run_ptr_ = devCopy("bc_run_ptr", run_ptr);
    }
    d_bc_type_ = IView("bc_type", nbc);
    d_bc_accum_ = DView("bc_accum", nbc);
    d_bc_in_ = DView("bc_in", nbc);
    d_bc_out_ = DView("bc_out", nbc);
    d_bc_requested_ = DView("bc_requested", nbc);
    d_bc_last_flow_ = DView("bc_last_flow", nbc);
    d_bc_last_dt_ = DView("bc_last_dt", nbc);
    bc_requested_host_.assign(nbc, 0.0);
    bc_last_flow_host_.assign(nbc, 0.0);
    bc_last_dt_host_.assign(nbc, 0.0);
    bc_in_host_.assign(nbc, 0.0);
    bc_out_host_.assign(nbc, 0.0);
    state.boundary_receipt_slots = bc_slot_host_;
    state.boundary_in_applied.assign(nbc, 0.0);
    state.boundary_out_applied.assign(nbc, 0.0);
    state.boundary_requested.assign(nbc, 0.0);
    state.boundary_last_flow.assign(nbc, 0.0);
    state.boundary_last_dt.assign(nbc, 0.0);
    d_bc_slope_ = DView("bc_slope", nbc);
    d_bc_head_ = DView("bc_head", nbc);
    d_bc_flow_ = DView("bc_flow", nbc);
    d_bc_q_ = DView("bc_q", nbc);

    // Coupling points (non-outfall; owned by the router, frozen topology).
    const std::size_t np = state.node_coupling ? state.node_coupling->size() : 0;
    {
        std::vector<int> cell(np), vert(np), node(np), law(np), hb(np), he(np), eq(np);
        std::vector<double> cd(np), area(np), geo(3 * np), coef(5 * np);
        for (std::size_t k = 0; k < np; ++k) {
            const auto& cp = (*state.node_coupling)[k];
            cell[k] = cp.cell_idx;
            vert[k] = cp.vertex_idx;
            node[k] = cp.node_idx;
            cd[k] = cp.cd;
            area[k] = cp.area;
            law[k] = cp.law;
            eq[k] = cp.equalize ? 1 : 0;
            hb[k] = cp.head_begin;
            he[k] = cp.head_end;
            geo[3 * k + 0] = cp.geo.z_crest;
            geo[3 * k + 1] = cp.geo.l_weir;
            geo[3 * k + 2] = cp.geo.a_orif;
            coef[5 * k + 0] = cp.coef.c_w;
            coef[5 * k + 1] = cp.coef.c_sw;
            coef[5 * k + 2] = cp.coef.c_oi;
            coef[5 * k + 3] = cp.coef.c_oo;
            coef[5 * k + 4] = cp.coef.h_eps;
        }
        d_cp_cell_ = devCopy("cp_cell", cell);
        d_cp_vertex_ = devCopy("cp_vertex", vert);
        d_cp_node_ = devCopy("cp_node", node);
        d_cp_cd_ = devCopy("cp_cd", cd);
        d_cp_area_ = devCopy("cp_area", area);
        d_cp_law_ = devCopy("cp_law", law);
        d_cp_eq_ = devCopy("cp_eq", eq);
        d_cp_head_b_ = devCopy("cp_head_b", hb);
        d_cp_head_e_ = devCopy("cp_head_e", he);
        d_cp_geo_ = devCopy("cp_geo", geo);
        d_cp_coef_ = devCopy("cp_coef", coef);
        const std::vector<int> head_cells =
            state.coupling_head_cells ? *state.coupling_head_cells : std::vector<int>{};
        d_head_cells_ = devCopy("head_cells", head_cells);

        // Exchange points interact only through a cell one of them drains or
        // fills (which another may also read through its head stencil) or
        // through a shared node's spill budget. Points linked that way form a
        // group that fires in serial point order; separate groups share no
        // state and fire concurrently, so the result equals the serial sweep.
        const int n_points = static_cast<int>(np);
        std::vector<int> root(np);
        std::iota(root.begin(), root.end(), 0);
        auto find = [&](int a) {
            while (root[a] != a)
                a = root[a] = root[root[a]];
            return a;
        };
        // The lower index becomes the root, so a group is keyed by its first point.
        auto join = [&](int a, int b) {
            a = find(a);
            b = find(b);
            if (a != b)
                root[std::max(a, b)] = std::min(a, b);
        };
        std::vector<int> cell_writer(static_cast<std::size_t>(nt), -1);
        std::map<int, int> node_user;
        for (int k = 0; k < n_points; ++k) {
            if (cell[k] < 0)
                continue; // never fires
            int& writer = cell_writer[cell[k]];
            if (writer < 0)
                writer = k;
            else
                join(k, writer);
            const auto [it, fresh] = node_user.try_emplace(node[k], k);
            if (!fresh)
                join(k, it->second);
        }
        auto reads = [&](int k, int c) {
            if (cell_writer[c] >= 0)
                join(k, cell_writer[c]);
        };
        for (int k = 0; k < n_points; ++k) {
            if (cell[k] < 0)
                continue;
            for (int p = hb[k]; p < he[k]; ++p)
                reads(k, head_cells[p]);
            if (vert[k] >= 0)
                for (int p = mesh.vert_stencil_ptr[vert[k]]; p < mesh.vert_stencil_ptr[vert[k] + 1];
                     ++p)
                    reads(k, mesh.vert_stencil_idx[p]);
        }
        std::vector<int> item(np), group_ptr;
        std::iota(item.begin(), item.end(), 0);
        std::stable_sort(item.begin(), item.end(), [&](int a, int b) { return find(a) < find(b); });
        for (int p = 0; p < n_points; ++p)
            if (p == 0 || find(item[p]) != find(item[p - 1]))
                group_ptr.push_back(p);
        n_cp_groups_ = static_cast<int>(group_ptr.size());
        group_ptr.push_back(n_points);
        cp_group_max_ = 0;
        for (int g = 0; g < n_cp_groups_; ++g)
            cp_group_max_ = std::max(cp_group_max_, group_ptr[g + 1] - group_ptr[g]);
        d_cp_group_ptr_ = devCopy("cp_group_ptr", group_ptr);
        d_cp_group_item_ = devCopy("cp_group_item", item);
    }
    d_exch_ = DView("exch", np);
    exch_host_.assign(np, 0.0);
    const std::size_t nn = state.nodes_1d ? state.nodes_1d->volume.size() : 0;
    d_node_drawn_ = DView("node_drawn", nn);
    d_node_head_ = DView("node_head", nn);
    d_node_depth_ = DView("node_depth", nn);
    d_node_volume_ = DView("node_volume", nn);
    d_node_fullvol_ = DView("node_fullvol", nn);
    d_node_invert_ = DView("node_invert", nn);
    d_node_fulldepth_ = DView("node_fulldepth", nn);
    d_node_pond_area_ = DView("node_pond_area", nn);
    if (nn > 0) {
        devRefresh(d_node_invert_, state.nodes_1d->invert_elev);
        devRefresh(d_node_fulldepth_, state.nodes_1d->full_depth);
        // The spill budget's pond base (see SurfaceStateData::node_pond_base).
        devRefresh(d_node_fullvol_,
                   state.node_pond_base ? *state.node_pond_base : state.nodes_1d->full_volume);
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
            "pin_cp", Kokkos::RangePolicy<ExecSpace>(0, (int)np), KOKKOS_LAMBDA(int k) {
                const int ci = cp_cell(k);
                if (ci >= 0)
                    pin(ci) = 1;
            });
    }

    reconstructAllDev();
    // The seeded head/depth must be readable before the first step, as on the
    // CPU marcher (whose reconstruct writes the host arrays directly).
    hostRefresh(state.head, d_head_);
    hostRefresh(state.depth, d_depth_);

    // Seed face momentum from the optional [2D_INITIAL_VELOCITY] rows
    // (== serial marcher; host-side build, one refresh). Mean depth is
    // V/A under both closures, so it is derived directly from the seeded
    // volumes. t = 0 only — reinitialize() still zeroes face momentum.
    {
        bool any_uv = false;
        for (int i = 0; i < nt && !any_uv; ++i)
            any_uv = mesh.tri_init_u[i] != 0.0 || mesh.tri_init_v[i] != 0.0;
        if (any_uv && swe_) {
            // Cell momentum seeds directly: q = h·u (nothing on the faces).
            std::vector<double> cxh(static_cast<std::size_t>(nt)), cyh(cxh.size());
            for (int i = 0; i < nt; ++i) {
                cxh[i] = state.depth[i] * mesh.tri_init_u[i];
                cyh[i] = state.depth[i] * mesh.tri_init_v[i];
            }
            devRefresh(d_qcx_, cxh);
            devRefresh(d_qcy_, cyh);
        } else if (any_uv) {
            auto hdep = [&](int i) {
                const double v = std::max(state.volume[i], 0.0);
                return (mesh.tri_area[i] > 1.0e-30) ? v / mesh.tri_area[i] : 0.0;
            };
            std::vector<double> qh(static_cast<std::size_t>(ne), 0.0);
            for (int e = 0; e < ne; ++e) {
                const int a = edges_.cL[e], b = edges_.cR[e];
                const double qax = hdep(a) * mesh.tri_init_u[a];
                const double qay = hdep(a) * mesh.tri_init_v[a];
                const double qbx = hdep(b) * mesh.tri_init_u[b];
                const double qby = hdep(b) * mesh.tri_init_v[b];
                qh[e] = edges_.conveyance[e] > 0.0
                            ? 0.5 * ((qax + qbx) * edges_.nx[e] + (qay + qby) * edges_.ny[e])
                            : 0.0;
            }
            devRefresh(d_q_, qh);
            if (have_perot_) {
                const auto& ed = edges_;
                std::vector<double> cxh(static_cast<std::size_t>(nt), 0.0);
                std::vector<double> cyh(static_cast<std::size_t>(nt), 0.0);
                for (int i = 0; i < nt; ++i) {
                    double sx = 0.0, sy = 0.0;
                    for (int p = ed.cell_ptr[i]; p < ed.cell_ptr[i + 1]; ++p) {
                        const int e = ed.cell_edge[p];
                        const double fq = static_cast<double>(ed.cell_sign[p]) * qh[e] * ed.xi[e] *
                                          ed.conveyance[e];
                        sx += fq * (ed.mx[e] - mesh.tri_cx[i]);
                        sy += fq * (ed.my[e] - mesh.tri_cy[i]);
                    }
                    const double inv_a = 1.0 / mesh.tri_area[i];
                    cxh[i] = sx * inv_a;
                    cyh[i] = sy * inv_a;
                }
                devRefresh(d_qcx_, cxh);
                devRefresh(d_qcy_, cyh);
            }
        }
    }
    if (swe_) {
        qcx_host_.assign(static_cast<std::size_t>(nt), 0.0);
        qcy_host_.assign(static_cast<std::size_t>(nt), 0.0);
        hostRefresh(qcx_host_, d_qcx_);
        hostRefresh(qcy_host_, d_qcy_);
    }
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
    const CellClosure cell = closure();
    Kokkos::parallel_for(
        "reconstructAll", Kokkos::RangePolicy<ExecSpace>(0, nt), KOKKOS_LAMBDA(int i) {
            double e, d;
            cell.etaDepth(i, vol(i), e, d);
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
    auto cvv = d_cell_v_;
    const bool vfr = (opts_->cell_closure == CellClosure2D::VFR);
    const double mwf = opts_->vfr_min_wet_frac;
    auto fmxL = d_fmxL_, fmxR = d_fmxR_, fmyL = d_fmyL_, fmyR = d_fmyR_;
    auto qcx = d_qcx_, qcy = d_qcy_;
    const bool swe = swe_;
    const CellClosure cell = closure();
    Kokkos::parallel_for(
        "settle", Kokkos::RangePolicy<ExecSpace>(0, nt), KOKKOS_LAMBDA(int i) {
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
            if (swe) {
                // Settle the momentum of mass already applied, so a re-tiered
                // or deactivated cell does not pick it up stale (== serial).
                double dmx = 0.0, dmy = 0.0;
                for (int p = ptr(i); p < ptr(i + 1); ++p) {
                    const int e = edge(p);
                    if (sign(p) > 0.0) {
                        dmx += fmxL(e);
                        fmxL(e) = 0.0;
                        dmy += fmyL(e);
                        fmyL(e) = 0.0;
                    } else {
                        dmx += fmxR(e);
                        fmxR(e) = 0.0;
                        dmy += fmyR(e);
                        fmyR(e) = 0.0;
                    }
                }
                if (dmx != 0.0 || dmy != 0.0) {
                    const double A = area(i);
                    qcx(i) += dmx / A;
                    qcy(i) += dmy / A;
                }
            }
            if (pending == 0.0)
                return;
            double v = vol(i) + pending;
            vol(i) = (v > 0.0) ? v : 0.0;
            double e2, d2;
            cell.etaDepth(i, vol(i), e2, d2);
            head(i) = e2;
            depth(i) = d2;
        });
}

void ExplicitKokkosSurfaceSolver::lazySourcesDev(double t) {
    const double dt_lazy = t - t_last_sync_;
    if (dt_lazy <= 0.0)
        return;
    const int nt = mesh_->n_triangles();
    auto vol = d_volume_, head = d_head_, depth = d_depth_;
    auto rain = d_rain_, coup = d_coup_, evap = d_evap_, infil = d_infil_;
    auto evap_app = d_evap_applied_;
    auto infil_app = d_infil_applied_;
    auto coup_app = d_coupling_applied_;
    auto active = d_active_;
    auto area = d_tri_area_, cz = d_tri_cz_, vz = d_vz_;
    auto cvv = d_cell_v_;
    const bool vfr = (opts_->cell_closure == CellClosure2D::VFR);
    const double mwf = opts_->vfr_min_wet_frac;
    const double dry = opts_->dry_depth;
    const CellClosure cell = closure();
    Kokkos::parallel_for(
        "lazySources", Kokkos::RangePolicy<ExecSpace>(0, nt), KOKKOS_LAMBDA(int i) {
            if (active(i))
                return;
            const auto budget = sourceBudget(vol(i), depth(i), area(i), dt_lazy, dry, rain(i),
                                             coup(i), infil(i), evap(i));
            if (!budget.applied)
                return;
            infil_app(i) += budget.infiltration / area(i);
            coup_app(i) += budget.coupling;
            evap_app(i) += budget.evaporation;
            vol(i) = budget.volume;
            double e2, d2;
            cell.etaDepth(i, vol(i), e2, d2);
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
    // Band scales with H_MOVE, capped at the historical ±1 mm (== serial).
    const double band = std::min(0.001, 0.5 * opts_->h_move);
    const double h_on = opts_->h_move + band;
    const double h_off = std::max(0.0, opts_->h_move - band);

    auto active = d_active_, seed = d_scratch_, pin = d_pin_t0_;
    auto depth = d_depth_, coup = d_coup_;
    // Optional stresses and rotation can accelerate any wet neighbor: the
    // momentum-physics mode keeps every cell active (== serial marcher).
    const bool all_cells = physics_;
    Kokkos::parallel_for(
        "seed", Kokkos::RangePolicy<ExecSpace>(0, nt), KOKKOS_LAMBDA(int i) {
            const double thresh = active(i) ? h_off : h_on;
            seed(i) = (all_cells || depth(i) >= thresh || coup(i) != 0.0 || pin(i)) ? 1 : 0;
        });
    Kokkos::parallel_for(
        "seed_to_active", Kokkos::RangePolicy<ExecSpace>(0, nt),
        KOKKOS_LAMBDA(int i) { active(i) = seed(i); });
    // Gather a cell's halo flag from immutable seeds; concurrent stores of
    // the same value are still data races on host/device memory models.
    {
        auto ptr = d_cell_ptr_, edge = d_cell_edge_, left = d_cL_, right = d_cR_;
        Kokkos::parallel_for(
            "halo", Kokkos::RangePolicy<ExecSpace>(0, nt), KOKKOS_LAMBDA(int i) {
                int on = seed(i);
                for (int p = ptr(i); p < ptr(i + 1) && !on; ++p) {
                    const int e = edge(p), j = left(e) == i ? right(e) : left(e);
                    on = seed(j);
                }
                active(i) = on;
            });
    }
    h_on_front_ = h_on;
    if (front_rebuild_) {
        // A dry-bed front can cross several rings between two macro cycles:
        // widen the halo (Jacobi rings, == serial marcher) and mark its outer
        // ring, whose wetting triggers the next rebuild.
        auto ptr = d_cell_ptr_, edge = d_cell_edge_, left = d_cL_, right = d_cR_;
        int n_seed = 0;
        Kokkos::parallel_reduce(
            "seed_count", Kokkos::RangePolicy<ExecSpace>(0, nt),
            KOKKOS_LAMBDA(int i, int& n) { n += seed(i); }, n_seed);
        if (n_seed != 0 && n_seed != nt) {
            for (int ring = 1; ring < kFrontHaloRings; ++ring) {
                Kokkos::deep_copy(seed, active);
                Kokkos::parallel_for(
                    "halo_ring", Kokkos::RangePolicy<ExecSpace>(0, nt), KOKKOS_LAMBDA(int i) {
                        if (seed(i))
                            return;
                        for (int p = ptr(i); p < ptr(i + 1); ++p) {
                            const int e = edge(p), j = left(e) == i ? right(e) : left(e);
                            if (seed(j)) {
                                active(i) = 1;
                                return;
                            }
                        }
                    });
            }
        }
        auto frontier = d_frontier_;
        Kokkos::parallel_for(
            "frontier", Kokkos::RangePolicy<ExecSpace>(0, nt), KOKKOS_LAMBDA(int i) {
                int f = 0;
                if (active(i))
                    for (int p = ptr(i); p < ptr(i + 1) && !f; ++p) {
                        const int e = edge(p), j = left(e) == i ? right(e) : left(e);
                        f = active(j) ? 0 : 1;
                    }
                frontier(i) = f;
            });
        front_breach_ = false;
    }

    // Per-cell CFL dt + exact min-reduce → dt0.
    if (diffusive_)
        refreshDiffusiveSlopes();
    auto dtcell = d_dtcell_;
    auto lchar = d_lchar_;
    auto qcx = d_qcx_, qcy = d_qcy_;
    auto mann = d_mannings_n_, dw_slope = d_dw_slope_, lpos = d_lpos_;
    const bool perot = have_perot_, diffusive = diffusive_, swe = swe_;
    const double alpha = opts_->cfl_number, dry = opts_->dry_depth;
    const double fr_max = opts_->froude_max, dh_eps = opts_->flux_dh_eps;
    const double dt_max = opts_->max_timestep;
    double dt0 = 1.0e30;
    Kokkos::parallel_reduce(
        "cfl_min", Kokkos::RangePolicy<ExecSpace>(0, nt),
        KOKKOS_LAMBDA(int i, double& mn) {
            if (!active(i)) {
                dtcell(i) = 1.0e30;
                return;
            }
            const double h = depth(i);
            double dt = 1.0e30;
            if (h > dry) {
                if (diffusive) {
                    // The explicit-diffusion bound replaces the gravity-wave one (== serial).
                    const double lc = lchar(i);
                    dt = diffusive::cellDiffusiveDt(alpha, lc, h, mann(i), dw_slope(i),
                                                    dh_eps / (lc > 1.0e-9 ? lc : 1.0e-9));
                } else if (swe) {
                    // Cell momentum over depth, against the positivity length.
                    const double speed =
                        (h > 1.0e-6) ? inertial::qMagnitude(qcx(i), qcy(i)) / h : 0.0;
                    dt = inertial::cellCflDt(alpha, lpos(i), h, speed);
                } else {
                    double speed = 0.0;
                    if (perot && h > 1.0e-6)
                        speed = inertial::cflCellSpeed(qcx(i), qcy(i), h, fr_max);
                    dt = inertial::cellCflDt(alpha, lchar(i), h, speed);
                }
            }
            dt = (dt < dt_max) ? dt : dt_max;
            dtcell(i) = dt;
            if (dt < mn)
                mn = dt;
        },
        Kokkos::Min<double>(dt0));
    if (dt0 >= 1.0e30)
        dt0 = opts_->max_timestep; // fully quiescent
    dt0_ = dt0;

    // Tier assignment (active cells only; stale tiers persist like serial).
    const int K = K_;
    // (refreshDt0 below re-evaluates this reduction between rebuilds,
    //  tighten-only — growth waits for the tier reassignment here.)
    auto tier = d_tier_;
    Kokkos::parallel_for(
        "tier_assign", Kokkos::RangePolicy<ExecSpace>(0, nt), KOKKOS_LAMBDA(int i) {
            if (!active(i))
                return;
            int tk = 0;
            if (K > 1) {
                const double ratio = dtcell(i) / dt0;
                if (ratio >= 2.0) {
                    // floor(log2(ratio)) exactly, == the CPU marcher's std::ilogb; a
                    // truncated std::log2 can round up within an ulp of a power of two.
                    const int lg = static_cast<int>(Kokkos::logb(ratio));
                    tk = (lg < K - 1) ? lg : (K - 1);
                }
                if (coup(i) != 0.0 || pin(i))
                    tk = 0;
            }
            tier(i) = tk;
        });

    if (front_rebuild_) {
        // Dry halo cells got the coarsest tier, so a front could enter them
        // only once per macro cycle. Pull each dry active cell down to the
        // finest tier among its active neighbors. The serial marcher does this
        // as a Gauss-Seidel sweep over the edges, whose result depends on the
        // edge order, so it runs on the host in that same order.
        auto tier_h = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace(), tier);
        auto active_h = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace(), active);
        auto depth_h = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace(), depth);
        const double dry_depth = opts_->dry_depth;
        for (int ring = 0; ring < kFrontHaloRings; ++ring) {
            bool changed = false;
            for (int e = 0; e < ne; ++e) {
                const int a = edges_.cL[e], b = edges_.cR[e];
                if (!active_h(a) || !active_h(b))
                    continue;
                if (depth_h(a) <= dry_depth && tier_h(a) > tier_h(b)) {
                    tier_h(a) = tier_h(b);
                    changed = true;
                }
                if (depth_h(b) <= dry_depth && tier_h(b) > tier_h(a)) {
                    tier_h(b) = tier_h(a);
                    changed = true;
                }
            }
            if (!changed)
                break;
        }
        Kokkos::deep_copy(tier, tier_h);
    }

    // Face tiers (min of incident, both-active) + stale-momentum zero.
    auto ftier = d_face_tier_;
    auto qv = d_q_;
    {
        auto cLv = d_cL_, cRv = d_cR_;
        Kokkos::parallel_for(
            "face_tier", Kokkos::RangePolicy<ExecSpace>(0, ne), KOKKOS_LAMBDA(int e) {
                const int a = cLv(e), b = cRv(e);
                if (active(a) && active(b)) {
                    const int ta = tier(a), tb = tier(b);
                    ftier(e) = (ta < tb) ? ta : tb;
                } else {
                    ftier(e) = K; // sentinel: not marched
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
                if (fin && f)
                    cells_c(cell_base + upd) = i;
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
                if (fin && f)
                    edges_c(edge_base + upd) = e;
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
    if (n_active_ == 0)
        return;
    if (diffusive_)
        refreshDiffusiveSlopes();
    auto active = d_active_;
    auto tier = d_tier_;
    auto depth = d_depth_;
    auto lchar = d_lchar_;
    auto qcx = d_qcx_, qcy = d_qcy_;
    auto mann = d_mannings_n_, dw_slope = d_dw_slope_, lpos = d_lpos_;
    const bool perot = have_perot_, diffusive = diffusive_, swe = swe_;
    const double alpha = opts_->cfl_number, dry = opts_->dry_depth;
    const double fr_max = opts_->froude_max, dh_eps = opts_->flux_dh_eps;
    double fresh = 1.0e30;
    Kokkos::parallel_reduce(
        "cfl_refresh", Kokkos::RangePolicy<ExecSpace>(0, nt),
        KOKKOS_LAMBDA(int i, double& mn) {
            if (!active(i))
                return;
            const double h = depth(i);
            if (h <= dry)
                return;
            double dt;
            if (diffusive) {
                const double lc = lchar(i);
                dt = diffusive::cellDiffusiveDt(alpha, lc, h, mann(i), dw_slope(i),
                                                dh_eps / (lc > 1.0e-9 ? lc : 1.0e-9));
            } else if (swe) {
                const double speed = (h > 1.0e-6) ? inertial::qMagnitude(qcx(i), qcy(i)) / h : 0.0;
                dt = inertial::cellCflDt(alpha, lpos(i), h, speed);
            } else {
                double speed = 0.0;
                if (perot && h > 1.0e-6)
                    speed = inertial::cflCellSpeed(qcx(i), qcy(i), h, fr_max);
                dt = inertial::cellCflDt(alpha, lchar(i), h, speed);
            }
            dt /= (1 << tier(i));
            if (dt < mn)
                mn = dt;
        },
        Kokkos::Min<double>(fresh));
    if (fresh < dt0_)
        dt0_ = fresh;
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
        "collapse_cells", Kokkos::RangePolicy<ExecSpace>(0, nt), KOKKOS_LAMBDA(int i) {
            if (active(i))
                tier(i) = 0;
        });
    auto ftier = d_face_tier_;
    {
        auto cLv = d_cL_, cRv = d_cR_;
        Kokkos::parallel_for(
            "collapse_faces", Kokkos::RangePolicy<ExecSpace>(0, ne), KOKKOS_LAMBDA(int e) {
                if (active(cLv(e)) && active(cRv(e)))
                    ftier(e) = 0;
                else
                    ftier(e) = K;
            });
    }
    auto cells_c = d_cells_compact_;
    auto edges_c = d_edges_compact_;
    int count = 0;
    Kokkos::parallel_scan(
        "collapse_compact_cells", Kokkos::RangePolicy<ExecSpace>(0, nt),
        KOKKOS_LAMBDA(int i, int& upd, bool fin) {
            const int f = active(i) ? 1 : 0;
            if (fin && f)
                cells_c(upd) = i;
            upd += f;
        },
        count);
    int fcount = 0;
    Kokkos::parallel_scan(
        "collapse_compact_edges", Kokkos::RangePolicy<ExecSpace>(0, ne),
        KOKKOS_LAMBDA(int e, int& upd, bool fin) {
            const int f = (ftier(e) == 0) ? 1 : 0;
            if (fin && f)
                edges_c(upd) = e;
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

void ExplicitKokkosSurfaceSolver::refreshConveyance() {
    if (!edges_.refreshConveyance(*mesh_))
        return;
    devRefresh(d_conveyance_, edges_.conveyance);
    auto conv = d_conveyance_, q = d_q_;
    Kokkos::parallel_for(
        "close_faces", Kokkos::RangePolicy<ExecSpace>(0, edges_.ne), KOKKOS_LAMBDA(int e) {
            if (conv(e) == 0.0)
                q(e) = 0.0;
        });
    if (!have_perot_)
        return;
    auto ptr = d_cell_ptr_, edge = d_cell_edge_;
    auto sign = d_sign_, xi = d_xi_, mx = d_mx_, my = d_my_;
    auto cx = d_tri_cx_, cy = d_tri_cy_, area = d_tri_area_;
    auto qcx = d_qcx_, qcy = d_qcy_;
    const bool li = !diffusive_;
    Kokkos::parallel_for(
        "refresh_cell_discharge", Kokkos::RangePolicy<ExecSpace>(0, mesh_->n_cells()),
        KOKKOS_LAMBDA(int i) {
            double sx = 0.0, sy = 0.0;
            for (int p = ptr(i); p < ptr(i + 1); ++p) {
                const int e = edge(p);
                double f = sign(p) * q(e) * xi(e);
                if (li)
                    f *= conv(e);
                sx += f * (mx(e) - cx(i));
                sy += f * (my(e) - cy(i));
            }
            qcx(i) = sx / area(i);
            qcy(i) = sy / area(i);
        });
    auto cells = d_bc_cell_, slots = d_bc_slot_, vertices = d_cell_v_, nvv = d_nv_;
    auto bc_q = d_bc_q_, length = d_edge_length_, vx = d_vx_, vy = d_vy_;
    const int nbc = static_cast<int>(bc_cell_host_.size());
    // Serial perimeter pass: several boundary slots can belong to the same cell.
    Kokkos::parallel_for(
        "refresh_boundary_discharge", Kokkos::RangePolicy<ExecSpace>(0, 1), KOKKOS_LAMBDA(int) {
            for (int k = 0; k < nbc; ++k) {
                const int i = cells(k), slot = slots(k), edge = slot % kMaxCellVerts;
                const int va = vertices(i * kMaxCellVerts + (edge + 1) % nvv(i));
                const int vb = vertices(i * kMaxCellVerts + (edge + 2) % nvv(i));
                const double mx = 0.5 * (vx(va) + vx(vb)), my = 0.5 * (vy(va) + vy(vb));
                const double fo = -bc_q(k) * length(slot), inv_a = 1.0 / area(i);
                qcx(i) += fo * (mx - cx(i)) * inv_a;
                qcy(i) += fo * (my - cy(i)) * inv_a;
            }
        });
}

void ExplicitKokkosSurfaceSolver::fireFaces(int k, double dt_f, bool global_step) {
    const int lo = global_step ? 0 : ftier_off_[k];
    const int hi = global_step ? ftier_off_[K_] : ftier_off_[k + 1];
    if (lo >= hi)
        return;
    if (diffusive_) {
        fireFacesDiffusive(lo, hi, dt_f, global_step);
        return;
    }
    if (swe_) {
        fireFacesSwe(lo, hi, dt_f, global_step);
        return;
    }
    auto list = d_edges_compact_;
    auto cLv = d_cL_, cRv = d_cR_;
    auto head = d_head_, vol = d_volume_;
    auto qv = d_q_, faccL = d_faccL_, faccR = d_faccR_;
    auto zface = d_zface_, inv_dx = d_inv_dx_, n2 = d_n2_, xi = d_xi_, conv = d_conveyance_;
    auto ze_lo = d_ze_lo_, ze_hi = d_ze_hi_;
    auto nxv = d_nx_, nyv = d_ny_;
    auto qcx = d_qcx_, qcy = d_qcy_;
    auto depthv = d_depth_;
    auto tier = d_tier_, ftier = d_face_tier_;
    const bool perot = have_perot_;
    // VFR_FACE: B&S Eq. 14 at the shared edge's true crest (== serial branch).
    const bool vfr_face = (opts_->face_reconstruction == FaceDepth2D::VFR_FACE);
    const double theta = opts_->theta;
    const bool physical_time_blend = opts_->theta_reference_step > 0.0;
    const double blend = inertial::momentumBlendFraction(theta, dt_f, opts_->theta_reference_step);
    const double dry = opts_->dry_depth;
    const double fr_max = opts_->froude_max;
    const double beta3 = opts_->exchange_beta / 3.0, beta4 = opts_->exchange_beta / 4.0;
    const bool quads = has_quads_;
    auto nvv = d_nv_;
    const bool advect = opts_->advection;
    Kokkos::parallel_for(
        "fireFaces", Kokkos::RangePolicy<ExecSpace>(lo, hi), KOKKOS_LAMBDA(int idx) {
            const int e = list(idx);
            if (conv(e) == 0.0) {
                qv(e) = 0.0;
                return;
            }
            const double width = xi(e) * conv(e);
            const int a = cLv(e), b = cRv(e);
            const double hf = vfr_face
                                  ? inertial::faceFlowDepthVfr(head(a), head(b), ze_lo(e), ze_hi(e))
                                  : inertial::faceFlowDepth(head(a), head(b), zface(e));
            if (hf <= dry) {
                qv(e) = 0.0;
                return;
            }
            double qhat = qv(e);
            double q_mag = std::fabs(qv(e));
            if (perot) {
                const double qfx = 0.5 * (qcx(a) + qcx(b));
                const double qfy = 0.5 * (qcy(a) + qcy(b));
                const double qn = qfx * nxv(e) + qfy * nyv(e);
                qhat = physical_time_blend ? qv(e) + blend * (qn - qv(e))
                                           : theta * qv(e) + (1.0 - theta) * qn;
                // Vector friction magnitude, floored at |q_n| (== serial).
                const double qm = inertial::qMagnitude(qfx, qfy);
                if (qm > q_mag)
                    q_mag = qm;
            }
            double deta = head(b) - head(a);
            if (std::fabs(deta) < inertial::kEtaDeadband)
                deta = 0.0;
            const double slope = deta * inv_dx(e);
            // Convective momentum flux (ADVECTION, opt-in) — == serial branch:
            // both cells wet, else the pure local-inertial law holds the front.
            double adv = 0.0;
            if (advect && perot) {
                const double hL = depthv(a), hR = depthv(b);
                if (hL > dry && hR > dry) {
                    const double unL = (qcx(a) * nxv(e) + qcy(a) * nyv(e)) / hL;
                    const double unR = (qcx(b) * nxv(e) + qcy(b) * nyv(e)) / hR;
                    adv = inertial::inertialAdvection(qv(e), unL, hL, unR, hR, inv_dx(e));
                }
            }
            double qn1 =
                inertial::inertialFaceUpdate(qv(e), qhat, hf, dt_f, slope, n2(e), q_mag, adv);
            qn1 = inertial::froudeCap(qn1, hf, fr_max);

            const int exp_cell = (qn1 > 0.0) ? a : b;
            const int refire = global_step ? 1 : (1 << (tier(exp_cell) - ftier(e)));
            const double vmax = vol(exp_cell) > 0.0 ? vol(exp_cell) : 0.0;
            const double beta_share = (quads && nvv(exp_cell) == 4) ? beta4 : beta3;
            const double budget = beta_share / refire * vmax;
            const double take = std::fabs(qn1) * width * dt_f;
            if (take > budget)
                qn1 *= (take > 0.0) ? budget / take : 0.0;
            qv(e) = qn1;

            const double dM = qn1 * width * dt_f;
            faccL(e) -= dM;
            faccR(e) += dM;
        });
    face_passes_ += (hi - lo);
}

// MOMENTUM_EQUATION DIFFUSIVE_WAVE: Manning quasi-steady face discharge. The
// arithmetic and its order follow ExplicitInertialSolver::fireFacesDiffusive.
void ExplicitKokkosSurfaceSolver::fireFacesDiffusive(int lo, int hi, double dt_f,
                                                     bool global_step) {
    auto list = d_edges_compact_;
    auto cLv = d_cL_, cRv = d_cR_;
    auto head = d_head_, vol = d_volume_;
    auto qv = d_q_, faccL = d_faccL_, faccR = d_faccR_;
    auto zface = d_zface_, inv_dx = d_inv_dx_, nf = d_n_face_, xi = d_xi_, conv = d_conveyance_;
    auto ze_lo = d_ze_lo_, ze_hi = d_ze_hi_;
    auto tier = d_tier_, ftier = d_face_tier_;
    const bool vfr_face = (opts_->face_reconstruction == FaceDepth2D::VFR_FACE);
    const double dry = opts_->dry_depth, dh_eps = opts_->flux_dh_eps;
    const double beta3 = opts_->exchange_beta / 3.0, beta4 = opts_->exchange_beta / 4.0;
    const bool quads = has_quads_;
    auto nvv = d_nv_;
    Kokkos::parallel_for(
        "fireFacesDiffusive", Kokkos::RangePolicy<ExecSpace>(lo, hi), KOKKOS_LAMBDA(int idx) {
            const int e = list(idx);
            const int a = cLv(e), b = cRv(e);
            const double hf = vfr_face
                                  ? inertial::faceFlowDepthVfr(head(a), head(b), ze_lo(e), ze_hi(e))
                                  : inertial::faceFlowDepth(head(a), head(b), zface(e));
            if (hf <= dry) {
                qv(e) = 0.0;
                return;
            }
            double deta = head(b) - head(a);
            if (std::fabs(deta) < inertial::kEtaDeadband)
                deta = 0.0;
            const double slope = deta * inv_dx(e);
            const double s_eps = dh_eps * inv_dx(e);
            double qn1 = diffusive::faceDischarge(hf, slope, nf(e), s_eps);
            qn1 *= conv(e);

            const int exp_cell = (qn1 > 0.0) ? a : b;
            const int refire = global_step ? 1 : (1 << (tier(exp_cell) - ftier(e)));
            const double vmax = vol(exp_cell) > 0.0 ? vol(exp_cell) : 0.0;
            const double beta_share = (quads && nvv(exp_cell) == 4) ? beta4 : beta3;
            const double budget = beta_share / refire * vmax;
            const double take = std::fabs(qn1) * xi(e) * dt_f;
            if (take > budget)
                qn1 *= (take > 0.0) ? budget / take : 0.0;
            qv(e) = qn1;

            const double dM = qn1 * xi(e) * dt_f;
            faccL(e) -= dM;
            faccR(e) += dM;
        });
    face_passes_ += (hi - lo);
}

// MOMENTUM_EQUATION FULL_SWE, first order: Godunov face flux (SweKernels.hpp).
// The arithmetic and its order follow ExplicitInertialSolver::fireFacesSwe.
void ExplicitKokkosSurfaceSolver::fireFacesSwe(int lo, int hi, double dt_f, bool global_step) {
    const int nt = mesh_->n_triangles();
    auto active = d_active_;
    auto head = d_head_, depth = d_depth_, vol = d_volume_;
    auto qcx = d_qcx_, qcy = d_qcy_;
    const double dry = opts_->dry_depth;
    // A finite water proposal can still exceed the pressure/kinetic-flux range
    // of double precision. Refuse it before booking any face. The lowest
    // offending active cell is reported, as the serial marcher's scan does.
    int bad = nt;
    Kokkos::parallel_reduce(
        "swe_range", Kokkos::RangePolicy<ExecSpace>(0, nt),
        KOKKOS_LAMBDA(int i, int& first) {
            if (!active(i))
                return;
            const double h = depth(i), qx = qcx(i), qy = qcy(i);
            if (!(h >= 0) || !Kokkos::isfinite(head(i)) || !Kokkos::isfinite(qx) ||
                !Kokkos::isfinite(qy) || !Kokkos::isfinite(.5 * swe::kGravity * h * h) ||
                (h > dry && !Kokkos::isfinite((qx / h) * qx + (qy / h) * qy)))
                if (i < first)
                    first = i;
        },
        Kokkos::Min<int>(bad));
    if (bad < nt)
        throw std::runtime_error("FULL_SWE pressure or momentum exceeds finite range at cell " +
                                 std::to_string(bad));

    if (second_order_)
        computeLimitedGradientsSwe();
    const bool so = muscl_;
    auto gex = d_gex_, gey = d_gey_, gux = d_gux_, guy = d_guy_, gvx = d_gvx_, gvy = d_gvy_;
    auto emx = d_mx_, emy = d_my_, ccx = d_tri_cx_, ccy = d_tri_cy_;

    auto list = d_edges_compact_;
    auto cLv = d_cL_, cRv = d_cR_;
    auto qv = d_q_, faccL = d_faccL_, faccR = d_faccR_;
    auto fmxL = d_fmxL_, fmxR = d_fmxR_, fmyL = d_fmyL_, fmyR = d_fmyR_;
    auto nxv = d_nx_, nyv = d_ny_, xi = d_xi_, conv = d_conveyance_;
    auto tier = d_tier_, ftier = d_face_tier_;
    const double beta3 = opts_->exchange_beta / 3.0, beta4 = opts_->exchange_beta / 4.0;
    const bool quads = has_quads_;
    auto nvv = d_nv_;
    Kokkos::parallel_for(
        "fireFacesSwe", Kokkos::RangePolicy<ExecSpace>(lo, hi), KOKKOS_LAMBDA(int idx) {
            const int e = list(idx);
            const int a = cLv(e), b = cRv(e);
            swe::FaceFlux F;
            double cLx, cLy, cRx, cRy;
            bool wet;
            if (so) {
                // MUSCL: extrapolate eta, depth and velocity to the face (== serial).
                const double ha = depth(a), hb = depth(b);
                const double za = head(a) - ha, zb = head(b) - hb;
                const double axa = emx(e) - ccx(a), aya = emy(e) - ccy(a);
                const double axb = emx(e) - ccx(b), ayb = emy(e) - ccy(b);
                const double ua = (ha > dry) ? qcx(a) / ha : 0.0;
                const double va = (ha > dry) ? qcy(a) / ha : 0.0;
                const double ub = (hb > dry) ? qcx(b) / hb : 0.0;
                const double vb = (hb > dry) ? qcy(b) / hb : 0.0;
                const double etaLf = head(a) + gex(a) * axa + gey(a) * aya;
                const double etaRf = head(b) + gex(b) * axb + gey(b) * ayb;
                const double uLf = ua + gux(a) * axa + guy(a) * aya;
                const double vLf = va + gvx(a) * axa + gvy(a) * aya;
                const double uRf = ub + gux(b) * axb + guy(b) * ayb;
                const double vRf = vb + gvx(b) * axb + gvy(b) * ayb;
                const double hLr = ha + gex(nt + a) * axa + gey(nt + a) * aya;
                const double hRr = hb + gex(nt + b) * axb + gey(nt + b) * ayb;
                const double hLf = hLr > 0.0 ? hLr : 0.0;
                const double hRf = hRr > 0.0 ? hRr : 0.0;
                wet = swe::faceFluxReconBed(etaLf, uLf, vLf, za, ha, etaRf, uRf, vRf, zb, hb,
                                            nxv(e), nyv(e), dry, F, cLx, cLy, cRx, cRy, etaLf - hLf,
                                            etaRf - hRf);
            } else {
                wet = swe::faceFlux(head(a), depth(a), qcx(a), qcy(a), head(b), depth(b), qcx(b),
                                    qcy(b), nxv(e), nyv(e), dry, F, cLx, cLy, cRx, cRy);
            }
            // Dry-dry face: no flux and no correction, so nothing to book.
            if (!wet) {
                qv(e) = 0.0;
                return;
            }
            // Face conveyance (leaky barriers) scales the whole flux vector.
            const double c = conv(e);
            double fh = F.mass * c, fmx = F.mx * c, fmy = F.my * c;
            // Positivity share: mass and momentum are scaled together.
            if (fh != 0.0) {
                const int exp_cell = (fh > 0.0) ? a : b;
                const int refire = global_step ? 1 : (1 << (tier(exp_cell) - ftier(e)));
                const double vmax = vol(exp_cell) > 0.0 ? vol(exp_cell) : 0.0;
                const double beta_share = (quads && nvv(exp_cell) == 4) ? beta4 : beta3;
                const double budget = beta_share / refire * vmax;
                const double take = std::fabs(fh) * xi(e) * dt_f;
                if (take > budget) {
                    const double lam = (take > 0.0) ? budget / take : 0.0;
                    fh *= lam;
                    fmx *= lam;
                    fmy *= lam;
                }
            }
            qv(e) = fh;
            const double xdt = xi(e) * dt_f;
            const double dM = fh * xdt; // m³, + = cL→cR
            faccL(e) -= dM;
            faccR(e) += dM;
            // Momentum: −flux + bed-slope correction on the exporter side,
            // +flux + correction on the receiver side (m⁴/s).
            fmxL(e) += (-fmx + cLx) * xdt;
            fmyL(e) += (-fmy + cLy) * xdt;
            fmxR(e) += (fmx + cRx) * xdt;
            fmyR(e) += (fmy + cRy) * xdt;
        });
    face_passes_ += (hi - lo);
}

// RECONSTRUCTION_ORDER 2: limited gradients. The arithmetic and its order
// follow ExplicitInertialSolver::computeLimitedGradientsSwe.
void ExplicitKokkosSurfaceSolver::computeLimitedGradientsSwe() {
    const int nt = mesh_->n_triangles();
    const int nv = mesh_->n_vertices();
    auto active = d_active_;
    auto head = d_head_, depth = d_depth_, qcx = d_qcx_, qcy = d_qcy_;
    auto vz = d_vz_, vx = d_vx_, vy = d_vy_;
    auto vs_ptr = d_vs_ptr_, vs_idx = d_vs_idx_;
    auto bounds = d_vbounds_;
    const double dry = opts_->dry_depth;
    // Velocity bounds at each vertex over the wet active cells that reach it.
    Kokkos::parallel_for(
        "swe_vertex_bounds", Kokkos::RangePolicy<ExecSpace>(0, nv), KOKKOS_LAMBDA(int v) {
            double ulo = 1e300, uhi = -1e300, vlo = 1e300, vhi = -1e300;
            for (int p = vs_ptr(v); p < vs_ptr(v + 1); ++p) {
                const int j = vs_idx(p);
                const double h = depth(j);
                if (!active(j) || h <= dry || head(j) - vz(v) <= dry)
                    continue;
                const double u = qcx(j) / h, w = qcy(j) / h;
                ulo = (u < ulo) ? u : ulo;
                uhi = (uhi < u) ? u : uhi;
                vlo = (w < vlo) ? w : vlo;
                vhi = (vhi < w) ? w : vhi;
            }
            bounds(4 * v) = ulo;
            bounds(4 * v + 1) = uhi;
            bounds(4 * v + 2) = vlo;
            bounds(4 * v + 3) = vhi;
        });
    auto ptr = d_cell_ptr_, edge = d_cell_edge_, cLv = d_cL_, cRv = d_cR_;
    auto cx = d_tri_cx_, cy = d_tri_cy_;
    auto arm_x = d_arm_x_, arm_y = d_arm_y_;
    auto cvv = d_cell_v_, nvv = d_nv_;
    auto gex = d_gex_, gey = d_gey_, gux = d_gux_, guy = d_guy_, gvx = d_gvx_, gvy = d_gvy_;
    Kokkos::parallel_for(
        "swe_gradients", Kokkos::RangePolicy<ExecSpace>(0, nt), KOKKOS_LAMBDA(int i) {
            if (!active(i))
                return;
            gex(nt + i) = gey(nt + i) = gex(i) = gey(i) = 0;
            gux(i) = guy(i) = gvx(i) = gvy(i) = 0;
            const double hi = depth(i);
            if (hi <= 10 * dry)
                return;
            const double wi[4] = {head(i), qcx(i) / hi, qcy(i) / hi, hi};
            double lo[4] = {wi[0], wi[1], wi[2], wi[3]}, hiw[4] = {wi[0], wi[1], wi[2], wi[3]};
            double xx = 0, xy = 0, yy = 0, bx[4] = {0, 0, 0, 0}, by[4] = {0, 0, 0, 0};
            unsigned connected = 0;
            int count = 0;
            bool shore = false;
            const int begin = ptr(i), end = ptr(i + 1);
            for (int p = begin; p < end; ++p) {
                const int e = edge(p), j = cLv(e) == i ? cRv(e) : cLv(e);
                const double hj = depth(j), ej = head(j);
                const double si = wi[0] - hi, sj = ej - hj;
                const double sill = (si < sj) ? sj : si;
                if (hj <= dry || !active(j) || wi[0] - sill <= dry || ej - sill <= dry) {
                    shore = true;
                    continue;
                }
                connected |= 1u << (p - begin);
                ++count;
                const double dx = cx(j) - cx(i), dy = cy(j) - cy(i);
                const double wt = 1 / (dx * dx + dy * dy);
                xx += wt * dx * dx;
                xy += wt * dx * dy;
                yy += wt * dy * dy;
                const double wj[4] = {ej, qcx(j) / hj, qcy(j) / hj, hj};
                for (int m = 0; m < 4; ++m) {
                    bx[m] += wt * dx * (wj[m] - wi[m]);
                    by[m] += wt * dy * (wj[m] - wi[m]);
                    lo[m] = (wj[m] < lo[m]) ? wj[m] : lo[m];
                    hiw[m] = (hiw[m] < wj[m]) ? wj[m] : hiw[m];
                }
            }
            if (count < 2)
                return;
            const double det = xx * yy - xy * xy;
            // Shoreline conditioning bound (condition number < 98).
            if (det <= 1e-2 * (xx + yy) * (xx + yy))
                return;
            double gx[4] = {0, 0, 0, 0}, gy[4] = {0, 0, 0, 0}, phi[4] = {1, 1, 1, 1};
            for (int m = 0; m < 4; ++m) {
                if (shore && (m == 1 || m == 2))
                    continue;
                gx[m] = (yy * bx[m] - xy * by[m]) / det;
                gy[m] = (xx * by[m] - xy * bx[m]) / det;
            }
            for (int p = begin; p < end; ++p) {
                if (!(connected & (1u << (p - begin))))
                    continue;
                for (int m = 0; m < 4; ++m) {
                    const double wf = wi[m] + gx[m] * arm_x(p) + gy[m] * arm_y(p);
                    const double lim = swe::bjLimiter(wi[m], wf, lo[m], hiw[m]);
                    phi[m] = (lim < phi[m]) ? lim : phi[m];
                }
            }
            const int nvc = nvv(i);
            if (!shore) {
                for (int k = 0; k < nvc; ++k) {
                    const int v = cvv(i * kMaxCellVerts + k);
                    if (head(i) - vz(v) <= dry) {
                        phi[1] = phi[2] = 0;
                        break;
                    }
                    const double ax = vx(v) - cx(i), ay = vy(v) - cy(i);
                    const double b0 = bounds(4 * v), b1 = bounds(4 * v + 1);
                    const double b2 = bounds(4 * v + 2), b3 = bounds(4 * v + 3);
                    const double l1 =
                        swe::bjLimiter(wi[1], wi[1] + gx[1] * ax + gy[1] * ay,
                                       (b0 < wi[1]) ? b0 : wi[1], (wi[1] < b1) ? b1 : wi[1]);
                    phi[1] = (l1 < phi[1]) ? l1 : phi[1];
                    const double l2 =
                        swe::bjLimiter(wi[2], wi[2] + gx[2] * ax + gy[2] * ay,
                                       (b2 < wi[2]) ? b2 : wi[2], (wi[2] < b3) ? b3 : wi[2]);
                    phi[2] = (l2 < phi[2]) ? l2 : phi[2];
                }
            }
            if (count < nvc) {
                double positive = 1;
                for (int e = 0; e < nvc; ++e) {
                    const int a = cvv(i * kMaxCellVerts + e);
                    const int b = cvv(i * kMaxCellVerts + (e + 1) % nvc);
                    const double ax = .5 * (vx(a) + vx(b)) - cx(i);
                    const double ay = .5 * (vy(a) + vy(b)) - cy(i);
                    const double dh = phi[3] * (gx[3] * ax + gy[3] * ay);
                    if (dh < -hi) {
                        const double r = -hi / dh;
                        positive = (r < positive) ? r : positive;
                    }
                }
                phi[0] *= positive;
                phi[3] *= positive;
            }
            gex(i) = phi[0] * gx[0];
            gey(i) = phi[0] * gy[0];
            gex(nt + i) = phi[3] * gx[3];
            gey(nt + i) = phi[3] * gy[3];
            gux(i) = phi[1] * gx[1];
            guy(i) = phi[1] * gy[1];
            gvx(i) = phi[2] * gx[2];
            gvy(i) = phi[2] * gy[2];
        });
}

// RECONSTRUCTION_ORDER 2: one SSP-RK2 step. Two global Euler stages, then the
// average with the beginning state (the surface-only path of
// ExplicitInertialSolver::runRk2Step).
void ExplicitKokkosSurfaceSolver::runRk2Step(double dt) {
    const int nt = mesh_->n_triangles();
    Kokkos::deep_copy(d_rk_v0_, d_volume_);
    Kokkos::deep_copy(d_rk_qx0_, d_qcx_);
    Kokkos::deep_copy(d_rk_qy0_, d_qcy_);
    Kokkos::deep_copy(d_rk_inf0_, d_infil_applied_);
    Kokkos::deep_copy(d_rk_cpl0_, d_coupling_applied_);
    Kokkos::deep_copy(d_rk_evap0_, d_evap_applied_);
    auto snapshot = [](DView& keep, const DView& live) {
        if (keep.extent(0) != live.extent(0))
            keep = DView(Kokkos::view_alloc(Kokkos::WithoutInitializing, live.label() + "_rk0"),
                         live.extent(0));
        Kokkos::deep_copy(keep, live);
    };
    snapshot(d_rk_bc_accum0_, d_bc_accum_);
    snapshot(d_rk_bc_in0_, d_bc_in_);
    snapshot(d_rk_bc_out0_, d_bc_out_);
    snapshot(d_rk_bc_req0_, d_bc_requested_);
    snapshot(d_rk_ex0_, d_exch_);
    snapshot(d_rk_drawn0_, d_node_drawn_);

    for (int stage = 0; stage < 2; ++stage) {
        fireFaces(0, dt, true);
        fireCells(0, dt, true);
    }

    auto active = d_active_;
    auto vol = d_volume_, head = d_head_, depth = d_depth_, qcx = d_qcx_, qcy = d_qcy_;
    auto infil = d_infil_applied_, coupling = d_coupling_applied_, evap = d_evap_applied_;
    auto v0 = d_rk_v0_, qx0 = d_rk_qx0_, qy0 = d_rk_qy0_;
    auto inf0 = d_rk_inf0_, cpl0 = d_rk_cpl0_, evap0 = d_rk_evap0_;
    const CellClosure cell = closure();
    const double dry = opts_->dry_depth;
    // An inactive cell was fired by neither stage, so its average is the
    // identity; only the dry-momentum reset is evaluated for it.
    Kokkos::parallel_for(
        "rk2_average", Kokkos::RangePolicy<ExecSpace>(0, nt), KOKKOS_LAMBDA(int i) {
            if (active(i)) {
                vol(i) = 0.5 * (v0(i) + vol(i));
                qcx(i) = 0.5 * (qx0(i) + qcx(i));
                qcy(i) = 0.5 * (qy0(i) + qcy(i));
                double e2, d2;
                cell.etaDepth(i, vol(i), e2, d2);
                head(i) = e2;
                depth(i) = d2;
                infil(i) = inf0(i) + 0.5 * (infil(i) - inf0(i));
                coupling(i) = cpl0(i) + 0.5 * (coupling(i) - cpl0(i));
            }
            evap(i) = evap0(i) + 0.5 * (evap(i) - evap0(i));
            if (depth(i) <= dry) {
                qcx(i) = 0.0;
                qcy(i) = 0.0;
            }
        });
    auto average = [](const DView& live, const DView& before) {
        const int n = static_cast<int>(live.extent(0));
        Kokkos::parallel_for(
            "rk2_average_ledger", Kokkos::RangePolicy<ExecSpace>(0, n),
            KOKKOS_LAMBDA(int k) { live(k) = before(k) + 0.5 * (live(k) - before(k)); });
    };
    average(d_bc_accum_, d_rk_bc_accum0_);
    average(d_bc_in_, d_rk_bc_in0_);
    average(d_bc_out_, d_rk_bc_out0_);
    average(d_bc_requested_, d_rk_bc_req0_);
    average(d_exch_, d_rk_ex0_);
    average(d_node_drawn_, d_rk_drawn0_);
    substeps_run_ += 2;
    last_steps_ += 2;
}

void ExplicitKokkosSurfaceSolver::refreshDiffusiveSlopes() {
    const int nt = mesh_->n_triangles();
    auto active = d_active_;
    auto ptr = d_cell_ptr_, edge = d_cell_edge_, cLv = d_cL_, cRv = d_cR_;
    auto head = d_head_, inv_dx = d_inv_dx_, dw_slope = d_dw_slope_;
    Kokkos::parallel_for(
        "dw_slopes", Kokkos::RangePolicy<ExecSpace>(0, nt), KOKKOS_LAMBDA(int i) {
            if (!active(i))
                return;
            double smax = 0.0;
            for (int p = ptr(i); p < ptr(i + 1); ++p) {
                const int e = edge(p);
                const double sl = std::fabs((head(cRv(e)) - head(cLv(e))) * inv_dx(e));
                if (sl > smax)
                    smax = sl;
            }
            dw_slope(i) = smax;
        });
}

void ExplicitKokkosSurfaceSolver::fireCells(int k, double dt_c, bool global_step) {
    const int lo = global_step ? 0 : tier_off_[k];
    const int hi = global_step ? tier_off_[K_] : tier_off_[k + 1];
    auto list = d_cells_compact_;
    auto vol = d_volume_, head = d_head_, depth = d_depth_;
    auto faccL = d_faccL_, faccR = d_faccR_;
    auto ptr = d_cell_ptr_, edge = d_cell_edge_;
    auto sign = d_sign_;
    auto rain = d_rain_, coup = d_coup_, evap = d_evap_, infil = d_infil_;
    auto evap_app = d_evap_applied_;
    auto infil_app = d_infil_applied_;
    auto coup_app = d_coupling_applied_;
    auto qv = d_q_, xi = d_xi_, mx = d_mx_, my = d_my_, conv = d_conveyance_;
    auto qcx = d_qcx_, qcy = d_qcy_;
    auto area = d_tri_area_, cz = d_tri_cz_, cx = d_tri_cx_, cy = d_tri_cy_;
    auto vz = d_vz_;
    auto cvv = d_cell_v_;
    const bool vfr = (opts_->cell_closure == CellClosure2D::VFR);
    const double mwf = opts_->vfr_min_wet_frac;
    const bool perot = have_perot_;
    const bool swe = swe_, li = !swe_ && !diffusive_;
    const double dry = opts_->dry_depth;
    const CellClosure cell = closure();
    auto nvv = d_nv_;
    auto slot_dist = d_slot_dist_;
    auto fmxL = d_fmxL_, fmxR = d_fmxR_, fmyL = d_fmyL_, fmyR = d_fmyR_;
    auto wall_ptr = d_wall_ptr_, wall_slot = d_wall_slot_;
    auto slot_nx = d_slot_nx_, slot_ny = d_slot_ny_, slot_len = d_edge_length_;
    auto manning = d_mannings_n_;
    const bool so = muscl_;
    // Optional momentum physics: boundary edges read the state held at the start
    // of this firing, the same beginning state the interior faces used.
    const bool held = physics_ && !bc_cell_host_.empty();
    auto held_vol = d_held_vol_, held_qx = d_held_qx_, held_qy = d_held_qy_;
    if (held) {
        Kokkos::deep_copy(held_vol, d_volume_);
        Kokkos::deep_copy(held_qx, d_qcx_);
        Kokkos::deep_copy(held_qy, d_qcy_);
    }
    const int ncell = mesh_->n_triangles();
    auto gex = d_gex_, gey = d_gey_, gux = d_gux_, guy = d_guy_, gvx = d_gvx_, gvy = d_gvy_;
    auto slot_mx = d_slot_mx_, slot_my = d_slot_my_;
    if (lo < hi) {
        Kokkos::parallel_for(
            "fireCells", Kokkos::RangePolicy<ExecSpace>(lo, hi), KOKKOS_LAMBDA(int idx) {
                const int i = list(idx);
                // FULL_SWE keeps the wall flux at the same beginning state as
                // the interior faces.
                const double wall_h0 = swe ? depth(i) : 0.0;
                const double wall_eta0 = swe ? head(i) : 0.0;
                double flux_m3 = 0.0;
                double dmx = 0.0, dmy = 0.0; // FULL_SWE momentum gather (m⁴/s)
                for (int p = ptr(i); p < ptr(i + 1); ++p) {
                    const int e = edge(p);
                    if (sign(p) > 0.0) {
                        flux_m3 += faccL(e);
                        faccL(e) = 0.0;
                        if (swe) {
                            dmx += fmxL(e);
                            fmxL(e) = 0.0;
                            dmy += fmyL(e);
                            fmyL(e) = 0.0;
                        }
                    } else {
                        flux_m3 += faccR(e);
                        faccR(e) = 0.0;
                        if (swe) {
                            dmx += fmxR(e);
                            fmxR(e) = 0.0;
                            dmy += fmyR(e);
                            fmyR(e) = 0.0;
                        }
                    }
                }
                const double landed = vol(i) + flux_m3;
                const auto budget = sourceBudget(landed > 0.0 ? landed : 0.0, depth(i), area(i),
                                                 dt_c, dry, rain(i), coup(i), infil(i), evap(i));
                infil_app(i) += budget.infiltration / area(i);
                coup_app(i) += budget.coupling;
                evap_app(i) += budget.evaporation;
                vol(i) = budget.volume;
                double e2, d2;
                cell.etaDepth(i, vol(i), e2, d2);
                head(i) = e2;
                depth(i) = d2;
                if (perot) {
                    double sx = 0.0, sy = 0.0;
                    for (int p = ptr(i); p < ptr(i + 1); ++p) {
                        const int e = edge(p);
                        // DIFFUSIVE_WAVE's face discharge already carries the conveyance.
                        double f = sign(p) * qv(e) * xi(e);
                        if (li)
                            f *= conv(e);
                        sx += f * (mx(e) - cx(i));
                        sy += f * (my(e) - cy(i));
                    }
                    const double inv_a = 1.0 / area(i);
                    qcx(i) = sx * inv_a;
                    qcy(i) = sy * inv_a;
                }
                // FULL_SWE momentum update: M = A·q + Σ face bookings, the
                // wall pressure fluxes at the beginning state, then
                // semi-implicit friction against the NEW depth (== serial).
                if (swe) {
                    const double A = area(i);
                    const double h_new = depth(i);
                    if (wall_h0 > dry) {
                        for (int w = wall_ptr(i); w < wall_ptr(i + 1); ++w) {
                            const int slot = wall_slot(w);
                            const double nx = slot_nx(slot), ny = slot_ny(slot);
                            double eta = wall_eta0, hf = wall_h0;
                            double ux = qcx(i) / wall_h0, uy = qcy(i) / wall_h0;
                            if (so) {
                                const double ax = slot_mx(slot) - cx(i), ay = slot_my(slot) - cy(i);
                                eta += gex(i) * ax + gey(i) * ay;
                                const double hr = hf + gex(ncell + i) * ax + gey(ncell + i) * ay;
                                hf = hr > 0. ? hr : 0.;
                                ux += gux(i) * ax + guy(i) * ay;
                                uy += gvx(i) * ax + gvy(i) * ay;
                            }
                            const double un = ux * nx + uy * ny, ut = -ux * ny + uy * nx;
                            const double ug = -un * nx - ut * ny, vg = -un * ny + ut * nx;
                            swe::FaceFlux F;
                            double cLx, cLy, cRx, cRy;
                            bool wet;
                            if (so) {
                                const double z = wall_eta0 - wall_h0, zf = eta - hf;
                                wet = swe::faceFluxReconBed(eta, ux, uy, z, wall_h0, eta, ug, vg, z,
                                                            wall_h0, nx, ny, dry, F, cLx, cLy, cRx,
                                                            cRy, zf, zf);
                            } else {
                                wet = swe::faceFlux(eta, wall_h0, qcx(i), qcy(i), eta, wall_h0,
                                                    wall_h0 * ug, wall_h0 * vg, nx, ny, dry, F, cLx,
                                                    cLy, cRx, cRy);
                            }
                            if (wet) {
                                const double xdt = slot_len(slot) * dt_c;
                                dmx += (-F.mx + cLx) * xdt;
                                dmy += (-F.my + cLy) * xdt;
                            }
                        }
                    }
                    if (h_new > dry) {
                        double qx = (A * qcx(i) + dmx) / A;
                        double qy = (A * qcy(i) + dmy) / A;
                        swe::frictionUpdate(qx, qy, h_new, manning(i), dt_c);
                        qcx(i) = qx;
                        qcy(i) = qy;
                    } else {
                        qcx(i) = 0.0;
                        qcy(i) = 0.0;
                    }
                }
            });
        if (front_rebuild_ && !front_breach_) {
            // The wetting front reached the halo's outer ring: rebuild next cycle.
            auto frontier = d_frontier_;
            const double h_on = h_on_front_;
            int breached = 0;
            Kokkos::parallel_reduce(
                "front_breach", Kokkos::RangePolicy<ExecSpace>(lo, hi),
                KOKKOS_LAMBDA(int idx, int& any) {
                    const int i = list(idx);
                    if (frontier(i) && depth(i) >= h_on)
                        any = 1;
                },
                Kokkos::Max<int>(breached));
            // Max<int> starts from the lowest int, so "no cell" is not zero.
            front_breach_ = breached > 0;
        }
    }
    if (k != 0)
        return;

    // Boundary edges (tier-0 cadence). One thread per cell's run of edges
    // preserves the serial per-cell ordering for multi-edge cells.
    const int nbc = static_cast<int>(bc_cell_host_.size());
    if (nbc > 0) {
        auto run_ptr = d_bc_run_ptr_;
        auto bc_cell = d_bc_cell_, bc_slot = d_bc_slot_, bc_type = d_bc_type_;
        auto bc_accum = d_bc_accum_, bc_slope = d_bc_slope_;
        auto bc_in = d_bc_in_, bc_out = d_bc_out_;
        auto bc_requested = d_bc_requested_, bc_last_flow = d_bc_last_flow_;
        auto bc_last_dt = d_bc_last_dt_;
        auto bc_head = d_bc_head_, bc_flow = d_bc_flow_;
        auto bc_q = d_bc_q_;
        auto active = d_active_;
        auto tier = d_tier_;
        auto edge_len = d_edge_length_;
        auto mann = d_mannings_n_;
        auto vxv = d_vx_, vyv = d_vy_;
        auto cxv = d_tri_cx_, cyv = d_tri_cy_;
        const bool vfr_face = (opts_->face_reconstruction == FaceDepth2D::VFR_FACE);
        const bool diffusive = diffusive_;
        auto bc_conv = d_bc_conv_;
        const double fr_max = opts_->froude_max, dh_eps = opts_->flux_dh_eps;
        const int bt_wall = static_cast<int>(BoundaryType::WALL);
        const int bt_normal = static_cast<int>(BoundaryType::NORMAL_FLOW);
        const int bt_flow = static_cast<int>(BoundaryType::SPECIFIED_FLOW);
        const int bt_rating = static_cast<int>(BoundaryType::RATING_CURVE);
        const int bt_stage = static_cast<int>(BoundaryType::SPECIFIED_STAGE);
        Kokkos::parallel_for(
            "fireBc", Kokkos::RangePolicy<ExecSpace>(0, n_bc_runs_), KOKKOS_LAMBDA(int run) {
                for (int kk = run_ptr(run); kk < run_ptr(run + 1); ++kk) {
                    const int i = bc_cell(kk);
                    if (tier(i) != 0 || !active(i))
                        continue;
                    const int idx = bc_slot(kk);
                    const int ty = bc_type(kk);
                    const double L = edge_len(idx);
                    const double n = mann(i);
                    double f = 0.0;
                    // FULL_SWE: the momentum this edge booked and the mass it
                    // asked for, so a clamp can take the same fraction back.
                    double swe_dqx = 0.0, swe_dqy = 0.0, f_requested_swe = 0.0;
                    if (swe) {
                        // Ghost-cell Riemann boundary: mass AND momentum
                        // (swe::boundaryFlux, shared with the serial marcher).
                        if (L > 1.0e-12) {
                            double eta_i = head(i), h_i = depth(i);
                            double qx_i = qcx(i), qy_i = qcy(i);
                            if (held) {
                                cell.etaDepth(i, held_vol(i), eta_i, h_i);
                                qx_i = held_qx(i);
                                qy_i = held_qy(i);
                            }
                            const swe::BoundaryFlux r = swe::boundaryFlux(
                                ty, L, slot_nx(idx), slot_ny(idx), eta_i, h_i, qx_i, qy_i, n,
                                bc_slope(kk), bc_head(kk), bc_flow(kk), bc_conv(kk), dry);
                            f = r.flow;
                            if (r.wet) {
                                swe_dqx = r.fmx * L * dt_c / area(i);
                                swe_dqy = r.fmy * L * dt_c / area(i);
                                qcx(i) += swe_dqx;
                                qcy(i) += swe_dqy;
                            }
                        }
                        f_requested_swe = f;
                    } else if (ty == bt_wall) {
                        f = 0.0;
                    } else if (ty == bt_normal) {
                        const double S = bc_slope(kk);
                        double h_out = depth(i);
                        if (vfr_face) {
                            const int e = idx % kMaxCellVerts, nvi = nvv(i);
                            const double za = vz(cvv(i * kMaxCellVerts + (e + 1) % nvi));
                            const double zb = vz(cvv(i * kMaxCellVerts + (e + 2) % nvi));
                            h_out = inertial::faceDepthFromEta(head(i), (za < zb) ? za : zb,
                                                               (za < zb) ? zb : za);
                        }
                        // Signed slope, as the two CPU marchers: positive
                        // drains the domain, negative feeds it.
                        if (S != 0.0 && depth(i) > 0.0 && h_out > 0.0 && n > 0.0) {
                            const double sgnS = (S > 0.0) ? 1.0 : -1.0;
                            const double h53 = h_out * std::cbrt(h_out * h_out);
                            f = -sgnS * (h53 * std::sqrt(S < 0.0 ? -S : S) / n) * L;
                        }
                    } else if (ty == bt_flow || ty == bt_rating) {
                        f = -bc_flow(kk) * L;
                    } else if (ty == bt_stage && diffusive) {
                        // Diffusive-wave stage boundary: the collapsed Manning
                        // conductance against the prescribed stage (== serial
                        // boundaryEdgeFlux).
                        const double eta_bc = bc_head(kk);
                        const double dh = head(i) - eta_bc;
                        // Triangle expression retained verbatim; a quad uses the
                        // centroid-to-edge normal distance (== serial).
                        const double dx_b =
                            (nvv(i) == 3) ? ((L > 1.0e-12) ? (2.0 * area(i)) / (3.0 * L) : 0.0)
                                          : slot_dist(idx);
                        if (n > 0.0 && dx_b > 1.0e-12) {
                            double h_up;
                            if (vfr_face) {
                                const int e = idx % kMaxCellVerts, nvi = nvv(i);
                                const double za = vz(cvv(i * kMaxCellVerts + (e + 1) % nvi));
                                const double zb = vz(cvv(i * kMaxCellVerts + (e + 2) % nvi));
                                h_up = inertial::faceDepthFromEta((dh > 0.0) ? head(i) : eta_bc,
                                                                  (za < zb) ? za : zb,
                                                                  (za < zb) ? zb : za);
                            } else {
                                const double hb = eta_bc - cz(i);
                                h_up = (dh > 0.0) ? depth(i) : (hb > 0.0 ? hb : 0.0);
                            }
                            if (h_up > 0.0)
                                f = diffusive::stageBoundaryFlux(h_up, dh, n, dx_b, L, dh_eps);
                        }
                    } else if (ty == bt_stage) {
                        // Inertial stage boundary (mirrors the serial
                        // marcher): the interior momentum law integrated
                        // against a ghost held at η_bc, zero-gradient q.
                        const double eta_bc = bc_head(kk);
                        double hf;
                        if (vfr_face) {
                            const int e = idx % kMaxCellVerts, nvi = nvv(i);
                            const double za = vz(cvv(i * kMaxCellVerts + (e + 1) % nvi));
                            const double zb = vz(cvv(i * kMaxCellVerts + (e + 2) % nvi));
                            const double eta_hi = (head(i) > eta_bc) ? head(i) : eta_bc;
                            hf = inertial::faceDepthFromEta(eta_hi, (za < zb) ? za : zb,
                                                            (za < zb) ? zb : za);
                        } else {
                            hf = inertial::faceFlowDepth(head(i), eta_bc, cz(i));
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
                        const double slope = (nvv(i) == 3) ? deta * (3.0 * L) / (2.0 * area(i))
                                                           : deta / slot_dist(idx);
                        double qn1 = inertial::inertialFaceUpdate(
                            bc_q(kk), bc_q(kk), hf, dt_c, slope, n * n, std::fabs(bc_q(kk)));
                        qn1 = inertial::froudeCap(qn1, hf, fr_max);
                        f = qn1 * L; // inflow-positive
                    }
                    bc_last_dt(kk) = dt_c;
                    if (f == 0.0) {
                        bc_q(kk) = 0.0;
                        continue;
                    }
                    // Volume-space clamp; booked flux re-derived from the
                    // applied change (mirrors ExplicitInertialSolver).
                    bc_requested(kk) += dt_c * f;
                    const double v_old = vol(i);
                    double v_new = v_old + dt_c * f;
                    if (ty == bt_stage && !swe) {
                        // Equilibrium clamp, kept as the tiny-cell /
                        // overshoot backstop: one substep moves the cell AT
                        // MOST to the prescribed stage.
                        const double v_eq = cell.volumeFromEta(i, bc_head(kk));
                        if (f < 0.0) {
                            const double lo = (v_old < v_eq) ? v_old : v_eq;
                            if (v_new < lo)
                                v_new = lo;
                        } else {
                            const double hi = (v_old > v_eq) ? v_old : v_eq;
                            if (v_new > hi)
                                v_new = hi;
                        }
                    }
                    if (v_new < 0.0)
                        v_new = 0.0; // availability floor
                    f = (v_new - v_old) / dt_c;
                    bc_last_flow(kk) = f;
                    // FULL_SWE booked its momentum against the REQUESTED mass;
                    // when the clamp shrank the mass, take the same fraction of
                    // the momentum back (== serial).
                    if (swe && f != f_requested_swe && (swe_dqx != 0.0 || swe_dqy != 0.0)) {
                        const double lam = (f_requested_swe != 0.0) ? f / f_requested_swe : 0.0;
                        qcx(i) += swe_dqx * (lam - 1.0);
                        qcy(i) += swe_dqy * (lam - 1.0);
                    }
                    // Momentum matches applied mass (== serial rescale).
                    bc_q(kk) = (L > 1.0e-12) ? f / L : 0.0;
                    if (f != 0.0) {
                        vol(i) = v_new;
                        bc_accum(kk) += dt_c * f;
                        const double applied = v_new - v_old;
                        if (applied > 0.0)
                            bc_in(kk) += applied;
                        else
                            bc_out(kk) -= applied;
                        double e2, d2;
                        cell.etaDepth(i, vol(i), e2, d2);
                        head(i) = e2;
                        depth(i) = d2;
                    }
                    // Perot completion: add the boundary edge's contribution
                    // the interior-only rebuild missed (== serial marcher).
                    if (perot && bc_q(kk) != 0.0) {
                        const int e = idx % kMaxCellVerts, nvi = nvv(i);
                        const int va = cvv(i * kMaxCellVerts + (e + 1) % nvi);
                        const int vb = cvv(i * kMaxCellVerts + (e + 2) % nvi);
                        const double mxb = 0.5 * (vxv(va) + vxv(vb));
                        const double myb = 0.5 * (vyv(va) + vyv(vb));
                        const double fo = -f; // outward flux (m³/s)
                        const double inv_a = 1.0 / area(i);
                        qcx(i) += fo * (mxb - cxv(i)) * inv_a;
                        qcy(i) += fo * (myb - cyv(i)) * inv_a;
                    }
                }
            });
    }

    // Live junction exchange at tier-0 cadence. Shared-cell drains and
    // shared-node spill budgets are order-dependent by contract, so points
    // that share either fire in serial point order on one thread; the groups
    // built at initialize are mutually independent and fire concurrently.
    const int np = static_cast<int>(exch_host_.size());
    if (np > 0 && state_->nodes_1d) {
        auto group_ptr = d_cp_group_ptr_, group_item = d_cp_group_item_;
        auto cp_cell = d_cp_cell_, cp_vertex = d_cp_vertex_, cp_node = d_cp_node_;
        auto cp_cd = d_cp_cd_, cp_area = d_cp_area_;
        auto cp_law = d_cp_law_, cp_hb = d_cp_head_b_, cp_he = d_cp_head_e_;
        auto head_cells = d_head_cells_;
        auto cp_geo = d_cp_geo_, cp_coef = d_cp_coef_;
        auto cp_eq = d_cp_eq_;
        auto npa = d_node_pond_area_;
        const double eq_window = state_->node_pond_area_m2 ? state_->coupling_window_s : 0.0;
        auto exch = d_exch_, node_drawn = d_node_drawn_;
        auto coup_app = d_coupling_applied_;
        auto nh = d_node_head_, nd = d_node_depth_, nv = d_node_volume_;
        auto nfv = d_node_fullvol_;
        auto ninv = d_node_invert_, nfd = d_node_fulldepth_;
        auto active = d_active_;
        auto vs_ptr = d_vs_ptr_, vs_idx = d_vs_idx_;
        auto vs_wt = d_vs_wt_;
        auto vzv = d_vz_;
        const double len12 = opts_->len_1d_to_2d;
        const double vol12 = opts_->vol_1d_to_2d;
        const double beta = opts_->exchange_beta;
        Kokkos::parallel_for(
            "fireExchange", Kokkos::RangePolicy<ExecSpace>(0, n_cp_groups_),
            KOKKOS_LAMBDA(int group) {
                for (int gi = group_ptr(group); gi < group_ptr(group + 1); ++gi) {
                    const int kk = group_item(gi);
                    const int ci = cp_cell(kk);
                    if (ci < 0 || !active(ci))
                        continue;
                    const int ni = cp_node(kk);
                    const int vtx = cp_vertex(kk);

                    // couplingHead2D: pseudo-Laplacian vertex head (FLAT) /
                    // wet-masked depth-weighted η (VFR) / cell head.
                    double h_2d;
                    if (cp_he(kk) > cp_hb(kk)) {
                        // COUPLING_HEAD VERTEX / STENCIL_WET_MEAN (couplingHead2D).
                        double num = 0.0, den = 0.0;
                        for (int p = cp_hb(kk); p < cp_he(kk); ++p) {
                            const int t = head_cells(p);
                            const double h = depth(t);
                            if (!(h >= dry))
                                continue;
                            num += h * head(t);
                            den += h;
                        }
                        h_2d = (den > 0.0) ? num / den : head(ci);
                    } else if (vtx < 0) {
                        h_2d = head(ci);
                    } else if (vfr) {
                        double num = 0.0, den = 0.0;
                        for (int p = vs_ptr(vtx); p < vs_ptr(vtx + 1); ++p) {
                            const int t = vs_idx(p);
                            const double h = depth(t);
                            if (!(h >= dry))
                                continue;
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
                            if (dpt > depth_2d_avail)
                                depth_2d_avail = dpt;
                        }
                    } else {
                        depth_2d_avail = depth(ci);
                    }
                    const double depth_1d_avail = nd(ni) * len12;

                    double Q;
                    if (cp_law(kk) == 1) {
                        // WEIR_ORIFICE (computeNodeCouplingQ): no rim gate,
                        // no area ramp — the law is zero below the crest.
                        openswmm::twoD::exchange::Geometry g;
                        g.z_crest = cp_geo(3 * kk + 0);
                        g.l_weir = cp_geo(3 * kk + 1);
                        g.a_orif = cp_geo(3 * kk + 2);
                        openswmm::twoD::exchange::Coefficients c;
                        c.c_w = cp_coef(5 * kk + 0);
                        c.c_sw = cp_coef(5 * kk + 1);
                        c.c_oi = cp_coef(5 * kk + 2);
                        c.c_oo = cp_coef(5 * kk + 3);
                        c.h_eps = cp_coef(5 * kk + 4);
                        Q = openswmm::twoD::exchange::exchange(g, c, h_2d, h_1d).q;
                    } else {
                        const double crown = (ninv(ni) + nfd(ni)) * len12;
                        const double h_max = (h_1d > h_2d) ? h_1d : h_2d;
                        const double A_eff =
                            devEffectiveArea(h_max, crown, nfd(ni), cp_area(kk), cp_area(kk) * 2.0);

                        Q = devOrificeFlow(h_2d - h_1d, cp_cd(kk), A_eff);

                        const double CAP_BAND = 0.05;
                        double ct = (h_max - crown) / CAP_BAND;
                        ct = (ct < 0.0) ? 0.0 : (ct > 1.0 ? 1.0 : ct);
                        Q *= ct * ct * (3.0 - 2.0 * ct);
                    }

                    Q *= (Q > 0.0) ? devWetRamp(depth_2d_avail, dry)
                                   : devWetRamp(depth_1d_avail, dry);
                    if (cp_eq(kk) && eq_window > 0.0) { // COUPLING_EQUALIZE
                        const double cap =
                            openswmm::twoD::pond::equalizeCap(h_2d - h_1d, eq_window, npa(ni));
                        Q = (Q > cap) ? cap : ((Q < -cap) ? -cap : Q);
                    }
                    if (Q == 0.0)
                        continue;

                    if (Q > 0.0) { // 2D → 1D drain: availability share
                        const double vmax = vol(ci) > 0.0 ? vol(ci) : 0.0;
                        const double cap = beta * vmax / dt_c;
                        if (Q > cap)
                            Q = cap;
                    } else {
                        // 1D → 2D spill: PONDED budget only (C3). See the CPU
                        // marcher for why total node storage is wrong — the
                        // spill exits at the rim and cannot reach below-crown
                        // conveyance storage. The two backends must agree.
                        const double ponded = (nv(ni) > nfv(ni)) ? (nv(ni) - nfv(ni)) : 0.0;
                        const double avail0 = ponded * vol12 - node_drawn(ni);
                        const double avail = (avail0 > 0.0) ? avail0 : 0.0;
                        if (avail <= 0.0)
                            continue;
                        const double want = -Q * dt_c;
                        const double take = (want < avail) ? want : avail;
                        node_drawn(ni) += take;
                        Q = -take / dt_c;
                    }
                    // C1: mirror of the CPU marcher — book the signed
                    // per-cell abstraction where the node exchange actually
                    // moves water. The two sites above book `coup`, which
                    // only carries the FORCING API's prescribed rate.
                    coup_app(ci) += -Q * dt_c;
                    vol(ci) -= Q * dt_c;
                    if (vol(ci) < 0.0)
                        vol(ci) = 0.0;
                    exch(kk) += Q * dt_c;
                    double e2, d2;
                    cell.etaDepth(ci, vol(ci), e2, d2);
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
            if (s % (1 << k))
                continue;
            fireFaces(k, (1 << k) * dt0);
        }
        for (int k = 0; k < K; ++k) {
            if ((s + 1) % (1 << k))
                continue;
            fireCells(k, (1 << k) * dt0);
        }
        ++substeps_run_;
        ++last_steps_;
    }
}

// ===========================================================================
// Optional momentum physics: Coriolis, turbulent mixing, wall drag
// ===========================================================================
// Each routine follows its MomentumPhysics2D counterpart and keeps that code's
// summation orders, so the OpenMP backend reproduces the CPU marcher exactly.

void ExplicitKokkosSurfaceSolver::physicsRotate(double angle) {
    if (angle == 0)
        return;
    const double c = std::cos(angle), s = std::sin(angle);
    auto qx = d_qcx_, qy = d_qcy_;
    Kokkos::parallel_for(
        "ph_rotate", Kokkos::RangePolicy<ExecSpace>(0, mesh_->n_triangles()), KOKKOS_LAMBDA(int i) {
            const double x = qx(i), y = qy(i);
            qx(i) = c * x + s * y;
            qy(i) = c * y - s * x;
        });
}

double ExplicitKokkosSurfaceSolver::physicsWallDrag(double seconds) {
    const int n = n_wall_drag_;
    if (n == 0)
        return 0.0;
    auto slots = d_wd_slot_, group = d_wd_group_;
    auto cw = d_wd_cw_, removed = d_wd_removed_;
    auto h = d_depth_, eta = d_head_, qx = d_qcx_, qy = d_qcy_;
    auto area = d_tri_area_, len = d_edge_length_, snx = d_slot_nx_, sny = d_slot_ny_;
    auto vz = d_vz_;
    auto cvv = d_cell_v_, nvv = d_nv_;
    const double dry = opts_->dry_depth;
    const bool vfr = opts_->cell_closure == CellClosure2D::VFR;
    // The CPU sweeps every slot forward, then every slot in reverse. Slots of
    // different cells are independent, so each cell runs its own forward and
    // reverse sweep; the energy terms are folded below in the CPU's order.
    Kokkos::parallel_for(
        "ph_wall_drag", Kokkos::RangePolicy<ExecSpace>(0, n_wall_groups_), KOKKOS_LAMBDA(int g) {
            const int first = group(g), last = group(g + 1);
            for (int pass = 0; pass < 2; ++pass) {
                for (int step = 0; step < last - first; ++step) {
                    const int s = pass == 0 ? first + step : last - 1 - step;
                    const int out = pass == 0 ? s : n + s;
                    removed(out) = 0.0;
                    const int slot = slots(s);
                    const int i = slot / kMaxCellVerts;
                    if (h(i) <= dry)
                        continue;
                    double hw = h(i);
                    if (vfr) {
                        const int e = slot % kMaxCellVerts, nvi = nvv(i);
                        const double za = vz(cvv(i * kMaxCellVerts + (e + 1) % nvi));
                        const double zb = vz(cvv(i * kMaxCellVerts + (e + 2) % nvi));
                        hw = inertial::faceDepthFromEta(eta(i), (zb < za) ? zb : za,
                                                        (za < zb) ? zb : za);
                    }
                    const double k = cw(s) * len(slot) * hw / (area(i) * h(i));
                    if (k == 0)
                        continue;
                    const double tx = -sny(slot), ty = snx(slot);
                    const double ut = (qx(i) * tx + qy(i) * ty) / h(i);
                    if (ut == 0)
                        continue;
                    const double next =
                        Kokkos::copysign(1.0 / (1.0 / std::fabs(ut) + .5 * seconds * k), ut);
                    const double dq = h(i) * (next - ut);
                    qx(i) += dq * tx;
                    qy(i) += dq * ty;
                    removed(out) = .5 * area(i) * h(i) * (ut - next) * (ut + next);
                }
            }
        });
    auto removed_h = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace(), removed);
    double sum = 0;
    for (int s = 0; s < n; ++s)
        sum += removed_h(s);
    for (int s = n - 1; s >= 0; --s)
        sum += removed_h(n + s);
    return sum;
}

double ExplicitKokkosSurfaceSolver::physicsEnergy(const DView& qx, const DView& qy) {
    const int nt = mesh_->n_triangles();
    auto h = d_depth_, area = d_tri_area_, term = d_ph_term_;
    const double dry = opts_->dry_depth;
    Kokkos::parallel_for(
        "ph_energy", Kokkos::RangePolicy<ExecSpace>(0, nt), KOKKOS_LAMBDA(int i) {
            term(i) = h(i) > dry ? .5 * area(i) * (qx(i) * qx(i) + qy(i) * qy(i)) / h(i) : 0.0;
        });
    // The total decides whether a mixing sub-step is accepted, so it is folded
    // in cell order like the CPU's.
    auto term_h = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace(), term);
    auto h_h = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace(), h);
    double sum = 0;
    for (int i = 0; i < nt; ++i)
        if (h_h(i) > dry)
            sum += term_h(i);
    return sum;
}

double ExplicitKokkosSurfaceSolver::physicsEvaluate(const DView& qx, const DView& qy,
                                                    const DView& rx, const DView& ry, double cs,
                                                    double background, double depth_coefficient) {
    const int nt = mesh_->n_triangles();
    const int ne = edges_.ne;
    auto h = d_depth_, eta = d_head_;
    auto conn = d_ph_conn_;
    auto cLv = d_cL_, cRv = d_cR_;
    auto conv = d_conveyance_;
    const double dry = opts_->dry_depth;
    Kokkos::parallel_for(
        "ph_connected", Kokkos::RangePolicy<ExecSpace>(0, ne), KOKKOS_LAMBDA(int e) {
            const int i = cLv(e), j = cRv(e);
            const double bi = eta(i) - h(i), bj = eta(j) - h(j);
            const double sill = (bi < bj) ? bj : bi;
            const double low = (eta(j) < eta(i)) ? eta(j) : eta(i);
            conn(e) = (conv(e) > 0 && h(i) > dry && h(j) > dry && low - sill > dry) ? 1 : 0;
        });

    auto ptr = d_cell_ptr_, edge = d_cell_edge_;
    auto cx = d_tri_cx_, cy = d_tri_cy_, area = d_tri_area_;
    auto stid = d_ph_stid_, stn = d_ph_stn_;
    auto stgx = d_ph_stgx_, stgy = d_ph_stgy_;
    auto visc = d_ph_visc_, walld = d_ph_walld_;
    const double kappa = 0.41; // MomentumPhysics2D::kappa
    int bad = 0;
    Kokkos::parallel_reduce(
        "ph_stencil", Kokkos::RangePolicy<ExecSpace>(0, nt),
        KOKKOS_LAMBDA(int i, int& flag) {
            visc(i) = 0;
            for (int k = 0; k < 5; ++k) {
                stid(5 * i + k) = 0;
                stgx(5 * i + k) = 0;
                stgy(5 * i + k) = 0;
            }
            stn(i) = 0;
            if (h(i) <= dry)
                return;
            int ids[5] = {i, 0, 0, 0, 0};
            double gx[5] = {0, 0, 0, 0, 0}, gy[5] = {0, 0, 0, 0, 0};
            int count = 1;
            double xx = 0, xy = 0, yy = 0;
            for (int p = ptr(i); p < ptr(i + 1); ++p) {
                const int e = edge(p);
                if (!conn(e))
                    continue;
                const int j = cLv(e) == i ? cRv(e) : cLv(e);
                const double dx = cx(j) - cx(i), dy = cy(j) - cy(i);
                const double w = 1 / (dx * dx + dy * dy);
                ids[count] = j;
                gx[count] = w * dx;
                gy[count] = w * dy;
                ++count;
                xx += w * dx * dx;
                xy += w * dx * dy;
                yy += w * dy * dy;
            }
            const double det = xx * yy - xy * xy;
            const bool single = count < 3 || det <= 1e-10 * (xx + yy) * (xx + yy);
            if (!single) {
                for (int k = 1; k < count; ++k) {
                    const double x = gx[k], y = gy[k];
                    gx[k] = (yy * x - xy * y) / det;
                    gy[k] = (xx * y - xy * x) / det;
                    gx[0] -= gx[k];
                    gy[0] -= gy[k];
                }
            }
            for (int k = 0; k < 5; ++k) {
                stid(5 * i + k) = ids[k];
                stgx(5 * i + k) = gx[k];
                stgy(5 * i + k) = gy[k];
            }
            if (single) {
                stn(i) = 1;
                return;
            }
            stn(i) = count;
            double ux = 0, uy = 0, vx = 0, vy = 0;
            for (int k = 0; k < count; ++k) {
                const int j = ids[k];
                const double u = qx(j) / h(j), v = qy(j) / h(j);
                ux += gx[k] * u;
                uy += gy[k] * u;
                vx += gx[k] * v;
                vy += gy[k] * v;
            }
            const double strain = std::sqrt(2 * ux * ux + 2 * vy * vy + (uy + vx) * (uy + vx));
            const double vorticity = std::fabs(vx - uy);
            if (depth_coefficient > 0) {
                const double a = depth_coefficient * h(i), b = kappa * walld(i);
                visc(i) = background + Kokkos::pow((b < a) ? b : a, 2.0) * vorticity;
            } else {
                visc(i) = background + cs * cs * area(i) * strain;
            }
            if (!Kokkos::isfinite(visc(i)))
                flag = 1;
        },
        Kokkos::Max<int>(bad));
    if (bad > 0)
        throw std::runtime_error("Nonfinite Smagorinsky viscosity.");

    // A corner with one wet neighbor cannot determine a transverse gradient: it
    // borrows a connected neighbor's complete stencil. A borrowing cell can
    // serve a later one, so these few cells run in ascending order on one thread.
    auto single = d_ph_single_;
    int n_single = 0;
    Kokkos::parallel_scan(
        "ph_single_list", Kokkos::RangePolicy<ExecSpace>(0, nt),
        KOKKOS_LAMBDA(int i, int& upd, bool fin) {
            const int f = (h(i) > dry && stn(i) == 1) ? 1 : 0;
            if (fin && f)
                single(upd) = i;
            upd += f;
        },
        n_single);
    if (n_single > 0)
        Kokkos::parallel_for(
            "ph_single", Kokkos::RangePolicy<ExecSpace>(0, 1), KOKKOS_LAMBDA(int) {
                for (int idx = 0; idx < n_single; ++idx) {
                    const int i = single(idx);
                    for (int p = ptr(i); p < ptr(i + 1); ++p) {
                        const int e = edge(p);
                        if (!conn(e))
                            continue;
                        const int j = cLv(e) == i ? cRv(e) : cLv(e);
                        if (stn(j) > 1) {
                            for (int k = 0; k < 5; ++k) {
                                stid(5 * i + k) = stid(5 * j + k);
                                stgx(5 * i + k) = stgx(5 * j + k);
                                stgy(5 * i + k) = stgy(5 * j + k);
                            }
                            stn(i) = stn(j);
                            break;
                        }
                    }
                    double ux = 0, uy = 0, vx = 0, vy = 0;
                    for (int k = 0; k < stn(i); ++k) {
                        const int j = stid(5 * i + k);
                        const double u = qx(j) / h(j), v = qy(j) / h(j);
                        ux += stgx(5 * i + k) * u;
                        uy += stgy(5 * i + k) * u;
                        vx += stgx(5 * i + k) * v;
                        vy += stgy(5 * i + k) * v;
                    }
                    const double strain =
                        std::sqrt(2 * ux * ux + 2 * vy * vy + (uy + vx) * (uy + vx));
                    const double vorticity = std::fabs(vx - uy);
                    if (depth_coefficient > 0) {
                        const double a = depth_coefficient * h(i), b = kappa * walld(i);
                        visc(i) = background + Kokkos::pow((b < a) ? b : a, 2.0) * vorticity;
                    } else {
                        visc(i) = background + cs * cs * area(i) * strain;
                    }
                }
            });

    // Face stress and its contribution to the step bound, one face each.
    auto nxv = d_nx_, nyv = d_ny_, xi = d_xi_, emx = d_mx_, emy = d_my_;
    auto etx = d_ph_etx_, ety = d_ph_ety_, ebx = d_ph_ebx_, eby = d_ph_eby_;
    bad = 0;
    Kokkos::parallel_reduce(
        "ph_face_stress", Kokkos::RangePolicy<ExecSpace>(0, ne),
        KOKKOS_LAMBDA(int e, int& flag) {
            etx(e) = ety(e) = ebx(e) = eby(e) = 0;
            if (!conn(e))
                return;
            const int i = cLv(e), j = cRv(e);
            const double nx = nxv(e), ny = nyv(e);
            const double dx = cx(j) - cx(i), dy = cy(j) - cy(i);
            const double dn = dx * nx + dy * ny;
            if (!(dn > 0)) {
                flag = 1;
                return;
            }
            int ids[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
            double gx[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
            double gy[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
            int count = 0;
            const double fx = emx(e), fy = emy(e);
            const double di = Kokkos::hypot(fx - cx(i), fy - cy(i));
            const double dj = Kokkos::hypot(fx - cx(j), fy - cy(j));
            const double wi = dj / (di + dj), wj = 1 - wi;
            for (int side = 0; side < 2; ++side) {
                const int cell = side == 0 ? i : j;
                const double w = side == 0 ? wi : wj;
                for (int k = 0; k < stn(cell); ++k) {
                    const int id = stid(5 * cell + k);
                    int m = 0;
                    while (m < count && ids[m] != id)
                        ++m;
                    if (m == count) {
                        ids[m] = id;
                        ++count;
                    }
                    gx[m] += w * stgx(5 * cell + k);
                    gy[m] += w * stgy(5 * cell + k);
                }
            }
            for (int side = 0; side < 2; ++side) {
                const int id = side == 0 ? i : j;
                int m = 0;
                while (m < count && ids[m] != id)
                    ++m;
                if (m == count) {
                    ids[m] = id;
                    ++count;
                }
                gx[m] += 0.0; // the CPU's add(id, 0, 0): keeps the sign of a zero identical
                gy[m] += 0.0;
            }
            double ux = 0, uy = 0, vx = 0, vy = 0, bx = 0, by = 0;
            const double nu = wi * visc(i) + wj * visc(j);
            const double low = (eta(j) < eta(i)) ? eta(j) : eta(i);
            const double bi = eta(i) - h(i), bj = eta(j) - h(j);
            const double span = low - ((bi < bj) ? bj : bi);
            const double wet = (0. < span) ? span : 0.;
            const double weight = xi(e) * wet * nu * conv(e);
            for (int k = 0; k < count; ++k) {
                const int id = ids[k];
                const double delta = (id == j ? 1. : 0.) - (id == i ? 1. : 0.);
                const double correction = (delta - gx[k] * dx - gy[k] * dy) / dn;
                gx[k] += nx * correction;
                gy[k] += ny * correction;
                ux += gx[k] * qx(id) / h(id);
                uy += gy[k] * qx(id) / h(id);
                vx += gx[k] * qy(id) / h(id);
                vy += gy[k] * qy(id) / h(id);
                bx += std::fabs(2 * nx * gx[k] + ny * gy[k]) + std::fabs(ny * gx[k]);
                by += std::fabs(nx * gy[k]) + std::fabs(nx * gx[k] + 2 * ny * gy[k]);
            }
            etx(e) = weight * (2 * ux * nx + (uy + vx) * ny);
            ety(e) = weight * (2 * vy * ny + (uy + vx) * nx);
            ebx(e) = weight * bx;
            eby(e) = weight * by;
        },
        Kokkos::Max<int>(bad));
    if (bad > 0)
        throw std::runtime_error("Invalid mixing face geometry.");

    // Each cell gathers its faces in ascending face order, the order in which
    // the CPU's face loop adds to it.
    auto sedge = d_ph_sedge_;
    double rate = 0;
    Kokkos::parallel_reduce(
        "ph_gather", Kokkos::RangePolicy<ExecSpace>(0, nt),
        KOKKOS_LAMBDA(int c, double& mx) {
            double sx = 0, sy = 0, bx = 0, by = 0;
            const double mass = area(c) * h(c);
            for (int p = ptr(c); p < ptr(c + 1); ++p) {
                const int e = sedge(p);
                if (!conn(e))
                    continue;
                if (cLv(e) == c) {
                    sx += etx(e) / area(c);
                    sy += ety(e) / area(c);
                } else {
                    sx -= etx(e) / area(c);
                    sy -= ety(e) / area(c);
                }
                bx += ebx(e) / mass;
                by += eby(e) / mass;
            }
            rx(c) = sx;
            ry(c) = sy;
            const double b = (bx < by) ? by : bx;
            if (mx < b)
                mx = b;
        },
        Kokkos::Max<double>(rate));
    if (rate < 0)
        rate = 0;
    if (!std::isfinite(rate))
        throw std::runtime_error("Nonfinite mixing timestep bound.");
    ph_last_limit_ = rate > 0 ? .4 / rate : std::numeric_limits<double>::infinity();
    return ph_last_limit_;
}

void ExplicitKokkosSurfaceSolver::physicsAdvance(double cs, double seconds, double background,
                                                 double depth_coefficient) {
    if (seconds <= 0 || (cs == 0 && background == 0 && depth_coefficient == 0))
        return;
    const int nt = mesh_->n_triangles();
    auto qx = d_qcx_, qy = d_qcy_;
    auto rx = d_ph_rx_, ry = d_ph_ry_, sx = d_ph_sx_, sy = d_ph_sy_;
    auto tx = d_ph_tx_, ty = d_ph_ty_;
    Kokkos::deep_copy(d_ph_oldx_, qx);
    Kokkos::deep_copy(d_ph_oldy_, qy);
    try {
        double elapsed = 0;
        while (elapsed < seconds) {
            const double bound = physicsEvaluate(qx, qy, rx, ry, cs, background, depth_coefficient);
            double dt = std::min(seconds - elapsed, bound);
            const double e0 = physicsEnergy(qx, qy);
            bool accepted = false;
            for (int retry = 0; retry < 40; ++retry) {
                Kokkos::parallel_for(
                    "ph_predict", Kokkos::RangePolicy<ExecSpace>(0, nt), KOKKOS_LAMBDA(int j) {
                        tx(j) = qx(j) + dt * rx(j);
                        ty(j) = qy(j) + dt * ry(j);
                    });
                const double stage_bound =
                    physicsEvaluate(tx, ty, sx, sy, cs, background, depth_coefficient);
                if (dt <= stage_bound) {
                    Kokkos::parallel_for(
                        "ph_correct", Kokkos::RangePolicy<ExecSpace>(0, nt), KOKKOS_LAMBDA(int j) {
                            tx(j) = .5 * (qx(j) + tx(j) + dt * sx(j));
                            ty(j) = .5 * (qy(j) + ty(j) + dt * sy(j));
                        });
                    const double e1 = physicsEnergy(tx, ty);
                    if (std::isfinite(e1) && e1 <= e0 + 1e-13 * std::max(e0, 1e-30)) {
                        accepted = true;
                        break;
                    }
                }
                dt *= .5;
                ++ph_retries_;
            }
            if (!accepted || elapsed + dt == elapsed)
                throw std::runtime_error("Smagorinsky mixing cannot find an admissible timestep.");
            Kokkos::deep_copy(qx, tx);
            Kokkos::deep_copy(qy, ty);
            elapsed = dt == seconds - elapsed ? seconds : elapsed + dt;
            ++ph_substeps_;
        }
        physicsEvaluate(qx, qy, rx, ry, cs, background, depth_coefficient);
    } catch (...) {
        Kokkos::deep_copy(qx, d_ph_oldx_);
        Kokkos::deep_copy(qy, d_ph_oldy_);
        throw;
    }
}

void ExplicitKokkosSurfaceSolver::applyMomentumPhysics(double seconds) {
    wall_drag_energy_ += physicsWallDrag(.5 * seconds);
    const double f = opts_->coriolisParameter();
    physicsRotate(.5 * seconds * f);
    if (opts_->turbulence == Turbulence2D::SMAGORINSKY)
        physicsAdvance(opts_->smagorinsky_cs, seconds, opts_->eddy_viscosity_background, 0.0);
    else if (opts_->turbulence == Turbulence2D::MIXING_LENGTH)
        physicsAdvance(0.0, seconds, opts_->eddy_viscosity_background,
                       opts_->mixing_length_coefficient);
    physicsRotate(.5 * seconds * f);
    wall_drag_energy_ += physicsWallDrag(.5 * seconds);
}

double ExplicitKokkosSurfaceSolver::runMomentumStep(double limit) {
    if (state_->transport.active() || state_->heat.enabled || state_->snow.active() ||
        (state_->node_coupling && !state_->node_coupling->empty()) ||
        state_->runtime_sources.active() || !state_->runtime_forcings.rows.empty())
        throw std::runtime_error(
            "Optional momentum physics has an unqualified coupled source owner.");
    const int nt = mesh_->n_triangles();
    double dt = limit;
    const double f = std::abs(opts_->coriolisParameter());
    if (f > 0)
        dt = std::min(dt, .25 / f);
    if (opts_->turbulence == Turbulence2D::SMAGORINSKY &&
        (opts_->smagorinsky_cs > 0 || opts_->eddy_viscosity_background > 0)) {
        dt = std::min(dt, physicsEvaluate(d_qcx_, d_qcy_, d_ph_rx_, d_ph_ry_, opts_->smagorinsky_cs,
                                          opts_->eddy_viscosity_background, 0.0));
    } else if (opts_->turbulence == Turbulence2D::MIXING_LENGTH &&
               (opts_->mixing_length_coefficient > 0 || opts_->eddy_viscosity_background > 0)) {
        dt = std::min(dt, physicsEvaluate(d_qcx_, d_qcy_, d_ph_rx_, d_ph_ry_, 0.0,
                                          opts_->eddy_viscosity_background,
                                          opts_->mixing_length_coefficient));
    }
    // Only momentum changes during a trial. Tighten the hydrodynamic CFL after
    // the first split half.
    Kokkos::deep_copy(d_ph_x0_, d_qcx_);
    Kokkos::deep_copy(d_ph_y0_, d_qcy_);
    const double wall_energy0 = wall_drag_energy_;
    auto depth = d_depth_, lpos = d_lpos_, qcx = d_qcx_, qcy = d_qcy_;
    const double alpha = opts_->cfl_number, dry = opts_->dry_depth;
    for (int trial = 0;; ++trial) {
        Kokkos::deep_copy(d_qcx_, d_ph_x0_);
        Kokkos::deep_copy(d_qcy_, d_ph_y0_);
        wall_drag_energy_ = wall_energy0;
        applyMomentumPhysics(.5 * dt);
        double stable = 1.0e300;
        Kokkos::parallel_reduce(
            "ph_wave_cfl", Kokkos::RangePolicy<ExecSpace>(0, nt),
            KOKKOS_LAMBDA(int i, double& mn) {
                if (depth(i) > dry) {
                    const double s = inertial::cellCflDt(alpha, lpos(i), depth(i),
                                                         Kokkos::hypot(qcx(i), qcy(i)) / depth(i));
                    if (s < mn)
                        mn = s;
                }
            },
            Kokkos::Min<double>(stable));
        stable = std::min(opts_->max_timestep, stable);
        if (dt <= stable)
            break;
        if (trial == 39) {
            Kokkos::deep_copy(d_qcx_, d_ph_x0_);
            Kokkos::deep_copy(d_qcy_, d_ph_y0_);
            wall_drag_energy_ = wall_energy0;
            throw std::runtime_error("Optional momentum physics cannot satisfy the wave CFL.");
        }
        dt = std::min(.5 * dt, stable);
    }
    if (second_order_)
        runRk2Step(dt);
    else
        runMacroCycle(dt, 1);
    applyMomentumPhysics(.5 * dt);
    return dt;
}

// ===========================================================================
// advance
// ===========================================================================

void ExplicitKokkosSurfaceSolver::pushForcings() {
    if (state_->snow.active()) {
        auto rain = state_->rainfall, evap = state_->evap_rate;
        for (int i = 0; i < int(rain.size()); ++i) {
            rain[i] = state_->liquidRainfall(i);
            evap[i] = state_->exposedEvaporation(i);
        }
        devRefresh(d_rain_, rain);
        devRefresh(d_evap_, evap);
    } else {
        devRefresh(d_rain_, state_->rainfall);
        devRefresh(d_evap_, state_->evap_rate);
    }
    devRefresh(d_coup_, state_->coupling_flux);
    // Held INFIL_STEP rate (plan §5.5, D-I1) — republished host-side by
    // Infil2D::updateRates, so it uploads with the other held forcings.
    devRefresh(d_infil_, state_->infil_rate);

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
        Kokkos::View<const int*, Kokkos::HostSpace, Kokkos::MemoryTraits<Kokkos::Unmanaged>> hty(
            ty.data(), ty.size());
        Kokkos::deep_copy(d_bc_type_, hty);
        devRefresh(d_bc_slope_, sl);
        devRefresh(d_bc_head_, hd);
        devRefresh(d_bc_flow_, fl);
    }
}

void ExplicitKokkosSurfaceSolver::pushNodeState() {
    if (!state_->nodes_1d || d_node_head_.extent(0) == 0)
        return;
    devRefresh(d_node_head_, state_->nodes_1d->head);
    devRefresh(d_node_depth_, state_->nodes_1d->depth);
    devRefresh(d_node_volume_, state_->nodes_1d->volume);
    if (state_->node_pond_area_m2 &&
        state_->node_pond_area_m2->size() == d_node_pond_area_.extent(0))
        devRefresh(d_node_pond_area_, *state_->node_pond_area_m2);
}

void ExplicitKokkosSurfaceSolver::publishAndCopyBack(double t_current, double t_target) {
    const int ne = edges_.ne;
    Kokkos::deep_copy(d_edge_flux_, 0.0);
    {
        auto ef = d_edge_flux_;
        auto cLv = d_cL_, cRv = d_cR_, slotL = d_slotL_, slotR = d_slotR_;
        auto head = d_head_, zface = d_zface_, qv = d_q_, xi = d_xi_, conv = d_conveyance_;
        auto ze_lo = d_ze_lo_, ze_hi = d_ze_hi_;
        const bool vfr_face = (opts_->face_reconstruction == FaceDepth2D::VFR_FACE);
        const double dry = opts_->dry_depth;
        const double fr_max = opts_->froude_max;
        // DIFFUSIVE_WAVE and FULL_SWE publish their last face mass flux, already
        // limited and scaled by the conveyance (== serial).
        const bool diffusive = diffusive_ || swe_;
        Kokkos::parallel_for(
            "publish_flux", Kokkos::RangePolicy<ExecSpace>(0, ne), KOKKOS_LAMBDA(int e) {
                const double hf =
                    vfr_face
                        ? inertial::faceFlowDepthVfr(head(cLv(e)), head(cRv(e)), ze_lo(e), ze_hi(e))
                        : inertial::faceFlowDepth(head(cLv(e)), head(cRv(e)), zface(e));
                double qp = 0.0;
                if (hf > dry)
                    qp = inertial::froudeCap(qv(e), hf, fr_max);
                const double F = diffusive ? qv(e) * xi(e) : qp * xi(e) * conv(e);
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
    if (swe_) {
        hostRefresh(qcx_host_, d_qcx_);
        hostRefresh(qcy_host_, d_qcy_);
    }
    if (!exch_host_.empty())
        hostRefresh(exch_host_, d_exch_);
    exch_span_ = span; // == ExplicitInertialSolver::exch_tau_ at publication

    // Publish plain arrays across the plugin boundary; the engine owns and
    // updates the receipt map. Keep totals additive until the router consumes
    // them, just like the applied cell sources below.
    hostRefresh(bc_in_host_, d_bc_in_);
    hostRefresh(bc_out_host_, d_bc_out_);
    hostRefresh(bc_requested_host_, d_bc_requested_);
    hostRefresh(bc_last_flow_host_, d_bc_last_flow_);
    hostRefresh(bc_last_dt_host_, d_bc_last_dt_);
    for (std::size_t k = 0; k < bc_slot_host_.size(); ++k) {
        state_->boundary_in_applied[k] += bc_in_host_[k];
        state_->boundary_out_applied[k] += bc_out_host_[k];
        state_->boundary_requested[k] += bc_requested_host_[k];
        state_->boundary_last_flow[k] = bc_last_flow_host_[k];
        state_->boundary_last_dt[k] = bc_last_dt_host_[k];
    }
    Kokkos::deep_copy(d_bc_in_, 0.0);
    Kokkos::deep_copy(d_bc_out_, 0.0);
    Kokkos::deep_copy(d_bc_requested_, 0.0);

    // Fixed cell-order host fold avoids a floating-point device sum and drains
    // only once per publication, including exact macro-cycle landings.
    hostRefresh(evap_applied_host_, d_evap_applied_);
    for (double v : evap_applied_host_)
        state_->evap_loss_total += v;
    Kokkos::deep_copy(d_evap_applied_, 0.0);

    // Drain the applied-infiltration accumulator ADDITIVELY: the host array is
    // consumed and zeroed by the mass balance on the routing-step cadence,
    // which need not line up with this publish.
    if (state_->infil_applied.size() == infil_applied_host_.size()) {
        hostRefresh(infil_applied_host_, d_infil_applied_);
        for (std::size_t i = 0; i < infil_applied_host_.size(); ++i)
            state_->infil_applied[i] += infil_applied_host_[i];
        Kokkos::deep_copy(d_infil_applied_, 0.0);
        hostRefresh(coupling_applied_host_, d_coupling_applied_);
        for (std::size_t i = 0; i < coupling_applied_host_.size(); ++i)
            state_->coupling_applied[i] += coupling_applied_host_[i];
        Kokkos::deep_copy(d_coupling_applied_, 0.0);
    }
}

double ExplicitKokkosSurfaceSolver::advance(double t_current, double t_target) {
    kperf::Scope performance_scope(perf_counters_);
    if (!initialized_ || t_target <= t_current)
        return t_target;
    kperf::count_advance();

    double t = t_current;
    if (t_last_sync_ > t_current)
        t_last_sync_ = t_current;
    // Tools regions (perf review Phase A3): group the advance for external
    // profilers (nsys via the nvtx connector, kernel-logger). No-ops when no
    // tool is loaded.
    Kokkos::Profiling::pushRegion("openswmm_2d_advance_push");
    refreshConveyance();
    pushForcings();
    pushNodeState();
    if (swe_ && !bc_slot_host_.empty()) {
        // Boundary slots are not interior faces: their conveyance is read per advance.
        bc_conv_host_.resize(bc_slot_host_.size());
        for (std::size_t k = 0; k < bc_slot_host_.size(); ++k)
            bc_conv_host_[k] = mesh_->edge_conveyance[static_cast<std::size_t>(bc_slot_host_[k])];
        if (d_bc_conv_.extent(0) != bc_conv_host_.size())
            d_bc_conv_ = DView("bc_conv", bc_conv_host_.size());
        devRefresh(d_bc_conv_, bc_conv_host_);
    }
    Kokkos::deep_copy(d_bc_accum_, 0.0);
    Kokkos::deep_copy(d_exch_, 0.0);
    Kokkos::deep_copy(d_node_drawn_, 0.0);
    last_steps_ = 0;
    int cycles_since_rebuild = cycles_since_rebuild_;
    Kokkos::Profiling::popRegion();

    Kokkos::Profiling::pushRegion("openswmm_2d_advance_march");
    while (t < t_target) {
        if (cycles_since_rebuild >= kRebuildEveryCycles ||
            (front_breach_ && cycles_since_rebuild > 0)) {
            syncAndRebuild(t);
            cycles_since_rebuild = 0;
        } else {
            refreshDt0();
        }
        if (physics_) {
            const double dt = runMomentumStep(std::min(dt0_, t_target - t));
            if (!(dt > 0) || t + dt == t)
                throw std::runtime_error("Optional momentum physics cannot advance the clock.");
            t = dt == t_target - t ? t_target : t + dt;
            last_dt_ = dt;
            ++cycles_since_rebuild;
            continue;
        }
        const int nsub_full = 1 << (K_ - 1);
        const double remaining = t_target - t;

        if (n_active_ == 0) {
            t = t_target;
            last_dt_ = remaining;
            cycles_since_rebuild = kRebuildEveryCycles;
            break;
        }

        if (nsub_full * dt0_ <= remaining) {
            if (second_order_)
                runRk2Step(dt0_); // K == 1: one global step
            else
                runMacroCycle(dt0_, nsub_full);
            t += nsub_full * dt0_;
            last_dt_ = dt0_;
            ++cycles_since_rebuild;
            continue;
        }

        // Tail: split the WHOLE remaining span into nt EQUAL global
        // substeps (each in (dt0_/2, dt0_] for nt >= 2 — CFL-safe, never
        // degenerate) and land exactly. Fire all compact segments with the
        // global positivity budget, retaining the frozen tier assignments. Mirrors the
        // CPU marcher's tail (ExplicitInertialSolver::advance).
        if (remaining <= dt0_ * 1.0e-9) {
            // Sub-ulp residue of the macro landing: no physics.
            t = t_target;
            break;
        }
        settleAccumulatorsDev();
        const int nt = std::max(1, static_cast<int>(std::ceil(remaining / dt0_)));
        const double dt_tail = remaining / nt;
        for (int step = 0; step < nt; ++step) {
            if (second_order_) {
                runRk2Step(dt_tail);
                continue;
            }
            fireFaces(0, dt_tail, true);
            fireCells(0, dt_tail, true);
            ++substeps_run_;
            ++last_steps_;
        }
        last_dt_ = dt_tail;
        ++cycles_since_rebuild; // preserve the CPU rebuild cadence
        t = t_target;           // exact landing — no 1-ulp re-entry
        break;
    }

    cycles_since_rebuild_ = cycles_since_rebuild;
    settleAccumulatorsDev();
    if (t_target > t_last_sync_) {
        if (cycles_since_rebuild_ >= kRebuildEveryCycles) {
            syncAndRebuild(t_target);
            cycles_since_rebuild_ = 0;
        } else {
            lazySourcesDev(t_target);
        }
    }
    Kokkos::Profiling::popRegion();

    Kokkos::Profiling::pushRegion("openswmm_2d_advance_publish");
    publishAndCopyBack(t_current, t_target);
    Kokkos::Profiling::popRegion();
    return t_target;
}

// ===========================================================================
// reinitialize / resync / finalize / stats
// ===========================================================================

void ExplicitKokkosSurfaceSolver::reinitialize(double /*t0*/) {
    kperf::Scope performance_scope(perf_counters_);
    if (!initialized_)
        return;
    // External state edit: volumes authoritative; momentum + pending stale.
    devRefresh(d_volume_, state_->volume);
    Kokkos::deep_copy(d_q_, 0.0);
    Kokkos::deep_copy(d_bc_q_, 0.0);
    Kokkos::deep_copy(d_faccL_, 0.0);
    Kokkos::deep_copy(d_faccR_, 0.0);
    if (swe_) {
        Kokkos::deep_copy(d_fmxL_, 0.0);
        Kokkos::deep_copy(d_fmxR_, 0.0);
        Kokkos::deep_copy(d_fmyL_, 0.0);
        Kokkos::deep_copy(d_fmyR_, 0.0);
        Kokkos::deep_copy(d_qcx_, 0.0);
        Kokkos::deep_copy(d_qcy_, 0.0);
        std::fill(qcx_host_.begin(), qcx_host_.end(), 0.0);
        std::fill(qcy_host_.begin(), qcy_host_.end(), 0.0);
    }
    reconstructAllDev();
    hostRefresh(state_->head, d_head_);
    hostRefresh(state_->depth, d_depth_);
    cycles_since_rebuild_ = 1000;
}

bool ExplicitKokkosSurfaceSolver::publishMomentumVelocity() {
    if (!initialized_ || !swe_)
        return false;
    const double dry = opts_->dry_depth;
    const int nt = mesh_->n_triangles();
    for (int i = 0; i < nt; ++i) {
        const double h = state_->depth[i];
        state_->face_vx[i] = h > dry ? -qcx_host_[i] / h : 0;
        state_->face_vy[i] = h > dry ? -qcy_host_[i] / h : 0;
    }
    return true;
}

void ExplicitKokkosSurfaceSolver::resyncFromVolumes(double t0) {
    syncPinnedVolumes(t0);
    // An arbitrary volume edit can wet an inactive cell: rebuild the active
    // set and tiers at the next advance, as the CPU marcher does.
    if (initialized_)
        cycles_since_rebuild_ = 1000;
}

void ExplicitKokkosSurfaceSolver::syncPinnedVolumes(double /*t0*/) {
    kperf::Scope performance_scope(perf_counters_);
    if (!initialized_)
        return;
    devRefresh(d_volume_, state_->volume);
    reconstructAllDev();
    hostRefresh(state_->head, d_head_);
    hostRefresh(state_->depth, d_depth_);
    // No rebuild: the edited cells are pinned, so they are always active on
    // tier zero. Rebuilding after every transfer changed the dt0 schedule.
}

void ExplicitKokkosSurfaceSolver::pinExternalSourceCells(const std::vector<int>& cells) {
    kperf::Scope performance_scope(perf_counters_);
    if (!initialized_)
        return;
    const int nt = mesh_->n_triangles();
    for (const int c : cells)
        if (c < 0 || c >= nt)
            throw std::out_of_range("pinExternalSourceCells: cell outside the mesh");
    auto pin = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace(), d_pin_t0_);
    for (const int c : cells)
        pin(c) = 1;
    Kokkos::deep_copy(d_pin_t0_, pin);
    cycles_since_rebuild_ = 1000;
}

void ExplicitKokkosSurfaceSolver::finalize() {
    kperf::Scope performance_scope(perf_counters_);
    if (!initialized_)
        return;
    kperf::dump();
    // The same end-of-run diagnostics as the serial marcher.
    if (n_wall_drag_ > 0)
        std::fprintf(
            stderr, "[openswmm 2D] quadratic wall drag: %d faces, energy removed/rho=%.17g m5/s2\n",
            n_wall_drag_, wall_drag_energy_);
    if (opts_ && opts_->turbulence != Turbulence2D::NONE)
        std::fprintf(stderr,
                     "[openswmm 2D] %s mixing: %llu substeps, %llu retries, "
                     "last diffusion bound %.17g s.\n",
                     turbulenceName(opts_->turbulence), ph_substeps_, ph_retries_, ph_last_limit_);
    // fireBc and fireExchange stay serial inside a run or group, so their
    // device latency follows the longest one rather than the mesh size.
    if (perf::enabled())
        std::fprintf(stderr,
                     "[PERF-2D-KOKKOS-SERIAL] bc_edges=%zu bc_runs=%d bc_run_max=%d "
                     "exchange_points=%zu exchange_groups=%d exchange_group_max=%d\n",
                     bc_cell_host_.size(), n_bc_runs_, bc_run_max_, exch_host_.size(), n_cp_groups_,
                     cp_group_max_);
    if (!telemetry_path_.empty() && !telemetry_.empty()) {
        if (std::FILE* f = openswmm::io::fopen_utf8(telemetry_path_, "w")) {
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

ISurfaceSolver::RunStats ExplicitKokkosSurfaceSolver::run_stats() const noexcept {
    RunStats s;
    s.nsteps = substeps_run_;
    s.nrhs = face_passes_;
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
        s.active_frac_min = mn;
        s.active_frac_max = mx;
        s.active_frac_mean = sum / static_cast<double>(telemetry_.size());
    }
    s.n_tiers = K_;
    for (int k = 0; k < s.n_tiers; ++k)
        s.tier_cells[k] = tier_occupancy_[static_cast<std::size_t>(k)];
    return s;
}

} // namespace openswmm::twoD::gpu
