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
 * @file PressurizedHeadSolver.cpp
 * @brief Implementation of the R2a implicit pressurized head update.
 * @ingroup engine_fv
 */

#include "PressurizedHeadSolver.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include "../HydClosureKernels.hpp"

namespace openswmm::fv {

namespace k = kernels;

namespace {

/// Friction coefficient γ in the semi-implicit denominator 1 + Δt·γ — the
/// SAME laws frictionFor applies to the cell momentum (Manning by default;
/// a full FORCE_MAIN switches to its own HW/DW slope), expressed per unit
/// time so the face conductance damps exactly as the cell does.
///
/// Evaluated at the PHYSICAL velocity Q/min(A, a_crown), not Q/A: above the
/// crown the conserved A carries the slot's numerical storage, and a
/// velocity diluted by it under-states the friction slope by exactly the
/// slot share — which is the mechanism that made the explicit scheme's
/// full-bore head loss a function of FV_SLOT_CELERITY (R1: 8.8 ft at c=30
/// against 13.30 analytic). The implicit face law is not a conservative
/// Godunov flux — no bore-speed consistency binds it — so it is free to use
/// the conveyance area, the same distinction legacy DW draws with
/// conveyArea (#144).
double frictionGamma(const FvGeometry& g, double q, double a, double h) {
    const double a_conv = std::min(a, std::max(g.a_crown, k::kDryArea));
    if (a_conv <= k::kDryArea) return 0.0;
    const double u = q / a_conv;
    const double absu = std::fabs(u);
    const double r = k::hydRadOfDepth(g, h);
    if (r <= 0.0) return 0.0;
    if (g.xs.type == static_cast<int>(XSectShape::FORCE_MAIN) &&
        h >= g.y_full) {
        if (absu <= 0.0) return 0.0;
        const double sf = (g.roughness < 1.0)
                              ? hydkernels::fricSlopeDW(u, r, g.roughness)
                              : hydkernels::fricSlopeHW(u, r, g.roughness);
        return k::kGravity * sf / absu;
    }
    if (g.rough_factor <= 0.0) return 0.0;
    const double r43 = r * std::cbrt(r);
    return g.rough_factor * absu / r43;
}

bool fixedHead(const FvStepForcing* f, std::size_t un) {
    return f && f->node_fixed_head && std::isfinite(f->node_fixed_head[un]);
}

/// algebraicActive, replicated from ExplicitFvSolver over the view's data:
/// eligible AND not demoted to the bucket path by a real pond above v_full.
bool algActive(const PressurizedView& v, int n) {
    const auto un = static_cast<std::size_t>(n);
    if (!(*v.node_alg)[un]) return false;
    if (v.mesh->node_can_pond[un] &&
        v.state->node_volume[un] > (*v.node_vfull)[un])
        return false;
    return true;
}

/// The node's physical ceiling — the same bracket top solveAlgebraicNode
/// uses: the rim for a pondable node (above it the water is real storage),
/// rim + surcharge depth for a sealed one, and a generous crown-derived
/// ceiling for a junction with no rim on record.
double nodeCeiling(const NetworkMeshData& m, int n) {
    const auto un = static_cast<std::size_t>(n);
    const double invert = m.node_invert[un];
    const double full = m.node_full_depth[un];
    if (full > 0.0)
        return invert +
               (m.node_can_pond[un] ? full : full + m.node_sur_depth[un]);
    double top = 1.0;
    for (int p = m.node_face_ptr[un]; p < m.node_face_ptr[un + 1]; ++p) {
        const auto uf = static_cast<std::size_t>(
            m.node_face_idx[static_cast<std::size_t>(p)]);
        const int cell = (m.face_cl[uf] >= 0) ? m.face_cl[uf] : m.face_cr[uf];
        if (cell < 0) continue;
        const FvGeometry& g = m.geom[static_cast<std::size_t>(
            m.cell_geom[static_cast<std::size_t>(cell)])];
        top = std::max(top, (m.face_zb[uf] - invert) + 2.0 * g.y_full);
    }
    return invert + top;
}

} // namespace

// ---------------------------------------------------------------------------
// Union-find
// ---------------------------------------------------------------------------

int PressurizedHeadSolver::ufFind(int i) {
    while (uf_parent_[static_cast<std::size_t>(i)] != i) {
        uf_parent_[static_cast<std::size_t>(i)] =
            uf_parent_[static_cast<std::size_t>(
                uf_parent_[static_cast<std::size_t>(i)])];
        i = uf_parent_[static_cast<std::size_t>(i)];
    }
    return i;
}

void PressurizedHeadSolver::ufUnion(int a, int b) {
    const int ra = ufFind(a), rb = ufFind(b);
    if (ra != rb) uf_parent_[static_cast<std::size_t>(std::max(ra, rb))] =
        std::min(ra, rb);
}

// ---------------------------------------------------------------------------
// classify — the memoryless subset derivation
// ---------------------------------------------------------------------------

bool PressurizedHeadSolver::classify(const PressurizedView& v) {
    const NetworkMeshData& mesh = *v.mesh;
    const NetworkStateData& state = *v.state;
    const auto nc = static_cast<std::size_t>(mesh.n_cells());
    const auto nn = static_cast<std::size_t>(mesh.n_nodes());

    imp_faces_.clear();
    cell_press_.assign(nc, 0);
    node_fold_.assign(nn, 0);

    bool any = false;
    for (std::size_t c = 0; c < nc; ++c) {
        if (!(*v.cell_active)[c]) continue;
        if (cellPressurized(mesh, state, static_cast<int>(c))) {
            cell_press_[c] = 1;
            any = true;
        }
    }
    if (!any) return false;

    cell_unk_.assign(nc, -1);
    node_unk_.assign(nn, -1);

    // Face pass: the implicit set is the FULL faces only. Transition
    // (kDelta) faces stay entirely explicit — flux and census bound — so
    // fronts keep shock-captured physics at a front-resolving dt (see the
    // kDelta taxonomy note in the header for the measured reasons).
    for (const int f : *v.active_faces) {
        const std::uint8_t mode = faceModeOf(mesh, state, f);
        if (mode != kFull) continue;
        ImpFace F;
        F.face = f;
        F.mode = mode;
        imp_faces_.push_back(F);
    }
    if (imp_faces_.empty()) return false;

    // Fold pass: a pressurized algebraic junction whose EVERY incident face
    // is FULL-implicit becomes an unknown row. Anything else stays Dirichlet
    // data (fixed heads, storage/demoted nodes, transition junctions).
    for (std::size_t un = 0; un < nn; ++un) {
        const int n = static_cast<int>(un);
        if (!algActive(v, n)) continue;
        if (fixedHead(v.forcing, un)) continue;
        const int b = mesh.node_face_ptr[un];
        const int e = mesh.node_face_ptr[un + 1];
        if (e <= b) continue;
        bool all_full = true;
        for (int p = b; p < e && all_full; ++p) {
            const int f = mesh.node_face_idx[static_cast<std::size_t>(p)];
            all_full = faceModeOf(mesh, state, f) == kFull;
        }
        if (all_full) node_fold_[un] = 1;
    }

    // Unknown table: pressurized cell sides of implicit faces, then folded
    // nodes. (A pressurized cell isolated behind gates never appears — its
    // faces keep their explicit laws and its dt keeps the explicit bound.)
    unk_entity_.clear();
    auto cellUnknown = [&](int c) {
        auto& id = cell_unk_[static_cast<std::size_t>(c)];
        if (id < 0) {
            id = static_cast<int>(unk_entity_.size());
            unk_entity_.push_back(c);
        }
        return id;
    };
    auto nodeUnknown = [&](int n) {
        auto& id = node_unk_[static_cast<std::size_t>(n)];
        if (id < 0) {
            id = static_cast<int>(unk_entity_.size());
            unk_entity_.push_back(mesh.n_cells() + n);
        }
        return id;
    };

    for (ImpFace& F : imp_faces_) {
        const auto uf = static_cast<std::size_t>(F.face);
        const int cl = mesh.face_cl[uf];
        const int cr = mesh.face_cr[uf];
        const int nd = mesh.face_node[uf];

        // Left side.
        if (cl >= 0) {
            if (cell_press_[static_cast<std::size_t>(cl)])
                F.unk_l = cellUnknown(cl);
            else
                F.dir_l = 0.0;  // value filled at solve time (cell_eta)
        } else if (nd >= 0 && node_fold_[static_cast<std::size_t>(nd)]) {
            F.unk_l = nodeUnknown(nd);
        }
        // Right side.
        if (cr >= 0) {
            if (cell_press_[static_cast<std::size_t>(cr)])
                F.unk_r = cellUnknown(cr);
        } else if (nd >= 0 && node_fold_[static_cast<std::size_t>(nd)]) {
            F.unk_r = nodeUnknown(nd);
        }
    }

    // Components over FULL faces whose both endpoints are unknowns.
    const int nu = static_cast<int>(unk_entity_.size());
    uf_parent_.resize(static_cast<std::size_t>(nu));
    for (int i = 0; i < nu; ++i) uf_parent_[static_cast<std::size_t>(i)] = i;
    for (const ImpFace& F : imp_faces_)
        if (F.unk_l >= 0 && F.unk_r >= 0) ufUnion(F.unk_l, F.unk_r);

    return true;
}

// ---------------------------------------------------------------------------
// solve
// ---------------------------------------------------------------------------

void PressurizedHeadSolver::solve(const PressurizedView& v, double dt) {
    if (imp_faces_.empty() || dt <= 0.0) return;
    const NetworkMeshData& mesh = *v.mesh;
    NetworkStateData& state = *v.state;
    const int nu = static_cast<int>(unk_entity_.size());
    const auto unu = static_cast<std::size_t>(nu);

    // ---- per-unknown constants -------------------------------------------
    unk_eta0_.assign(unu, 0.0);
    unk_store_.assign(unu, 0.0);
    unk_rhs_ext_.assign(unu, 0.0);
    unk_head_.assign(unu, 0.0);
    unk_dirichlet_.assign(unu, 0);
    unk_dirvalue_.assign(unu, 0.0);

    for (int u = 0; u < nu; ++u) {
        const int ent = unk_entity_[static_cast<std::size_t>(u)];
        if (ent < mesh.n_cells()) {
            const auto uc = static_cast<std::size_t>(ent);
            const FvGeometry& g =
                mesh.geom[static_cast<std::size_t>(mesh.cell_geom[uc])];
            const double h = state.cell_h[uc];
            const double T = std::max(k::widthOfDepth(g, h), 1.0e-12);
            unk_store_[static_cast<std::size_t>(u)] =
                T * mesh.cell_dx[uc] / dt;
            unk_eta0_[static_cast<std::size_t>(u)] = v.cell_eta[uc];
            if (v.cell_qlat && !v.cell_qlat->empty())
                unk_rhs_ext_[static_cast<std::size_t>(u)] +=
                    (*v.cell_qlat)[uc];
            // Explicit faces of a pressurized cell — transition faces,
            // gates, culvert caps, walls — enter as constants: the
            // divergence updateCells will integrate has to be the
            // divergence this solve balanced.
            const int faces[2] = {mesh.cell_face0[uc], mesh.cell_face1[uc]};
            const std::int8_t sides[2] = {mesh.cell_side0[uc],
                                          mesh.cell_side1[uc]};
            for (int e2 = 0; e2 < 2; ++e2) {
                const int f = faces[e2];
                if (faceModeOf(mesh, state, f) == kFull) continue;
                const double fa = v.f_mass[static_cast<std::size_t>(f)];
                unk_rhs_ext_[static_cast<std::size_t>(u)] +=
                    (sides[e2] == 0) ? -fa : fa;
            }
        } else {
            const int n = ent - mesh.n_cells();
            const auto un = static_cast<std::size_t>(n);
            // Algebraic junction row: zero storage (today's convention — the
            // balance is instantaneous), sources on the RHS. The carry bleeds
            // in exactly as solveAlgebraicNode bled it.
            unk_eta0_[static_cast<std::size_t>(u)] = state.node_head[un];
            double q_ext = (*v.node_qstruct)[un] + (*v.node_carry)[un] / dt;
            if (!(v.node_lat_div && !v.node_lat_div->empty() &&
                  (*v.node_lat_div)[un]) &&
                v.forcing && v.forcing->node_lateral)
                q_ext += v.forcing->node_lateral[un];
            unk_rhs_ext_[static_cast<std::size_t>(u)] = q_ext;
        }
    }

    // ---- per-face coefficients ---------------------------------------------
    for (ImpFace& F : imp_faces_) {
        const auto uf = static_cast<std::size_t>(F.face);
        const k::FaceState& L = v.f_state_l[uf];
        const k::FaceState& R = v.f_state_r[uf];
        // Conveyance area, not conserved area: the slot's numerical storage
        // must not conduct (see frictionGamma above). Capped at the FACE
        // section's crown area — both sides were reconstructed in it.
        const FvGeometry& gf =
            mesh.geom[static_cast<std::size_t>(mesh.face_geom[uf])];
        const double ahat =
            std::min(0.5 * (L.a + R.a), std::max(gf.a_crown, k::kDryArea));
        const double Lf = mesh.face_dx[uf];
        F.cond = 0.0;
        if (ahat <= k::kDryArea || Lf <= 0.0) continue;

        double gamma = 0.0;
        int ngam = 0;
        for (const int c : {mesh.face_cl[uf], mesh.face_cr[uf]}) {
            if (c < 0) continue;
            const auto uc = static_cast<std::size_t>(c);
            const FvGeometry& g =
                mesh.geom[static_cast<std::size_t>(mesh.cell_geom[uc])];
            // A TPA-flagged cell (issue #156) is full at any piezometric
            // depth: friction evaluates at y_full (R = r_full, and a full
            // FORCE_MAIN keeps its pressurized law) rather than at a
            // sub-atmospheric h the table would read as part-full.
            const double h_fric =
                (!state.cell_tpa.empty() && state.cell_tpa[uc] != 0 &&
                 state.cell_h[uc] < g.y_full)
                    ? g.y_full : state.cell_h[uc];
            gamma += frictionGamma(g, state.cell_q[uc], state.cell_a[uc],
                                   h_fric);
            ++ngam;
        }
        if (ngam > 0) gamma /= static_cast<double>(ngam);

        const int cl = mesh.face_cl[uf];
        const int cr = mesh.face_cr[uf];
        const double qstar0 = 0.5 * (L.q + R.q);

        // Unsteady friction (issue #156): the Vitkovsky local-acceleration
        // part folds into the SAME semi-implicit denominator steady friction
        // uses — q^{n+1}·(1 + Δt·γ + k3) = (1+k3)·q* − Δt·gÂ/L·Δη −
        // Δt·k3·Â·grad — so the implicit path damps exactly as
        // kernels::ufUpdate does on the explicit path (without this the
        // fully-pressurized e1 waterhammer is byte-inert under UF, U-G2).
        // The convective part rides the old-state uf_grad snapshot the
        // solver precomputed BEFORE this pass. Dead-band and the
        // half-momentum clamp mirror the kernel, so a discretely-at-rest
        // deck stays bit-identical; k3 = 0 (or UF off) leaves every
        // coefficient bit-unchanged (adding 0.0 is exact).
        double k3 = 0.0, uf_src = 0.0;
        if (v.opts && v.opts->unsteady_friction != 0 &&
            v.opts->uf_k3 > 0.0 && v.uf_grad && !v.uf_grad->empty() &&
            std::fabs(qstar0 / ahat) >= 0.01 /* ft/s dead-band */) {
            k3 = v.opts->uf_k3;
            double gterm = 0.0;
            if (cl >= 0 && cr >= 0) {
                const auto ucl = static_cast<std::size_t>(cl);
                const auto ucr = static_cast<std::size_t>(cr);
                if (mesh.cell_conduit[ucl] == mesh.cell_conduit[ucr])
                    gterm = 0.5 * ((*v.uf_grad)[ucl] + (*v.uf_grad)[ucr]);
                // else: VJ splice — the frames may oppose; drop the
                // convective part rather than risk an energizing sign
                // (same rule as the cell stencil, one-sided there).
            } else if (cl >= 0) {
                gterm = (*v.uf_grad)[static_cast<std::size_t>(cl)];
            } else if (cr >= 0) {
                gterm = (*v.uf_grad)[static_cast<std::size_t>(cr)];
            }
            uf_src = dt * k3 * ahat * gterm;
            const double cap = 0.5 * std::fabs(qstar0);
            if (std::fabs(uf_src) > cap)
                uf_src = (uf_src > 0.0) ? cap : -cap;
        }

        F.alpha = 1.0 / (1.0 + dt * gamma + k3);
        F.cond  = F.alpha * dt * k::kGravity * ahat / Lf;
        F.qstar = qstar0 * (1.0 + k3) - uf_src;

        // Time-n heads / Dirichlet values of each side.
        const int nd = mesh.face_node[uf];
        F.eta_l = (cl >= 0) ? v.cell_eta[static_cast<std::size_t>(cl)]
                            : state.node_head[static_cast<std::size_t>(nd)];
        F.eta_r = (cr >= 0) ? v.cell_eta[static_cast<std::size_t>(cr)]
                            : state.node_head[static_cast<std::size_t>(nd)];
        F.dir_l = F.eta_l;
        F.dir_r = F.eta_r;
    }

    // ---- assemble + solve, with a bounded demote-and-re-solve loop --------
    // A folded junction whose solved head violates its physical ceiling (or
    // floor) is demoted to a Dirichlet row at the clamp and the system is
    // re-solved; the flux imbalance that creates lands in the carry ledger,
    // where settleAlgebraicNode books flooding/ponding exactly as today.
    for (int pass = 0; pass < 4; ++pass) {
        assembleRhs(v, dt);
        buildComponents();
        const int ncomp =
            static_cast<int>(comp_ptr_.empty() ? 0 : comp_ptr_.size() - 1);
        for (int cidx = 0; cidx < ncomp; ++cidx)
            solveComponent(cidx, v, dt);

        bool violated = false;
        for (int u = 0; u < nu; ++u) {
            const auto uu = static_cast<std::size_t>(u);
            if (unk_dirichlet_[uu]) continue;
            const int ent = unk_entity_[uu];
            if (ent < mesh.n_cells()) continue;
            const int n = ent - mesh.n_cells();
            const double lo = mesh.node_invert[static_cast<std::size_t>(n)];
            const double hi = nodeCeiling(mesh, n);
            if (unk_head_[uu] > hi) {
                unk_dirichlet_[uu] = 1;
                unk_dirvalue_[uu] = hi;
                violated = true;
            } else if (unk_head_[uu] < lo) {
                unk_dirichlet_[uu] = 1;
                unk_dirvalue_[uu] = lo;
                violated = true;
            }
        }
        if (!violated) break;
    }

    backSubstitute(v);

    // Folded junction heads: the solve owns them (updateNodes leaves
    // algebraic heads alone, and settleAlgebraicNode never recomputes one).
    for (int u = 0; u < nu; ++u) {
        const auto uu = static_cast<std::size_t>(u);
        const int ent = unk_entity_[uu];
        if (ent < mesh.n_cells()) continue;
        const auto un = static_cast<std::size_t>(ent - mesh.n_cells());
        state.node_head[un] =
            unk_dirichlet_[uu] ? unk_dirvalue_[uu] : unk_head_[uu];
    }
}

void PressurizedHeadSolver::assembleRhs(const PressurizedView& v, double dt) {
    (void)v;
    (void)dt;
    const int nu = static_cast<int>(unk_entity_.size());
    diag_.assign(static_cast<std::size_t>(nu), 0.0);
    rhs_.assign(static_cast<std::size_t>(nu), 0.0);

    for (int u = 0; u < nu; ++u) {
        const auto uu = static_cast<std::size_t>(u);
        if (unk_dirichlet_[uu]) {
            diag_[uu] = 1.0;
            rhs_[uu] = unk_dirvalue_[uu];
            continue;
        }
        diag_[uu] = unk_store_[uu];
        rhs_[uu] = unk_store_[uu] * unk_eta0_[uu] + unk_rhs_ext_[uu];
    }

    for (const ImpFace& F : imp_faces_) {
        if (F.cond <= 0.0) continue;
        // Base flux — the Casulli predictor (the implicit set is FULL
        // faces only; transition faces never reach this assembly).
        const double base = F.alpha * F.qstar;
        const bool ul = F.unk_l >= 0 &&
                        !unk_dirichlet_[static_cast<std::size_t>(F.unk_l)];
        const bool ur = F.unk_r >= 0 &&
                        !unk_dirichlet_[static_cast<std::size_t>(F.unk_r)];
        const double val_l =
            (F.unk_l >= 0)
                ? (ul ? 0.0
                      : unk_dirvalue_[static_cast<std::size_t>(F.unk_l)])
                : F.dir_l;
        const double val_r =
            (F.unk_r >= 0)
                ? (ur ? 0.0
                      : unk_dirvalue_[static_cast<std::size_t>(F.unk_r)])
                : F.dir_r;

        if (ul) {
            const auto lu = static_cast<std::size_t>(F.unk_l);
            diag_[lu] += F.cond;
            rhs_[lu] -= base;
            if (!ur) rhs_[lu] += F.cond * val_r;
        }
        if (ur) {
            const auto ru = static_cast<std::size_t>(F.unk_r);
            diag_[ru] += F.cond;
            rhs_[ru] += base;
            if (!ul) rhs_[ru] += F.cond * val_l;
        }
    }
}

void PressurizedHeadSolver::buildComponents() {
    const int nu = static_cast<int>(unk_entity_.size());
    // Component ids via union-find roots, compacted in ascending root order
    // for a deterministic solve order.
    comp_ptr_.clear();
    comp_unks_.clear();
    unk_comp_.assign(static_cast<std::size_t>(nu), -1);

    std::vector<int> roots;
    roots.reserve(static_cast<std::size_t>(nu));
    for (int u = 0; u < nu; ++u) roots.push_back(ufFind(u));

    std::vector<int> order(static_cast<std::size_t>(nu));
    for (int u = 0; u < nu; ++u) order[static_cast<std::size_t>(u)] = u;
    std::sort(order.begin(), order.end(), [&](int a, int b) {
        const int ra = roots[static_cast<std::size_t>(a)];
        const int rb = roots[static_cast<std::size_t>(b)];
        return (ra != rb) ? ra < rb : a < b;
    });

    int prev_root = -1;
    for (const int u : order) {
        const int r = roots[static_cast<std::size_t>(u)];
        if (r != prev_root) {
            comp_ptr_.push_back(static_cast<int>(comp_unks_.size()));
            prev_root = r;
        }
        unk_comp_[static_cast<std::size_t>(u)] =
            static_cast<int>(comp_ptr_.size()) - 1;
        comp_unks_.push_back(u);
    }
    comp_ptr_.push_back(static_cast<int>(comp_unks_.size()));

    // Global adjacency (CSR over unknowns) from the FULL both-unknown faces.
    const auto unu = static_cast<std::size_t>(nu);
    degree_.assign(unu, 0);
    for (const ImpFace& F : imp_faces_) {
        if (F.cond <= 0.0 || F.unk_l < 0 || F.unk_r < 0) continue;
        ++degree_[static_cast<std::size_t>(F.unk_l)];
        ++degree_[static_cast<std::size_t>(F.unk_r)];
    }
    adj_ptr_.assign(unu + 1, 0);
    for (std::size_t u2 = 0; u2 < unu; ++u2)
        adj_ptr_[u2 + 1] = adj_ptr_[u2] + degree_[u2];
    adj_idx_.assign(static_cast<std::size_t>(adj_ptr_[unu]), -1);
    adj_c_.assign(static_cast<std::size_t>(adj_ptr_[unu]), 0.0);
    std::vector<int> fill(unu, 0);
    for (const ImpFace& F : imp_faces_) {
        if (F.cond <= 0.0 || F.unk_l < 0 || F.unk_r < 0) continue;
        const auto lu = static_cast<std::size_t>(F.unk_l);
        const auto ru = static_cast<std::size_t>(F.unk_r);
        adj_idx_[static_cast<std::size_t>(adj_ptr_[lu] + fill[lu])] = F.unk_r;
        adj_c_[static_cast<std::size_t>(adj_ptr_[lu] + fill[lu])] = F.cond;
        ++fill[lu];
        adj_idx_[static_cast<std::size_t>(adj_ptr_[ru] + fill[ru])] = F.unk_l;
        adj_c_[static_cast<std::size_t>(adj_ptr_[ru] + fill[ru])] = F.cond;
        ++fill[ru];
    }
}

void PressurizedHeadSolver::solveComponent(int comp, const PressurizedView& v,
                                           double dt) {
    (void)v;
    (void)dt;
    const int b = comp_ptr_[static_cast<std::size_t>(comp)];
    const int e = comp_ptr_[static_cast<std::size_t>(comp) + 1];
    const int n = e - b;
    if (n <= 0) return;

    // Demoted (Dirichlet) rows solve trivially; edges to them were folded
    // into neighbours' RHS at assembly. They still occupy a slot here so the
    // indexing stays uniform.
    auto isDir = [&](int u) {
        return unk_dirichlet_[static_cast<std::size_t>(u)] != 0;
    };
    auto edgeActive = [&](int a2, int b2) { return !isDir(a2) && !isDir(b2); };

    if (n == 1) {
        const int u = comp_unks_[static_cast<std::size_t>(b)];
        const auto uu = static_cast<std::size_t>(u);
        unk_head_[uu] = (diag_[uu] > 0.0) ? rhs_[uu] / diag_[uu]
                                          : unk_eta0_[uu];
        return;
    }

    // Path test: within the component every unknown has adjacency degree ≤ 2
    // (counting only active edges) and the active edge count is n_active−1.
    int m_edges = 0;
    bool path = true;
    int endpoint = -1;
    for (int p = b; p < e; ++p) {
        const int u = comp_unks_[static_cast<std::size_t>(p)];
        if (isDir(u)) continue;
        int deg = 0;
        for (int a2 = adj_ptr_[static_cast<std::size_t>(u)];
             a2 < adj_ptr_[static_cast<std::size_t>(u) + 1]; ++a2)
            if (edgeActive(u, adj_idx_[static_cast<std::size_t>(a2)])) ++deg;
        m_edges += deg;
        if (deg > 2) path = false;
        if (deg <= 1 && endpoint < 0) endpoint = u;
    }
    m_edges /= 2;

    // Collect the active unknowns of this component.
    std::vector<int> act;
    act.reserve(static_cast<std::size_t>(n));
    for (int p = b; p < e; ++p) {
        const int u = comp_unks_[static_cast<std::size_t>(p)];
        if (!isDir(u)) act.push_back(u);
    }
    if (act.empty()) return;
    if (m_edges != static_cast<int>(act.size()) - 1) path = false;

    if (path && endpoint >= 0) {
        // Order the path from an endpoint and run Thomas — exact, O(n).
        path_order_.clear();
        path_order_.reserve(act.size());
        int prev = -1, cur = endpoint;
        while (cur >= 0) {
            path_order_.push_back(cur);
            int next = -1;
            for (int a2 = adj_ptr_[static_cast<std::size_t>(cur)];
                 a2 < adj_ptr_[static_cast<std::size_t>(cur) + 1]; ++a2) {
                const int w = adj_idx_[static_cast<std::size_t>(a2)];
                if (w == prev || !edgeActive(cur, w)) continue;
                next = w;
                break;
            }
            prev = cur;
            cur = next;
        }
        const int np = static_cast<int>(path_order_.size());
        if (np == static_cast<int>(act.size())) {
            thom_b_.assign(static_cast<std::size_t>(np), 0.0);
            thom_a_.assign(static_cast<std::size_t>(np), 0.0);
            thom_c_.assign(static_cast<std::size_t>(np), 0.0);
            thom_d_.assign(static_cast<std::size_t>(np), 0.0);
            for (int i = 0; i < np; ++i) {
                const int u = path_order_[static_cast<std::size_t>(i)];
                thom_b_[static_cast<std::size_t>(i)] =
                    diag_[static_cast<std::size_t>(u)];
                thom_d_[static_cast<std::size_t>(i)] =
                    rhs_[static_cast<std::size_t>(u)];
                if (i + 1 < np) {
                    const int w = path_order_[static_cast<std::size_t>(i) + 1];
                    double cval = 0.0;
                    for (int a2 = adj_ptr_[static_cast<std::size_t>(u)];
                         a2 < adj_ptr_[static_cast<std::size_t>(u) + 1]; ++a2)
                        if (adj_idx_[static_cast<std::size_t>(a2)] == w)
                            cval += adj_c_[static_cast<std::size_t>(a2)];
                    thom_c_[static_cast<std::size_t>(i)] = -cval;
                    thom_a_[static_cast<std::size_t>(i) + 1] = -cval;
                }
            }
            for (int i = 1; i < np; ++i) {
                const double w =
                    thom_a_[static_cast<std::size_t>(i)] /
                    thom_b_[static_cast<std::size_t>(i) - 1];
                thom_b_[static_cast<std::size_t>(i)] -=
                    w * thom_c_[static_cast<std::size_t>(i) - 1];
                thom_d_[static_cast<std::size_t>(i)] -=
                    w * thom_d_[static_cast<std::size_t>(i) - 1];
            }
            double x = thom_d_[static_cast<std::size_t>(np - 1)] /
                       thom_b_[static_cast<std::size_t>(np - 1)];
            unk_head_[static_cast<std::size_t>(
                path_order_[static_cast<std::size_t>(np - 1)])] = x;
            for (int i = np - 2; i >= 0; --i) {
                x = (thom_d_[static_cast<std::size_t>(i)] -
                     thom_c_[static_cast<std::size_t>(i)] * x) /
                    thom_b_[static_cast<std::size_t>(i)];
                unk_head_[static_cast<std::size_t>(
                    path_order_[static_cast<std::size_t>(i)])] = x;
            }
            return;
        }
        // Walk failed to cover the component (shouldn't happen for a true
        // path) — fall through to CG.
    }

    // Jacobi-preconditioned CG on the component — SPD by construction.
    const std::size_t na = act.size();
    cg_r_.assign(na, 0.0);
    cg_p_.assign(na, 0.0);
    cg_ap_.assign(na, 0.0);
    cg_z_.assign(na, 0.0);
    std::vector<int> lidx(unk_entity_.size(), -1);
    for (std::size_t i = 0; i < na; ++i)
        lidx[static_cast<std::size_t>(act[i])] = static_cast<int>(i);

    std::vector<double> x(na);
    for (std::size_t i = 0; i < na; ++i)
        x[i] = unk_eta0_[static_cast<std::size_t>(act[i])];

    auto matvec = [&](const std::vector<double>& in, std::vector<double>& out) {
        for (std::size_t i = 0; i < na; ++i) {
            const int u = act[i];
            double s = diag_[static_cast<std::size_t>(u)] * in[i];
            for (int a2 = adj_ptr_[static_cast<std::size_t>(u)];
                 a2 < adj_ptr_[static_cast<std::size_t>(u) + 1]; ++a2) {
                const int w = adj_idx_[static_cast<std::size_t>(a2)];
                if (isDir(w)) continue;
                const int lw = lidx[static_cast<std::size_t>(w)];
                if (lw < 0) continue;
                s -= adj_c_[static_cast<std::size_t>(a2)] *
                     in[static_cast<std::size_t>(lw)];
            }
            out[i] = s;
        }
    };

    matvec(x, cg_ap_);
    double bnorm = 0.0;
    for (std::size_t i = 0; i < na; ++i) {
        cg_r_[i] = rhs_[static_cast<std::size_t>(act[i])] - cg_ap_[i];
        bnorm += rhs_[static_cast<std::size_t>(act[i])] *
                 rhs_[static_cast<std::size_t>(act[i])];
    }
    const double tol2 = std::max(1.0e-24 * bnorm, 1.0e-28);
    double rz = 0.0;
    for (std::size_t i = 0; i < na; ++i) {
        cg_z_[i] = cg_r_[i] / diag_[static_cast<std::size_t>(act[i])];
        rz += cg_r_[i] * cg_z_[i];
        cg_p_[i] = cg_z_[i];
    }
    const int itmax = 200 + 2 * static_cast<int>(na);
    for (int it = 0; it < itmax; ++it) {
        double r2 = 0.0;
        for (std::size_t i = 0; i < na; ++i) r2 += cg_r_[i] * cg_r_[i];
        if (r2 <= tol2) break;
        matvec(cg_p_, cg_ap_);
        double pap = 0.0;
        for (std::size_t i = 0; i < na; ++i) pap += cg_p_[i] * cg_ap_[i];
        if (!(pap > 0.0)) break;
        const double a2 = rz / pap;
        for (std::size_t i = 0; i < na; ++i) {
            x[i] += a2 * cg_p_[i];
            cg_r_[i] -= a2 * cg_ap_[i];
        }
        double rz_new = 0.0;
        for (std::size_t i = 0; i < na; ++i) {
            cg_z_[i] = cg_r_[i] / diag_[static_cast<std::size_t>(act[i])];
            rz_new += cg_r_[i] * cg_z_[i];
        }
        const double beta = (rz > 0.0) ? rz_new / rz : 0.0;
        rz = rz_new;
        for (std::size_t i = 0; i < na; ++i)
            cg_p_[i] = cg_z_[i] + beta * cg_p_[i];
    }
    for (std::size_t i = 0; i < na; ++i)
        unk_head_[static_cast<std::size_t>(act[i])] = x[i];
}

void PressurizedHeadSolver::backSubstitute(const PressurizedView& v) {
    auto headOf = [&](int unk, double dirval) {
        if (unk < 0) return dirval;
        const auto uu = static_cast<std::size_t>(unk);
        return unk_dirichlet_[uu] ? unk_dirvalue_[uu] : unk_head_[uu];
    };
    for (const ImpFace& F : imp_faces_) {
        if (F.cond <= 0.0) continue;
        const auto uf = static_cast<std::size_t>(F.face);
        const double base = F.alpha * F.qstar;
        const double hl = headOf(F.unk_l, F.dir_l);
        const double hr = headOf(F.unk_r, F.dir_r);
        const double q = base - F.cond * (hr - hl);
        v.f_mass[uf] = q;
        // Keep the species-facing copies coherent: transport rides the SAME
        // mass flux the water used, and the contact sign selects its upwind
        // side (plan §3.2).
        v.f_flux[uf].mass = q;
        v.f_flux[uf].sstar = q;
        v.f_sstar[uf] = q;
    }
}

// ---------------------------------------------------------------------------
// finalizeCells — slave the pressurized cells' momentum to the face solve
// ---------------------------------------------------------------------------

void PressurizedHeadSolver::finalizeCells(const PressurizedView& v) {
    if (imp_faces_.empty()) return;
    const NetworkMeshData& mesh = *v.mesh;
    NetworkStateData& state = *v.state;
    const int nc = mesh.n_cells();
    for (int c = 0; c < nc; ++c) {
        const auto uc = static_cast<std::size_t>(c);
        if (cell_unk_[uc] < 0 || !cell_press_[uc]) continue;
        const int faces[2] = {mesh.cell_face0[uc], mesh.cell_face1[uc]};
        const std::int8_t sides[2] = {mesh.cell_side0[uc], mesh.cell_side1[uc]};
        double q_thru = 0.0;
        for (int e2 = 0; e2 < 2; ++e2) {
            const auto uf = static_cast<std::size_t>(faces[e2]);
            const double fa = v.f_mass[uf];
            q_thru += (sides[e2] == 0)
                          ? static_cast<double>(mesh.face_dir_l[uf]) * fa
                          : static_cast<double>(mesh.face_dir_r[uf]) * fa;
        }
        const double a = state.cell_a[uc];
        const double q = 0.5 * q_thru;
        state.cell_q[uc] = q;
        v.cell_u[uc] = (a > k::kDryArea) ? q / a : 0.0;
    }
}

} // namespace openswmm::fv
