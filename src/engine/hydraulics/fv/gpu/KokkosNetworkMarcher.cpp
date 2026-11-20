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
 * @file      KokkosNetworkMarcher.cpp
 * @brief     Kokkos substep loop of the 1D finite volume network solver.
 *
 * @details One advance() is: copy the routing step's state and forcing to the
 *          device, march global substeps there, copy the state and ledgers
 *          back. The loop is ExplicitFvSolver::advanceImpl's global path with
 *          the census interval at 1, and the per-element work is
 *          FvSubstepKernels.hpp compiled with OPENSWMM_KERNEL_FN =
 *          KOKKOS_INLINE_FUNCTION.
 *
 *          The step size is read back to the host once per accepted substep
 *          (the post-step census, reused as the next pre-step census: same
 *          function of the same state).
 *
 * @see ../FvSubstepKernels.hpp, ../FvDeviceMarcher.hpp
 * @ingroup engine_fv
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include <Kokkos_Core.hpp>

#define OPENSWMM_KERNEL_FN KOKKOS_INLINE_FUNCTION

#include "KokkosNetworkMarcher.hpp"

#include "../FvDeviceMarcher.hpp"
#include "../FvSubstepKernels.hpp"
#include "../../../2d/gpu/KokkosTypes.hpp"

#ifdef OPENSWMM_ENABLE_METAL_FACES
#include "../../../2d/gpu/metal/FaceClient.hpp"
#endif
#include <cfloat>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>

namespace openswmm::fv::gpu {

namespace {

using twoD::gpu::ExecSpace;
using twoD::gpu::MemSpace;

template <class T> using DevView = Kokkos::View<T*, MemSpace>;
template <class T>
using HostConst = Kokkos::View<const T*, Kokkos::HostSpace, Kokkos::MemoryUnmanaged>;
template <class T> using HostMut = Kokkos::View<T*, Kokkos::HostSpace, Kokkos::MemoryUnmanaged>;

using Range = Kokkos::RangePolicy<ExecSpace>;

// The substep loop's constants (ExplicitFvSolver.hpp).
constexpr double kStepAcceptRatio = 0.5;
constexpr int kMaxStepRetries = 8;
constexpr double kMinSubstep = 1.0e-6;

template <class T> DevView<T> alloc(const char* name, int n) {
    return DevView<T>(Kokkos::view_alloc(std::string(name), Kokkos::WithoutInitializing),
                      static_cast<std::size_t>(n > 0 ? n : 0));
}

template <class T> void copyIn(const DevView<T>& dev, const T* host) {
    if (dev.extent(0) == 0 || host == nullptr)
        return;
    Kokkos::deep_copy(dev, HostConst<T>(host, dev.extent(0)));
}

template <class T> void copyOut(T* host, const DevView<T>& dev) {
    if (dev.extent(0) == 0 || host == nullptr)
        return;
    Kokkos::deep_copy(HostMut<T>(host, dev.extent(0)), dev);
}

/// Allocate and fill from a host array.
template <class T> DevView<T> upload(const char* name, const T* host, int n) {
    DevView<T> v = alloc<T>(name, n);
    copyIn(v, host);
    return v;
}

template <class T> void fill(const DevView<T>& dev, T value) {
    if (dev.extent(0) == 0)
        return;
    Kokkos::deep_copy(dev, value);
}

#if defined(OPENSWMM_GPU_EXECSPACE_CUDA)
constexpr const char* kLabel = "cuda";
#elif defined(OPENSWMM_GPU_EXECSPACE_HIP)
constexpr const char* kLabel = "hip";
#elif defined(OPENSWMM_GPU_EXECSPACE_SYCL)
constexpr const char* kLabel = "sycl";
#else
constexpr const char* kLabel = "omp";
#endif

/// Phases of one advance, for the OPENSWMM_PERF report.
enum Phase : int {
    kPush = 0,
    kFlux,
    kNodeSolve,
    kPositivity,
    kCells,
    kNodes,
    kCensus,
    kSave,
    kRestore,
    kPull,
    kPhaseCount
};
constexpr const char* kPhaseName[kPhaseCount] = {"push",    "flux",  "node_solve", "positivity",
                                                 "cells",   "nodes", "census",     "save",
                                                 "restore", "pull"};

/**
 * @brief Work counts and, on request, per-phase wall time of the marcher.
 *
 * Presence-only switches, like the surface plugin's: OPENSWMM_PERF prints one
 * `[PERF-FV1D]` block when the marcher is destroyed; OPENSWMM_PERF_FENCE also
 * waits for the device after each phase, so the phase times are device times
 * rather than launch times. Fencing changes the run time, never the result.
 */
struct Perf {
    bool on = std::getenv("OPENSWMM_PERF") != nullptr;
    bool fence = std::getenv("OPENSWMM_PERF_FENCE") != nullptr;
    long advances = 0, substeps = 0, attempts = 0, rejected = 0, launches = 0, reductions = 0;
    double seconds[kPhaseCount] = {};
    double total = 0.0;

    using Clock = std::chrono::steady_clock;
    Clock::time_point start() const { return on ? Clock::now() : Clock::time_point(); }
    void stop(Phase p, Clock::time_point t0) {
        if (!on)
            return;
        if (fence)
            Kokkos::fence();
        seconds[p] += std::chrono::duration<double>(Clock::now() - t0).count();
    }
};

} // namespace

/**
 * @brief The marcher. A named class outside any unnamed namespace, with every
 *        kernel-launching method public: CUDA refuses a device lambda whose
 *        enclosing function is private, protected or unnamed.
 */
class KokkosNetworkMarcher final : public IFvDeviceMarcher {
public:
    explicit KokkosNetworkMarcher(bool metal) {
#ifdef OPENSWMM_ENABLE_METAL_FACES
        if (metal)
            network_ = std::make_unique<twoD::metal::NetworkClient>();
#else
        (void)metal;
#endif
    }
    ~KokkosNetworkMarcher() override { report(); }
    void bind(const FvDeviceMesh& mesh) override;
    FvDeviceResult advance(const FvDeviceAdvance& step) override;
    const char* label() const noexcept override {
#ifdef OPENSWMM_ENABLE_METAL_FACES
        if (network_)
            return "metal";
#endif
        return kLabel;
    }

    // Public only for the reason above; not part of the interface.
    double census();
    void takeSubstep(double dt);
#ifdef OPENSWMM_ENABLE_METAL_FACES
    void metalFaceFluxes();
#endif
    void save();
    void restore();

private:
    void pushStep(const substep::Arrays& h);
    void pullStep(const substep::Arrays& h);
    void report() const;

#ifdef OPENSWMM_ENABLE_METAL_FACES
    std::unique_ptr<twoD::metal::NetworkClient> network_;
    std::vector<int> interior_faces_, host_faces_;
#endif
    Perf perf_;
    substep::Arrays d_{}; ///< every pointer is a device view's data()
    int n_conduits_ = 0;
    std::string error_;

    // ---- mesh -------------------------------------------------------------
    DevView<int> cell_geom_, cell_conduit_, cell_face0_, cell_face1_;
    DevView<std::int8_t> cell_side0_, cell_side1_;
    DevView<double> cell_dx_, cell_zb_;
    DevView<int> face_cl_, face_cr_, face_node_, face_geom_, face_culvert_, face_npos_;
    DevView<std::uint8_t> face_gate_;
    DevView<double> face_zb_, face_a_rim_;
    DevView<std::int8_t> face_dir_l_, face_dir_r_;
    DevView<int> conduit_section_;
    DevView<double> conduit_roughness_, conduit_rough_factor_, conduit_loss_inlet_,
        conduit_loss_outlet_, conduit_slope_;
    DevView<hydkernels::CulvertCurve> conduit_culvert_curve_;
    DevView<std::uint8_t> conduit_culvert_mitered_;
    DevView<int> node_face_ptr_, node_face_idx_, node_face_sorted_, node_struct_ptr_,
        node_struct_idx_, node_vol_off_, node_vj_face_;
    DevView<double> node_face_sign_, node_invert_, node_full_depth_, node_ponded_area_,
        node_sur_depth_, node_surf_area_, node_vfull_, node_vol_dmax_, node_vol_atop_,
        node_vol_tbl_;
    DevView<std::uint8_t> node_can_pond_, node_kind_, node_alg_, node_pass_static_,
        node_dummy_drain_;
    DevView<int> struct_n1_, struct_n2_;
    DevView<std::uint8_t> struct_is_dummy_;
    DevView<FvGeometry> geom_;

    // ---- forcing ------------------------------------------------------------
    DevView<double> node_lateral_, node_fixed_head_, node_source_max_, node_withdrawal_, struct_q_,
        struct_q_cap_, conduit_loss_, cell_qlat_;
    DevView<std::uint8_t> node_pass_, node_lat_div_;

    // ---- state --------------------------------------------------------------
    DevView<double> cell_a_, cell_q_, cell_h_, cell_eta_, cell_u_, cell_ah_, cell_t_, cell_i1_,
        cell_q_int_;
    DevView<double> node_volume_, node_head_, node_carry_, node_exch_, node_in_, node_out_,
        flood_vol_, unmet_, node_qstruct_, node_qdummy_;
    DevView<double> f_mass_, f_mom_, f_corr_l_, f_corr_r_, relax_beta_, face_lat_spill_;
    DevView<kernels::FaceState> f_state_l_, f_state_r_;
    DevView<double> struct_applied_vol_, dummy_vol_;
    DevView<std::uint8_t> inlet_control_;

    // ---- scratch and the step-rejection snapshot ------------------------------
    DevView<double> out_cell_, out_node_, node_struct_scale_, dummy_q_, settle_carry_, absorb_;
    DevView<std::uint8_t> dep_;
    DevView<double> sv_cell_a_, sv_cell_q_, sv_cell_q_int_;
    DevView<double> sv_node_volume_, sv_node_head_, sv_node_carry_, sv_node_exch_, sv_node_in_,
        sv_node_out_, sv_flood_vol_, sv_unmet_;
    DevView<double> sv_struct_applied_vol_, sv_dummy_vol_;
};

void KokkosNetworkMarcher::bind(const FvDeviceMesh& mesh) {
    const substep::Arrays& h = mesh.host;
    const int nc = h.n_cells, nf = h.n_faces, nn = h.n_nodes, ns = h.n_structs;
    n_conduits_ = mesh.n_conduits;
#ifdef OPENSWMM_ENABLE_METAL_FACES
    if (network_) {
        interior_faces_.clear();
        host_faces_.clear();
        for (int f = 0; f < nf; ++f) {
            if (h.face_cl[f] >= 0 && h.face_cr[f] >= 0 && h.face_node[f] < 0 &&
                h.face_gate[f] == 0 && h.face_culvert[f] < 0)
                interior_faces_.push_back(f);
            else
                host_faces_.push_back(f);
        }
        if (interior_faces_.empty())
            throw std::runtime_error("Metal FV network requires an interior conduit face");
        network_->inputs.resize(interior_faces_.size());
    }
#endif

    cell_geom_ = upload("fv_cell_geom", h.cell_geom, nc);
    cell_conduit_ = upload("fv_cell_conduit", h.cell_conduit, nc);
    cell_face0_ = upload("fv_cell_face0", h.cell_face0, nc);
    cell_face1_ = upload("fv_cell_face1", h.cell_face1, nc);
    cell_side0_ = upload("fv_cell_side0", h.cell_side0, nc);
    cell_side1_ = upload("fv_cell_side1", h.cell_side1, nc);
    cell_dx_ = upload("fv_cell_dx", h.cell_dx, nc);
    cell_zb_ = upload("fv_cell_zb", h.cell_zb, nc);

    face_cl_ = upload("fv_face_cl", h.face_cl, nf);
    face_cr_ = upload("fv_face_cr", h.face_cr, nf);
    face_node_ = upload("fv_face_node", h.face_node, nf);
    face_geom_ = upload("fv_face_geom", h.face_geom, nf);
    face_culvert_ = upload("fv_face_culvert", h.face_culvert, nf);
    face_npos_ = upload("fv_face_npos", h.face_npos, nf);
    face_gate_ = upload("fv_face_gate", h.face_gate, nf);
    face_zb_ = upload("fv_face_zb", h.face_zb, nf);
    face_a_rim_ = upload("fv_face_a_rim", h.face_a_rim, nf);
    face_dir_l_ = upload("fv_face_dir_l", h.face_dir_l, nf);
    face_dir_r_ = upload("fv_face_dir_r", h.face_dir_r, nf);

    conduit_section_ = upload("fv_conduit_section", h.conduit_section, n_conduits_);
    conduit_roughness_ = upload("fv_conduit_roughness", h.conduit_roughness, n_conduits_);
    conduit_rough_factor_ = upload("fv_conduit_rough_factor", h.conduit_rough_factor, n_conduits_);
    conduit_loss_inlet_ = upload("fv_conduit_loss_inlet", h.conduit_loss_inlet, n_conduits_);
    conduit_loss_outlet_ = upload("fv_conduit_loss_outlet", h.conduit_loss_outlet, n_conduits_);
    conduit_slope_ = upload("fv_conduit_slope", h.conduit_slope, n_conduits_);
    conduit_culvert_curve_ =
        upload("fv_conduit_culvert_curve", h.conduit_culvert_curve, n_conduits_);
    conduit_culvert_mitered_ =
        upload("fv_conduit_culvert_mitered", h.conduit_culvert_mitered, n_conduits_);

    node_face_ptr_ = upload("fv_node_face_ptr", h.node_face_ptr, nn + 1);
    node_face_idx_ = upload("fv_node_face_idx", h.node_face_idx, mesh.n_node_faces);
    node_face_sorted_ = upload("fv_node_face_sorted", h.node_face_sorted, mesh.n_node_faces);
    node_face_sign_ = upload("fv_node_face_sign", h.node_face_sign, mesh.n_node_faces);
    node_struct_ptr_ = upload("fv_node_struct_ptr", h.node_struct_ptr, nn + 1);
    node_struct_idx_ = upload("fv_node_struct_idx", h.node_struct_idx, mesh.n_node_structs);
    node_invert_ = upload("fv_node_invert", h.node_invert, nn);
    node_full_depth_ = upload("fv_node_full_depth", h.node_full_depth, nn);
    node_ponded_area_ = upload("fv_node_ponded_area", h.node_ponded_area, nn);
    node_sur_depth_ = upload("fv_node_sur_depth", h.node_sur_depth, nn);
    node_surf_area_ = alloc<double>("fv_node_surf_area", nn);
    node_vfull_ = upload("fv_node_vfull", h.node_vfull, nn);
    node_vol_dmax_ = upload("fv_node_vol_dmax", h.node_vol_dmax, nn);
    node_vol_atop_ = upload("fv_node_vol_atop", h.node_vol_atop, nn);
    node_vol_tbl_ = upload("fv_node_vol_tbl", h.node_vol_tbl, mesh.n_vol_tbl);
    node_vol_off_ = upload("fv_node_vol_off", h.node_vol_off, nn);
    node_vj_face_ = upload("fv_node_vj_face", h.node_vj_face, nn);
    node_can_pond_ = upload("fv_node_can_pond", h.node_can_pond, nn);
    node_kind_ = upload("fv_node_kind", h.node_kind, nn);
    node_alg_ = upload("fv_node_alg", h.node_alg, nn);
    node_pass_static_ = upload("fv_node_pass_static", h.node_pass_static, nn);
    node_dummy_drain_ = alloc<std::uint8_t>("fv_node_dummy_drain", nn);
    if (h.node_dummy_drain)
        copyIn(node_dummy_drain_, h.node_dummy_drain);
    else
        fill(node_dummy_drain_, std::uint8_t{0});

    struct_n1_ = upload("fv_struct_n1", h.struct_n1, ns);
    struct_n2_ = upload("fv_struct_n2", h.struct_n2, ns);
    struct_is_dummy_ = alloc<std::uint8_t>("fv_struct_is_dummy", ns);
    if (h.struct_is_dummy)
        copyIn(struct_is_dummy_, h.struct_is_dummy);
    else
        fill(struct_is_dummy_, std::uint8_t{0});

    geom_ = upload("fv_geom", h.geom, mesh.n_geom);

    node_lateral_ = alloc<double>("fv_node_lateral", nn);
    node_fixed_head_ = alloc<double>("fv_node_fixed_head", nn);
    node_source_max_ = alloc<double>("fv_node_source_max", nn);
    node_withdrawal_ = alloc<double>("fv_node_withdrawal", nn);
    struct_q_ = alloc<double>("fv_struct_q", ns);
    struct_q_cap_ = alloc<double>("fv_struct_q_cap", ns);
    conduit_loss_ = alloc<double>("fv_conduit_loss", n_conduits_);
    cell_qlat_ = alloc<double>("fv_cell_qlat", nc);
    node_pass_ = alloc<std::uint8_t>("fv_node_pass", nn);
    node_lat_div_ = alloc<std::uint8_t>("fv_node_lat_div", nn);

    cell_a_ = alloc<double>("fv_cell_a", nc);
    cell_q_ = alloc<double>("fv_cell_q", nc);
    cell_h_ = alloc<double>("fv_cell_h", nc);
    cell_eta_ = alloc<double>("fv_cell_eta", nc);
    cell_u_ = alloc<double>("fv_cell_u", nc);
    cell_ah_ = alloc<double>("fv_cell_ah", nc);
    cell_t_ = alloc<double>("fv_cell_t", nc);
    cell_i1_ = alloc<double>("fv_cell_i1", nc);
    cell_q_int_ = alloc<double>("fv_cell_q_int", nc);
    node_volume_ = alloc<double>("fv_node_volume", nn);
    node_head_ = alloc<double>("fv_node_head", nn);
    node_carry_ = alloc<double>("fv_node_carry", nn);
    node_exch_ = alloc<double>("fv_node_exch", nn);
    node_in_ = alloc<double>("fv_node_in", nn);
    node_out_ = alloc<double>("fv_node_out", nn);
    flood_vol_ = alloc<double>("fv_flood_vol", nn);
    unmet_ = alloc<double>("fv_unmet", nn);
    node_qstruct_ = alloc<double>("fv_node_qstruct", nn);
    node_qdummy_ = alloc<double>("fv_node_qdummy", nn);
    f_mass_ = alloc<double>("fv_f_mass", nf);
    f_mom_ = alloc<double>("fv_f_mom", nf);
    f_corr_l_ = alloc<double>("fv_f_corr_l", nf);
    f_corr_r_ = alloc<double>("fv_f_corr_r", nf);
    relax_beta_ = alloc<double>("fv_relax_beta", nf);
    face_lat_spill_ = alloc<double>("fv_face_lat_spill", nf);
    f_state_l_ = alloc<kernels::FaceState>("fv_f_state_l", nf);
    f_state_r_ = alloc<kernels::FaceState>("fv_f_state_r", nf);
    struct_applied_vol_ = alloc<double>("fv_struct_applied_vol", ns);
    dummy_vol_ = alloc<double>("fv_dummy_vol", ns);
    inlet_control_ = alloc<std::uint8_t>("fv_inlet_control", n_conduits_);

    out_cell_ = alloc<double>("fv_out_cell", nc);
    out_node_ = alloc<double>("fv_out_node", nn);
    node_struct_scale_ = alloc<double>("fv_node_struct_scale", nn);
    dummy_q_ = alloc<double>("fv_dummy_q", ns);
    settle_carry_ = alloc<double>("fv_settle_carry", nn);
    absorb_ = alloc<double>("fv_absorb", nf);
    dep_ = alloc<std::uint8_t>("fv_dep", nn);
    fill(relax_beta_, 0.0);
    fill(absorb_, 0.0);
    fill(settle_carry_, 0.0);
    fill(dep_, std::uint8_t{0});
    fill(node_qdummy_, 0.0);

    sv_cell_a_ = alloc<double>("fv_sv_cell_a", nc);
    sv_cell_q_ = alloc<double>("fv_sv_cell_q", nc);
    sv_cell_q_int_ = alloc<double>("fv_sv_cell_q_int", nc);
    sv_node_volume_ = alloc<double>("fv_sv_node_volume", nn);
    sv_node_head_ = alloc<double>("fv_sv_node_head", nn);
    sv_node_carry_ = alloc<double>("fv_sv_node_carry", nn);
    sv_node_exch_ = alloc<double>("fv_sv_node_exch", nn);
    sv_node_in_ = alloc<double>("fv_sv_node_in", nn);
    sv_node_out_ = alloc<double>("fv_sv_node_out", nn);
    sv_flood_vol_ = alloc<double>("fv_sv_flood_vol", nn);
    sv_unmet_ = alloc<double>("fv_sv_unmet", nn);
    sv_struct_applied_vol_ = alloc<double>("fv_sv_struct_applied_vol", ns);
    sv_dummy_vol_ = alloc<double>("fv_sv_dummy_vol", ns);

    // The device's own view of the arrays: sizes, options and static flags
    // from the host description, every pointer re-aimed at a device view.
    d_ = h;
    d_.cell_geom = cell_geom_.data();
    d_.cell_conduit = cell_conduit_.data();
    d_.cell_face0 = cell_face0_.data();
    d_.cell_face1 = cell_face1_.data();
    d_.cell_side0 = cell_side0_.data();
    d_.cell_side1 = cell_side1_.data();
    d_.cell_dx = cell_dx_.data();
    d_.cell_zb = cell_zb_.data();
    d_.face_cl = face_cl_.data();
    d_.face_cr = face_cr_.data();
    d_.face_node = face_node_.data();
    d_.face_geom = face_geom_.data();
    d_.face_culvert = face_culvert_.data();
    d_.face_npos = face_npos_.data();
    d_.face_gate = face_gate_.data();
    d_.face_zb = face_zb_.data();
    d_.face_dir_l = face_dir_l_.data();
    d_.face_dir_r = face_dir_r_.data();
    d_.face_a_rim = face_a_rim_.data();
    d_.conduit_section = conduit_section_.data();
    d_.conduit_roughness = conduit_roughness_.data();
    d_.conduit_rough_factor = conduit_rough_factor_.data();
    d_.conduit_loss_inlet = conduit_loss_inlet_.data();
    d_.conduit_loss_outlet = conduit_loss_outlet_.data();
    d_.conduit_slope = conduit_slope_.data();
    d_.conduit_culvert_curve = conduit_culvert_curve_.data();
    d_.conduit_culvert_mitered = conduit_culvert_mitered_.data();
    d_.node_face_ptr = node_face_ptr_.data();
    d_.node_face_idx = node_face_idx_.data();
    d_.node_face_sign = node_face_sign_.data();
    d_.node_face_sorted = node_face_sorted_.data();
    d_.node_struct_ptr = node_struct_ptr_.data();
    d_.node_struct_idx = node_struct_idx_.data();
    d_.node_invert = node_invert_.data();
    d_.node_full_depth = node_full_depth_.data();
    d_.node_ponded_area = node_ponded_area_.data();
    d_.node_sur_depth = node_sur_depth_.data();
    d_.node_surf_area = node_surf_area_.data();
    d_.node_vfull = node_vfull_.data();
    d_.node_vol_dmax = node_vol_dmax_.data();
    d_.node_vol_atop = node_vol_atop_.data();
    d_.node_vol_tbl = node_vol_tbl_.data();
    d_.node_vol_off = node_vol_off_.data();
    d_.node_vj_face = node_vj_face_.data();
    d_.node_can_pond = node_can_pond_.data();
    d_.node_kind = node_kind_.data();
    d_.node_alg = node_alg_.data();
    d_.node_pass_static = node_pass_static_.data();
    d_.node_dummy_drain = node_dummy_drain_.data();
    d_.struct_n1 = struct_n1_.data();
    d_.struct_n2 = struct_n2_.data();
    d_.struct_is_dummy = struct_is_dummy_.data();
    d_.geom = geom_.data();

    d_.node_lateral = node_lateral_.data();
    d_.node_fixed_head = node_fixed_head_.data();
    d_.node_source_max = node_source_max_.data();
    d_.node_withdrawal = node_withdrawal_.data();
    d_.struct_q = struct_q_.data();
    d_.struct_q_cap = struct_q_cap_.data();
    d_.conduit_loss = conduit_loss_.data();
    d_.cell_qlat = cell_qlat_.data();
    d_.node_pass = node_pass_.data();
    d_.node_lat_div = node_lat_div_.data();

    d_.cell_a = cell_a_.data();
    d_.cell_q = cell_q_.data();
    d_.cell_h = cell_h_.data();
    d_.cell_eta = cell_eta_.data();
    d_.cell_u = cell_u_.data();
    d_.cell_ah = cell_ah_.data();
    d_.cell_t = cell_t_.data();
    d_.cell_i1 = cell_i1_.data();
    d_.cell_q_int = cell_q_int_.data();
    d_.node_volume = node_volume_.data();
    d_.node_head = node_head_.data();
    d_.node_carry = node_carry_.data();
    d_.node_exch = node_exch_.data();
    d_.node_in = node_in_.data();
    d_.node_out = node_out_.data();
    d_.flood_vol = flood_vol_.data();
    d_.unmet = unmet_.data();
    d_.node_qstruct = node_qstruct_.data();
    d_.node_qdummy = node_qdummy_.data();
    d_.f_mass = f_mass_.data();
    d_.f_mom = f_mom_.data();
    d_.f_corr_l = f_corr_l_.data();
    d_.f_corr_r = f_corr_r_.data();
    d_.relax_beta = relax_beta_.data();
    d_.face_lat_spill = face_lat_spill_.data();
    d_.f_state_l = f_state_l_.data();
    d_.f_state_r = f_state_r_.data();
    d_.struct_applied_vol = struct_applied_vol_.data();
    d_.dummy_vol = dummy_vol_.data();
    d_.inlet_control = inlet_control_.data();
    d_.out_cell = out_cell_.data();
    d_.out_node = out_node_.data();
    d_.node_struct_scale = node_struct_scale_.data();
    d_.dummy_q = dummy_q_.data();
    d_.settle_carry = settle_carry_.data();
    d_.absorb = absorb_.data();
    d_.dep = dep_.data();
}

void KokkosNetworkMarcher::pushStep(const substep::Arrays& h) {
    // Forcing. An absent array keeps the value that reproduces the CPU
    // solver's null-pointer branch; the has_* flags carry the distinction
    // wherever the two could differ.
    d_.has_lateral = h.has_lateral;
    d_.has_source_max = h.has_source_max;
    d_.has_structure_flow = h.has_structure_flow;
    d_.has_q_cap = h.has_q_cap;
    d_.has_conduit_loss = h.has_conduit_loss;
    d_.has_withdrawal = h.has_withdrawal;
    if (h.has_lateral)
        copyIn(node_lateral_, h.node_lateral);
    else
        fill(node_lateral_, 0.0);
    copyIn(node_fixed_head_, h.node_fixed_head);
    if (h.has_source_max)
        copyIn(node_source_max_, h.node_source_max);
    if (h.has_withdrawal)
        copyIn(node_withdrawal_, h.node_withdrawal);
    if (h.has_structure_flow)
        copyIn(struct_q_, h.struct_q);
    if (h.has_q_cap)
        copyIn(struct_q_cap_, h.struct_q_cap);
    if (h.has_conduit_loss)
        copyIn(conduit_loss_, h.conduit_loss);
    copyIn(cell_qlat_, h.cell_qlat);
    copyIn(node_pass_, h.node_pass);
    copyIn(node_lat_div_, h.node_lat_div);
    copyIn(node_surf_area_, h.node_surf_area);

    // State and ledgers.
    copyIn(cell_a_, h.cell_a);
    copyIn(cell_q_, h.cell_q);
    copyIn(cell_h_, h.cell_h);
    copyIn(cell_eta_, h.cell_eta);
    copyIn(cell_u_, h.cell_u);
    copyIn(cell_ah_, h.cell_ah);
    copyIn(cell_t_, h.cell_t);
    copyIn(cell_i1_, h.cell_i1);
    copyIn(cell_q_int_, h.cell_q_int);
    copyIn(node_volume_, h.node_volume);
    copyIn(node_head_, h.node_head);
    copyIn(node_carry_, h.node_carry);
    copyIn(node_exch_, h.node_exch);
    copyIn(node_in_, h.node_in);
    copyIn(node_out_, h.node_out);
    copyIn(flood_vol_, h.flood_vol);
    copyIn(unmet_, h.unmet);
    copyIn(node_qstruct_, h.node_qstruct);
    if (d_.have_dummy)
        copyIn(node_qdummy_, h.node_qdummy);
    copyIn(face_lat_spill_, h.face_lat_spill);
    copyIn(struct_applied_vol_, h.struct_applied_vol);
    copyIn(dummy_vol_, h.dummy_vol);
    copyIn(inlet_control_, h.inlet_control);
}

void KokkosNetworkMarcher::pullStep(const substep::Arrays& h) {
    copyOut(h.cell_a, cell_a_);
    copyOut(h.cell_q, cell_q_);
    copyOut(h.cell_h, cell_h_);
    copyOut(h.cell_eta, cell_eta_);
    copyOut(h.cell_u, cell_u_);
    copyOut(h.cell_ah, cell_ah_);
    copyOut(h.cell_t, cell_t_);
    copyOut(h.cell_i1, cell_i1_);
    copyOut(h.cell_q_int, cell_q_int_);
    copyOut(h.node_volume, node_volume_);
    copyOut(h.node_head, node_head_);
    copyOut(h.node_carry, node_carry_);
    copyOut(h.node_exch, node_exch_);
    copyOut(h.node_in, node_in_);
    copyOut(h.node_out, node_out_);
    copyOut(h.flood_vol, flood_vol_);
    copyOut(h.unmet, unmet_);
    copyOut(h.node_qstruct, node_qstruct_);
    if (d_.have_dummy)
        copyOut(h.node_qdummy, node_qdummy_);
    copyOut(h.f_mass, f_mass_);
    copyOut(h.f_mom, f_mom_);
    copyOut(h.f_corr_l, f_corr_l_);
    copyOut(h.f_corr_r, f_corr_r_);
    copyOut(h.f_state_l, f_state_l_);
    copyOut(h.f_state_r, f_state_r_);
    copyOut(h.relax_beta, relax_beta_);
    copyOut(h.face_lat_spill, face_lat_spill_);
    copyOut(h.struct_applied_vol, struct_applied_vol_);
    copyOut(h.dummy_vol, dummy_vol_);
    copyOut(h.inlet_control, inlet_control_);
}

double KokkosNetworkMarcher::census() {
    const substep::Arrays D = d_;
    const int nf = D.n_faces;
    double dt = DBL_MAX;
    const auto t0 = perf_.start();
    ++perf_.reductions;
    Kokkos::parallel_reduce(
        "fv1d_census", Range(0, nf + D.n_nodes),
        KOKKOS_LAMBDA(const int i, double& m) {
            const double v = (i < nf) ? substep::censusFace(D, i) : substep::censusNode(D, i - nf);
            if (v < m)
                m = v;
        },
        Kokkos::Min<double>(dt));
    perf_.stop(kCensus, t0);
    if (dt >= DBL_MAX * 0.5)
        dt = 1.0e30;
    return dt;
}

#ifdef OPENSWMM_ENABLE_METAL_FACES
// The OpenMP host plugin owns the FP64 mesh, boundary conditions, node solves
// and conservation ledgers. Batch only ordinary interior Riemann problems.
void KokkosNetworkMarcher::metalFaceFluxes() {
    const auto D = d_;
    const auto* interior = interior_faces_.data();
    const auto* boundaries = host_faces_.data();
    auto* inputs = network_->inputs.data();
    Kokkos::parallel_for(
        "fv_metal_boundary_faces", Range(0, host_faces_.size()),
        KOKKOS_LAMBDA(int i) { substep::faceFlux(D, boundaries[i], false); });
    Kokkos::parallel_for(
        "fv_metal_pack", Range(0, interior_faces_.size()), KOKKOS_LAMBDA(int i) {
            const int f = interior[i], cl = D.face_cl[f], cr = D.face_cr[f];
            const double z = std::max(D.cell_zb[cl], D.cell_zb[cr]);
            kernels::FaceState left, right;
            double il, ir, bed;
            substep::faceSide(D, f, cl, -1, z, D.face_dir_l[f], 0., left, il, bed, false);
            substep::faceSide(D, f, cr, -1, z, D.face_dir_r[f], 0., right, ir, bed, false);
            D.f_state_l[f] = left;
            D.f_state_r[f] = right;
            D.f_corr_l[f] = kernels::kGravity * (il - left.i1);
            D.f_corr_r[f] = kernels::kGravity * (ir - right.i1);
            auto pack = [](const kernels::FaceState& v) -> twoD::metal::NetworkSide {
                return {float(v.a), float(v.q), float(v.u), float(v.c), float(v.i1), v.press};
            };
            inputs[i] = {pack(left), pack(right)};
        });
    ExecSpace().fence();
    network_->run({});
    const auto* result = network_->outputs.data();
    Kokkos::parallel_for(
        "fv_metal_unpack", Range(0, interior_faces_.size()), KOKKOS_LAMBDA(int i) {
            const int f = interior[i];
            D.f_mass[f] = result[i].mass;
            D.f_mom[f] = result[i].momentum;
        });
    ExecSpace().fence();
}
#endif

void KokkosNetworkMarcher::takeSubstep(double dt) {
    const substep::Arrays D = d_;
    const int nc = D.n_cells, nf = D.n_faces, nn = D.n_nodes, ns = D.n_structs;

    auto t0 = perf_.start();
#ifdef OPENSWMM_ENABLE_METAL_FACES
    if (network_)
        metalFaceFluxes();
    else
#endif
        Kokkos::parallel_for(
            "fv1d_flux", Range(0, nf),
            KOKKOS_LAMBDA(const int f) { substep::faceFlux(D, f, false); });
    perf_.stop(kFlux, t0);
    t0 = perf_.start();
    Kokkos::parallel_for(
        "fv1d_node_solve", Range(0, nn),
        KOKKOS_LAMBDA(const int n) { substep::solveNode(D, n, dt); });
    perf_.stop(kNodeSolve, t0);
    perf_.launches += 2;

    t0 = perf_.start();
    perf_.launches += D.has_source_max ? 4 : 3;
    if (D.has_source_max)
        Kokkos::parallel_for(
            "fv1d_pos_cap", Range(0, nf),
            KOKKOS_LAMBDA(const int f) { substep::positivityCapFace(D, f); });
    Kokkos::parallel_for(
        "fv1d_pos_cell", Range(0, nc),
        KOKKOS_LAMBDA(const int c) { substep::positivityCell(D, c, dt); });
    Kokkos::parallel_for(
        "fv1d_pos_node", Range(0, nn),
        KOKKOS_LAMBDA(const int n) { substep::positivityNode(D, n, dt); });
    Kokkos::parallel_for(
        "fv1d_pos_scale", Range(0, nf),
        KOKKOS_LAMBDA(const int f) { substep::positivityScaleFace(D, f); });
    perf_.stop(kPositivity, t0);

    t0 = perf_.start();
    Kokkos::parallel_for(
        "fv1d_cells", Range(0, nc), KOKKOS_LAMBDA(const int c) { substep::updateCell(D, c, dt); });
    perf_.stop(kCells, t0);
    ++perf_.launches;

    t0 = perf_.start();
    perf_.launches += 3;
    if (D.has_structure_flow && ns > 0 && dt > 0.0) {
        perf_.launches += 3;
        Kokkos::parallel_for(
            "fv1d_struct_scale", Range(0, nn),
            KOKKOS_LAMBDA(const int n) { substep::structureScaleNode(D, n, dt); });
        Kokkos::parallel_for(
            "fv1d_struct_flow", Range(0, nn),
            KOKKOS_LAMBDA(const int n) { substep::structureFlowNode(D, n); });
        Kokkos::parallel_for(
            "fv1d_struct_applied", Range(0, ns),
            KOKKOS_LAMBDA(const int s) { substep::structureApplied(D, s, dt); });
    }
    const bool dummies = D.have_dummy && dt > 0.0;
    if (dummies)
        ++perf_.launches;
    if (dummies)
        Kokkos::parallel_for(
            "fv1d_dummy_flow", Range(0, ns),
            KOKKOS_LAMBDA(const int s) { substep::dummyFlow(D, s, dt); });
    Kokkos::parallel_for(
        "fv1d_nodes", Range(0, nn), KOKKOS_LAMBDA(const int n) {
            if (dummies)
                substep::dummyFlowNode(D, n);
            substep::lateralSpillNode(D, n);
            substep::nodeUpdate(D, n, dt);
        });
    Kokkos::parallel_for(
        "fv1d_deposit_cell", Range(0, nc),
        KOKKOS_LAMBDA(const int c) { substep::depositCell(D, c); });
    Kokkos::parallel_for(
        "fv1d_deposit_node", Range(0, nn),
        KOKKOS_LAMBDA(const int n) { substep::depositNode(D, n); });
    perf_.stop(kNodes, t0);
}

void KokkosNetworkMarcher::report() const {
    if (!perf_.on || perf_.advances == 0)
        return;
    std::fprintf(stderr,
                 "[PERF-FV1D] backend=%s cells=%d faces=%d nodes=%d structs=%d advances=%ld "
                 "substeps=%ld attempts=%ld rejected=%ld launches=%ld reductions=%ld "
                 "total=%.6f s fenced=%d\n",
                 kLabel, d_.n_cells, d_.n_faces, d_.n_nodes, d_.n_structs, perf_.advances,
                 perf_.substeps, perf_.attempts, perf_.rejected, perf_.launches, perf_.reductions,
                 perf_.total, perf_.fence ? 1 : 0);
    for (int p = 0; p < kPhaseCount; ++p)
        std::fprintf(stderr, "[PERF-FV1D]   %-11s %.6f s\n", kPhaseName[p], perf_.seconds[p]);
}

void KokkosNetworkMarcher::save() {
    const auto t0 = perf_.start();
    perf_.launches += (d_.n_structs > 0) ? 3 : 2;
    const auto ca = cell_a_, cq = cell_q_, ci = cell_q_int_;
    const auto sa = sv_cell_a_, sq = sv_cell_q_, si = sv_cell_q_int_;
    Kokkos::parallel_for(
        "fv1d_save_cells", Range(0, d_.n_cells), KOKKOS_LAMBDA(const int c) {
            sa(c) = ca(c);
            sq(c) = cq(c);
            si(c) = ci(c);
        });
    const auto nv = node_volume_, nh = node_head_, ncar = node_carry_, nx = node_exch_,
               ni = node_in_, no = node_out_, fl = flood_vol_, um = unmet_;
    const auto snv = sv_node_volume_, snh = sv_node_head_, sncar = sv_node_carry_,
               snx = sv_node_exch_, sni = sv_node_in_, sno = sv_node_out_, sfl = sv_flood_vol_,
               sum = sv_unmet_;
    Kokkos::parallel_for(
        "fv1d_save_nodes", Range(0, d_.n_nodes), KOKKOS_LAMBDA(const int n) {
            snv(n) = nv(n);
            snh(n) = nh(n);
            sncar(n) = ncar(n);
            snx(n) = nx(n);
            sni(n) = ni(n);
            sno(n) = no(n);
            sfl(n) = fl(n);
            sum(n) = um(n);
        });
    if (d_.n_structs > 0) {
        const auto ap = struct_applied_vol_, dv = dummy_vol_;
        const auto sap = sv_struct_applied_vol_, sdv = sv_dummy_vol_;
        Kokkos::parallel_for(
            "fv1d_save_structs", Range(0, d_.n_structs), KOKKOS_LAMBDA(const int s) {
                sap(s) = ap(s);
                sdv(s) = dv(s);
            });
    }
    perf_.stop(kSave, t0);
}

void KokkosNetworkMarcher::restore() {
    const auto t0 = perf_.start();
    perf_.launches += (d_.n_structs > 0) ? 4 : 3;
    const substep::Arrays D = d_;
    const auto ca = cell_a_, cq = cell_q_, ci = cell_q_int_;
    const auto sa = sv_cell_a_, sq = sv_cell_q_, si = sv_cell_q_int_;
    // Cell depth, surface, velocity and the closure cache are derived, so
    // they are rebuilt from the restored area (ExplicitFvSolver::restoreState).
    Kokkos::parallel_for(
        "fv1d_restore_cells", Range(0, d_.n_cells), KOKKOS_LAMBDA(const int c) {
            ca(c) = sa(c);
            cq(c) = sq(c);
            ci(c) = si(c);
            substep::refreshDepthCell(D, c);
        });
    const auto nv = node_volume_, nh = node_head_, ncar = node_carry_, nx = node_exch_,
               ni = node_in_, no = node_out_, fl = flood_vol_, um = unmet_;
    const auto snv = sv_node_volume_, snh = sv_node_head_, sncar = sv_node_carry_,
               snx = sv_node_exch_, sni = sv_node_in_, sno = sv_node_out_, sfl = sv_flood_vol_,
               sum = sv_unmet_;
    Kokkos::parallel_for(
        "fv1d_restore_nodes", Range(0, d_.n_nodes), KOKKOS_LAMBDA(const int n) {
            nv(n) = snv(n);
            nh(n) = snh(n);
            ncar(n) = sncar(n);
            nx(n) = snx(n);
            ni(n) = sni(n);
            no(n) = sno(n);
            fl(n) = sfl(n);
            um(n) = sum(n);
        });
    if (d_.n_structs > 0) {
        const auto ap = struct_applied_vol_, dv = dummy_vol_;
        const auto sap = sv_struct_applied_vol_, sdv = sv_dummy_vol_;
        Kokkos::parallel_for(
            "fv1d_restore_structs", Range(0, d_.n_structs), KOKKOS_LAMBDA(const int s) {
                ap(s) = sap(s);
                dv(s) = sdv(s);
            });
    }
    fill(face_lat_spill_, 0.0);
    perf_.stop(kRestore, t0);
}

FvDeviceResult KokkosNetworkMarcher::advance(const FvDeviceAdvance& step) {
    FvDeviceResult r;
    const auto t_all = perf_.start();
    try {
        auto t0 = perf_.start();
        pushStep(step.host);
        perf_.stop(kPush, t0);

        const double t_target = step.t_target;
        double t = step.t_current;
        long steps = 0;
        bool have_census = false;
        double dt_census = 0.0;
        const long nf = d_.n_faces;

        while (t < t_target) {
            if (!have_census)
                dt_census = census();
            have_census = false;

            const double remaining = t_target - t;
            double dt = (dt_census < remaining) ? dt_census : remaining;
            if (!(dt > 0.0))
                dt = remaining;
            if (dt < kMinSubstep)
                dt = (kMinSubstep < remaining) ? kMinSubstep : remaining;

            for (int attempt = 0;; ++attempt) {
                save();
                takeSubstep(dt);
                ++perf_.attempts;
                r.flux_evals += nf;
                if (attempt >= kMaxStepRetries || dt <= kMinSubstep)
                    break;
                const double dt_post = census();
                if (dt_post >= kStepAcceptRatio * dt) {
                    // The accepted state is the one the next substep censuses.
                    dt_census = dt_post;
                    have_census = true;
                    break;
                }
                restore();
                ++perf_.rejected;
                dt = (0.9 * dt_post > kMinSubstep) ? 0.9 * dt_post : kMinSubstep;
                if (dt > remaining)
                    dt = remaining;
            }
            r.dt_cache = dt;

            t += dt;
            ++steps;
            r.last_h = dt;
            r.min_h = (r.min_h <= 0.0) ? dt : ((dt < r.min_h) ? dt : r.min_h);
            if (steps > 2000000L)
                break;
        }

        Kokkos::fence();
        t0 = perf_.start();
        pullStep(step.host);
        perf_.stop(kPull, t0);
        r.t_reached = t;
        r.steps = steps;
        ++perf_.advances;
        perf_.substeps += steps;
    } catch (const std::exception& e) {
        error_ = e.what();
        r.ok = false;
        r.error = error_.c_str();
    } catch (...) {
        error_ = "unknown failure in the device substep loop";
        r.ok = false;
        r.error = error_.c_str();
    }
    if (perf_.on)
        perf_.total += std::chrono::duration<double>(Perf::Clock::now() - t_all).count();
    return r;
}

IFvDeviceMarcher* makeKokkosNetworkMarcher(bool metal) {
    return new KokkosNetworkMarcher(metal);
}

} // namespace openswmm::fv::gpu
