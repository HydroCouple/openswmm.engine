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
 * @file      FvSubstepKernels.hpp
 * @brief     Per-element bodies of one global explicit FV substep, written
 *            against raw arrays so a device backend can run them.
 *
 * @details These are the bodies of ExplicitFvSolver's GLOBAL-step path
 *          (takeSubstep and the CFL census) for the configuration the device
 *          marcher supports: first order, forward Euler, slot closure, no
 *          species, no unsteady friction, no implicit pressurized pass, no
 *          accepted-water tracking, every cell and face active.
 *
 *          Each function keeps the floating-point expressions of the member
 *          function it mirrors, in the same order, so the two give the same
 *          bits. The mirrored function is named in each comment; a change to
 *          one must be made in the other, and tests/gpu/test_fv_device_marcher
 *          is what catches a miss.
 *
 *          The one deliberate difference is in the node update. The CPU loop
 *          visits nodes in index order and lets a pass-through junction write
 *          its window residual into the incident cells before the next node
 *          tests those cells for pressurization. Here that test reads the
 *          cells as they stood before any node wrote (nodeUpdate), the
 *          deposits are applied per cell in node order (depositCell), and each
 *          node then closes its ledger (depositNode). The CPU solver runs the
 *          same snapshot rule when OPENSWMM_FV_SETTLE_SNAPSHOT=1.
 *
 *          Every function writes only elements owned by the index it is called
 *          with (a face, a cell, a node, a structure), so each kernel may run
 *          over its range in any order or in parallel.
 *
 * @see ExplicitFvSolver.cpp, FvDeviceMarcher.hpp
 * @ingroup engine_fv
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_FV_SUBSTEP_KERNELS_HPP
#define OPENSWMM_ENGINE_FV_SUBSTEP_KERNELS_HPP

#include <cfloat>
#include <cmath>
#include <cstdint>
#include <limits>

#include "FvKernels.hpp"
#include "FvSubstepArrays.hpp"
#include "../HydClosureKernels.hpp"

namespace openswmm::fv::substep {

namespace k = kernels;

// The junction solve's constants (ExplicitFvSolver.cpp, anonymous namespace).
inline constexpr double kAlgQEps = 1.0e-12;
inline constexpr int kAlgMaxIters = 16;
inline constexpr double kAlgHeadTol = 1.0e-6;
inline constexpr int kAlgMaxExpand = 64;

/// std::isfinite, spelled so it is callable in device code on every compiler.
OPENSWMM_KERNEL_FN bool finite(double x) noexcept {
    return std::fabs(x) <= DBL_MAX;
}

/// The one cell a boundary face touches (or the left cell of an interior one).
OPENSWMM_KERNEL_FN int faceCell(const Arrays& A, int f) noexcept {
    return (A.face_cl[f] >= 0) ? A.face_cl[f] : A.face_cr[f];
}

/// ExplicitFvSolver::algebraicActive.
OPENSWMM_KERNEL_FN bool algebraicActive(const Arrays& A, int n) noexcept {
    if (!A.node_alg[n])
        return false;
    if (A.node_can_pond[n] && A.node_volume[n] > A.node_vfull[n])
        return false;
    return true;
}

/// ExplicitFvSolver::pondBase.
OPENSWMM_KERNEL_FN double pondBase(const Arrays& A, int n) noexcept {
    return (A.node_alg[n] && A.node_can_pond[n]) ? A.node_vfull[n] : 0.0;
}

/// ExplicitFvSolver::nodeLateral.
OPENSWMM_KERNEL_FN double nodeLateral(const Arrays& A, int n) noexcept {
    if (A.node_lat_div[n])
        return 0.0;
    return A.has_lateral ? A.node_lateral[n] : 0.0;
}

/// ExplicitFvSolver::nodeQDummy.
OPENSWMM_KERNEL_FN double nodeQDummy(const Arrays& A, int n) noexcept {
    return A.have_dummy ? A.node_qdummy[n] : 0.0;
}

OPENSWMM_KERNEL_FN bool fixedHead(const Arrays& A, int n) noexcept {
    return finite(A.node_fixed_head[n]);
}

// ===========================================================================
// Node storage relation (ExplicitFvSolver::nodeVolumeFromDepth and inverse)
// ===========================================================================

OPENSWMM_KERNEL_FN double nodeVolumeBelowRim(const Arrays& A, int n, double depth) noexcept {
    const int off = A.node_vol_off[n];
    if (off < 0) // junction: linear in depth
        return A.node_surf_area[n] * depth;

    const double dmax = A.node_vol_dmax[n];
    const int ns = kNodeVolSamples;
    const double vmax = A.node_vol_tbl[off + ns - 1];
    if (depth >= dmax)
        return vmax + A.node_vol_atop[n] * (depth - dmax);

    const double dd = dmax / static_cast<double>(ns - 1);
    int i = static_cast<int>(depth / dd);
    if (i > ns - 2)
        i = ns - 2;
    if (i < 0)
        i = 0;
    const double t = (depth - static_cast<double>(i) * dd) / dd;
    const double v0 = A.node_vol_tbl[off + i];
    const double v1 = A.node_vol_tbl[off + i + 1];
    return v0 + (v1 - v0) * t;
}

OPENSWMM_KERNEL_FN double nodeVolumeFromDepth(const Arrays& A, int n, double depth) noexcept {
    if (depth <= 0.0)
        return 0.0;
    const double full = A.node_full_depth[n];
    if (A.node_can_pond[n] && full > 0.0 && depth > full)
        return nodeVolumeBelowRim(A, n, full) + (depth - full) * A.node_ponded_area[n];
    return nodeVolumeBelowRim(A, n, depth);
}

OPENSWMM_KERNEL_FN double nodeDepthFromVolume(const Arrays& A, int n, double volume) noexcept {
    if (volume <= 0.0)
        return 0.0;
    const double full = A.node_full_depth[n];
    if (A.node_can_pond[n] && full > 0.0) {
        const double v_full = nodeVolumeFromDepth(A, n, full);
        if (volume > v_full)
            return full + (volume - v_full) / A.node_ponded_area[n];
    }
    const int off = A.node_vol_off[n];
    if (off < 0) {
        const double a = A.node_surf_area[n];
        return (a > 0.0) ? volume / a : 0.0;
    }
    const int ns = kNodeVolSamples;
    const double dmax = A.node_vol_dmax[n];
    const double dd = dmax / static_cast<double>(ns - 1);
    const double vmax = A.node_vol_tbl[off + ns - 1];
    if (volume >= vmax) {
        const double atop = A.node_vol_atop[n];
        if (atop > 0.0)
            return dmax + (volume - vmax) / atop;
        return volume == vmax ? dmax : full + A.node_sur_depth[n] + 1.0;
    }

    int lo = 0, hi = ns - 1;
    while (hi - lo > 1) {
        const int mid = (lo + hi) / 2;
        if (A.node_vol_tbl[off + mid] <= volume)
            lo = mid;
        else
            hi = mid;
    }
    const double v0 = A.node_vol_tbl[off + lo];
    const double v1 = A.node_vol_tbl[off + lo + 1];
    const double t = (v1 > v0) ? (volume - v0) / (v1 - v0) : 0.0;
    return (static_cast<double>(lo) + t) * dd;
}

// ===========================================================================
// Cell closure cache (ExplicitFvSolver::cacheClosure, refreshDepths)
// ===========================================================================

OPENSWMM_KERNEL_FN void cacheClosure(const Arrays& A, int c, const FvGeometry& g, double h,
                                     double* radius) noexcept {
    if (h > k::kDryDepth) {
        const k::ClosureEval e =
            radius ? k::closureEval(g.closure_tbl, h, *radius) : k::closureEval(g.closure_tbl, h);
        A.cell_ah[c] = e.a;
        A.cell_t[c] = e.t;
        A.cell_i1[c] = e.i1;
    } else {
        A.cell_ah[c] = 0.0;
        A.cell_t[c] = 0.0;
        A.cell_i1[c] = 0.0;
        if (radius)
            *radius = k::hydRadOfDepth(g, h);
    }
}

/// One cell of ExplicitFvSolver::refreshDepths.
OPENSWMM_KERNEL_FN void refreshDepthCell(const Arrays& A, int c) noexcept {
    const FvGeometry& g = A.geom[A.cell_geom[c]];
    double a = A.cell_a[c];
    if (a < 0.0)
        a = 0.0;
    const double h = k::depthOfArea(g, a);
    A.cell_a[c] = a;
    A.cell_h[c] = h;
    A.cell_eta[c] = A.cell_zb[c] + h;
    cacheClosure(A, c, g, h, nullptr);
    if (h <= k::kDryDepth) {
        A.cell_q[c] = 0.0;
        A.cell_u[c] = 0.0;
    } else {
        A.cell_u[c] = A.cell_q[c] / a;
    }
}

// ===========================================================================
// Face reconstruction and flux (ExplicitFvSolver::faceSide, computeFaceFlux)
// ===========================================================================

OPENSWMM_KERNEL_FN void faceSide(const Arrays& A, int face, int cell, int node, double zstar,
                                 int dir, double u_interior, k::FaceState& out, double& i1_raw,
                                 double& z_side, bool measure_only) noexcept {
    int gi = 0; // index of the side's own section
    double eta = 0.0, h_raw = 0.0, u = 0.0;
    z_side = 0.0;
    bool centred = false;
    int uc_cached = 0;
    bool node_head_ghost = false;

    // NetworkMeshData::faceSection.
    const int face_nd = A.face_node[face];
    const bool splicing = face_nd >= 0 && A.node_pass[face_nd] && algebraicActive(A, face_nd);
    const int c_of_face = faceCell(A, face);
    const int gfi =
        (splicing || face_nd < 0 || c_of_face < 0) ? A.face_geom[face] : A.cell_geom[c_of_face];
    const FvGeometry& gf = A.geom[gfi];

    if (cell >= 0) {
        gi = A.cell_geom[cell];
        eta = A.cell_eta[cell];
        h_raw = A.cell_h[cell];
        u = static_cast<double>(dir) * A.cell_u[cell];
        z_side = A.cell_zb[cell];
        centred = true;
        uc_cached = cell;
    } else {
        const int other = faceCell(A, face);
        gi = A.cell_geom[other];
        if (node >= 0) {
            eta = A.node_head[node];
            h_raw = std::max(0.0, eta - A.face_zb[face]);
            u = u_interior;
            z_side = A.face_zb[face];
            node_head_ghost = true;
            if (A.node_pass[node] && algebraicActive(A, node)) {
                const int nb2 = A.node_face_ptr[node];
                const int ne2 = A.node_face_ptr[node + 1];
                for (int p2 = nb2; p2 < ne2; ++p2) {
                    const int of = A.node_face_idx[p2];
                    if (of == face)
                        continue;
                    const int oc = faceCell(A, of);
                    if (oc >= 0) {
                        eta = A.cell_eta[oc];
                        h_raw = A.cell_h[oc];
                        z_side = A.cell_zb[oc];
                        node_head_ghost = false;
                    }
                }
            }
        } else {
            eta = A.cell_eta[other];
            h_raw = A.cell_h[other];
            u = -u_interior;
            z_side = A.face_zb[face];
        }
    }

    if (measure_only) {
        i1_raw = 0.0;
        out = k::FaceState{};
        return;
    }

    if (cell >= 0 && h_raw > k::kDryDepth) {
        i1_raw = A.cell_i1[uc_cached]; // first order: every cell side is centred
    } else {
        i1_raw = 0.0;
    }

    const double h_star = std::max(0.0, eta - zstar);
    if (h_star <= k::kDryDepth) {
        out = k::FaceState{};
        return;
    }
    if (centred && gfi == gi && h_star == h_raw) {
        out.a = A.cell_ah[uc_cached];
        out.u = u;
        out.q = out.a * u;
        out.c = k::celerity(out.a, A.cell_t[uc_cached]);
        out.i1 = A.cell_i1[uc_cached];
    } else {
        const k::ClosureEval e = k::closureEval(gf.closure_tbl, h_star);
        out.a = e.a;
        out.u = u;
        out.q = e.a * u;
        out.c = k::celerity(e.a, e.t);
        out.i1 = e.i1;
    }
    if (A.mixed_wave) {
        out.press =
            (A.mw_rh && !gf.is_open && h_star >= gf.y_full) ? std::uint8_t{1} : std::uint8_t{0};
        if (node_head_ghost && node >= 0 && A.node_sur_depth[node] <= 0.0)
            out.press = A.mw_rh ? std::uint8_t{2} : std::uint8_t{0};
    }
}

OPENSWMM_KERNEL_FN void faceFlux(const Arrays& A, int f, bool mass_only) noexcept {
    const int cl = A.face_cl[f];
    const int cr = A.face_cr[f];
    const int nd = A.face_node[f];

    k::FaceState probe;
    double zl = 0.0, zr = 0.0, i1probe = 0.0;
    if (cl >= 0 && cr >= 0) {
        zl = A.cell_zb[cl];
        zr = A.cell_zb[cr];
    } else {
        faceSide(A, f, cl, nd, 0.0, A.face_dir_l[f], 0.0, probe, i1probe, zl, true);
        faceSide(A, f, cr, nd, 0.0, A.face_dir_r[f], 0.0, probe, i1probe, zr, true);
    }
    const double zstar = std::max(zl, zr);

    const int interior = (cl >= 0) ? cl : cr;
    double u_int = (interior >= 0)
                       ? static_cast<double>((cl >= 0) ? A.face_dir_l[f] : A.face_dir_r[f]) *
                             A.cell_u[interior]
                       : 0.0;

    if (nd >= 0 && A.node_pass[nd] && algebraicActive(A, nd)) {
        const int nb2 = A.node_face_ptr[nd];
        const int ne2 = A.node_face_ptr[nd + 1];
        double s_this = 0.0, s_far = 0.0;
        int oc = -1;
        int uof = 0;
        for (int p2 = nb2; p2 < ne2; ++p2) {
            const int of = A.node_face_idx[p2];
            if (of == f) {
                s_this = A.node_face_sign[p2];
                continue;
            }
            uof = of;
            s_far = A.node_face_sign[p2];
            oc = faceCell(A, uof);
        }
        if (oc >= 0 && s_this != 0.0 && s_far != 0.0) {
            const double odir = (A.face_cl[uof] >= 0) ? static_cast<double>(A.face_dir_l[uof])
                                                      : static_cast<double>(A.face_dir_r[uof]);
            u_int = -s_this * s_far * odir * A.cell_u[oc];
        }
    }

    k::FaceState L, R;
    double i1l = 0.0, i1r = 0.0, zdummy = 0.0;
    faceSide(A, f, cl, nd, zstar, A.face_dir_l[f], u_int, L, i1l, zdummy, false);
    faceSide(A, f, cr, nd, zstar, A.face_dir_r[f], u_int, R, i1r, zdummy, false);

    if (mass_only) {
        double mass = k::riemannMassFlux(L, R);
        const std::uint8_t gate_m = A.face_gate[f];
        if (gate_m != 0 && ((mass > 0.0 && (gate_m & 1u)) || (mass < 0.0 && (gate_m & 2u))))
            mass = 0.0;
        const int cvr_m = A.face_culvert[f];
        if (cvr_m >= 0 && mass > 0.0 && nd >= 0) {
            const FvGeometry& gc = A.geom[A.conduit_section[cvr_m]];
            const double head = A.node_head[nd] - A.face_zb[f];
            double dqdh = 0.0;
            const double q_cap = hydkernels::culvertInflow(
                mass, head, gc.y_full, gc.xs.a_full, A.conduit_slope[cvr_m],
                A.conduit_culvert_curve[cvr_m], A.conduit_culvert_mitered[cvr_m] != 0, dqdh);
            if (q_cap < mass) {
                mass = q_cap;
                A.inlet_control[cvr_m] = 1;
            }
        }
        if (nd >= 0 && A.has_source_max) {
            const double cap = A.node_source_max[nd];
            if (cl < 0 && mass > cap)
                mass = cap;
            if (cr < 0 && mass < -cap)
                mass = -cap;
        }
        A.f_mass[f] = mass;
        return;
    }

    k::FaceFlux fl = k::riemannFlux(L, R);

    const std::uint8_t gate = A.face_gate[f];
    if (gate != 0 && ((fl.mass > 0.0 && (gate & 1u)) || (fl.mass < 0.0 && (gate & 2u)))) {
        if (cl < 0) {
            L = R;
            L.u = -R.u;
            L.q = -R.q;
        } else {
            R = L;
            R.u = -L.u;
            R.q = -L.q;
        }
        fl = k::riemannFlux(L, R);
        fl.mass = 0.0;
    }

    const int cvr = A.face_culvert[f];
    if (cvr >= 0 && fl.mass > 0.0 && nd >= 0) {
        const FvGeometry& gc = A.geom[A.conduit_section[cvr]];
        const double head = A.node_head[nd] - A.face_zb[f];
        double dqdh = 0.0;
        const double q_cap = hydkernels::culvertInflow(
            fl.mass, head, gc.y_full, gc.xs.a_full, A.conduit_slope[cvr],
            A.conduit_culvert_curve[cvr], A.conduit_culvert_mitered[cvr] != 0, dqdh);
        if (q_cap < fl.mass) {
            const k::FaceState& up = (cl < 0) ? L : R;
            const double a_up = (up.a > k::kDryArea) ? up.a : k::kDryArea;
            fl.mass = q_cap;
            fl.mom = q_cap * q_cap / a_up + k::kGravity * up.i1;
            A.inlet_control[cvr] = 1;
        }
    }

    if (nd >= 0 && A.has_source_max) {
        const double cap = A.node_source_max[nd];
        const double accepted = cl < 0 ? std::min(fl.mass, cap) : std::max(fl.mass, -cap);
        if (accepted != fl.mass) {
            const k::FaceState& up = cl < 0 ? L : R;
            fl.mass = accepted;
            fl.mom = accepted * accepted / std::max(up.a, k::kDryArea) + k::kGravity * up.i1;
        }
    }
    A.f_mass[f] = fl.mass;
    A.f_mom[f] = fl.mom;
    A.f_state_l[f] = L;
    A.f_state_r[f] = R;
    A.f_corr_l[f] = (cl >= 0) ? k::kGravity * (i1l - L.i1) : 0.0;
    A.f_corr_r[f] = (cr >= 0) ? k::kGravity * (i1r - R.i1) : 0.0;
}

// ===========================================================================
// Node coupling (relaxOneNode, solveAlgebraicNode)
// ===========================================================================

/// ExplicitFvSolver::relaxOneNode.
OPENSWMM_KERNEL_FN void relaxOneNode(const Arrays& A, int n, double dt) noexcept {
    if (A.node_kind[n] == kNodeVirtual)
        return;
    if (fixedHead(A, n))
        return;
    if (!(A.node_surf_area[n] > 0.0))
        return;
    if (A.node_dummy_drain && A.node_dummy_drain[n])
        return;

    const int b = A.node_face_ptr[n];
    const int e = A.node_face_ptr[n + 1];

    const double q_lat = nodeLateral(A, n) + A.node_qstruct[n] + nodeQDummy(A, n);
    const double h0 = A.node_head[n];

    double sum_f = 0.0, resist = 0.0;
    for (int p = b; p < e; ++p) {
        const int f = A.node_face_idx[p];
        sum_f += A.node_face_sign[p] * A.f_mass[f];
        A.relax_beta[f] = 0.0;

        const int cell = faceCell(A, f);
        if (cell < 0)
            continue;
        const FvGeometry& g = A.geom[A.cell_geom[cell]];
        const double hg = h0 - A.face_zb[f];
        if (hg <= k::kDryDepth)
            continue;
        const double ag = k::areaOfDepth(g, hg);
        const double tg = k::widthOfDepth(g, hg);
        if (ag <= k::kDryArea || tg <= 0.0)
            continue;
        const double beta = std::sqrt(k::kGravity * ag * tg);
        A.relax_beta[f] = beta;
        resist += beta;
    }
    if (resist <= 0.0)
        return;

    const double as = A.node_surf_area[n];
    const double dh = dt * (sum_f + q_lat) / (as + dt * resist);
    if (dh == 0.0)
        return;

    for (int p = b; p < e; ++p) {
        const int f = A.node_face_idx[p];
        const double a = A.relax_beta[f];
        if (a <= 0.0)
            continue;
        A.f_mass[f] += -A.node_face_sign[p] * a * dh;
    }
}

/// Running record of the junction root find (the lambdas' captures).
struct AlgTrial {
    double h_best = 0.0;
    double r_best = std::numeric_limits<double>::infinity();
    double h_last = std::numeric_limits<double>::quiet_NaN();
};

OPENSWMM_KERNEL_FN double algResidual(const Arrays& A, int n, int b, int e, double q_ext, double h,
                                      AlgTrial& t) noexcept {
    A.node_head[n] = h;
    t.h_last = h;
    double r = q_ext;
    for (int p = b; p < e; ++p) {
        const int f = A.node_face_idx[p];
        faceFlux(A, f, true);
        r += A.node_face_sign[p] * A.f_mass[f];
    }
    if (std::fabs(r) < std::fabs(t.r_best)) {
        t.h_best = h;
        t.r_best = r;
    }
    return r;
}

OPENSWMM_KERNEL_FN void algFinalize(const Arrays& A, int n, int b, int e,
                                    const AlgTrial& t) noexcept {
    if (!(t.h_last == t.h_last))
        return; // no trial ran
    const double keep = A.node_head[n];
    A.node_head[n] = t.h_last;
    for (int p = b; p < e; ++p)
        faceFlux(A, A.node_face_idx[p], false);
    A.node_head[n] = keep;
}

OPENSWMM_KERNEL_FN double algResistAt(const Arrays& A, int b, int e, double h) noexcept {
    double resist = 0.0;
    for (int p = b; p < e; ++p) {
        const int f = A.node_face_idx[p];
        const int cell = faceCell(A, f);
        if (cell < 0)
            continue;
        const FvGeometry& g = A.geom[A.cell_geom[cell]];
        const double hg = h - A.face_zb[f];
        if (hg <= k::kDryDepth)
            continue;
        const double ag = k::areaOfDepth(g, hg);
        const double tg = k::widthOfDepth(g, hg);
        if (ag <= k::kDryArea || tg <= 0.0)
            continue;
        resist += std::sqrt(k::kGravity * ag * tg);
    }
    return resist;
}

OPENSWMM_KERNEL_FN bool algPrescribeDegree1(const Arrays& A, int n, int b, int e,
                                            double q_ext) noexcept {
    if (e - b != 1 || q_ext == 0.0)
        return false;
    const int f0 = A.node_face_idx[b];
    const int cell = faceCell(A, f0);
    if (cell < 0 || A.face_culvert[f0] >= 0 || A.face_gate[f0] != 0)
        return false;
    const double invert = A.node_invert[n];
    const double eta_c = A.cell_eta[cell];
    const double rim =
        invert + A.node_full_depth[n] + (A.node_can_pond[n] ? 0.0 : A.node_sur_depth[n]);
    if (A.node_full_depth[n] > 0.0 && eta_c >= rim)
        return false;
    if (A.deg1_guard_slot) {
        const FvGeometry& gg = A.geom[A.cell_geom[cell]];
        if (!gg.is_open && A.cell_h[cell] >= gg.y_full)
            return false;
    }
    const double sgn = A.node_face_sign[b];
    const k::FaceState& in = (A.face_cl[f0] >= 0) ? A.f_state_l[f0] : A.f_state_r[f0];
    A.f_mass[f0] = -sgn * q_ext; // sign·F + q_ext = 0
    A.f_mom[f0] = (in.a > k::kDryArea) ? k::kGravity * in.i1 : 0.0;
    A.node_head[n] = std::max(invert, eta_c);
    return true;
}

/// ExplicitFvSolver::solveAlgebraicNode.
OPENSWMM_KERNEL_FN void solveAlgebraicNode(const Arrays& A, int n, double dt) noexcept {
    if (fixedHead(A, n))
        return;
    double dummy_capacity = 0.0;
    if (A.node_dummy_drain && A.node_dummy_drain[n]) {
        dummy_capacity = std::numeric_limits<double>::infinity();
        for (int q = A.node_struct_ptr[n]; q < A.node_struct_ptr[n + 1]; ++q) {
            const int s = A.node_struct_idx[q];
            if (!A.struct_is_dummy[s] || A.struct_n1[s] != n)
                continue;
            if (A.has_q_cap) {
                const double cap = A.struct_q_cap[s];
                if (cap >= 0)
                    dummy_capacity = std::min(dummy_capacity, cap);
            }
        }
        if (A.has_source_max)
            dummy_capacity = std::min(dummy_capacity, A.node_source_max[n]);
        if (!finite(dummy_capacity)) {
            A.node_head[n] = A.node_invert[n];
            return;
        }
    }
    if (A.node_pass[n]) {
        const int b0 = A.node_face_ptr[n];
        const int e0 = A.node_face_ptr[n + 1];
        double s = 0.0, lowest = 0.0;
        int m = 0;
        bool any = false;
        for (int p = b0; p < e0; ++p) {
            const int c = faceCell(A, A.node_face_idx[p]);
            if (c < 0)
                continue;
            const double eta = A.cell_eta[c];
            if (!any || eta < lowest) {
                lowest = eta;
                any = true;
            }
            if (A.cell_h[c] <= k::kDryDepth)
                continue;
            s += eta;
            ++m;
        }
        if (m > 0)
            A.node_head[n] = s / m;
        else if (any)
            A.node_head[n] = lowest;
        return;
    }
    const int b = A.node_face_ptr[n];
    const int e = A.node_face_ptr[n + 1];
    const double invert = A.node_invert[n];
    const double q_ext =
        nodeLateral(A, n) + A.node_qstruct[n] + A.node_carry[n] / dt - dummy_capacity;

    AlgTrial t;

    const double full = A.node_full_depth[n];
    double top;
    if (full > 0.0) {
        top = A.node_can_pond[n] ? full : full + A.node_sur_depth[n];
    } else {
        top = 1.0;
        for (int p = b; p < e; ++p) {
            const int f = A.node_face_idx[p];
            const int cell = faceCell(A, f);
            if (cell < 0)
                continue;
            const FvGeometry& g = A.geom[A.cell_geom[cell]];
            top = std::max(top, (A.face_zb[f] - invert) + 2.0 * g.y_full);
        }
    }
    const double lo = invert;
    const double hi = invert + top;

    const double h0 = A.node_head[n];
    double h = std::min(std::max(h0, lo), hi);
    double r =
        (h == h0) ? algResidual(A, n, b, e, q_ext, h0, t) : algResidual(A, n, b, e, q_ext, h, t);
    if (std::fabs(r) <= kAlgQEps) {
        A.node_head[n] = h0;
        algFinalize(A, n, b, e, t);
        return;
    }

    const double dir = (r > 0.0) ? 1.0 : -1.0;
    const double res0 = algResistAt(A, b, e, h);
    double step = 2.0 * std::max(kAlgHeadTol, (res0 > 0.0) ? std::fabs(r) / res0 : kAlgHeadTol);
    double h_a = h, r_a = r;
    bool bracketed = false;
    for (int it = 0; it < kAlgMaxExpand; ++it) {
        double h_b = h_a + dir * step;
        if (h_b <= lo)
            h_b = lo;
        if (h_b >= hi)
            h_b = hi;
        const double r_b = algResidual(A, n, b, e, q_ext, h_b, t);
        if (std::fabs(r_b) <= kAlgQEps) {
            algFinalize(A, n, b, e, t);
            return;
        }
        if ((r_a > 0.0) != (r_b > 0.0)) {
            h = h_b;
            r = r_b;
            bracketed = true;
            break;
        }
        if (h_b == lo || h_b == hi) {
            algFinalize(A, n, b, e, t);
            if (h_b == hi && r_b > 0.0 && algPrescribeDegree1(A, n, b, e, q_ext))
                return;
            return;
        }
        h_a = h_b;
        r_a = r_b;
        step *= 2.0;
    }
    if (!bracketed) {
        if (t.h_best != h_a)
            algResidual(A, n, b, e, q_ext, t.h_best, t);
        algFinalize(A, n, b, e, t);
        return;
    }

    for (int it = 0; it < kAlgMaxIters; ++it) {
        double h_new = (r != r_a) ? h - r * (h - h_a) / (r - r_a) : 0.5 * (h_a + h);
        if (!(h_new > std::min(h_a, h) && h_new < std::max(h_a, h)))
            h_new = 0.5 * (h_a + h);
        const double r_new = algResidual(A, n, b, e, q_ext, h_new, t);
        const double dh = h_new - h;
        if ((r_new > 0.0) != (r > 0.0)) {
            h_a = h;
            r_a = r;
        } else {
            r_a *= 0.5;
        }
        h = h_new;
        r = r_new;
        if (std::fabs(dh) <= kAlgHeadTol || std::fabs(r) <= kAlgQEps)
            break;
    }
    if (std::fabs(t.r_best) < std::fabs(r))
        algResidual(A, n, b, e, q_ext, t.h_best, t);
    algFinalize(A, n, b, e, t);
}

/// One node of ExplicitFvSolver::relaxNodeFluxes / solveNodes (global path).
OPENSWMM_KERNEL_FN void solveNode(const Arrays& A, int n, double dt) noexcept {
    if (algebraicActive(A, n))
        solveAlgebraicNode(A, n, dt);
    else
        relaxOneNode(A, n, dt);
}

// ===========================================================================
// Positivity limiting (ExplicitFvSolver::limitPositivity)
// ===========================================================================

/// First face loop: bound the shared flux by the node's source limit.
OPENSWMM_KERNEL_FN void positivityCapFace(const Arrays& A, int f) noexcept {
    const int nd = A.face_node[f];
    if (nd >= 0 && A.has_source_max) {
        const double cap = A.node_source_max[nd];
        const bool left = A.face_cl[f] < 0;
        const double accepted = left ? std::min(A.f_mass[f], cap) : std::max(A.f_mass[f], -cap);
        if (accepted != A.f_mass[f]) {
            const k::FaceState& up = left ? A.f_state_l[f] : A.f_state_r[f];
            A.f_mass[f] = accepted;
            A.f_mom[f] = accepted * accepted / std::max(up.a, k::kDryArea) + k::kGravity * up.i1;
        }
    }
}

/// Outflow of one cell through its two faces, then its scale.
OPENSWMM_KERNEL_FN void positivityCell(const Arrays& A, int c, double dt) noexcept {
    double out = 0.0;
    const int faces[2] = {A.cell_face0[c], A.cell_face1[c]};
    // The face loop this replaces adds a cell's two faces in face order. With
    // two terms starting from zero the order cannot change the sum.
    for (int j = 0; j < 2; ++j) {
        const int f = faces[j];
        if (j == 1 && f == faces[0])
            break;
        const double fa = A.f_mass[f];
        if (fa == 0.0)
            continue;
        if (fa > 0.0) {
            if (A.face_cl[f] == c)
                out += fa;
        } else {
            if (A.face_cr[f] == c)
                out -= fa;
        }
    }
    if (out <= 0.0) {
        A.out_cell[c] = 1.0;
        return;
    }
    A.out_cell[c] = k::positivityScale(A.cell_a[c] * A.cell_dx[c], out, dt);
}

/// Outflow of one node through its faces, then its scale.
OPENSWMM_KERNEL_FN void positivityNode(const Arrays& A, int n, double dt) noexcept {
    double out = 0.0;
    for (int p = A.node_face_ptr[n]; p < A.node_face_ptr[n + 1]; ++p) {
        const int f = A.node_face_sorted[p];
        const double fa = A.f_mass[f];
        if (fa == 0.0)
            continue;
        if (fa > 0.0) {
            if (A.face_cl[f] < 0)
                out += fa;
        } else {
            if (A.face_cr[f] < 0)
                out -= fa;
        }
    }
    if (out <= 0.0) {
        A.out_node[n] = 1.0;
        return;
    }
    if (fixedHead(A, n)) {
        A.out_node[n] = 1.0;
        return;
    }
    if (algebraicActive(A, n)) {
        A.out_node[n] = 1.0;
        return;
    }
    A.out_node[n] = k::positivityScale(A.node_volume[n] - pondBase(A, n), out, dt);
}

/// Last face loop: the identical scaled flux updates both incident volumes.
OPENSWMM_KERNEL_FN void positivityScaleFace(const Arrays& A, int f) noexcept {
    const double fa = A.f_mass[f];
    if (fa == 0.0)
        return;
    double s;
    if (fa > 0.0) {
        s = (A.face_cl[f] >= 0) ? A.out_cell[A.face_cl[f]]
                                : ((A.face_node[f] >= 0) ? A.out_node[A.face_node[f]] : 1.0);
    } else {
        s = (A.face_cr[f] >= 0) ? A.out_cell[A.face_cr[f]]
                                : ((A.face_node[f] >= 0) ? A.out_node[A.face_node[f]] : 1.0);
    }
    if (s < 1.0) {
        A.f_mass[f] *= s;
        A.f_mom[f] *= s;
    }
}

// ===========================================================================
// Cell update (ExplicitFvSolver::updateCells and its helpers)
// ===========================================================================

/// ExplicitFvSolver::injectDivertedLateral.
OPENSWMM_KERNEL_FN double injectDivertedLateral(const Arrays& A, int c, double a_new, double dt,
                                                double inv_dx) noexcept {
    const double add = dt * A.cell_qlat[c] * inv_dx;
    if (add <= 0.0)
        return a_new + add;
    const int faces[2] = {A.cell_face0[c], A.cell_face1[c]};
    int f_rim = -1;
    double a_rim = std::numeric_limits<double>::infinity();
    for (int j = 0; j < 2; ++j) {
        const int f = faces[j];
        if (A.face_a_rim[f] < a_rim) {
            a_rim = A.face_a_rim[f];
            f_rim = f;
        }
    }
    if (f_rim < 0 || a_new + add <= a_rim)
        return a_new + add;
    const double room = std::max(0.0, a_rim - a_new);
    A.face_lat_spill[f_rim] += (add - room) / inv_dx;
    return a_new + room;
}

/// ExplicitFvSolver::frictionFor.
OPENSWMM_KERNEL_FN double frictionFor(const FvGeometry& g, double roughness, double rough_factor,
                                      double q, double u, double h, double dt, double r) noexcept {
    if (g.xs.type == static_cast<int>(XSectShape::FORCE_MAIN) && h >= g.y_full) {
        const double absu = std::fabs(u);
        if (absu <= 0.0)
            return q;
        const double sf = (roughness < 1.0) ? hydkernels::fricSlopeDW(u, r, roughness)
                                            : hydkernels::fricSlopeHW(u, r, roughness);
        return q / (1.0 + dt * k::kGravity * sf / absu);
    }
    return k::frictionUpdate(q, u, r, dt, rough_factor);
}

/// One cell of ExplicitFvSolver::updateCells.
OPENSWMM_KERNEL_FN void updateCell(const Arrays& A, int c, double dt) noexcept {
    const FvGeometry& g = A.geom[A.cell_geom[c]];
    const double dx = A.cell_dx[c];
    const double inv_dx = 1.0 / dx;

    const int faces[2] = {A.cell_face0[c], A.cell_face1[c]};
    const std::int8_t sides[2] = {A.cell_side0[c], A.cell_side1[c]};

    double dA = 0.0, dQ = 0.0, k_loss = 0.0, q_thru = 0.0;
    for (int e = 0; e < 2; ++e) {
        const int f = faces[e];
        const double fa = A.f_mass[f];
        const double fq = A.f_mom[f];
        if (sides[e] == 0) {
            dA -= fa;
            dQ -= static_cast<double>(A.face_dir_l[f]) * (fq + A.f_corr_l[f]);
            q_thru += static_cast<double>(A.face_dir_l[f]) * fa;
        } else {
            dA += fa;
            dQ += static_cast<double>(A.face_dir_r[f]) * (fq + A.f_corr_r[f]);
            q_thru += static_cast<double>(A.face_dir_r[f]) * fa;
        }
        if (A.face_node[f] >= 0)
            k_loss += (A.face_cl[f] < 0) ? A.conduit_loss_inlet[A.cell_conduit[c]]
                                         : A.conduit_loss_outlet[A.cell_conduit[c]];
    }
    A.cell_q_int[c] += 0.5 * q_thru * dt;

    const double a_old = A.cell_a[c];
    double a_new = a_old + dt * dA * inv_dx;
    double q_new = A.cell_q[c] + dt * dQ * inv_dx;

    if (A.cell_qlat[c] != 0.0)
        a_new = injectDivertedLateral(A, c, a_new, dt, inv_dx);

    if (A.has_conduit_loss) {
        const int cr = A.cell_conduit[c];
        if (cr >= 0)
            a_new -= dt * A.conduit_loss[cr];
    }
    if (a_new < 0.0)
        a_new = 0.0;

    const double h_new = k::depthOfArea(g, a_new);
    A.cell_a[c] = a_new;
    A.cell_h[c] = h_new;
    A.cell_eta[c] = A.cell_zb[c] + h_new;
    double radius;
    cacheClosure(A, c, g, h_new, &radius);
    if (h_new <= k::kDryDepth) {
        A.cell_q[c] = 0.0;
        A.cell_u[c] = 0.0;
        return;
    }
    const double u = q_new / a_new;
    q_new = frictionFor(g, A.conduit_roughness[A.cell_conduit[c]],
                        A.conduit_rough_factor[A.cell_conduit[c]], q_new, u, h_new, dt, radius);
    if (k_loss > 0.0)
        q_new = k::localLossUpdate(q_new, u, k_loss, dx, dt);

    A.cell_q[c] = q_new;
    A.cell_u[c] = q_new / a_new;
}

// ===========================================================================
// Node update (ExplicitFvSolver::updateNodes and its helpers)
// ===========================================================================

/// The `arrived` lambda of updateNodes: net face volume reaching a node.
OPENSWMM_KERNEL_FN double arrivedVolume(const Arrays& A, int n, double dt) noexcept {
    double sum = 0.0;
    for (int p = A.node_face_ptr[n]; p < A.node_face_ptr[n + 1]; ++p)
        sum += A.node_face_sign[p] * A.f_mass[A.node_face_idx[p]];
    return sum * dt;
}

OPENSWMM_KERNEL_FN bool structSkip(const Arrays& A, int s) noexcept {
    return A.have_dummy && A.struct_is_dummy[s];
}

OPENSWMM_KERNEL_FN int structDonor(const Arrays& A, int s, double q) noexcept {
    return (q > 0.0) ? A.struct_n1[s] : A.struct_n2[s];
}

/// limitStructureFlows, first half: what a bucket donor can pay out.
OPENSWMM_KERNEL_FN void structureScaleNode(const Arrays& A, int n, double dt) noexcept {
    double demand = 0.0;
    for (int q = A.node_struct_ptr[n]; q < A.node_struct_ptr[n + 1]; ++q) {
        const int s = A.node_struct_idx[q];
        const double qs = A.struct_q[s];
        if (structSkip(A, s) || qs == 0.0)
            continue;
        if (structDonor(A, s, qs) == n)
            demand += std::fabs(qs) * dt;
    }
    const bool bucket =
        A.node_kind[n] != kNodeVirtual && !algebraicActive(A, n) && !fixedHead(A, n);
    if (demand <= 0.0 || !bucket) {
        A.node_struct_scale[n] = 1.0;
        return;
    }
    const double held =
        A.node_volume[n] + arrivedVolume(A, n, dt) + dt * std::max(0.0, nodeLateral(A, n));
    A.node_struct_scale[n] = (held >= demand) ? 1.0 : std::max(0.0, held) / demand;
}

/// limitStructureFlows, second half: the scaled source/sink at one node.
OPENSWMM_KERNEL_FN void structureFlowNode(const Arrays& A, int n) noexcept {
    double v = 0.0;
    for (int q = A.node_struct_ptr[n]; q < A.node_struct_ptr[n + 1]; ++q) {
        const int s = A.node_struct_idx[q];
        if (structSkip(A, s))
            continue;
        double qs = A.struct_q[s];
        if (qs == 0.0)
            continue;
        const double scale = A.node_struct_scale[structDonor(A, s, qs)];
        if (scale < 1.0)
            qs *= scale;
        if (A.struct_n1[s] == n)
            v -= qs;
        if (A.struct_n2[s] == n)
            v += qs;
    }
    A.node_qstruct[n] = v;
}

/// limitStructureFlows: the volume one structure actually moved.
OPENSWMM_KERNEL_FN void structureApplied(const Arrays& A, int s, double dt) noexcept {
    if (structSkip(A, s))
        return;
    double qs = A.struct_q[s];
    if (qs == 0.0)
        return;
    const double scale = A.node_struct_scale[structDonor(A, s, qs)];
    if (scale < 1.0)
        qs *= scale;
    A.struct_applied_vol[s] += qs * dt;
}

/// refreshDummyFlows: the pass-through discharge of one DUMMY link.
OPENSWMM_KERNEL_FN void dummyFlow(const Arrays& A, int s, double dt) noexcept {
    if (!A.struct_is_dummy[s]) {
        A.dummy_q[s] = 0.0;
        return;
    }
    const int u1 = A.struct_n1[s];
    double q = arrivedVolume(A, u1, dt) / dt + nodeLateral(A, u1) + A.node_qstruct[u1];
    if (q < 0.0)
        q = 0.0;
    if (A.has_q_cap) {
        const double cap = A.struct_q_cap[s];
        if (cap >= 0.0 && q > cap)
            q = cap;
    }
    if (A.has_source_max)
        q = std::min(q, A.node_source_max[u1]);
    A.dummy_q[s] = q;
    A.dummy_vol[s] += q * dt;
}

/// refreshDummyFlows: the pass-through source/sink at one node.
OPENSWMM_KERNEL_FN void dummyFlowNode(const Arrays& A, int n) noexcept {
    double v = 0.0;
    for (int q = A.node_struct_ptr[n]; q < A.node_struct_ptr[n + 1]; ++q) {
        const int s = A.node_struct_idx[q];
        if (!A.struct_is_dummy[s])
            continue;
        if (A.struct_n1[s] == n)
            v -= A.dummy_q[s];
        if (A.struct_n2[s] == n)
            v += A.dummy_q[s];
    }
    A.node_qdummy[n] = v;
}

/// ExplicitFvSolver::bookLateralSpills, one node.
OPENSWMM_KERNEL_FN void lateralSpillNode(const Arrays& A, int n) noexcept {
    if (!A.node_pass_static[n])
        return;
    double spill = 0.0;
    for (int p = A.node_face_ptr[n]; p < A.node_face_ptr[n + 1]; ++p) {
        const int f = A.node_face_idx[p];
        spill += A.face_lat_spill[f];
        A.face_lat_spill[f] = 0.0;
    }
    if (spill <= 0.0)
        return;
    if (A.node_can_pond[n])
        A.node_volume[n] = std::max(A.node_volume[n], A.node_vfull[n]) + spill;
    else
        A.flood_vol[n] += spill;
}

/// ExplicitFvSolver::incidentPressurized.
OPENSWMM_KERNEL_FN bool incidentPressurized(const Arrays& A, int n) noexcept {
    for (int p = A.node_face_ptr[n]; p < A.node_face_ptr[n + 1]; ++p) {
        const int c = faceCell(A, A.node_face_idx[p]);
        if (c < 0)
            continue;
        const FvGeometry& g = A.geom[A.cell_geom[c]];
        if (!g.is_open && A.cell_h[c] >= g.y_crown)
            return true;
    }
    return false;
}

/// ExplicitFvSolver::capWithdrawal.
OPENSWMM_KERNEL_FN double capWithdrawal(const Arrays& A, int n, double balance,
                                        double dt) noexcept {
    if (balance < 0 && A.has_withdrawal) {
        const double refused = std::min(-balance, std::max(0.0, A.node_withdrawal[n]) * dt);
        A.unmet[n] += refused;
        balance += refused;
    }
    return balance;
}

/// ExplicitFvSolver::applyNodeCapacity.
OPENSWMM_KERNEL_FN void applyNodeCapacity(const Arrays& A, int n, double v_prev, double& vol,
                                          double& depth) noexcept {
    const double full = A.node_full_depth[n];
    if (full <= 0.0 || depth <= full)
        return;
    const double v_full = nodeVolumeFromDepth(A, n, full);
    if (A.node_can_pond[n]) {
        A.flood_vol[n] += std::max(vol - std::max(v_prev, v_full), 0.0);
        return;
    }
    const double y_max = full + A.node_sur_depth[n];
    if (depth <= y_max)
        return;
    const double v_max = nodeVolumeFromDepth(A, n, y_max);
    A.flood_vol[n] += vol - v_max;
    vol = v_max;
    depth = y_max;
}

/// ExplicitFvSolver::settleAlgebraicNode, except the pass-through deposit,
/// which is only recorded here (dep, settle_carry) and applied by depositCell
/// and depositNode.
OPENSWMM_KERNEL_FN void settleAlgebraicNode(const Arrays& A, int n, double carry) noexcept {
    const double invert = A.node_invert[n];
    const double depth = std::max(0.0, A.node_head[n] - invert);
    const double full = A.node_full_depth[n];
    const double published = std::max(0.0, A.node_volume[n] - pondBase(A, n));
    const double ledger = std::max(0.0, A.node_carry[n]);
    if (published > ledger)
        carry += published - ledger;
    if (carry > 0.0 && full > 0.0) {
        const double eps = 1.0e-9;
        if (A.node_can_pond[n]) {
            if (depth >= full - eps) {
                A.node_volume[n] = A.node_vfull[n] + carry;
                A.node_head[n] = invert + nodeDepthFromVolume(A, n, A.node_volume[n]);
                A.node_carry[n] = 0.0;
                return;
            }
        } else if (depth >= full + A.node_sur_depth[n] - eps) {
            A.flood_vol[n] += carry;
            A.node_carry[n] = 0.0;
            A.node_volume[n] = 0.0;
            return;
        }
    }
    if (A.node_pass[n] && carry != 0.0 && !incidentPressurized(A, n)) {
        A.dep[n] = 1;
        A.settle_carry[n] = carry;
        return;
    }
    A.node_carry[n] = carry;
    A.node_volume[n] = std::max(0.0, carry);
}

/// One node of the ledger loop of ExplicitFvSolver::updateNodes.
OPENSWMM_KERNEL_FN void nodeUpdate(const Arrays& A, int n, double dt) noexcept {
    A.dep[n] = 0;
    if (A.node_kind[n] == kNodeVirtual) {
        const int face = A.node_vj_face[n];
        if (face >= 0) {
            // ExplicitFvSolver::bookSpliceReceipt.
            const double flux = A.f_mass[face];
            double h = 0.0;
            if (A.node_lat_div[n] && A.has_lateral)
                h = 0.5 * A.node_lateral[n];
            const double left = flux - h, right = -(flux + h);
            A.node_in[n] += (std::max(left, 0.0) + std::max(right, 0.0)) * dt;
            A.node_out[n] += (std::max(-left, 0.0) + std::max(-right, 0.0)) * dt;
        }
        return;
    }

    double sum_faces = 0.0;
    const int b = A.node_face_ptr[n];
    const int e = A.node_face_ptr[n + 1];
    for (int p = b; p < e; ++p)
        sum_faces += A.node_face_sign[p] * A.f_mass[A.node_face_idx[p]];
    A.node_exch[n] += sum_faces * dt;
    for (int p2 = b; p2 < e; ++p2) {
        const double q = A.node_face_sign[p2] * A.f_mass[A.node_face_idx[p2]];
        if (q > 0.0)
            A.node_in[n] += q * dt;
        else
            A.node_out[n] -= q * dt;
    }
    // ExplicitFvSolver::bookDivertedLateral.
    if (A.node_lat_div[n] && A.has_lateral) {
        const double lat = A.node_lateral[n];
        if (lat > 0.0)
            A.node_out[n] += lat * dt;
    }

    if (fixedHead(A, n)) {
        A.node_head[n] = A.node_fixed_head[n];
        return;
    }

    const double q_lat = nodeLateral(A, n) + A.node_qstruct[n] + nodeQDummy(A, n);

    if (algebraicActive(A, n)) {
        settleAlgebraicNode(A, n,
                            capWithdrawal(A, n, A.node_carry[n] + dt * (sum_faces + q_lat), dt));
        return;
    }

    const double v_prev = A.node_volume[n];
    double vol = capWithdrawal(A, n, v_prev + dt * (sum_faces + q_lat), dt);
    const double v_floor = pondBase(A, n);
    if (vol < v_floor)
        vol = v_floor;

    double depth = nodeDepthFromVolume(A, n, vol);
    applyNodeCapacity(A, n, v_prev, vol, depth);
    A.node_volume[n] = vol;
    A.node_head[n] = A.node_invert[n] + depth;
}

/// The cell half of a pass-through junction's residual deposit: this cell
/// takes its share from each depositing end node, lower node first.
OPENSWMM_KERNEL_FN void depositCell(const Arrays& A, int c) noexcept {
    int f_of[2] = {-1, -1};
    int n_of[2] = {-1, -1};
    int cnt = 0;
    const int faces[2] = {A.cell_face0[c], A.cell_face1[c]};
    for (int j = 0; j < 2; ++j) {
        const int f = faces[j];
        if (j == 1 && f == faces[0])
            break;
        const int nd = A.face_node[f];
        if (nd >= 0 && A.dep[nd]) {
            f_of[cnt] = f;
            n_of[cnt] = nd;
            ++cnt;
        }
    }
    if (cnt == 0)
        return;
    if (cnt == 2 && (n_of[1] < n_of[0] ||
                     (n_of[1] == n_of[0] && A.face_npos[f_of[1]] < A.face_npos[f_of[0]]))) {
        const int tf = f_of[0], tn = n_of[0];
        f_of[0] = f_of[1];
        n_of[0] = n_of[1];
        f_of[1] = tf;
        n_of[1] = tn;
    }
    const FvGeometry& g = A.geom[A.cell_geom[c]];
    const double dx = A.cell_dx[c];
    for (int j = 0; j < cnt; ++j) {
        const int nd = n_of[j];
        const int b = A.node_face_ptr[nd];
        const int e = A.node_face_ptr[nd + 1];
        const int deg = (e > b) ? (e - b) : 1;
        const double carry = A.settle_carry[nd];
        double a_new = A.cell_a[c] + (carry / static_cast<double>(deg)) / dx;
        if (a_new < 0.0)
            a_new = 0.0;
        A.absorb[f_of[j]] = (a_new - A.cell_a[c]) * dx;
        const double h_new = k::depthOfArea(g, a_new);
        A.cell_a[c] = a_new;
        A.cell_h[c] = h_new;
        A.cell_eta[c] = A.cell_zb[c] + h_new;
        cacheClosure(A, c, g, h_new, nullptr);
        if (h_new <= k::kDryDepth) {
            A.cell_q[c] = 0.0;
            A.cell_u[c] = 0.0;
        } else {
            A.cell_u[c] = A.cell_q[c] / a_new;
        }
    }
}

/// The node half: whatever the cells could not take stays in the carry.
OPENSWMM_KERNEL_FN void depositNode(const Arrays& A, int n) noexcept {
    if (!A.dep[n])
        return;
    double left = A.settle_carry[n];
    for (int p = A.node_face_ptr[n]; p < A.node_face_ptr[n + 1]; ++p) {
        const int f = A.node_face_idx[p];
        if (faceCell(A, f) < 0)
            continue;
        left -= A.absorb[f];
    }
    A.node_carry[n] = (std::fabs(left) > 1.0e-12) ? left : 0.0;
    A.node_volume[n] = std::max(0.0, A.node_carry[n]);
}

// ===========================================================================
// CFL census (ExplicitFvSolver::censusDt, global path, LTS not running)
// ===========================================================================

/// ExplicitFvSolver::passThroughFarCell.
OPENSWMM_KERNEL_FN int passThroughFarCell(const Arrays& A, int face, int nd) noexcept {
    if (nd < 0 || !A.node_pass[nd] || !algebraicActive(A, nd))
        return -1;
    for (int p = A.node_face_ptr[nd]; p < A.node_face_ptr[nd + 1]; ++p) {
        const int of = A.node_face_idx[p];
        if (of == face)
            continue;
        return faceCell(A, of);
    }
    return -1;
}

/// Stable step of one face; a value at or above 1e300 means "no bound".
OPENSWMM_KERNEL_FN double censusFace(const Arrays& A, int f) noexcept {
    const int cl = A.face_cl[f];
    const int cr = A.face_cr[f];
    const int nd = A.face_node[f];

    double speed = 0.0;
    double dx_ref = 1.0e30;

    for (int side = 0; side < 2; ++side) {
        const int cell = (side == 0) ? cl : cr;
        if (cell < 0)
            continue;
        dx_ref = std::min(dx_ref, A.cell_dx[cell]);
        const double h = A.cell_h[cell];
        if (h <= k::kDryDepth)
            continue;
        speed = std::max(speed,
                         std::fabs(A.cell_u[cell]) + k::celerity(A.cell_a[cell], A.cell_t[cell]));
    }

    if (nd >= 0) {
        const int other = (cl >= 0) ? cl : cr;
        const int far = A.census_pass_far ? passThroughFarCell(A, f, nd) : -1;
        if (far >= 0) {
            if (A.cell_h[far] > k::kDryDepth)
                speed = std::max(speed, std::fabs(A.cell_u[far]) +
                                            k::celerity(A.cell_a[far], A.cell_t[far]));
        } else if (other >= 0) {
            const FvGeometry& g = A.geom[A.cell_geom[other]];
            const double hg = A.node_head[nd] - A.face_zb[f];
            if (hg > k::kDryDepth) {
                const double ag = k::areaOfDepth(g, hg);
                speed = std::max(speed, std::fabs(A.cell_u[other]) +
                                            k::celerity(ag, k::widthOfDepth(g, hg)));
            }
        }
    }

    if (speed > 1.0e-12 && dx_ref < 1.0e29)
        return A.cfl * dx_ref / speed;
    return DBL_MAX;
}

/// Stable step a storage (bucket) node imposes; DBL_MAX when none.
OPENSWMM_KERNEL_FN double censusNode(const Arrays& A, int n) noexcept {
    double dt = DBL_MAX;
    if (A.node_kind[n] == kNodeVirtual)
        return dt;
    if (A.node_alg[n])
        return dt; // FV_NODE_FEEDBACK_DT is not supported on the device
    if (A.node_kind[n] == kNodeOutfall)
        return dt;
    const double as = A.node_surf_area[n];
    if (!(as > 0.0))
        return dt;
    for (int p = A.node_face_ptr[n]; p < A.node_face_ptr[n + 1]; ++p) {
        const int cell = faceCell(A, A.node_face_idx[p]);
        if (cell < 0)
            continue;
        const double h = A.cell_h[cell];
        if (h <= k::kDryDepth)
            continue;
        const FvGeometry& g = A.geom[A.cell_geom[cell]];
        const double t = k::widthOfDepth(g, h);
        if (t <= 0.0)
            continue;
        const double c_wave = k::celerity(A.cell_a[cell], t);
        const double dt_n = k::faceCflDt(A.cfl, as / t, A.cell_u[cell], c_wave);
        if (dt_n < dt)
            dt = dt_n;
    }
    return dt;
}

} // namespace openswmm::fv::substep

#endif // OPENSWMM_ENGINE_FV_SUBSTEP_KERNELS_HPP
