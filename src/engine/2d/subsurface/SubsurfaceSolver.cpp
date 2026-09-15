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
 * @file SubsurfaceSolver.cpp
 * @brief The two-zone groundwater kernel (G-steps 3, 6-12, 14, 16).
 *
 * @section sy Specific yield — one derivation, three closures
 *
 * Write total water per unit area as `W = θ_s·h_g + S_col`, with `S_col` the
 * unsaturated column's storage. Each closure gives the same shape:
 *
 * | closure | `S_col` | saturated equation |
 * |---|---|---|
 * | CLOSED_FORM | `hᵤ`, its own ODE | `θ_s·ḣ_g = q₀ + lat/A − deep − node/A` |
 * | SIGMA       | `Σ θ_j·L·Δσ`      | `(θ_s − θ_bot)·ḣ_g = q₀_phys + …` |
 * | ENSLAVED    | `hᵤ*(L)`, algebraic | `(θ_s − θ(ψ=L))·ḣ_g = q⁺ + …` |
 *
 * The SIGMA row is the one the plan text writes with `θ_s`. It is wrong by
 * exactly the handover: the column's bottom flux is `f_bot = q₀_phys −
 * θ_bot·L̇`, and `L̇ = −ḣ_g`, so `θ_s·ḣ_g = f_bot` rearranges to
 * `(θ_s − θ_bot)·ḣ_g = q₀_phys`. Same equation, but only the second form can
 * be written down without also tracking `f_bot`, and only the second form
 * conserves once the column retains water above a falling table.
 *
 * ENSLAVED falls out of the same algebra: `S_col = hᵤ*(L)` gives
 * `Ṡ_col = d(hᵤ*)/dL · L̇ = −θ(ψ=L)·ḣ_g`, since `d(hᵤ*)/dL = θ(L)` by the
 * fundamental theorem. So the enslaved column contributes its top water
 * content as a storage debit — no lag, no separate state, and `q⁺` drives the
 * table directly. This is the reduction plan step 14 asks for, and it is one
 * line of code rather than a separate branch of physics.
 *
 * @section split Why the split is ordered the way it is
 *
 * `fireCell` runs: q₀ → saturated update → clamp → column sweep at the
 * CLAMPED `L̇` → apply the column's overflow/deficit → Dunne. Deriving `L̇`
 * from the clamped `h_g` is what makes the clamp harmless: the column and the
 * saturated zone always agree on how far the table actually moved, so no
 * water is created at `h_g = z_s` or destroyed at `h_g = 0`.
 *
 * @ingroup engine_2d
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include "SubsurfaceSolver.hpp"

#include "../data/MeshData.hpp"
#include "../data/SolverOptions2D.hpp"
#include "../data/SurfaceStateData.hpp"
#include "../solver/InertialEdges.hpp"
#include "../../data/NodeData.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace openswmm::twoD {

namespace {

constexpr double kTiny     = 1.0e-12;
/// Minimum drainable porosity. Guards `Sy` against a column that has
/// saturated to `θ_s` at the table (which is physically where it should be —
/// the specific yield genuinely vanishes there, and the table stops moving).
constexpr double kSyFloor  = 1.0e-3;
/// Availability share a single face may take of its donor cell per step —
/// the surface solver's `exchange_beta` idiom, applied to Darcy.
constexpr double kFaceShare = 0.5;

double harmonic(double a, double b) noexcept {
    if (a <= 0.0 || b <= 0.0) return 0.0;
    return 2.0 * a * b / (a + b);
}

}  // namespace

// ---------------------------------------------------------------------------
// initialize
// ---------------------------------------------------------------------------

soil::Params SubsurfaceSolver::paramsOf(int i) const noexcept {
    const auto u = static_cast<std::size_t>(i);
    soil::Params p;
    p.law     = static_cast<SoilChar>(state_.soil_char[u]);
    p.Ks      = state_.Ks[u];
    p.theta_s = state_.theta_s[u];
    p.theta_r = state_.theta_r[u];
    p.alpha   = state_.alpha[u];
    p.psi_b   = state_.psi_b[u];
    p.lambda  = state_.lambda[u];
    p.vg_n    = state_.vg_n[u];
    p.vg_L    = state_.vg_L[u];
    return p;
}

void SubsurfaceSolver::resolveRows(const MeshData& mesh,
                                   const GwUnitFactors& uf,
                                   SubsurfaceConfig& cfg,
                                   std::vector<std::string>& warnings) {
    // The authored config stays in the user's units for its whole life; SI
    // exists only here, in the state. See GwUnitFactors' note — this split is
    // what makes the writer a verbatim echo and double conversion
    // unreachable.
    const int n = state_.n_cells;

    // `* < TAG < CELL` by scope, then by authored order within a scope — the
    // same precedence the 2D infiltration and roughness sections use, so a
    // user who has learned one has learned all of them.
    for (int pass = 0; pass < 3; ++pass) {
        for (const auto& r : cfg.rows) {
            if (r.scope != pass) continue;
            for (int i = 0; i < n; ++i) {
                if (pass == 1) {
                    if (i >= static_cast<int>(mesh.tri_tag.size()) ||
                        mesh.tri_tag[static_cast<std::size_t>(i)] != r.tag)
                        continue;
                } else if (pass == 2) {
                    if (r.cell != i) continue;
                }
                const auto u = static_cast<std::size_t>(i);
                state_.Ks[u]      = r.Ks * uf.rate;
                state_.zs[u]      = r.zs * uf.length;
                state_.theta_s[u] = r.theta_s;      // dimensionless
                state_.theta_r[u] = r.theta_r;      // dimensionless
                state_.alpha[u]   = r.alpha * uf.inv_len;
                state_.psi_b[u]   = r.psi_b * uf.length;
                state_.lambda[u]  = r.lambda;       // dimensionless
                state_.vg_n[u]    = r.vg_n;         // dimensionless
                state_.vg_L[u]    = r.vg_L;         // dimensionless
                state_.c_loss[u]  = r.c_loss * uf.rate;
                state_.soil_char[u] = static_cast<int8_t>(
                    r.soil_char_set ? r.soil_char : options_.soil_char);
                state_.closure[u] = static_cast<int8_t>(
                    r.closure_set ? r.closure : options_.closure);
                if (r.hg0 >= 0.0)
                    state_.hg[u] = std::min(r.hg0 * uf.length, state_.zs[u]);
            }
        }
    }

    for (int i = 0; i < n; ++i) {
        const auto u = static_cast<std::size_t>(i);
        state_.area[u] = (i < static_cast<int>(mesh.tri_area.size()))
                             ? mesh.tri_area[u] : 1.0;
        const double zc = (i < static_cast<int>(mesh.tri_cz.size()))
                              ? mesh.tri_cz[u] : 0.0;
        state_.z_bed[u] = zc - state_.zs[u];
        if (state_.theta_r[u] >= state_.theta_s[u]) {
            state_.theta_r[u] = 0.5 * state_.theta_s[u];
            warnings.emplace_back(
                "2D aquifer: THETA_R >= THETA_S on a cell; clamped to "
                "THETA_S/2.");
        }
    }
}

void SubsurfaceSolver::resolveClosures(std::vector<std::string>& warnings) {
    // Step 6. AUTO reads the dimensionless group αL of the cell's INITIAL
    // column. The thresholds are the plan's: a column that equilibrates far
    // faster than the table moves is enslaved; one that does not equilibrate
    // at all within the quasi-steady assumption gets the real column.
    long n_ens = 0, n_cf = 0, n_sig = 0;
    for (int i = 0; i < state_.n_cells; ++i) {
        const auto u = static_cast<std::size_t>(i);
        auto c = static_cast<GwClosure>(state_.closure[u]);
        if (c == GwClosure::AUTO) {
            const double L  = std::max(state_.zs[u] - state_.hg[u], kTiny);
            const double aL = soil::alphaL(paramsOf(i), L);
            if (aL < 1.0)                              c = GwClosure::ENSLAVED;
            else if (aL < 5.0 || options_.force_closed_form)
                                                       c = GwClosure::CLOSED_FORM;
            else                                       c = GwClosure::SIGMA;
            state_.closure[u] = static_cast<int8_t>(c);
        }
        switch (c) {
            case GwClosure::ENSLAVED: ++n_ens; break;
            case GwClosure::SIGMA:    ++n_sig; break;
            default:                  ++n_cf;  break;
        }
    }
    if (n_sig + n_cf + n_ens > 0) {
        char buf[192];
        std::snprintf(buf, sizeof buf,
                      "2D aquifer closures resolved: %ld ENSLAVED, %ld "
                      "CLOSED_FORM, %ld SIGMA.",
                      n_ens, n_cf, n_sig);
        warnings.emplace_back(buf);
    }
}

std::string SubsurfaceSolver::initialize(const MeshData& mesh,
                                         const InertialEdges& edges,
                                         const SolverOptions2D& opts,
                                         const GwUnitFactors& uf, int n_nodes,
                                         SubsurfaceConfig& cfg,
                                         std::vector<std::string>& warnings) {
    state_ = SubsurfaceState{};
    if (cfg.empty()) return {};

    options_   = cfg.options;
    node_beds_ = cfg.node_beds;
    mesh_  = &mesh;
    edges_ = &edges;
    opts_  = &opts;

    const int n = mesh.n_cells();
    if (n <= 0) return "2D aquifer: [2D_AQUIFER] authored but the mesh is empty.";
    if (options_.m_layers < 2 || options_.m_layers > 128)
        return "2D aquifer: M_LAYERS must be in [2, 128].";

    state_.resize(n, options_.m_layers);
    resolveRows(mesh, uf, cfg, warnings);
    resolveClosures(warnings);

    // Seed each closure-B column to hydrostatic equilibrium above its table.
    // Closure A's `hu` gets the same equilibrium, so the two closures start
    // from the identical water content and the closure-ladder benchmark
    // (step 18) compares trajectories, not initial conditions.
    for (int i = 0; i < n; ++i) {
        const auto u = static_cast<std::size_t>(i);
        const double L = std::max(state_.zs[u] - state_.hg[u], 0.0);
        const soil::Params p = paramsOf(i);
        if (static_cast<GwClosure>(state_.closure[u]) == GwClosure::SIGMA) {
            sigma::seedHydrostatic(p, &state_.theta_sigma[u], state_.m_layers,
                                   n, L);
            state_.hu[u] = sigma::columnStorage(&state_.theta_sigma[u],
                                                state_.m_layers, n, L);
        } else {
            state_.hu[u] = soil::equilibriumStorage(p, L);
        }
    }

    // Lateral topology. PER_SUBCATCH has none — degenerate cells, zero
    // lateral flux, and the identical code path everywhere else (§8).
    const int ne = options_.per_subcatch ? 0 : edges.ne;
    state_.eacc_L.assign(static_cast<std::size_t>(ne), 0.0);
    state_.eacc_R.assign(static_cast<std::size_t>(ne), 0.0);
    face_tier_.assign(static_cast<std::size_t>(ne), 0);
    cell_nface_.assign(static_cast<std::size_t>(n), 0);
    if (ne > 0 && static_cast<int>(edges.cell_ptr.size()) == n + 1) {
        for (int i = 0; i < n; ++i)
            cell_nface_[static_cast<std::size_t>(i)] =
                edges.cell_ptr[static_cast<std::size_t>(i) + 1] -
                edges.cell_ptr[static_cast<std::size_t>(i)];
    }

    // Node beds: resolve each coupled node onto its containing cell. A bed
    // authored for a node outside the mesh is a warning, not an error — the
    // 1D node keeps working, it simply has no aquifer under it.
    state_.nacc.assign(node_beds_.size(), 0.0);
    node_exchange_vol_.assign(static_cast<std::size_t>(std::max(0, n_nodes)),
                              0.0);
    {
        // `node_beds_` is the solver's own copy, so converting it in place
        // leaves the authored config — and therefore the writer — untouched.
        for (auto& b : node_beds_) {
            if (b.cell < 0 || b.cell >= n) {
                b.cell = -1;
                warnings.emplace_back(
                    "2D aquifer: [2D_AQUIFER_NODE] row has no containing mesh "
                    "cell; the node exchanges with nothing.");
                continue;
            }
            b.Kc   *= uf.rate;
            b.dC   *= uf.length;
            b.area *= uf.area;
            if (b.area <= 0.0)
                b.area = state_.area[static_cast<std::size_t>(b.cell)];
        }
    }

    pending_flag_.assign(static_cast<std::size_t>(n), 0);
    pending_surface_.clear();
    accumulators_pending_ = false;
    state_.active = true;
    // Seed the tier lists so the kernel is usable BEFORE the first
    // syncAndRebuild. dt0 is not known yet, so everything starts on the
    // finest rung — conservative, and it means a solver that is initialized
    // and then immediately marched does physics rather than silently nothing.
    // (It silently did nothing, and gates 6 and 7 are what said so.)
    assignTiers(0.0, std::clamp(opts.lts_tiers, 1, 8));
    state_.led_init_storage = state_.storage();
    return {};
}

// ---------------------------------------------------------------------------
// stability and tiering (steps 10, 11)
// ---------------------------------------------------------------------------

void SubsurfaceSolver::refreshDtCell(const MeshData& mesh,
                                     const InertialEdges& edges) {
    if (!state_.active) return;
    const int n  = state_.n_cells;
    const int ne = static_cast<int>(state_.eacc_L.size());
    const bool has_lat = ne > 0 &&
                         static_cast<int>(edges.cell_ptr.size()) == n + 1;

    for (int i = 0; i < n; ++i) {
        const auto u = static_cast<std::size_t>(i);
        const double A  = std::max(state_.area[u], kTiny);
        const double zs = state_.zs[u];
        const double L  = std::max(zs - state_.hg[u], kTiny);
        const soil::Params p = paramsOf(i);

        // --- Δt_g: explicit diffusion on the saturated head ---------------
        // A cell's transmissivity sum over its faces; the drainable storage
        // A·Sy is the capacitance. `T` uses the WETTER of the pair with a
        // floor, so an empty aquifer does not report an infinite step and
        // then take one.
        double sumT = 0.0;
        if (has_lat) {
            const int b0 = edges.cell_ptr[u];
            const int b1 = edges.cell_ptr[u + 1];
            for (int k = b0; k < b1; ++k) {
                const int e = edges.cell_edge[static_cast<std::size_t>(k)];
                const auto eu = static_cast<std::size_t>(e);
                const int  j  = (edges.cL[eu] == i) ? edges.cR[eu] : edges.cL[eu];
                const auto ju = static_cast<std::size_t>(j);
                const double Ke = harmonic(state_.Ks[u], state_.Ks[ju]);
                const double Te = std::max({state_.hg[u], state_.hg[ju],
                                            0.01 * zs});
                sumT += Ke * Te * edges.xi[eu] * edges.inv_dx[eu];
            }
        }
        // Deep loss is a linear sink with rate c_loss/z_s; the node exchange
        // conductance is folded in where a bed sits on this cell.
        double lin = state_.c_loss[u] / std::max(zs, kTiny) * A;
        for (const auto& b : node_beds_) {
            if (b.cell != i) continue;
            lin += (b.Kc > 0.0 && b.dC > 0.0)
                       ? b.Kc * b.area / b.dC
                       : state_.Ks[u] * b.area / std::max(0.5 * zs, kTiny);
        }
        const double Sy  = std::max(state_.theta_s[u] - state_.theta_r[u],
                                    kSyFloor);
        const double den = sumT + lin;
        double dt_g = 1.0e30;
        if (den > kTiny) dt_g = options_.c_gw * A * Sy / den;

        // --- Δt_u: the column's own bound --------------------------------
        double dt_u = 1.0e30;
        const auto cl = static_cast<GwClosure>(state_.closure[u]);
        if (cl == GwClosure::SIGMA) {
            // L̇ is unknown before the step; use the previous firing's table
            // velocity implied by q0_last, which is the term that actually
            // drives it.
            const double Ldot = -state_.q0_last[u] / Sy;
            dt_u = sigma::columnDtLimit(p, &state_.theta_sigma[u],
                                        state_.m_layers, n, L, Ldot,
                                        options_.c_col,
                                        options_.capillary_diff);
        } else if (cl == GwClosure::CLOSED_FORM) {
            // The closed form is a relaxation ẏ = −C·(y − y*); its stiffness
            // is C = −∂q₀/∂hᵤ. Taken numerically so every soil law is
            // covered by one expression rather than four differentiations.
            const double hu = state_.hu[u];
            const double d  = std::max(1.0e-6 * std::max(hu, 1.0e-3), 1.0e-9);
            const double C  = (soil::rechargeQ0(p, L, hu) -
                               soil::rechargeQ0(p, L, hu + d)) / d;
            if (C > kTiny) dt_u = options_.c_col / C;
        }
        // ENSLAVED has no unsaturated state, so no unsaturated step bound —
        // that is the whole point of the reduction.

        state_.dt_cell[u] = std::min(dt_g, dt_u);
    }
}

int SubsurfaceSolver::requiredTiers(double dt0) const noexcept {
    if (!state_.active || dt0 <= 0.0) return 1;
    double dtmax = dt0;
    for (int i = 0; i < state_.n_cells; ++i)
        dtmax = std::max(dtmax, state_.dt_cell[static_cast<std::size_t>(i)]);
    if (!std::isfinite(dtmax) || dtmax <= dt0) return 1;
    // K such that 2^(K−1)·dt0 >= dt_max.
    const int k = static_cast<int>(std::ceil(std::log2(dtmax / dt0))) + 1;
    return std::max(1, k);
}

void SubsurfaceSolver::assignTiers(double dt0, int n_tiers) {
    if (!state_.active) return;
    const int K = std::max(1, n_tiers);
    cells_by_tier_.assign(static_cast<std::size_t>(K), {});
    faces_by_tier_.assign(static_cast<std::size_t>(K), {});
    tier_firings_.assign(static_cast<std::size_t>(K), 0);

    for (int i = 0; i < state_.n_cells; ++i) {
        const auto u = static_cast<std::size_t>(i);
        int t = 0;
        if (dt0 > 0.0 && std::isfinite(state_.dt_cell[u])) {
            const double ratio = state_.dt_cell[u] / dt0;
            // std::ilogb(ratio) is exactly floor(log2(ratio)) for finite
            // positive ratios and is what the surface marcher uses — the two
            // ladders must agree on the arithmetic or a cell can sit one rung
            // off its twin for no physical reason.
            t = (ratio >= 1.0) ? std::ilogb(ratio) : 0;
        }
        // G-A: the tier is `min(Δt_g, Δt_u)` and nothing else. A cell with
        // node exchange, infiltration or Dunne is NOT pinned to tier 0 — the
        // cross-domain volume accumulates and is gathered when this cell
        // fires. Pinning is the surface twin's business.
        t = std::clamp(t, 0, K - 1);
        state_.tier[u] = static_cast<uint8_t>(t);
        cells_by_tier_[static_cast<std::size_t>(t)].push_back(i);
    }

    // A face fires at the FINER of its two cells — the standard LTS face
    // rule. The finer cell then gathers a full-cadence flux and the coarser
    // one gathers the accumulated sum, which is where G-B's ±Δ pair earns
    // its keep.
    const int ne = static_cast<int>(state_.eacc_L.size());
    for (int e = 0; e < ne; ++e) {
        const auto eu = static_cast<std::size_t>(e);
        const int tL = state_.tier[static_cast<std::size_t>(edges_->cL[eu])];
        const int tR = state_.tier[static_cast<std::size_t>(edges_->cR[eu])];
        const int t  = std::min(tL, tR);
        face_tier_[eu] = static_cast<uint8_t>(t);
        faces_by_tier_[static_cast<std::size_t>(t)].push_back(e);
    }
}

// ---------------------------------------------------------------------------
// lateral Darcy (step 9)
// ---------------------------------------------------------------------------

void SubsurfaceSolver::fireGwFaces(int tier, double dt) {
    if (!state_.active || !tierHasFaces(tier)) return;
    const auto& list = faces_by_tier_[static_cast<std::size_t>(tier)];
    const InertialEdges& E = *edges_;

    for (const int e : list) {
        const auto eu = static_cast<std::size_t>(e);
        const auto l  = static_cast<std::size_t>(E.cL[eu]);
        const auto r  = static_cast<std::size_t>(E.cR[eu]);

        const double HL = state_.z_bed[l] + state_.hg[l];
        const double HR = state_.z_bed[r] + state_.hg[r];
        const double dH = HL - HR;
        if (dH == 0.0) continue;

        // Upwind transmissivity: the DONOR cell's saturated thickness. Using
        // the mean here is the classic way to make a drying aquifer pump
        // water it does not have.
        const std::size_t don = (dH > 0.0) ? l : r;
        const double T = state_.hg[don];
        if (T <= kTiny) continue;

        const double K = harmonic(state_.Ks[l], state_.Ks[r]);
        if (K <= 0.0) continue;

        double Q = K * T * E.xi[eu] * E.inv_dx[eu] * dH;   // m³/s, + is L→R

        // Positivity: no face may take more than its share of the donor's
        // drainable water in one step. The share is split across the donor's
        // own faces so a cell with many wet neighbours cannot be emptied
        // several times over within one firing.
        const double Sy = std::max(state_.theta_s[don] - state_.theta_r[don],
                                   kSyFloor);
        const int nf = std::max(1, cell_nface_[don]);
        const double cap = kFaceShare * T * Sy * state_.area[don] /
                           (static_cast<double>(nf) * std::max(dt, kTiny));
        if (std::abs(Q) > cap) Q = (Q > 0.0) ? cap : -cap;

        const double vol = Q * dt;
        state_.eacc_L[eu] -= vol;   // leaves cL
        state_.eacc_R[eu] += vol;   // enters cR
    }
    accumulators_pending_ = true;
}

double SubsurfaceSolver::gatherLateral(int i) noexcept {
    if (state_.eacc_L.empty()) return 0.0;
    const InertialEdges& E = *edges_;
    const auto u = static_cast<std::size_t>(i);
    if (u + 1 >= E.cell_ptr.size()) return 0.0;
    double v = 0.0;
    const int b0 = E.cell_ptr[u];
    const int b1 = E.cell_ptr[u + 1];
    for (int k = b0; k < b1; ++k) {
        const auto ku = static_cast<std::size_t>(k);
        const auto eu = static_cast<std::size_t>(E.cell_edge[ku]);
        if (E.cell_sign[ku] > 0) { v += state_.eacc_L[eu]; state_.eacc_L[eu] = 0.0; }
        else                     { v += state_.eacc_R[eu]; state_.eacc_R[eu] = 0.0; }
    }
    return v;
}

// ---------------------------------------------------------------------------
// node exchange (step 8)
// ---------------------------------------------------------------------------

void SubsurfaceSolver::resetNodeExchangeVolumes() noexcept {
    std::fill(node_exchange_vol_.begin(), node_exchange_vol_.end(), 0.0);
}

void SubsurfaceSolver::sampleNodeExchange(const NodeData* nodes, double dt) {
    if (!state_.active || node_beds_.empty() || nodes == nullptr) return;
    const double l12 = opts_ ? opts_->len_1d_to_2d : 1.0;

    for (std::size_t b = 0; b < node_beds_.size(); ++b) {
        const auto& bed = node_beds_[b];
        if (bed.cell < 0) continue;
        const auto ni = static_cast<std::size_t>(bed.node);
        if (ni >= nodes->invert_elev.size()) continue;
        const auto ci = static_cast<std::size_t>(bed.cell);

        // 1D heads are project units; the 2D module is metres throughout.
        const double h_pipe = (nodes->invert_elev[ni] + nodes->depth[ni]) * l12;
        const double h_gw   = state_.z_bed[ci] + state_.hg[ci];

        // MODFLOW-River conductance. With a semi-confining bed the bed
        // dominates (config b); without one the aquifer's own conductivity
        // over half the soil column is the characteristic path (config a).
        const double cond = (bed.Kc > 0.0 && bed.dC > 0.0)
            ? bed.Kc * bed.area / bed.dC
            : state_.Ks[ci] * bed.area /
                  std::max(0.5 * state_.zs[ci], kTiny);

        double Q = cond * (h_gw - h_pipe);   // m³/s, + out of the aquifer
        if (Q > 0.0) {
            // Cap an aquifer→pipe drain at the cell's drainable water.
            const double Sy = std::max(
                state_.theta_s[ci] - state_.theta_r[ci], kSyFloor);
            const double avail = state_.hg[ci] * Sy * state_.area[ci];
            Q = std::min(Q, kFaceShare * avail / std::max(dt, kTiny));
        }
        if (Q == 0.0) continue;

        const double vol = Q * dt;
        state_.nacc[b] += vol;                       // gathered at the GW firing
        if (ni < node_exchange_vol_.size())
            node_exchange_vol_[ni] += vol;           // flushed by the router
    }
    accumulators_pending_ = true;
}

double SubsurfaceSolver::gatherNode(int i) noexcept {
    double v = 0.0;
    for (std::size_t b = 0; b < node_beds_.size(); ++b) {
        if (node_beds_[b].cell != i) continue;
        v += state_.nacc[b];
        state_.nacc[b] = 0.0;
    }
    return v;
}

// ---------------------------------------------------------------------------
// surface ↔ subsurface (step 11b)
// ---------------------------------------------------------------------------

void SubsurfaceSolver::bookInfiltrationFromSurface(int cell,
                                                   double vol_m3) noexcept {
    if (!state_.active || cell < 0 || cell >= state_.n_cells) return;
    state_.xacc_from_surface[static_cast<std::size_t>(cell)] += vol_m3;
    accumulators_pending_ = true;
}

void SubsurfaceSolver::markPendingSurface(int i) noexcept {
    const auto u = static_cast<std::size_t>(i);
    if (pending_flag_[u]) return;
    pending_flag_[u] = 1;
    pending_surface_.push_back(i);
}

double SubsurfaceSolver::takeToSurface(int cell) noexcept {
    // Called from the surface marcher's per-cell loop, which is parallel, so
    // this touches only cell-indexed slots. The pending LIST is compacted
    // serially in compactPending() instead of being erased from here — an
    // O(n) erase inside the hot cell loop is exactly the kind of thing that
    // does not show up until a large deck.
    if (!state_.active || cell < 0 || cell >= state_.n_cells) return 0.0;
    const auto u = static_cast<std::size_t>(cell);
    const double v = state_.xacc_to_surface[u];
    state_.xacc_to_surface[u] = 0.0;
    pending_flag_[u] = 0;
    return v;
}

void SubsurfaceSolver::compactPending() noexcept {
    if (pending_surface_.empty()) return;
    pending_surface_.erase(
        std::remove_if(pending_surface_.begin(), pending_surface_.end(),
                       [this](int c) {
                           return pending_flag_[static_cast<std::size_t>(c)] == 0;
                       }),
        pending_surface_.end());
}

// ---------------------------------------------------------------------------
// the cell firing (steps 3, 7, 12, 14)
// ---------------------------------------------------------------------------

void SubsurfaceSolver::fireCell(int i, double dt, SurfaceStateData& surf) {
    const auto u = static_cast<std::size_t>(i);
    const int  n = state_.n_cells;
    const double A  = std::max(state_.area[u], kTiny);
    const double zs = state_.zs[u];
    const double ts = state_.theta_s[u];
    const soil::Params p = paramsOf(i);
    const auto cl = static_cast<GwClosure>(state_.closure[u]);

    const double hg0 = state_.hg[u];
    const double L0  = std::max(zs - hg0, 0.0);

    // --- gather the accumulated cross-cadence volumes ---------------------
    const double lat_vol  = gatherLateral(i);                 // m³, + in
    const double node_vol = gatherNode(i);                    // m³, + OUT
    double infil_vol = state_.xacc_from_surface[u];           // m³, + in
    state_.xacc_from_surface[u] = 0.0;

    const double q_in = infil_vol / (A * dt);                 // m/s at the top
    state_.qplus_last[u] = q_in;
    state_.led_infil_in += infil_vol;

    // --- ET demand (step 7) -----------------------------------------------
    // BOUNDARY_ET removes water from the unsaturated column's top with a
    // Feddes stress from its mean suction. The retired behaviour was a hard
    // `if (infil <= 0)` gate; the smooth multiplier is what §7 asks for, and
    // it is why a drying column stops taking ET gradually instead of at a
    // step.
    double q_et = 0.0;
    const bool et_boundary = (options_.gw_et == "BOUNDARY_ET" ||
                              options_.gw_et == "BOTH");
    const bool et_caprise  = (options_.gw_et == "CAPILLARY_RISE" ||
                              options_.gw_et == "BOTH");
    if (et_boundary && u < surf.evap_rate.size()) {
        const double demand = std::max(surf.evap_rate[u], 0.0);
        if (demand > 0.0) {
            const double Se = (state_.hu[u] > 0.0 && L0 > kTiny)
                ? std::clamp((state_.hu[u] / L0 - state_.theta_r[u]) /
                                 std::max(ts - state_.theta_r[u], kTiny),
                             1.0e-6, 1.0)
                : 1.0e-6;
            const double psi   = soil::suctionAtSaturation(p, Se);
            const double psi_w = 150.0;   // ≈ −15 bar wilting point, in metres
            q_et = demand * soil::feddesStress(psi, psi_w);
        }
    }

    // --- 1. recharge across the table -------------------------------------
    double q0 = 0.0;      // m/s, + down
    double Sy = std::max(ts - state_.theta_r[u], kSyFloor);
    double theta_bot = state_.theta_r[u];

    if (cl == GwClosure::SIGMA) {
        const auto bot = static_cast<std::size_t>(state_.m_layers - 1) *
                             static_cast<std::size_t>(n) + u;
        theta_bot = state_.theta_sigma[bot];
        // The plan writes θ_s here. See the file header: with the handover
        // explicit, θ_s − θ_bot is what conserves, and it IS specific yield.
        Sy = std::max(ts - theta_bot, kSyFloor);
        // The physical Darcy flux across the table is the bottom layer's
        // gravity drainage. It is passed INTO the sweep as `q0_phys` and
        // comes back inside `f_bot` together with the handover, which is why
        // the saturated update above books `q0` and not `f_bot`.
        q0 = soil::conductivity(p, soil::suctionAtSaturation(
                 p, std::clamp((theta_bot - state_.theta_r[u]) /
                                   std::max(ts - state_.theta_r[u], kTiny),
                               1.0e-6, 1.0)));
    } else if (cl == GwClosure::ENSLAVED) {
        // hᵤ = hᵤ*(L) algebraically ⇒ the storage debit is θ(ψ=L), the
        // equilibrium water content at the SURFACE. `q⁺` then drives the
        // table with no unsaturated lag at all.
        const double theta_top = soil::waterContent(p, L0);
        Sy = std::max(ts - theta_top, kSyFloor);
        q0 = q_in - q_et;
    } else {
        q0 = soil::rechargeQ0(p, std::max(L0, kTiny), state_.hu[u]);
        // CONSISTENCY, not taste: `Sy` and the handover slab content this
        // branch uses below must sum to θ_s, or the two zones disagree about
        // how much water a moving table carries and the cell leaks at a rate
        // proportional to |ḣ_g|. The bulk store's own mean content is the
        // self-consistent choice — and it reduces to the textbook θ_s − θ_r
        // once the column has drained.
        theta_bot = std::clamp(state_.hu[u] / std::max(L0, kTiny),
                               state_.theta_r[u], ts);
        Sy = std::max(ts - theta_bot, kSyFloor);
    }
    if (!et_caprise && q0 < 0.0) q0 = 0.0;   // GW_ET NONE: no capillary rise

    // Availability, the same idiom as every other flux here: a column may not
    // hand down more than it holds above residual, and the table may not hand
    // up more than it holds. Without the first cap a closure-A cell whose
    // equilibrium it cannot reach drains itself past residual and then keeps
    // "delivering" at K̄ forever — a leak that grows without bound and looks,
    // from the ledger, like recharge.
    if (cl != GwClosure::ENSLAVED) {
        if (q0 > 0.0) {
            const double give = std::max(state_.hu[u] - state_.theta_r[u] * L0,
                                         0.0) / dt;
            q0 = std::min(q0, give);
        } else if (q0 < 0.0) {
            const double take = std::max(hg0, 0.0) *
                                std::max(ts - state_.theta_r[u], 0.0) / dt;
            q0 = std::max(q0, -take);
        }
    }
    state_.q0_last[u] = q0;

    // --- 2. saturated explicit FV update ----------------------------------
    const double q_deep = state_.c_loss[u] * hg0 / std::max(zs, kTiny);
    state_.qdeep_last[u] = q_deep;
    state_.qlat_last[u]  = lat_vol / dt;
    state_.qnode_last[u] = node_vol / dt;

    // Every saturated source shares one storage coefficient — that is the
    // §sy table's whole content.
    const double dV_sat = (q0 - q_deep) * A * dt + lat_vol - node_vol;
    double hg1 = hg0 + dV_sat / (Sy * A);

    if (cl == GwClosure::ENSLAVED) {
        // The line above is that balance LINEARISED at `Sy(L0)`. For the
        // other two closures that is the discretisation and the unsaturated
        // update below matches it exactly, so it conserves. ENSLAVED is
        // different: its column is not integrated, it is EVALUATED at the new
        // L, so the linearised step and the exact storage disagree by
        // O(Δh_g²·θ'(L)) — small per step, one-signed under a steadily rising
        // table, and therefore a drift rather than noise (gate 4 measured
        // 4e-7 m3 over an hour).
        //
        // The balance is algebraic, so solve it algebraically:
        //     F(h) = θ_s·(h − h_g0) + hᵤ*(z_s − h) − hᵤ*(L0) − ΔV/A = 0
        //     F'(h) = θ_s − θ(z_s − h)                 [the same Sy, at the new L]
        // Newton from the linearised guess converges in one or two passes;
        // three is belt and braces and still cheaper than the soil law it
        // calls.
        const double target = dV_sat / A;
        const double hu0    = soil::equilibriumStorage(p, L0);
        for (int it = 0; it < 3; ++it) {
            const double Lh = std::clamp(zs - hg1, 0.0, zs);
            const double F  = ts * (hg1 - hg0) +
                              soil::equilibriumStorage(p, Lh) - hu0 - target;
            const double dF = std::max(ts - soil::waterContent(p, Lh), kSyFloor);
            const double step = F / dF;
            hg1 -= step;
            if (std::fabs(step) < 1.0e-14 * std::max(1.0, zs)) break;
        }
    }

    // Clamp with the books kept. A negative table means the sinks asked for
    // more than the aquifer holds; give back what was not there rather than
    // silently creating it.
    double dunne_vol = 0.0;
    if (hg1 > zs) {
        dunne_vol = (hg1 - zs) * Sy * A;
        hg1 = zs;
    } else if (hg1 < 0.0) {
        const double short_vol = -hg1 * Sy * A;
        hg1 = 0.0;
        // Refund proportionally to the sinks that overdrew: deep loss first
        // (it is the least physical), then the node.
        const double deep_vol = q_deep * A * dt;
        const double refund_deep = std::min(short_vol, deep_vol);
        state_.qdeep_last[u] = (deep_vol - refund_deep) / (A * dt);
        const double rest = short_vol - refund_deep;
        if (rest > 0.0 && node_vol > 0.0) {
            const double refund_node = std::min(rest, node_vol);
            state_.qnode_last[u] = (node_vol - refund_node) / dt;
        }
    }
    state_.led_recharge += q0 * A * dt;
    if (q0 < 0.0) state_.led_caprise += -q0 * A * dt;
    state_.led_deep += state_.qdeep_last[u] * A * dt;
    state_.led_lateral += lat_vol;
    state_.led_node    += state_.qnode_last[u] * dt;

    // --- 3-4. the unsaturated zone sees the CLAMPED table ------------------
    const double L1 = std::max(zs - hg1, 0.0);
    double rejected = 0.0;

    if (cl == GwClosure::SIGMA) {
        sigma::ColumnStep cs;
        cs.L_old     = std::max(L0, kTiny);
        cs.L_new     = std::max(L1, kTiny);
        cs.dt        = dt;
        cs.q_in      = q_in;
        cs.q_et      = q_et;
        cs.q0_phys   = q0;
        cs.capillary = options_.capillary_diff;
        sigma::advanceColumn(p, &state_.theta_sigma[u], state_.m_layers, n, cs);
        rejected = cs.rejected;
        state_.qet_last[u] = cs.et_taken;
        state_.led_et += cs.et_taken * A * dt;
        state_.hu[u] = sigma::columnStorage(&state_.theta_sigma[u],
                                            state_.m_layers, n, cs.L_new);
        // The compression surplus / stretch deficit the ALE sweep could not
        // hold. Applying it here, in the same firing, is what plan gate 5
        // measures; clamping it away inside the sweep is the classic σ-grid
        // conservation bug and it cost 3.9e-1 m before this line existed.
        const double net = (cs.overflow_to_sat - cs.deficit_from_sat) * dt;
        if (net != 0.0) {
            hg1 += net / Sy;
            if (hg1 > zs) { dunne_vol += (hg1 - zs) * Sy * A; hg1 = zs; }
            if (hg1 < 0.0) hg1 = 0.0;
        }
    } else if (cl == GwClosure::ENSLAVED) {
        // No state to advance — the column IS its equilibrium. Rejection is
        // whatever the (now saturated) table cannot accept.
        state_.hu[u] = soil::equilibriumStorage(p, L1);
        state_.qet_last[u] = q_et;
        state_.led_et += q_et * A * dt;
    } else {
        // Closure A: one bulk ODE, ḣᵤ = q⁺ − q_ET − q₀ + the handover the
        // moving table performs on the bulk store. The handover term keeps
        // total water exact for the same reason it does in the σ column.
        const double dL = L1 - L0;
        // `theta_bot` is the slab content the saturated update above paired
        // with `Sy = θ_s − theta_bot`. The two MUST be the same number.
        double hu1 = state_.hu[u] + (q_in - q_et - q0) * dt + theta_bot * dL;
        const double hu_max = ts * L1;
        if (hu1 > hu_max) { rejected = (hu1 - hu_max) / dt; hu1 = hu_max; }
        const double hu_min = state_.theta_r[u] * L1;
        state_.qet_last[u] = q_et;
        if (hu1 < hu_min) {
            // A table RISING faster than the fluxes refill the column absorbs
            // its bottom slab at `theta_bot` and leaves the rest below
            // residual. This is closure A's version of the sigma column's
            // `deficit_from_sat`, and it is settled the same way: draw the
            // shortfall back UP out of the saturated zone in this same
            // firing. Simply raising `hu1` to residual — which is what this
            // did first — creates water out of nothing, and gate 6 found it
            // at 2e-8 m3.
            //
            // Lowering the table by δ hands the column a slab holding θ_s·δ
            // and lengthens it by δ, so residual is met when
            //     hu1 + θ_s·δ >= θ_r·(L1 + δ)   ⇒   δ = deficit/(θ_s − θ_r).
            // Total water is untouched: −θ_s·δ from the table, +θ_s·δ to the
            // column.
            const double deficit = hu_min - hu1;
            const double drain   = std::max(ts - state_.theta_r[u], kSyFloor);
            double delta = deficit / drain;
            if (delta > hg1) delta = hg1;   // the table has no more to give
            hg1 -= delta;
            hu1 += ts * delta;
            const double still = state_.theta_r[u] * (zs - hg1) - hu1;
            if (still > 0.0) {
                // Genuinely dry: there is no water anywhere to meet residual.
                // Book the shortfall as ET that was never actually available
                // rather than inventing it.
                state_.qet_last[u] = std::max(0.0, q_et - still / dt);
                hu1 += still;
            }
        }
        state_.led_et += state_.qet_last[u] * A * dt;
        state_.hu[u] = hu1;
    }

    // --- 5. Dunne saturation excess (step 12) -----------------------------
    // Two routes reach the surface twin and they are physically distinct:
    // `dunne_vol` is the table itself arriving at the ground (`h_g → z_s`),
    // and `reject_vol` is water the top layer would not take. Both are
    // saturation excess, both are booked to the same accumulator, and
    // neither is a correction — together they ARE the mass balance
    // (draft decision 9), which is why DUNNE defaults ON.
    const double reject_vol = rejected * A * dt;
    const double to_surface = (options_.dunne ? dunne_vol : 0.0) + reject_vol;
    if (to_surface > 0.0) {
        state_.xacc_to_surface[u] += to_surface;
        markPendingSurface(i);
        state_.led_dunne += to_surface;
    }
    state_.dunne_last[u] = to_surface / dt;

    state_.hg[u] = hg1;
}

void SubsurfaceSolver::fireGwCells(int tier, double dt, SurfaceStateData& surf) {
    if (!state_.active || !tierHasCells(tier)) return;
    for (const int i : cells_by_tier_[static_cast<std::size_t>(tier)])
        fireCell(i, dt, surf);
    if (tier < static_cast<int>(tier_firings_.size()))
        ++tier_firings_[static_cast<std::size_t>(tier)];
}

void SubsurfaceSolver::settle(SurfaceStateData& surf) {
    // Deliberately NOT "apply everything pending".
    //
    // The surface marcher's settleAccumulators can add pending face volume
    // straight into `volume[i]`, because volume IS its conserved state. The
    // subsurface's state is a water TABLE, and turning a volume into a table
    // rise also moves the unsaturated store — the two zones share a moving
    // boundary. A settle that did the saturated half and skipped the
    // unsaturated one leaked at a rate proportional to |ḣ_g|; it passed a
    // closed-cell gate (nothing pending) and failed every forced one. That
    // version of this function is why gates 2-4 exist.
    //
    // So all volume goes through `fireCell`, which owns the paired update,
    // and settling reduces to what actually needs doing here:
    //
    //  - Nothing can strand. Side accumulators are indexed by EDGE and
    //    gathered through the cell's CSR incidence; neither changes on a
    //    re-tier. The owner picks its volume up at its next firing, and the
    //    macro cycle fires every tier at least once per cycle.
    //  - `storage()` therefore is not the whole invariant between firings.
    //    Anything auditing continuity (the ledger, a hotstart, a gate) must
    //    add the accumulators — see gw2d_gates.cpp's `totalWater()`.
    //  - The one genuine hazard is the OTHER direction: `xacc_to_surface` is
    //    drained by the surface cell, and a dry surface cell that never
    //    fires would hold exfiltrated water forever. That is what
    //    `pendingSurfaceCells()` is for, and it is the marcher's job to pin
    //    those cells active.
    (void)surf;
    compactPending();
    accumulators_pending_ = false;
}

}  // namespace openswmm::twoD
