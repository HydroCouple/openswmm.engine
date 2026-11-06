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
#include "../../data/NodeData.hpp"

namespace openswmm::twoD::gpu {

namespace {

/// Active-set / tier rebuild cadence in macro cycles (== serial marcher).
constexpr int kRebuildEveryCycles = 4;

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
    // Direct plugin users must receive the same restriction as the factory.
    if (opts.momentum != Momentum2D::LOCAL_INERTIAL || state.transport.active())
        throw std::invalid_argument("Kokkos marcher requires local-inertial water-only flow");
    for (int i = 0; i < mesh.n_cells(); ++i)
        if (mesh.cell_vertex_count(i) != 3)
            throw std::invalid_argument("Kokkos marcher currently requires triangles");
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
    // Padded [cell*kMaxCellVerts + k] connectivity (MeshData). This plugin
    // is gated to ALL-TRIANGLE meshes by SurfaceSolverFactory (R6), so the
    // kernels below read slots 0..2 only.
    d_cell_v_ = devCopy("cell_v", mesh.cell_v);
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
    have_perot_ = (opts.theta < 1.0 || opts.advection);
    d_qcx_ = DView("qcx", have_perot_ ? nt : 0);
    d_qcy_ = DView("qcy", have_perot_ ? nt : 0);
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

    // Seed face momentum from the optional [2D_INITIAL_VELOCITY] rows
    // (== serial marcher; host-side build, one refresh). Mean depth is
    // V/A under both closures, so it is derived directly from the seeded
    // volumes. t = 0 only — reinitialize() still zeroes face momentum.
    {
        bool any_uv = false;
        for (int i = 0; i < nt && !any_uv; ++i)
            any_uv = mesh.tri_init_u[i] != 0.0 || mesh.tri_init_v[i] != 0.0;
        if (any_uv) {
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
    auto cvv = d_cell_v_;
    const bool vfr = (opts_->cell_closure == CellClosure2D::VFR);
    const double mwf = opts_->vfr_min_wet_frac;
    Kokkos::parallel_for(
        "reconstructAll", Kokkos::RangePolicy<ExecSpace>(0, nt), KOKKOS_LAMBDA(int i) {
            double e, d;
            inertial::etaDepthScalar(area(i), cz(i), vz(cvv(i * kMaxCellVerts + 0)),
                                     vz(cvv(i * kMaxCellVerts + 1)), vz(cvv(i * kMaxCellVerts + 2)),
                                     vfr, mwf, vol(i), e, d);
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
            if (pending == 0.0)
                return;
            double v = vol(i) + pending;
            vol(i) = (v > 0.0) ? v : 0.0;
            double e2, d2;
            inertial::etaDepthScalar(area(i), cz(i), vz(cvv(i * kMaxCellVerts + 0)),
                                     vz(cvv(i * kMaxCellVerts + 1)), vz(cvv(i * kMaxCellVerts + 2)),
                                     vfr, mwf, vol(i), e2, d2);
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
            inertial::etaDepthScalar(area(i), cz(i), vz(cvv(i * kMaxCellVerts + 0)),
                                     vz(cvv(i * kMaxCellVerts + 1)), vz(cvv(i * kMaxCellVerts + 2)),
                                     vfr, mwf, vol(i), e2, d2);
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
    Kokkos::parallel_for(
        "seed", Kokkos::RangePolicy<ExecSpace>(0, nt), KOKKOS_LAMBDA(int i) {
            const double thresh = active(i) ? h_off : h_on;
            seed(i) = (depth(i) >= thresh || coup(i) != 0.0 || pin(i)) ? 1 : 0;
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
            if (!active(i)) {
                dtcell(i) = 1.0e30;
                return;
            }
            const double h = depth(i);
            double speed = 0.0;
            if (perot && h > 1.0e-6)
                speed = inertial::qMagnitude(qcx(i), qcy(i)) / h;
            double dt = (h > dry) ? inertial::cellCflDt(alpha, lchar(i), h, speed) : 1.0e30;
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
                    int lg = (int)std::log2(ratio);
                    tk = (lg < K - 1) ? lg : (K - 1);
                }
                if (coup(i) != 0.0 || pin(i))
                    tk = 0;
            }
            tier(i) = tk;
        });

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
    auto active = d_active_;
    auto tier = d_tier_;
    auto depth = d_depth_;
    auto lchar = d_lchar_;
    auto qcx = d_qcx_, qcy = d_qcy_;
    const bool perot = have_perot_;
    const double alpha = opts_->cfl_number, dry = opts_->dry_depth;
    double fresh = 1.0e30;
    Kokkos::parallel_reduce(
        "cfl_refresh", Kokkos::RangePolicy<ExecSpace>(0, nt),
        KOKKOS_LAMBDA(int i, double& mn) {
            if (!active(i))
                return;
            const double h = depth(i);
            if (h <= dry)
                return;
            double speed = 0.0;
            if (perot && h > 1.0e-6)
                speed = inertial::qMagnitude(qcx(i), qcy(i)) / h;
            const double dt = inertial::cellCflDt(alpha, lchar(i), h, speed) / (1 << tier(i));
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
    Kokkos::parallel_for(
        "refresh_cell_discharge", Kokkos::RangePolicy<ExecSpace>(0, mesh_->n_cells()),
        KOKKOS_LAMBDA(int i) {
            double sx = 0.0, sy = 0.0;
            for (int p = ptr(i); p < ptr(i + 1); ++p) {
                const int e = edge(p);
                const double f = sign(p) * q(e) * xi(e) * conv(e);
                sx += f * (mx(e) - cx(i));
                sy += f * (my(e) - cy(i));
            }
            qcx(i) = sx / area(i);
            qcy(i) = sy / area(i);
        });
    auto cells = d_bc_cell_, slots = d_bc_slot_, vertices = d_cell_v_;
    auto bc_q = d_bc_q_, length = d_edge_length_, vx = d_vx_, vy = d_vy_;
    const int nbc = static_cast<int>(bc_cell_host_.size());
    // Serial perimeter pass: several boundary slots can belong to the same cell.
    Kokkos::parallel_for(
        "refresh_boundary_discharge", Kokkos::RangePolicy<ExecSpace>(0, 1), KOKKOS_LAMBDA(int) {
            for (int k = 0; k < nbc; ++k) {
                const int i = cells(k), slot = slots(k), edge = slot % kMaxCellVerts;
                const int va = vertices(i * kMaxCellVerts + (edge + 1) % 3);
                const int vb = vertices(i * kMaxCellVerts + (edge + 2) % 3);
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
    const double blend =
        inertial::momentumBlendFraction(theta, dt_f, opts_->theta_reference_step);
    const double dry = opts_->dry_depth;
    const double fr_max = opts_->froude_max;
    const double beta_share = opts_->exchange_beta / 3.0;
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
    const double dry = opts_->dry_depth;
    if (lo < hi) {
        Kokkos::parallel_for(
            "fireCells", Kokkos::RangePolicy<ExecSpace>(lo, hi), KOKKOS_LAMBDA(int idx) {
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
                const double landed = vol(i) + flux_m3;
                const auto budget = sourceBudget(landed > 0.0 ? landed : 0.0, depth(i), area(i),
                                                 dt_c, dry, rain(i), coup(i), infil(i), evap(i));
                infil_app(i) += budget.infiltration / area(i);
                coup_app(i) += budget.coupling;
                evap_app(i) += budget.evaporation;
                vol(i) = budget.volume;
                double e2, d2;
                inertial::etaDepthScalar(area(i), cz(i), vz(cvv(i * kMaxCellVerts + 0)),
                                         vz(cvv(i * kMaxCellVerts + 1)),
                                         vz(cvv(i * kMaxCellVerts + 2)), vfr, mwf, vol(i), e2, d2);
                head(i) = e2;
                depth(i) = d2;
                if (perot) {
                    double sx = 0.0, sy = 0.0;
                    for (int p = ptr(i); p < ptr(i + 1); ++p) {
                        const int e = edge(p);
                        const double f = sign(p) * qv(e) * xi(e) * conv(e);
                        sx += f * (mx(e) - cx(i));
                        sy += f * (my(e) - cy(i));
                    }
                    const double inv_a = 1.0 / area(i);
                    qcx(i) = sx * inv_a;
                    qcy(i) = sy * inv_a;
                }
            });
    }
    if (k != 0)
        return;

    // Boundary edges (tier-0 cadence; single-thread device kernel preserves
    // the serial per-cell ordering for multi-edge cells).
    const int nbc = static_cast<int>(bc_cell_host_.size());
    if (nbc > 0) {
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
        const double fr_max = opts_->froude_max;
        const int bt_wall = static_cast<int>(BoundaryType::WALL);
        const int bt_normal = static_cast<int>(BoundaryType::NORMAL_FLOW);
        const int bt_flow = static_cast<int>(BoundaryType::SPECIFIED_FLOW);
        const int bt_rating = static_cast<int>(BoundaryType::RATING_CURVE);
        const int bt_stage = static_cast<int>(BoundaryType::SPECIFIED_STAGE);
        Kokkos::parallel_for(
            "fireBc", Kokkos::RangePolicy<ExecSpace>(0, 1), KOKKOS_LAMBDA(int) {
                for (int kk = 0; kk < nbc; ++kk) {
                    const int i = bc_cell(kk);
                    if (tier(i) != 0 || !active(i))
                        continue;
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
                            const int e = idx % kMaxCellVerts;
                            const int vv[3] = {cvv(i * kMaxCellVerts + 0),
                                               cvv(i * kMaxCellVerts + 1),
                                               cvv(i * kMaxCellVerts + 2)};
                            const double za = vz(vv[(e + 1) % 3]);
                            const double zb = vz(vv[(e + 2) % 3]);
                            h_out = inertial::faceDepthFromEta(head(i), (za < zb) ? za : zb,
                                                               (za < zb) ? zb : za);
                        }
                        // Signed slope, as the two CPU marchers: positive
                        // drains the domain, negative feeds it.
                        if (S != 0.0 && h_out > 0.0 && n > 0.0) {
                            const double sgnS = (S > 0.0) ? 1.0 : -1.0;
                            const double h53 = h_out * std::cbrt(h_out * h_out);
                            f = -sgnS * (h53 * std::sqrt(S < 0.0 ? -S : S) / n) * L;
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
                            const int e = idx % kMaxCellVerts;
                            const int vv[3] = {cvv(i * kMaxCellVerts + 0),
                                               cvv(i * kMaxCellVerts + 1),
                                               cvv(i * kMaxCellVerts + 2)};
                            const double za = vz(vv[(e + 1) % 3]);
                            const double zb = vz(vv[(e + 2) % 3]);
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
                        const double slope = deta * (3.0 * L) / (2.0 * area(i));
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
                    if (ty == bt_stage) {
                        // Equilibrium clamp, kept as the tiny-cell /
                        // overshoot backstop: one substep moves the cell AT
                        // MOST to the prescribed stage.
                        const double v_eq = inertial::volumeFromEtaScalar(
                            area(i), cz(i), vz(cvv(i * kMaxCellVerts + 0)),
                            vz(cvv(i * kMaxCellVerts + 1)), vz(cvv(i * kMaxCellVerts + 2)), vfr,
                            mwf, bc_head(kk));
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
                        inertial::etaDepthScalar(area(i), cz(i), vz(cvv(i * kMaxCellVerts + 0)),
                                                 vz(cvv(i * kMaxCellVerts + 1)),
                                                 vz(cvv(i * kMaxCellVerts + 2)), vfr, mwf, vol(i),
                                                 e2, d2);
                        head(i) = e2;
                        depth(i) = d2;
                    }
                    // Perot completion: add the boundary edge's contribution
                    // the interior-only rebuild missed (== serial marcher).
                    if (perot && bc_q(kk) != 0.0) {
                        const int e = idx % kMaxCellVerts;
                        const int vv[3] = {cvv(i * kMaxCellVerts + 0), cvv(i * kMaxCellVerts + 1),
                                           cvv(i * kMaxCellVerts + 2)};
                        const int va = vv[(e + 1) % 3];
                        const int vb = vv[(e + 2) % 3];
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

    // Live junction exchange at tier-0 cadence — single-thread device kernel
    // preserves the serial point ordering (shared-cell drains and shared-node
    // spill budgets are order-dependent by contract).
    const int np = static_cast<int>(exch_host_.size());
    if (np > 0 && state_->nodes_1d) {
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
            "fireExchange", Kokkos::RangePolicy<ExecSpace>(0, 1), KOKKOS_LAMBDA(int) {
                for (int kk = 0; kk < np; ++kk) {
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
                    inertial::etaDepthScalar(area(ci), cz(ci), vz(cvv(ci * kMaxCellVerts + 0)),
                                             vz(cvv(ci * kMaxCellVerts + 1)),
                                             vz(cvv(ci * kMaxCellVerts + 2)), vfr, mwf, vol(ci), e2,
                                             d2);
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
        Kokkos::parallel_for(
            "publish_flux", Kokkos::RangePolicy<ExecSpace>(0, ne), KOKKOS_LAMBDA(int e) {
                const double hf =
                    vfr_face
                        ? inertial::faceFlowDepthVfr(head(cLv(e)), head(cRv(e)), ze_lo(e), ze_hi(e))
                        : inertial::faceFlowDepth(head(cLv(e)), head(cRv(e)), zface(e));
                double qp = 0.0;
                if (hf > dry)
                    qp = inertial::froudeCap(qv(e), hf, fr_max);
                const double F = qp * xi(e) * conv(e);
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
    if (!exch_host_.empty())
        hostRefresh(exch_host_, d_exch_);

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
    Kokkos::deep_copy(d_bc_accum_, 0.0);
    Kokkos::deep_copy(d_exch_, 0.0);
    Kokkos::deep_copy(d_node_drawn_, 0.0);
    last_steps_ = 0;
    int cycles_since_rebuild = cycles_since_rebuild_;
    Kokkos::Profiling::popRegion();

    Kokkos::Profiling::pushRegion("openswmm_2d_advance_march");
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
            cycles_since_rebuild = kRebuildEveryCycles;
            break;
        }

        if (nsub_full * dt0_ <= remaining) {
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
    reconstructAllDev();
    cycles_since_rebuild_ = 1000;
}

void ExplicitKokkosSurfaceSolver::resyncFromVolumes(double /*t0*/) {
    kperf::Scope performance_scope(perf_counters_);
    if (!initialized_)
        return;
    devRefresh(d_volume_, state_->volume);
    reconstructAllDev();
    // External outfall transfers can activate previously dry cells.
    cycles_since_rebuild_ = 1000;
}

void ExplicitKokkosSurfaceSolver::finalize() {
    kperf::Scope performance_scope(perf_counters_);
    if (!initialized_)
        return;
    kperf::dump();
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
