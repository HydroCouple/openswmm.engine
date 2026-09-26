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
#include "../gw/GwTransportData.hpp"   // T7.1
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
/// G-X1: a column within this fraction of z_s of the ground is "saturated"
/// for the node → aquifer guard.
constexpr double kSatGuard  = 1.0e-4;

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
    bed_exchange_cum_.assign(node_beds_.size(), 0.0);   // G-O
    bed_last_out_.assign(node_beds_.size(), 0.0);       // T7.4
    node_exchange_vol_.assign(static_cast<std::size_t>(std::max(0, n_nodes)),
                              0.0);
    node_drawn_gw_ = node_exchange_vol_;                // G-X1
    {
        // `node_beds_` is the solver's own copy, so converting it in place
        // leaves the authored config — and therefore the writer — untouched.
        for (auto& b : node_beds_) {
            if (!b.exchange) { b.cell = -1; continue; }   // G-X2: opted out
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

        // T7.1: the tuple rides the same water, at the DONOR's saturated
        // concentration — the upwind rule the transmissivity above already
        // chose, so mass and water never disagree about which cell exported.
        // T7.2 adds the dispersive term below, on the same face, into the
        // same accumulators, at the same cadence.
        if (tr_.active() && vol != 0.0) {
            const double vd = satVolume(static_cast<int>(don));
            if (vd > 0.0) {
                const auto ne = static_cast<std::size_t>(E.ne);
                for (int s = 0; s < tr_.n_species; ++s) {
                    const double m_don = tr_.sat_mass[tr_.idx(s, static_cast<int>(don))];
                    if (m_don == 0.0) continue;
                    // T7.2: only the DISSOLVED share travels — what is
                    // sorbed on the grains stays with the cell. That single
                    // multiplier is the whole of retardation here, which is
                    // why no channel carries an R of its own.
                    const double mob =
                        tr_.dissolvedFraction(s, static_cast<int>(don),
                                              state_.theta_s[don]);
                    double dm = std::abs(vol) * (m_don / vd) * mob;
                    // Never export more than the donor holds (the water's
                    // own positivity share bounds the volume the same way);
                    // the signed temperature row has no floor.
                    if (!tr_.signedRow(s) && dm > std::abs(m_don))
                        dm = std::abs(m_don);
                    const double sgn = (vol > 0.0) ? 1.0 : -1.0;
                    const auto k = static_cast<std::size_t>(s) * ne + eu;
                    tr_.sacc_L[k] -= sgn * dm;
                    tr_.sacc_R[k] += sgn * dm;
                }
            }
        }

        // T7.2: hydrodynamic dispersion, D = α_L·|v| + D_m with the pore
        // velocity v = Q/(θ_s·h_g·B). Booked on the SAME face into the SAME
        // accumulators as the advective term above, so it inherits the
        // marcher's tier consistency rather than needing its own argument —
        // the surface's §S2 rule, and the reason the limiter below mirrors
        // the surface's limiter down to the 1/nf composition bound.
        if (tr_.active() && tr_.dispersion_on) {
            const double Ta = state_.hg[l], Tb = state_.hg[r];
            const double va = satVolume(static_cast<int>(l));
            const double vb = satVolume(static_cast<int>(r));
            if (Ta > kTiny && Tb > kTiny && va > 0.0 && vb > 0.0) {
                const double hbar = 0.5 * (Ta + Tb);
                const double B    = E.xi[eu];              // face width (m)
                const double thet = 0.5 * (state_.theta_s[l] + state_.theta_s[r]);
                // Pore velocity across this face, from the advective Q the
                // block above already computed.
                const double area_f = std::max(thet * hbar * B, kTiny);
                const double vpore  = std::abs(Q) / area_f;
                const double alpha  = 0.5 * (tr_.alpha_L[l] + tr_.alpha_L[r]);
                const double Dm     = 0.5 * (tr_.D_m[l] + tr_.D_m[r]);
                const double D      = alpha * vpore + Dm;
                if (D > 0.0) {
                    // Exchanged volume-equivalent: D·(θ·h̄·B)·dt/d.
                    const double cond = D * area_f * E.inv_dx[eu] * dt;
                    const auto ne = static_cast<std::size_t>(E.ne);
                    const int nf_r = std::max(1, cell_nface_[r]);
                    const int nf_l = std::max(1, cell_nface_[l]);
                    for (int s = 0; s < tr_.n_species; ++s) {
                        const double ma = tr_.sat_mass[tr_.idx(s, static_cast<int>(l))];
                        const double mb = tr_.sat_mass[tr_.idx(s, static_cast<int>(r))];
                        const double ca = ma / va *
                            tr_.dissolvedFraction(s, static_cast<int>(l), state_.theta_s[l]);
                        const double cb = mb / vb *
                            tr_.dissolvedFraction(s, static_cast<int>(r), state_.theta_s[r]);
                        double dMd = cond * (ca - cb);      // + = l → r
                        if (dMd == 0.0) continue;
                        // The pairwise equalisation bound, divided by the
                        // RECEIVER's face count: pairwise bounds do not
                        // compose, and a cell fed by several faces each
                        // closing its whole gap can end richer than every
                        // donor. The surface proved this the hard way
                        // (its own comment records 15.31 against a max of
                        // 10); the same 1/nf argument holds here.
                        const std::size_t recv = (dMd > 0.0) ? r : l;
                        const double eq = (ca - cb) * va * vb / (va + vb) /
                                          static_cast<double>((dMd > 0.0) ? nf_r : nf_l);
                        double cap = std::abs(eq);
                        // …and a positivity share of the giver's mass. The
                        // SIGNED temperature row has no positivity to
                        // guard and would bind every exchange across a
                        // warm/cold front, so for it the equalisation
                        // bound alone is the cap.
                        if (!tr_.signedRow(s)) {
                            const double give = std::abs((dMd > 0.0) ? ma : mb) * kFaceShare;
                            cap = std::min(cap, give);
                        }
                        if (std::abs(dMd) > cap) {
                            dMd = (dMd > 0.0) ? cap : -cap;
                            ++tr_.dispersion_limiter_binds;
                        }
                        const auto k = static_cast<std::size_t>(s) * ne + eu;
                        tr_.sacc_L[k] -= dMd;
                        tr_.sacc_R[k] += dMd;
                    }
                }
            }
        }
    }
    accumulators_pending_ = true;
}

double SubsurfaceSolver::gatherLateralMass(int i, int s) noexcept {
    if (tr_.sacc_L.empty() || edges_ == nullptr) return 0.0;
    const InertialEdges& E = *edges_;
    const auto u = static_cast<std::size_t>(i);
    if (u + 1 >= E.cell_ptr.size()) return 0.0;
    const auto ne = static_cast<std::size_t>(E.ne);
    double m = 0.0;
    const int b0 = E.cell_ptr[u];
    const int b1 = E.cell_ptr[u + 1];
    for (int k = b0; k < b1; ++k) {
        const auto ku = static_cast<std::size_t>(k);
        const auto eu = static_cast<std::size_t>(E.cell_edge[ku]);
        const auto j  = static_cast<std::size_t>(s) * ne + eu;
        if (E.cell_sign[ku] > 0) { m += tr_.sacc_L[j]; tr_.sacc_L[j] = 0.0; }
        else                     { m += tr_.sacc_R[j]; tr_.sacc_R[j] = 0.0; }
    }
    return m;
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
    std::fill(node_drawn_gw_.begin(), node_drawn_gw_.end(), 0.0);   // G-X1
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
        const double Sy = std::max(
            state_.theta_s[ci] - state_.theta_r[ci], kSyFloor);
        if (Q > 0.0) {
            // Cap an aquifer→pipe drain at the cell's drainable water.
            const double avail = state_.hg[ci] * Sy * state_.area[ci];
            Q = std::min(Q, kFaceShare * avail / std::max(dt, kTiny));
        } else if (Q < 0.0) {
            // G-X1 (2026-09-19): node → aquifer, the direction that was
            // uncapped. (i) Saturation guard: a column whose table is at the
            // ground takes nothing — water it accepted here would come
            // straight back as Dunne excess in the same firing, and the
            // one-step lag between the frozen node head and the delivery
            // would ping-pong it (program plan §B.4b). (ii) Headroom: the
            // saturated zone can hold (z_s − h_g)·Sy·A, taken in the same
            // per-step share as the drain side so a step cannot fill it
            // more than once. (iii) The node's own water: what it holds
            // (frozen for the batch, so the batch as a whole may not draw
            // more than that — `node_drawn_gw_`, the surface's `node_drawn_`
            // rule) plus what flows through it — a junction stores nothing
            // below its rim (legacy: junction volume is ponding only) but
            // can lose water at the rate it is fed.
            const double L = state_.zs[ci] - state_.hg[ci];
            if (L <= kSatGuard * state_.zs[ci]) {
                Q = 0.0;
            } else {
                const double headroom = L * Sy * state_.area[ci];
                double cap = kFaceShare * headroom / std::max(dt, kTiny);
                const double v12 = opts_ ? opts_->vol_1d_to_2d : 1.0;
                const double held = (ni < nodes->volume.size())
                    ? std::max(0.0, nodes->volume[ni]) * v12 : 0.0;
                const double drawn = (ni < node_drawn_gw_.size())
                    ? node_drawn_gw_[ni] : 0.0;
                const double through = (ni < nodes->inflow.size())
                    ? std::max(0.0, nodes->inflow[ni]) * v12 : 0.0;
                cap = std::min(cap, std::max(0.0, held - drawn) /
                                        std::max(dt, kTiny) + through);
                Q = std::max(Q, -cap);
                if (ni < node_drawn_gw_.size())
                    node_drawn_gw_[ni] += std::min(-Q * dt, std::max(0.0, held - drawn));
            }
        }
        if (Q == 0.0) continue;

        const double vol = Q * dt;
        // T7.4: a node pushing water DOWN its bed (Q < 0) sends its own
        // quality with it. The row layout is shared with the 1D (both come
        // from TransportPolicy), so row s means the same species on both
        // sides and no mapping is needed — only the unit factor the volume
        // already takes. The aquifer→node direction is booked at the CELL's
        // firing instead, where the cell's concentration is known.
        if (tr_.active() && vol < 0.0 && node_row_conc_ != nullptr &&
            node_row_ns_ == tr_.n_species) {
            const double vol_1d_to_2d = opts_ ? opts_->vol_1d_to_2d : 1.0;
            const double v_1d = -vol / std::max(vol_1d_to_2d, kTiny);  // 1D volume units
            const auto nb = node_beds_.size();
            for (int sp = 0; sp < tr_.n_species; ++sp) {
                const double c = node_row_conc_[ni * static_cast<std::size_t>(node_row_ns_) +
                                                static_cast<std::size_t>(sp)];
                if (c == 0.0) continue;
                tr_.nacc_mass[static_cast<std::size_t>(sp) * nb + b] += v_1d * c;
            }
        }
        if (b < bed_last_out_.size())                 // T7.4: the split's weights
            bed_last_out_[b] += std::max(vol, 0.0);
        state_.nacc[b] += vol;                       // gathered at the GW firing
        bed_exchange_cum_[b] += vol;                 // G-O: the per-bed series
        if (ni < node_exchange_vol_.size())
            node_exchange_vol_[ni] += vol;           // flushed by the router
    }
    accumulators_pending_ = true;
}

void SubsurfaceSolver::clearBedDrawn() noexcept {
    std::fill(bed_last_out_.begin(), bed_last_out_.end(), 0.0);
}

double SubsurfaceSolver::gatherNodeMass(int i, int s) noexcept {
    if (tr_.nacc_mass.empty()) return 0.0;
    const auto nb = node_beds_.size();
    double m = 0.0;
    for (std::size_t b = 0; b < nb; ++b) {
        if (node_beds_[b].cell != i) continue;
        const auto k = static_cast<std::size_t>(s) * nb + b;
        m += tr_.nacc_mass[k];
        tr_.nacc_mass[k] = 0.0;
    }
    return m;
}

void SubsurfaceSolver::bookLinkSeepageMass(int cell, int species,
                                           double mass) noexcept {
    if (!tr_.active() || mass == 0.0) return;
    if (cell < 0 || cell >= tr_.n_cells) return;
    if (species < 0 || species >= tr_.n_species) return;
    tr_.lacc_mass[tr_.idx(species, cell)] += mass;
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
// T7.1 — the transported tuple
// ---------------------------------------------------------------------------

void SubsurfaceSolver::initTransport(const RowLayoutLite& rows,
                                     const GwTransportData* gw,
                                     const std::vector<double>& pollut_decay,
                                     std::vector<std::string>& warnings) {
    tr_.clear();
    if (!state_.active || rows.n_species <= 0) return;
    const int ne = options_.per_subcatch ? 0
                                         : (edges_ ? edges_->ne : 0);
    tr_.resize(rows.n_species, state_.n_cells, ne);
    // T7.4: the two 1D seams' accumulators, sized against the bed and node
    // counts the water side already uses.
    tr_.nacc_mass.assign(static_cast<std::size_t>(rows.n_species) *
                             node_beds_.size(), 0.0);
    tr_.node_out_mass.assign(static_cast<std::size_t>(rows.n_species) *
                                 node_exchange_vol_.size(), 0.0);
    tr_.n_pollut  = rows.n_pollut;
    tr_.n_msx     = rows.n_msx;
    tr_.age_row   = rows.age_row;
    tr_.temp_row  = rows.temp_row;
    tr_.row_names = rows.names;

    // Seed both stores from `[GW_INITIAL_QUALITY]`. A row is a
    // CONCENTRATION; the mass it seeds is that concentration times the
    // zone's water volume, so a cell whose table starts at zero starts with
    // no saturated mass however the row is written.
    if (gw != nullptr) {
        for (const auto& r : gw->initial_quality) {
            const int s = tr_.rowIndex(r.species);
            if (s < 0) continue;   // resolveGwTransport already warned
            for (int i = 0; i < state_.n_cells; ++i) {
                if (r.scope == GwScope::CELL && r.cell != i) continue;
                if (r.scope == GwScope::TAG &&
                    (mesh_ == nullptr ||
                     i >= static_cast<int>(mesh_->tri_tag.size()) ||
                     mesh_->tri_tag[static_cast<std::size_t>(i)] != r.tag))
                    continue;
                const double v = (r.zone == GwZone::SAT) ? satVolume(i)
                                                         : unsatVolume(i);
                if (!(v > 0.0)) continue;
                auto& store = (r.zone == GwZone::SAT) ? tr_.sat_mass
                                                      : tr_.unsat_mass;
                store[tr_.idx(s, i)] = r.value * v;
            }
        }
        // LAYER rows address a closure-B σ layer, which T7.1 does not carry
        // (the unsaturated store is bulk — see SubsurfaceTransportState's
        // header). Say so once rather than seeding them into the wrong
        // place or dropping them silently.
        bool any_layer = false;
        for (const auto& r : gw->initial_quality)
            if (r.zone == GwZone::LAYER) { any_layer = true; break; }
        if (any_layer)
            warnings.emplace_back(
                "[GW_INITIAL_QUALITY] LAYER rows were not applied: the "
                "unsaturated store is bulk in this release (per-layer "
                "transport is a later phase). Use ZONE UNSAT to seed the "
                "whole column.");
    }
    // T7.2: resolve `[GW_TRANSPORT_PARAMS]` and `[GW_SORPTION]` onto cells,
    // `* < TAG < CELL` by pass — the same precedence the `[2D_AQUIFER]`
    // rows use, so one deck's two aquifer sections behave the same way.
    if (gw != nullptr) {
        tr_.dispersion_on = gw->options.dispersion;
        for (int i = 0; i < state_.n_cells; ++i) {
            const auto u = static_cast<std::size_t>(i);
            tr_.rho_b[u] = 2650.0 * (1.0 - state_.theta_s[u]);
        }
        for (int pass = 0; pass < 3; ++pass) {
            for (const auto& r : gw->params) {
                if (static_cast<int>(r.scope) != pass) continue;
                for (int i = 0; i < state_.n_cells; ++i) {
                    if (!scopeCoversCell(i, r.scope, r.tag, r.cell)) continue;
                    const auto u = static_cast<std::size_t>(i);
                    tr_.alpha_L[u] = r.alpha_L;
                    tr_.alpha_T[u] = r.alpha_T;
                    tr_.D_m[u]     = r.D_m;
                    // Bulk density from the grain density and the porosity
                    // the aquifer row already resolved: ρ_b = ρ_s(1 − θ_s).
                    tr_.rho_b[u]   = r.rho_s * (1.0 - state_.theta_s[u]);
                }
            }
            for (const auto& r : gw->sorption) {
                if (static_cast<int>(r.scope) != pass) continue;
                const int s = tr_.rowIndex(r.species);
                if (s < 0) continue;
                for (int i = 0; i < state_.n_cells; ++i) {
                    if (!scopeCoversCell(i, r.scope, r.tag, r.cell)) continue;
                    // The row is L/kg; the kernel works in m³/kg.
                    tr_.kd[tr_.idx(s, i)] = r.kd * 1.0e-3;
                    // A negative decay means "use the [POLLUTANTS] Kdecay",
                    // which the engine already holds in 1/s; an authored one
                    // is 1/day.
                    tr_.decay[tr_.idx(s, i)] =
                        (r.decay >= 0.0) ? r.decay / 86400.0
                                         : pollutantDecay(s, pollut_decay);
                }
            }
        }
    }

    for (int s = 0; s < tr_.n_species; ++s)
        tr_.init_mass[static_cast<std::size_t>(s)] = tr_.storage(s);
}

bool SubsurfaceSolver::scopeCoversCell(int i, GwScope scope,
                                       const std::string& tag,
                                       int cell) const noexcept {
    switch (scope) {
        case GwScope::CELL: return cell == i;
        case GwScope::TAG:
            return mesh_ != nullptr &&
                   i < static_cast<int>(mesh_->tri_tag.size()) &&
                   mesh_->tri_tag[static_cast<std::size_t>(i)] == tag;
        default: return true;   // GLOBAL
    }
}

double SubsurfaceSolver::pollutantDecay(
    int s, const std::vector<double>& pollut_decay) const noexcept {
    const auto u = static_cast<std::size_t>(s);
    return (u < pollut_decay.size()) ? pollut_decay[u] : 0.0;
}

void SubsurfaceSolver::bookInfiltrationMass(int cell, int species,
                                            double mass) noexcept {
    if (!tr_.active() || mass == 0.0) return;
    if (cell < 0 || cell >= tr_.n_cells) return;
    if (species < 0 || species >= tr_.n_species) return;
    tr_.xacc_from_surface[tr_.idx(species, cell)] += mass;
}

double SubsurfaceSolver::takeToSurfaceMass(int cell, int species) noexcept {
    if (!tr_.active()) return 0.0;
    if (cell < 0 || cell >= tr_.n_cells) return 0.0;
    if (species < 0 || species >= tr_.n_species) return 0.0;
    double& m = tr_.xacc_to_surface[tr_.idx(species, cell)];
    const double v = m;
    m = 0.0;
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

// G-X3 (2026-09-19): conduit seepage lands in the saturated zone of the
// cells the conduit crosses. Booked per routing step, gathered per firing —
// the same cross-cadence accumulator the surface infiltration uses.
void SubsurfaceSolver::bookLinkSeepage(int cell, double vol_m3) noexcept {
    if (!state_.active || cell < 0 || cell >= state_.n_cells) return;
    state_.lacc[static_cast<std::size_t>(cell)] += vol_m3;
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
    const double hu_pre = state_.hu[u];   // T7.1: the column store this firing starts from
    const double L0  = std::max(zs - hg0, 0.0);

    // --- gather the accumulated cross-cadence volumes ---------------------
    const double lat_vol  = gatherLateral(i);                 // m³, + in
    const double node_vol = gatherNode(i);                    // m³, + OUT
    double infil_vol = state_.xacc_from_surface[u];           // m³, + in
    state_.xacc_from_surface[u] = 0.0;
    // G-X3: conduit seepage delivered to this cell since its last firing —
    // a saturated-zone inflow, so it rides the table update with the
    // lateral and node volumes and shares their storage coefficient.
    const double link_vol = state_.lacc[u];                   // m³, + in
    state_.lacc[u] = 0.0;
    state_.qlink_last[u] = link_vol / dt;
    state_.led_link += link_vol;

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
        // G-X4 (2026-09-20): …and when the FLOOR binds, the pairing above is
        // the floored number, not the raw content. A column already at θ_s
        // just under the table (θ_s − θ_bot → 0) otherwise loses
        // `Sy_floor·|Δh|` from the saturated zone and hands `θ_bot·|Δh|` to
        // the unsaturated one — the two no longer sum to θ_s and the cell
        // CREATES `(θ_bot − (θ_s − Sy_floor))·|Δh|·A` every firing. Invisible
        // until something withdrew hard from a near-saturated column: a
        // conduit under a high water table did, at 0.08 m³ over half an hour
        // on the G-X4 gate deck. Deriving the slab from the Sy actually used
        // makes the identity hold unconditionally, and is a no-op whenever
        // the floor does not bind (every deck that was already right).
        theta_bot = ts - Sy;
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
    const double dV_sat = (q0 - q_deep) * A * dt + lat_vol - node_vol + link_vol;   // G-X3: + link_vol
    double hg1 = hg0 + dV_sat / (Sy * A);

    // G1-c (2026-09-19): the ENSLAVED solve is BRACKETED. The Newton it
    // replaced started from the linearised guess and took unguarded steps;
    // with the table a centimetre under the ground the surface content is
    // ≈ θ_s, `Sy` sits on its floor, the guess lands above z_s, the clamped
    // L = 0 makes F' the floor too, and the step is O(−1e5 m): the table
    // was zeroed and the column re-evaluated at full length in one firing
    // (measured: 23.5 m³ created on a two-cell exfiltration deck, −32 % on
    // the Dunne gate deck). The balance W(h) = θ_s·h + hᵤ*(z_s − h) is
    // monotone in h, so it is solved on [0, z_s] with a safeguarded Newton
    // (bisection fallback), and what the interval cannot hold is booked
    // exactly: above z_s as Dunne excess, below 0 as the sinks' refund —
    // the same two channels the clamp below serves for the other closures.
    double ens_dunne = -1.0, ens_short = -1.0;   // < 0: not decided here
    if (cl == GwClosure::ENSLAVED) {
        const double hu0    = soil::equilibriumStorage(p, L0);
        const double target = ts * hg0 + hu0 + dV_sat / A;   // W(h1) wanted
        auto W = [&](double h) {
            return ts * h + soil::equilibriumStorage(p, std::clamp(zs - h, 0.0, zs));
        };
        const double W_hi = W(zs), W_lo = W(0.0);
        if (target >= W_hi) {
            hg1 = zs;
            ens_dunne = (target - W_hi) * A;
        } else if (target <= W_lo) {
            hg1 = 0.0;
            ens_short = (W_lo - target) * A;
        } else {
            // G-X1 (2026-09-19): W is monotone but can be FLAT across the capillary fringe
            // (θ(L) = θ_s for L below the air-entry head): there the true
            // derivative is zero and Newton has nothing to say, so the step
            // falls back to bisection — never a floored derivative, which
            // made Newton creep and leave the balance unmet by up to
            // 1e-5 m³ per firing. Convergence is on the RESIDUAL (the
            // volume), not on the step in h.
            double lo = 0.0, hi = zs;          // W(lo) < target < W(hi)
            double h = std::clamp(hg1, lo, hi);
            const double f_tol = 1.0e-15 * std::max(1.0, std::fabs(target));
            for (int it = 0; it < 200; ++it) {
                const double F = W(h) - target;
                if (std::fabs(F) <= f_tol) break;
                if (F > 0.0) hi = h; else lo = h;
                if (hi - lo <= 1.0e-15 * zs) break;
                const double dF = ts - soil::waterContent(p, std::clamp(zs - h, 0.0, zs));
                double hn = (dF > kTiny) ? h - F / dF : 0.5 * (lo + hi);
                if (!(hn > lo && hn < hi)) hn = 0.5 * (lo + hi);   // safeguard
                h = hn;
            }
            hg1 = h;
        }
    }

    // Clamp with the books kept. A negative table means the sinks asked for
    // more than the aquifer holds; give back what was not there rather than
    // silently creating it.
    double dunne_vol = 0.0;
    if (ens_dunne >= 0.0) {
        dunne_vol = ens_dunne;                       // ENSLAVED: exact, above
    } else if (hg1 > zs) {
        dunne_vol = (hg1 - zs) * Sy * A;
        hg1 = zs;
    } else if (hg1 < 0.0 || ens_short >= 0.0) {
        const double short_vol = (ens_short >= 0.0) ? ens_short : -hg1 * Sy * A;
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

    // --- T7.1: the tuple rides the volumes this firing just decided -------
    // Every number below is the one the water used, read AFTER the refund
    // paths may have rewritten a rate (`qnode_last`, `qdeep_last`, `qet_last`),
    // so mass can never travel on a volume the water gave back.
    if (tr_.active()) {
        CellFlux f;
        f.v_sat0   = hg0 * ts * A;
        f.v_uns0   = hu_pre * A;
        f.lateral  = lat_vol;
        f.node_out = state_.qnode_last[u] * dt;
        f.infil_in = infil_vol;
        f.link     = link_vol;
        f.recharge = q0 * A * dt;
        f.et       = state_.qet_last[u] * A * dt;
        f.deep     = state_.qdeep_last[u] * A * dt;
        f.dt       = dt;   // T7.2
        f.dunne    = options_.dunne ? dunne_vol : 0.0;
        f.reject   = reject_vol;
        // What the column's own balance could not have been produced by a
        // flux IS the moving boundary's swap. Reading it off the final
        // store rather than re-deriving `θ_bot·ΔL` keeps it exact under
        // every closure — including the σ column, whose handover lives
        // inside its ALE sweep.
        f.handover = (state_.hu[u] * A - f.v_uns0) - f.infil_in + f.et +
                     f.recharge + f.reject;
        fireCellSpecies(i, f);
    }

    state_.hg[u] = hg1;
}

// ---------------------------------------------------------------------------
// T7.1 — one cell's species pass
// ---------------------------------------------------------------------------

void SubsurfaceSolver::fireCellSpecies(int i, const CellFlux& f) noexcept {
    const auto k0 = static_cast<std::size_t>(i);
    (void)k0;
    // Arrivals land before anything is taken, so a cell that receives and
    // gives in the same firing gives at the MIXED concentration — the
    // surface marcher's rule (its face gather lands before the sinks read
    // the cell). The denominators are the post-arrival water volumes; every
    // sink in this firing reads the same pair, because the water update
    // decided all of them simultaneously.
    const double v_sat = f.v_sat0 + f.lateral + std::max(f.link, 0.0);
    const double v_uns = f.v_uns0 + f.infil_in;
    const auto   uc = static_cast<std::size_t>(i);
    // T7.2: the water contents retardation is computed against. The
    // saturated zone is at θ_s by definition; the column's mean content is
    // its store over its thickness, floored so a vanishing column does not
    // divide.
    const double ts_cell  = state_.theta_s[uc];
    const double L_cell   = std::max(state_.zs[uc] - state_.hg[uc], kTiny);
    const double theta_uns = std::clamp(state_.hu[uc] / L_cell,
                                        state_.theta_r[uc], ts_cell);

    for (int s = 0; s < tr_.n_species; ++s) {
        const auto k  = tr_.idx(s, i);
        const auto us = static_cast<std::size_t>(s);
        double& msat = tr_.sat_mass[k];
        double& muns = tr_.unsat_mass[k];

        // Take `vol` worth of water out of `m`, which is riding `v_store`.
        // The clamp is the surface's: never remove more than is there, and
        // no floor at all on the signed temperature row, where °C·m³ below
        // zero is a state rather than an error.
        // T7.2: `mob` is the dissolved share — sorbed mass does not travel,
        // in any channel. Passing it through the ONE place a sink reads a
        // concentration is what keeps retardation from having to be
        // remembered at each of the eight channels below.
        const double mob_sat = tr_.dissolvedFraction(s, i, ts_cell);
        const double mob_uns = tr_.dissolvedFraction(s, i, theta_uns);
        auto take = [&](double& m, double vol, double v_store,
                        double mob) -> double {
            if (!(vol > 0.0) || !(v_store > 0.0) || m == 0.0) return 0.0;
            double dm = vol * (m / v_store) * mob;
            if (!tr_.signedRow(s) && dm > m) dm = m;
            m -= dm;
            return dm;
        };

        // (1) lateral Darcy — internal between cells, so the sum over the
        //     mesh is zero on a closed domain and the ledger says exactly
        //     that (the water's `led_lateral` behaves identically).
        const double lat_m = gatherLateralMass(i, s);
        msat += lat_m;
        tr_.net_lateral[us] += lat_m;

        // (2) infiltration the surface handed down, with the mass the water
        //     was carrying when it left the surface cell.
        const double in_m = tr_.xacc_from_surface[k];
        if (in_m != 0.0) {
            tr_.xacc_from_surface[k] = 0.0;
            muns += in_m;
            tr_.gained_infil[us] += in_m;
        }

        // (3) conduit seepage (G-X3/G-X4). T7.4 opened this seam: a LEAKING
        //     pipe now fills the cell with water carrying the conduit's own
        //     concentration, booked by the router at the same cadence and
        //     with the same length weights the volume took. The 1D already
        //     DEBITS that mass as its exfiltration loss
        //     (`qual_routing_seep`), so this side only receives it —
        //     booking a second removal there would be the classic
        //     double-count. A GAINING pipe draws water out, and that water
        //     leaves at the cell's own concentration.
        const double link_in_m = tr_.lacc_mass[k];
        if (link_in_m != 0.0) {
            tr_.lacc_mass[k] = 0.0;
            msat += link_in_m;
            tr_.gained_link[us] += link_in_m;
        }
        if (f.link < 0.0)
            tr_.lost_link[us] += take(msat, -f.link, v_sat, mob_sat);

        // (4) recharge across the table, or capillary rise — internal to
        //     this cell, so it is informational only.
        if (f.recharge > 0.0) {
            const double dm = take(muns, f.recharge, v_uns, mob_uns);
            msat += dm;
            tr_.internal_recharge[us] += dm;
        } else if (f.recharge < 0.0) {
            const double dm = take(msat, -f.recharge, v_sat, mob_sat);
            muns += dm;
            tr_.internal_recharge[us] -= dm;
        }

        // (4b) …and the moving table's own swap, which carries water across
        //      the same boundary with no flux behind it. Without this the
        //      TOTAL still conserves — the gate would pass — while the two
        //      zones quietly end up holding each other's solute.
        if (f.handover > 0.0) {
            // The moving boundary carries the water's whole content across,
            // sorbed included: the grains themselves change zone when the
            // table passes them. `mob = 1`.
            const double dm = take(msat, f.handover, v_sat, 1.0);
            muns += dm;
            tr_.internal_recharge[us] -= dm;
        } else if (f.handover < 0.0) {
            const double dm = take(muns, -f.handover, v_uns, 1.0);
            msat += dm;
            tr_.internal_recharge[us] += dm;
        }

        // (5) the saturated zone's sinks, and the node seam (T7.4). A node
        //     RECHARGING the aquifer sent its own quality down the bed;
        //     `sampleNodeExchange` sampled it at the node's concentration
        //     when it sampled the head, and this is where the cell takes
        //     it. The reverse — the aquifer draining INTO the node — leaves
        //     at the cell's concentration and is booked per NODE here, for
        //     the router to hand to that node's coupling queues.
        const double node_in_m = gatherNodeMass(i, s);
        if (node_in_m != 0.0) {
            msat += node_in_m;
            tr_.gained_node[us] += node_in_m;
        }
        if (f.node_out > 0.0) {
            const double dm = take(msat, f.node_out, v_sat, mob_sat);
            tr_.lost_node[us] += dm;
            if (dm != 0.0 && !tr_.node_out_mass.empty()) {
                // Split across the beds this cell serves, in proportion to
                // what each one drew — a cell can carry more than one.
                const auto nn = tr_.node_out_mass.size() /
                                static_cast<std::size_t>(tr_.n_species);
                double drew = 0.0;
                for (std::size_t b = 0; b < node_beds_.size(); ++b)
                    if (node_beds_[b].cell == i)
                        drew += std::max(bed_last_out_[b], 0.0);
                if (drew > 0.0) {
                    for (std::size_t b = 0; b < node_beds_.size(); ++b) {
                        if (node_beds_[b].cell != i) continue;
                        const double share = std::max(bed_last_out_[b], 0.0) / drew;
                        const auto ni = static_cast<std::size_t>(node_beds_[b].node);
                        if (ni < nn)
                            tr_.node_out_mass[static_cast<std::size_t>(s) * nn + ni] +=
                                dm * share;
                    }
                }
            }
        }
        tr_.lost_deep[us] += take(msat, f.deep, v_sat, mob_sat);

        // (6) ET carries the INTENSIVE rows only: the solutes stay and the
        //     column up-concentrates (GW plan §3.5), while temperature and
        //     age leave with the water at the column's own mean — the
        //     program plan's D-A20 convention, the same one the surface
        //     marcher applies to evaporation, so a parcel's age is not
        //     changed by the act of evaporating part of it.
        if (f.et > 0.0 && (s == tr_.age_row || s == tr_.temp_row))
            tr_.lost_et[us] += take(muns, f.et, v_uns, 1.0);

        // (7) saturation excess back to the surface: the table's own water
        //     from the saturated zone, the column's rejection from the
        //     unsaturated one. Both are owed to the surface twin, which
        //     drains them with `takeToSurfaceMass`.
        double up = 0.0;
        if (f.dunne  > 0.0) up += take(msat, f.dunne,  v_sat, mob_sat);
        if (f.reject > 0.0) up += take(muns, f.reject, v_uns, mob_uns);
        if (up != 0.0) {
            tr_.xacc_to_surface[k] += up;
            tr_.lost_dunne[us] += up;
        }

        // (8) T7.2: first-order decay, on the TOTAL mass of both zones —
        //     dissolved and sorbed alike, the usual assumption for a
        //     partitioning solute whose reaction does not care which phase
        //     it is in. Applied last, on what the channels left behind, and
        //     never to age or temperature: `__WATER_AGE__` grows with time
        //     by construction and a temperature does not decay.
        const double kdec = tr_.decay.empty() ? 0.0 : tr_.decay[k];
        if (kdec > 0.0 && f.dt > 0.0 && s != tr_.age_row && s != tr_.temp_row) {
            const double keep = std::exp(-kdec * f.dt);
            const double gone = (msat + muns) * (1.0 - keep);
            msat *= keep;
            muns *= keep;
            tr_.lost_reaction[us] += gone;
        }
    }
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
