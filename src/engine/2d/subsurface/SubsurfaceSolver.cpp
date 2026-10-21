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
 * For an unbounded SIGMA bottom flux, `f_bot = q₀_phys − θ_bot·L̇`,
 * so `θ_s·ḣ_g = f_bot` rearranges to `(θ_s − θ_bot)·ḣ_g = q₀_phys`.
 * The latter gives the table predictor. The final discrete balance uses
 * actual bounded bottom water, compression surplus and residual support,
 * since an explicit column sweep can limit or redirect these transfers.
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
 * `fireCell` gathers completed exchanges, computes Darcy recharge and a
 * specific-yield table predictor. SIGMA then brackets the physical table
 * interval while replaying the existing explicit column sweep from its
 * original state. One final column/table pair closes the actual discrete
 * bottom transfer; only excess outside the bracket becomes Dunne water or
 * a withdrawal refund. CLOSED_FORM and ENSLAVED keep their existing update.
 *
 * @ingroup engine_2d
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include "SubsurfaceSolver.hpp"
#include "GroundwaterDispersion.hpp"
#include "LayerTransport.hpp"

#include "../data/MeshData.hpp"
#include "../data/SolverOptions2D.hpp"
#include "../data/SurfaceStateData.hpp"
#include "../solver/InertialEdges.hpp"
#include "../gw/GwTransportData.hpp"   // T7.1
#include "../heat/LidNodeHeat.hpp"
#include "../../data/NodeData.hpp"
#include "../../transport/components/HeatFluxModules/SurfaceExchange.hpp"
#include "../../core/SimulationContext.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <numeric>
#include <stdexcept>

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

// A conserved-water division may put a derived theta one ulp beyond a
// physical endpoint. Normalize only that roundoff; authored state stays
// strictly validated and a material stage excursion still rejects the trial.
double rk2SigmaView(double theta,double lower,double upper) {
    const double roundoff=16*std::numeric_limits<double>::epsilon();
    if(!std::isfinite(theta)||theta<lower-roundoff||theta>upper+roundoff)
        throw std::runtime_error("Coupled RK2 SIGMA derived water content exceeds roundoff bounds.");
    return std::clamp(theta,lower,upper);
}

// Exact Green-Ampt intake over a held interval with frozen suction/moisture.
// Solving its integral removes the singular instantaneous rate at F=0.
double frontIntake(double F, double B, double Ks, double dt) noexcept {
    const double target=Ks*dt;
    if (!(target>0.0))return 0.0;
    if (!(B>0.0))return target;
    F=std::max(F,0.0);
    auto integral=[&](double x){
        const double y=x/(F+B);
        const double remainder=y<1e-3
            ? y*y*(0.5-y/3.0+y*y/4.0-y*y*y/5.0+y*y*y*y/6.0)
            : y-std::log1p(y);
        return F*y+B*remainder;
    };
    double lo=0.0,hi=target+std::sqrt(2.0*B*target);
    while(integral(hi)<target)hi*=2.0;
    double x=hi;
    for(int iteration=0;iteration<40;++iteration){
        const double residual=integral(x)-target;
        if(std::fabs(residual)<=1e-12*target)return x;
        if(residual>0.0)hi=x;else lo=x;
        const double derivative=(F+x)/(F+B+x);
        const double next=x-residual/derivative;
        x=(next>lo&&next<hi)?next:0.5*(lo+hi);
    }
    return lo; // conservative lower bracket if convergence is limited
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
    covered_.assign(static_cast<std::size_t>(n), options_.per_subcatch ? 1 : 0);

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
                covered_[u] = 1;
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
    boundaries_.clear();cell_boundaries_.clear();
    state_ = SubsurfaceState{};
    heat_.clear();heat_link_out_volume_.clear();heat_preset_out_j_.clear();heat_preset_out_water_.clear();heat_node_out_volume_.clear();heat_node_gathered_out_.clear();
    heat_faces_.clear();heat_pending_left_.clear();heat_pending_right_.clear();heat_dt_.clear();
    porewater_preset_out_.clear();
    if (cfg.empty()) return {};

    options_   = cfg.options;
    if(options_.gw_et=="AUTO")options_.gw_et=options_.per_subcatch?"NONE":"BOTH";
    if(options_.link_seepage==GwLinkMode::DEFAULT)options_.link_seepage=options_.per_subcatch?GwLinkMode::AUTO:GwLinkMode::TWO_WAY;
    options_.wilting_suction=cfg.options.wilting_suction_set?cfg.options.wilting_suction*uf.length:150.0;
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

    // Cells no row covers have no aquifer. Their defaulted parameters stay
    // in the arrays but hold no water and are never fired.
    {
        const long n_covered = std::count(covered_.begin(), covered_.end(), uint8_t{1});
        if (n_covered == 0)
            return "2D aquifer: no mesh cell matches a [2D_AQUIFER] row; add a '*' row or rows for the aquifer's cells.";
        if (n_covered < n) {
            const std::size_t m = static_cast<std::size_t>(state_.m_layers);
            for (int i = 0; i < n; ++i) {
                const auto u = static_cast<std::size_t>(i);
                if (covered_[u]) continue;
                state_.hg[u] = 0.0;
                state_.hu[u] = 0.0;
                for (std::size_t k = 0; k < m; ++k)
                    state_.theta_sigma[k * static_cast<std::size_t>(n) + u] = 0.0;
            }
            warnings.emplace_back("2D aquifer: " + std::to_string(n - n_covered) + " of " +
                std::to_string(n) + " mesh cells have no [2D_AQUIFER] row and no aquifer; "
                "they keep surface infiltration and their faces are no-flow.");
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
    cell_completed_.assign(n,0.0);
    richards_clock_valid_=true;
    richards_bed_owned_.assign(node_beds_.size(),false);
    richards_down_cum_.assign(node_beds_.size(),0.0);
    richards_up_cum_.assign(node_beds_.size(),0.0);
    lid_receipts_enabled_=false;
    lid_receipts_.assign(node_beds_.size(),{});
    lid_booked_end_.assign(node_beds_.size(),0.0);
    mobile_lid_bed_.assign(node_beds_.size(),false);
    lid_bed_factor_.assign(node_beds_.size(),1.0);
    lid_bed_diagnostics_.assign(node_beds_.size(),{});
    node_pending_down_.assign(node_beds_.size(),0.0);node_pending_up_=node_pending_down_;
    bed_gathered_.assign(node_beds_.size(), 0.0);       // T7.4
    node_exchange_vol_.assign(static_cast<std::size_t>(std::max(0, n_nodes)),
                              0.0);
    node_drawn_gw_ = node_exchange_vol_;                // G-X1
    drain_cap_binds_ = 0;                               // G-W1b
    node_refunds_    = 0;                               // G-W1b
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
            if (!covered_[static_cast<std::size_t>(b.cell)]) {
                b.cell = -1;
                warnings.emplace_back(
                    "2D aquifer: [2D_AQUIFER_NODE] row lies on a cell with no "
                    "[2D_AQUIFER] row; the node exchanges with nothing.");
                continue;
            }
            b.Kc   *= uf.rate;
            b.dC   *= uf.length;
            b.area *= uf.area;
            if (b.area <= 0.0)
                b.area = state_.area[static_cast<std::size_t>(b.cell)];
        }
    }

    cell_beds_.assign(static_cast<std::size_t>(n), {});
    for (std::size_t b = 0; b < node_beds_.size(); ++b)
        if (node_beds_[b].cell >= 0) cell_beds_[node_beds_[b].cell].push_back(b);
    pending_flag_.assign(static_cast<std::size_t>(n), 0);
    source_owned_.assign(n,false);source_cell_end_.assign(n,0);source_receipts_.assign(n,{});source_returns_.clear();source_species_.clear();source_completed_end_=0;
    pending_surface_.clear();
    accumulators_pending_ = false;
    state_.active = true;
    recharge_backend_={};recharge_inputs_.clear();recharge_values_.clear();recharge_slots_.assign(n,-1);
    recharge_batches_=recharge_cells_=0;
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
                if (!covered_[u] || !covered_[ju]) continue;   // no-flow face
                const double Ke = harmonic(state_.Ks[u], state_.Ks[ju]);
                const double Te = std::max({state_.hg[u], state_.hg[ju],
                                            0.01 * zs});
                sumT += Ke * Te * edges.xi[eu] * edges.inv_dx[eu];
            }
        }
        if(u<cell_boundaries_.size())for(auto id:cell_boundaries_[u]) {
            const auto& edge=boundaries_[id];if(!edge.head)continue;
            double outside=std::abs(edge.signal.constant-state_.z_bed[u]);
            for(double value:edge.signal.ordinates())outside=std::max(outside,std::abs(value-state_.z_bed[u]));
            // Conservative bound on the unconfined head-flux derivative,
            // including a prescribed level above the aquifer top. A boundary
            // cannot bypass the same explicit storage/flux stability gate.
            sumT+=state_.Ks[u]*edge.length/edge.distance*(2*zs+outside);
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

        const double hg=state_.hg[u];
        double dt_solute=1e30;
        if(tr_.active()&&tr_.dispersion_on&&hg>kTiny&&!options_.per_subcatch) {
            dispersion::GradientFit fit;
            const double head=state_.z_bed[u]+hg;
            for(int p=edges.cell_ptr[u];p<edges.cell_ptr[u+1];++p) {
                const int e=edges.cell_edge[p],other=edges.cL[e]==i?edges.cR[e]:edges.cL[e];
                if(!covered_[other])continue;
                fit.add(mesh.tri_cx[other]-mesh.tri_cx[u],mesh.tri_cy[other]-mesh.tri_cy[u],
                    state_.z_bed[other]+state_.hg[other]-head);
            }
            const auto g=fit.value();
            double speed=state_.Ks[u]*std::hypot(g[0],g[1])/state_.theta_s[u];
            if(u<cell_boundaries_.size())for(auto id:cell_boundaries_[u]) {
                const auto& b=boundaries_[id];double maximum=std::abs(b.signal.constant-(b.head?head:0));
                for(double v:b.signal.ordinates())maximum=std::max(maximum,std::abs(v-(b.head?head:0)));
                speed=std::max(speed,b.head?state_.Ks[u]*maximum/(state_.theta_s[u]*b.distance):
                    maximum/(state_.theta_s[u]*hg*b.length));
            }
            const double diffusion=tr_.D_m[u]+std::max(tr_.alpha_L[u],tr_.alpha_T[u])*speed;
            double sum=0;
            for(int p=edges.cell_ptr[u];p<edges.cell_ptr[u+1];++p) {
                const int e=edges.cell_edge[p],other=edges.cL[e]==i?edges.cR[e]:edges.cL[e];
                if(!covered_[other]||state_.hg[other]<=kTiny)continue;
                const double distance=1/edges.inv_dx_normal[e];
                // Extra factor bounds the explicitly deferred skew correction.
                const double skew=std::max(1.,edges.inv_dx_normal[e]/edges.inv_dx[e]);
                sum+=diffusion*edges.xi[e]*skew/distance;
            }
            if(sum>0)dt_solute=std::min(.5,tr_.c_diff)*A/sum;
        }
        state_.dt_cell[u] = std::min({dt_g, dt_u,dt_solute});
        if(heat_.active()&&!heat_.cells[u].column.energy.empty())
            state_.dt_cell[u]=std::min(state_.dt_cell[u],heat_.config->options.thermal_max_step);
        if(u<heat_dt_.size())state_.dt_cell[u]=std::min(state_.dt_cell[u],heat_dt_[u]);
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
        if (covered_[u]) cells_by_tier_[static_cast<std::size_t>(t)].push_back(i);
    }

    // A face fires at the FINER of its two cells — the standard LTS face
    // rule. The finer cell then gathers a full-cadence flux and the coarser
    // one gathers the accumulated sum, which is where G-B's ±Δ pair earns
    // its keep.
    const int ne = static_cast<int>(state_.eacc_L.size());
    for (int e = 0; e < ne; ++e) {
        const auto eu = static_cast<std::size_t>(e);
        if (!covered_[static_cast<std::size_t>(edges_->cL[eu])] ||
            !covered_[static_cast<std::size_t>(edges_->cR[eu])]) continue;   // no-flow face
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
    if(!state_.active)return;
    if(tr_.n_layers>0&&heat_.active())for(int i=0;i<state_.n_cells;++i)if(covers(i)) {
        if(auto error=refreshSigmaPorewater(i);!error.empty())throw std::runtime_error(error);
        heat_.refreshView(i,tr_);
    }
    if(!tierHasFaces(tier))return;
    const auto& list = faces_by_tier_[static_cast<std::size_t>(tier)];
    const InertialEdges& E = *edges_;

    const auto concentration=[&](int cell,int species) {
        const double water=satVolume(cell);
        if(!(water>0))return 0.;
        return tr_.sat_mass[tr_.idx(species,cell)]*
            tr_.dissolvedFraction(species,cell,state_.theta_s[cell])/water;
    };
    const auto gradient=[&](int cell,int species) {
        dispersion::GradientFit fit;
        const auto value=[&](int c){return species<0?state_.z_bed[c]+state_.hg[c]:concentration(c,species);};
        const double center=value(cell);
        for(int p=E.cell_ptr[cell];p<E.cell_ptr[cell+1];++p) {
            const int e=E.cell_edge[p],other=E.cL[e]==cell?E.cR[e]:E.cL[e];
            if(!covered_[other]||(species>=0&&!(satVolume(other)>0)))continue;
            fit.add(mesh_->tri_cx[other]-mesh_->tri_cx[cell],mesh_->tri_cy[other]-mesh_->tri_cy[cell],value(other)-center);
        }
        if(species<0&&std::size_t(cell)<cell_boundaries_.size())for(auto id:cell_boundaries_[cell]) {
            const auto& boundary=boundaries_[id];
            const auto slot=MeshData::slot(cell,boundary.edge);
            const double nx=mesh_->edge_nx[slot],ny=mesh_->edge_ny[slot];
            const double time=cell_completed_[cell];
            if(boundary.head)fit.add(nx*boundary.distance,ny*boundary.distance,boundary.signal.at(time)-center);
            else if(state_.Ks[cell]>0&&state_.hg[cell]>0)
                fit.add(nx,ny,boundary.signal.at(time)/(state_.Ks[cell]*state_.hg[cell]*boundary.length));
        }
        return fit.value();
    };
    for (const int e : list) {
        const auto eu = static_cast<std::size_t>(e);
        const auto l  = static_cast<std::size_t>(E.cL[eu]);
        const auto r  = static_cast<std::size_t>(E.cR[eu]);
        if(!heat_faces_.empty())fireLateralHeatFace(e,dt);

        const double HL = state_.z_bed[l] + state_.hg[l];
        const double HR = state_.z_bed[r] + state_.hg[r];
        const double dH = HL - HR;
        // A zero Darcy gradient still permits molecular dispersion.

        // Upwind transmissivity: the DONOR cell's saturated thickness. Using
        // the mean here is the classic way to make a drying aquifer pump
        // water it does not have.
        const std::size_t don = (dH > 0.0) ? l : r;
        const double T = state_.hg[don];

        const double K = harmonic(state_.Ks[l], state_.Ks[r]);

        double Q = K * T * E.xi[eu] * E.inv_dx[eu] * dH;   // m³/s, + is L→R

        // Positivity: no face may take more than its share of the donor's
        // drainable water in one step. The share is split across the donor's
        // own faces so a cell with many wet neighbours cannot be emptied
        // several times over within one firing.
        const double Sy = std::max(specificYieldOf(int(don)),kSyFloor);
        const int nf = std::max(1, cell_nface_[don]);
        double cap = std::min(kFaceShare * T * Sy * state_.area[don] /
                           static_cast<double>(nf),unreservedDrainableWater(int(don))) /
                           std::max(dt,kTiny);
        if(lid_receipts_enabled_) {
            NodeWaterBudget budget;
            if(auto e=nodeWaterBudget(int(don),dt,budget);!e.empty())throw std::runtime_error(e);
            cap=std::min(cap,budget.donor/dt);
        }
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
                    const bool layered_porewater=tr_.n_layers==0&&heat_.active()&&s!=tr_.temp_row&&
                        heat_.cells[don].porewater.active();
                    const double mobile_donor=layered_porewater?saturatedPorewaterMobileMass(int(don),s):
                        m_don*tr_.dissolvedFraction(s,static_cast<int>(don),state_.theta_s[don]);
                    if(layered_porewater&&(!std::isfinite(mobile_donor)||mobile_donor<0)) {
                        auto& owner=heat_.cells[don];owner.failed=1;
                        try{owner.error="Invalid reserved-layer porewater donor for an accepted face transfer.";}catch(...){}
                        continue;
                    }
                    if (mobile_donor == 0.0) {
                        if(heat_.active()&&s==tr_.temp_row)bookPresetHeatOut(int(don),std::abs(vol),0);
                        continue;
                    }
                    // T7.2: only the DISSOLVED share travels — what is
                    // sorbed on the grains stays with the cell. That single
                    // multiplier is the whole of retardation here, which is
                    // why no channel carries an R of its own.
                    double dm = std::abs(vol) * (mobile_donor / vd);
                    if(heat_.active()&&s==tr_.temp_row)bookPresetHeatOut(int(don),std::abs(vol),dm);
                    // Never export more than the donor holds (the water's
                    // own positivity share bounds the volume the same way);
                    // the signed temperature row has no floor.
                    if (!tr_.signedRow(s)) {
                        const double available=layered_porewater?mobile_donor:
                            std::max(0.,m_don-pendingSaturatedMassExport(int(don),s));
                        if(dm>available)dm=available;
                    }
                    bookPresetPorewaterOut(int(don),s,dm);
                    const double sgn = (vol > 0.0) ? 1.0 : -1.0;
                    const auto k = static_cast<std::size_t>(s) * ne + eu;
                    tr_.sacc_L[k] -= sgn * dm;
                    tr_.sacc_R[k] += sgn * dm;
                }
            }
        }

        // Hydrodynamic tensor dispersion follows the same conservative face
        // receipts as advection. Normal material resistance is harmonic;
        // reconstructed gradients retain transverse and skew contributions.
        if (tr_.active() && tr_.dispersion_on) {
            const double Ta = state_.hg[l], Tb = state_.hg[r];
            const double va = satVolume(static_cast<int>(l));
            const double vb = satVolume(static_cast<int>(r));
            if (Ta > kTiny && Tb > kTiny && va > 0.0 && vb > 0.0) {
                const double B=E.xi[eu],nx=E.nx[eu],ny=E.ny[eu];
                const std::array<double,2> armL{E.mx[eu]-mesh_->tri_cx[l],E.my[eu]-mesh_->tri_cy[l]};
                const std::array<double,2> armR{E.mx[eu]-mesh_->tri_cx[r],E.my[eu]-mesh_->tri_cy[r]};
                const double dl=armL[0]*nx+armL[1]*ny,dr=-armR[0]*nx-armR[1]*ny;
                const auto headL=gradient(int(l),-1),headR=gradient(int(r),-1);
                const auto coefficient=[&](std::size_t cell,const std::array<double,2>& head) {
                    const double theta=state_.theta_s[cell],factor=-state_.Ks[cell]/theta;
                    auto tensor=dispersion::tensor(factor*head[0],factor*head[1],tr_.alpha_L[cell],tr_.alpha_T[cell],
                        tr_.D_m[cell]);
                    const double section=theta*state_.hg[cell];
                    tensor.xx*=section;tensor.xy*=section;tensor.yy*=section;return tensor;
                };
                {
                    const auto ne = static_cast<std::size_t>(E.ne);
                    const int nf_r = std::max(1, cell_nface_[r]);
                    const int nf_l = std::max(1, cell_nface_[l]);
                    for (int s = 0; s < tr_.n_species; ++s) {
                        const double ma = tr_.sat_mass[tr_.idx(s, static_cast<int>(l))];
                        const double mb = tr_.sat_mass[tr_.idx(s, static_cast<int>(r))];
                        const bool layered_porewater_a=tr_.n_layers==0&&heat_.active()&&s!=tr_.temp_row&&heat_.cells[l].porewater.active();
                        const bool layered_porewater_b=tr_.n_layers==0&&heat_.active()&&s!=tr_.temp_row&&heat_.cells[r].porewater.active();
                        const double mobile_a=layered_porewater_a?saturatedPorewaterMobileMass(int(l),s):
                            ma*tr_.dissolvedFraction(s,static_cast<int>(l),state_.theta_s[l]);
                        const double mobile_b=layered_porewater_b?saturatedPorewaterMobileMass(int(r),s):
                            mb*tr_.dissolvedFraction(s,static_cast<int>(r),state_.theta_s[r]);
                        if((layered_porewater_a&&(!std::isfinite(mobile_a)||mobile_a<0))||
                           (layered_porewater_b&&(!std::isfinite(mobile_b)||mobile_b<0))) {
                            if(layered_porewater_a) {
                                auto& owner=heat_.cells[l];owner.failed=1;
                                try{owner.error="Invalid reserved-layer porewater donor for accepted dispersion.";}catch(...){}
                            }
                            if(layered_porewater_b) {
                                auto& owner=heat_.cells[r];owner.failed=1;
                                try{owner.error="Invalid reserved-layer porewater donor for accepted dispersion.";}catch(...){}
                            }
                            continue;
                        }
                        const double ca = mobile_a / va;
                        const double cb = mobile_b / vb;
                        // Porous heat already resolves molecular phase
                        // conduction. Only mechanical dispersion rides the
                        // water temperature row; solute D_m is unchanged.
                        const bool porous_temperature=heat_.active()&&s==tr_.temp_row;
                        double dMd=dt*dispersion::faceFlux(coefficient(l,headL),
                            coefficient(r,headR),nx,ny,dl,dr,armL,armR,
                            gradient(int(l),s),gradient(int(r),s),ca,cb,B);
                        if(porous_temperature) {
                            // Preserve the qualified porous thermal operator.
                            // Its separate conduction owner already supplies
                            // molecular phase heat, so only mechanical mixing
                            // rides this accepted water-temperature packet.
                            const double alpha=.5*(tr_.alpha_L[l]+tr_.alpha_L[r]);
                            dMd=alpha*std::abs(Q)*E.inv_dx[eu]*dt*(ca-cb);
                        }
                        if (dMd == 0.0) continue;
                        // Bound each contribution by the local stencil range;
                        // tensor cross terms can exchange across equal face
                        // concentrations, so a two-point gap is not a bound.
                        double low=std::min(ca,cb),high=std::max(ca,cb);
                        for(int cell:{int(l),int(r)})for(int p=E.cell_ptr[cell];p<E.cell_ptr[cell+1];++p) {
                            const int e=E.cell_edge[p],other=E.cL[e]==cell?E.cR[e]:E.cL[e];
                            if(!covered_[other]||!(satVolume(other)>0))continue;
                            const double c=concentration(other,s);low=std::min(low,c);high=std::max(high,c);
                        }
                        const double capacityL=va/tr_.dissolvedFraction(s,int(l),state_.theta_s[l]);
                        const double capacityR=vb/tr_.dissolvedFraction(s,int(r),state_.theta_s[r]);
                        double cap=dMd>0?std::min((ca-low)*capacityL/nf_l,(high-cb)*capacityR/nf_r):
                            std::min((cb-low)*capacityR/nf_r,(high-ca)*capacityL/nf_l);
                        cap=std::max(0.,cap);
                        if(porous_temperature)cap=std::abs((ca-cb)*va*vb/(va+vb)/
                            static_cast<double>(dMd>0?nf_r:nf_l));
                        // …and a positivity share of the giver's mass. The
                        // SIGNED temperature row has no positivity to
                        // guard and would bind every exchange across a
                        // warm/cold front, so it uses the stencil-range
                        // bound without a nonnegative-mass restriction.
                        if (!tr_.signedRow(s)) {
                            const bool layered_donor=dMd>0?layered_porewater_a:layered_porewater_b;
                            const double give = std::abs((dMd > 0.0)
                                ? (layered_donor?mobile_a:ma)
                                : (layered_donor?mobile_b:mb)) * kFaceShare;
                            const int donor=int(dMd>0?l:r);
                            const double held=dMd>0?ma:mb;
                            const double available=layered_donor?(dMd>0?mobile_a:mobile_b):
                                std::max(0.,held-pendingSaturatedMassExport(donor,s));
                            cap = std::min({cap,give,available});
                        }
                        if (std::abs(dMd) > cap) {
                            dMd = (dMd > 0.0) ? cap : -cap;
                            ++tr_.dispersion_limiter_binds;
                        }
                        const auto k = static_cast<std::size_t>(s) * ne + eu;
                        bookPresetPorewaterOut(int(dMd>0?l:r),s,std::abs(dMd));
                        tr_.sacc_L[k] -= dMd;
                        tr_.sacc_R[k] += dMd;
                    }
                }
            }
        }
    }
    if(tr_.n_layers>0&&heat_.active())for(int i=0;i<state_.n_cells;++i)if(covers(i))
        if(auto error=refreshSigmaPorewater(i);!error.empty())throw std::runtime_error(error);
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
        if (E.cell_sign[ku] > 0) { m += consumePending(tr_.sacc_L[j]); }
        else                     { m += consumePending(tr_.sacc_R[j]); }
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
        if (E.cell_sign[ku] > 0) { v += consumePending(state_.eacc_L[eu]); }
        else                     { v += consumePending(state_.eacc_R[eu]); }
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

void SubsurfaceSolver::sampleNodeExchange(const NodeData* nodes, double dt, double end) {
    if (!state_.active || node_beds_.empty() || nodes == nullptr) return;
    const double l12 = opts_ ? opts_->len_1d_to_2d : 1.0;

    for (std::size_t b = 0; b < node_beds_.size(); ++b) {
        const auto& bed = node_beds_[b];
        if (richards_bed_owned_[b]) continue; // accepted porous owner, never generic mobile exchange
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

        // An unsaturated receiving bed is disconnected from the table:
        // its leakage driver ends at the bed, and it cannot drain SAT upward.
        const double contact=nodes->invert_elev[ni]*l12-bed.dC;
        const double receiver=tr_.n_layers>0?std::max(h_gw,contact):h_gw;
        double Q = cond * (receiver - h_pipe) * lid_bed_factor_[b];
        if(tr_.n_layers>0 && h_gw<contact && Q>0)Q=0;   // m³/s, + out of the aquifer
        const double requested=Q*dt;
        if (Q > 0.0) {
            // Cap an aquifer→pipe drain at the cell's drainable water.
            //
            // G-W1 (2026-09-22): against the SAME specific yield the column
            // will spend it against. This used the textbook θ_s − θ_r while
            // the column used the floored θ_s − θ_bot; for a table a few
            // centimetres under the ground those are 0.35 and 0.001, so the
            // cap sat 350× above anything reachable and never bound. The
            // column then went short on essentially every firing and the
            // shortfall was refunded (fireCell §2) into the AQUIFER's books
            // alone — the router had already handed the unrefunded volume to
            // the node. Both ledgers closed and the seam still created water:
            // 0.625 m³ delivered against 0.067 m³ released on the gate deck,
            // 89 % of it refunded. With the two agreeing, the refund path no
            // longer fires for the node at all (measured: 0 refunds, and
            // still 0 across three closures and five geometries).
            //
            // The FILL direction below keeps θ_s − θ_r, and that is a
            // decision rather than an oversight — see the handoff §3. Two
            // attempts to make it consistent too both regressed G-X1's
            // `FloodedManholeRechargesUntilTheColumnIsFullThenStops`: the
            // floored yield leaves a saturated column claiming 1e-3 of
            // headroom it does not have (a 3.15e-5 m³/s trickle into a full
            // column — the ping-pong G-X1's guard exists to stop), and the
            // unfloored one takes the bed-only deck's continuity from
            // −0.66 % to −1.99 %. The measured leak was on the drain side;
            // the fill side needs its own round.
            const double Sy_col = specificYield(
                ci, paramsOf(bed.cell), state_.theta_s[ci],
                std::max(state_.zs[ci] - state_.hg[ci], 0.0),
                static_cast<GwClosure>(state_.closure[ci]), nullptr);
            const double avail = state_.hg[ci] * Sy_col * state_.area[ci];
            // G-W1b (2026-09-23): count the firings this cap actually
            // CLAMPS. A gate needs to prove its own deck exercises the cap
            // before it can claim anything from the deck's numbers, and the
            // ratio between the two yields does not tell it that: a deck
            // whose ratio is 56.8× was measured BIT-IDENTICAL with the
            // defect present, because `avail` at h_g = 7.6 m is enormous
            // under either yield and the cap never binds. What binds is
            // `avail` being small in ABSOLUTE terms, which only the clamp
            // itself knows. No threshold to tune, and no geometry can make
            // this lie.
            double cap = std::min(kFaceShare * avail,unreservedDrainableWater(bed.cell)) / std::max(dt,kTiny);
            if(lid_receipts_enabled_) {
                NodeWaterBudget budget;
                if(auto e=nodeWaterBudget(bed.cell,dt,budget);!e.empty())throw std::runtime_error(e);
                cap=std::min(cap,budget.donor/dt);
            }
            if (Q > cap) {
                Q = cap;
                ++drain_cap_binds_;
            }
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
                // Use the physical column deficit, including other pending
                // receipts. The old residual-content slab overpromised
                // storage when an equilibrium column approached saturation.
                const double headroom = infiltrationHeadroom(bed.cell);
                double cap = kFaceShare * headroom / std::max(dt, kTiny);
                const double v12 = opts_ ? opts_->vol_1d_to_2d : 1.0;
                const double held = (ni < nodes->volume.size())
                    ? std::max(0.0, nodes->volume[ni]) * v12 : 0.0;
                const double drawn = (ni < node_drawn_gw_.size())
                    ? node_drawn_gw_[ni] : 0.0;
                const double through = (!mobile_lid_bed_[b] && ni < nodes->inflow.size())
                    ? std::max(0.0, nodes->inflow[ni]) * v12 : 0.0;
                cap = std::min(cap, std::max(0.0, held - drawn) /
                                        std::max(dt, kTiny) + through);
                Q = std::max(Q, -cap);
                if (ni < node_drawn_gw_.size())
                    node_drawn_gw_[ni] += std::min(-Q * dt, std::max(0.0, held - drawn));
            }
        }
        if(mobile_lid_bed_[b]) {
            const double sampled=end>=0?end:cell_completed_[ci];
            lid_bed_diagnostics_[b]={std::max(0.,sampled-dt),sampled,cell_completed_[ci],h_pipe,h_gw,
                std::max(0.,-requested),std::max(0.,requested),std::max(0.,-Q*dt),std::max(0.,Q*dt),
                std::max(0.,-Q*dt),std::max(0.,Q*dt)};
        }
        if (Q == 0.0) continue;

        const double vol = Q * dt;
        // T7.4: a node pushing water DOWN its bed (Q < 0) sends its own
        // quality with it. The row layout is shared with the 1D (both come
        // from TransportPolicy), so row s means the same species on both
        // sides and no mapping is needed — only the unit factor the volume
        // already takes. The aquifer→node direction is booked at the CELL's
        // firing instead, where the cell's concentration is known.
        //     Review F22 (2026-10-15): the store is concentration × m³, so
        //     the mass rides the 2D volume `-vol`. This booked the 1D's
        //     ft³ instead, and every recharge carried 35.31× its water's
        //     concentration into the aquifer — invisible to both ledgers,
        //     because the booking and the store shared the factor.
        if (tr_.active() && vol < 0.0 && node_row_conc_ != nullptr &&
            node_row_ns_ == tr_.n_species) {
            const double v_2d = -vol;  // m³, the aquifer's own volume unit
            const auto nb = node_beds_.size();
            for (int sp = 0; sp < tr_.n_species; ++sp) {
                const double c = node_row_conc_[ni * static_cast<std::size_t>(node_row_ns_) +
                                                static_cast<std::size_t>(sp)];
                if (c == 0.0) continue;
                const int layer=contactLayer(bed.cell,contact);
                if(layer>=0)tr_.contact_node_mass[tr_.layerIdx(sp,layer,bed.cell)]+=v_2d*c;
                else tr_.nacc_mass[static_cast<std::size_t>(sp) * nb + b] += v_2d * c;
            }
        }
        if(tr_.n_layers>0 && vol>0)tr_.contact_node_out[b]+=vol;
        if(heat_.active()&&vol>0)heat_node_out_volume_[b]+=vol;
        if(vol<0) {
            const int layer=contactLayer(bed.cell,contact);
            if(layer>=0)tr_.contact_water[std::size_t(layer)*state_.n_cells+ci]-=vol;
        }
        state_.nacc[b] += vol;                       // gathered at the GW firing
        if(lid_receipts_enabled_) {
            node_pending_down_[b]+=std::max(0.,-vol);node_pending_up_[b]+=std::max(0.,vol);
        }
        bed_exchange_cum_[b] += vol;                 // G-O: the per-bed series
        if(mobile_lid_bed_[b]) {
            richards_down_cum_[b]+=std::max(0.,-vol);
            richards_up_cum_[b]+=std::max(0.,vol);
        }
        if (ni < node_exchange_vol_.size())
            node_exchange_vol_[ni] += vol;           // flushed by the router
    }
    accumulators_pending_ = true;
}

double SubsurfaceSolver::gatherNodeMass(int i, int s) noexcept {
    if (tr_.nacc_mass.empty()) return 0.0;
    const auto nb = node_beds_.size();
    double m = 0.0;
    for (std::size_t b = 0; b < nb; ++b) {
        if (node_beds_[b].cell != i) continue;
        const auto k = static_cast<std::size_t>(s) * nb + b;
        m += consumePending(tr_.nacc_mass[k]);
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

// G-W1 (2026-09-21): the single owner of specific yield.
//
// `Sy` is the storage coefficient every saturated source spends against, so
// a withdrawal CAPPED against `h_g·Sy·A` and then SPENT against `Sy·A` must
// use the same `Sy` or the cap does not bind. It did not: `sampleNodeExchange`
// used the textbook θ_s − θ_r while the column used the floored θ_s − θ_bot,
// which for a table a few centimetres under the ground sits on `kSyFloor` —
// 0.35 against 0.001, a factor of 350. The cap was therefore never reached,
// the column went short on essentially every firing, and the shortfall was
// refunded (fireCell §2's clamp) into the aquifer's OWN books only. The 1D
// had already been handed the unrefunded volume by the router, so the
// aquifer's continuity closed to 0.000 % while the node received water the
// aquifer never released: 0.625 m³ delivered against 0.067 m³ given up on
// the G-W1 gate deck, 89 % of it refunded.
//
// One function, so the cap and the spend cannot disagree again. `theta_bot`
// is reported back because the CLOSED_FORM branch pairs the handover slab
// with the `Sy` actually used (G-X4) and SIGMA's recharge flux needs the
// bottom layer's content.
double SubsurfaceSolver::specificYield(std::size_t u, const soil::Params& p,
                                       double ts, double L0, GwClosure cl,
                                       double* theta_bot_out) const noexcept {
    double theta_bot = state_.theta_r[u];
    double Sy = std::max(ts - theta_bot, kSyFloor);
    if (cl == GwClosure::SIGMA) {
        const auto n = static_cast<std::size_t>(state_.n_cells);
        const auto bot =
            static_cast<std::size_t>(state_.m_layers - 1) * n + u;
        if (bot < state_.theta_sigma.size()) {
            theta_bot = state_.theta_sigma[bot];
            // The plan writes θ_s here. See the file header: with the
            // handover explicit, θ_s − θ_bot is what conserves, and it IS
            // specific yield.
            Sy = std::max(ts - theta_bot, kSyFloor);
        }
    } else if (cl == GwClosure::ENSLAVED) {
        // hᵤ = hᵤ*(L) algebraically ⇒ the storage debit is θ(ψ=L), the
        // equilibrium water content at the SURFACE. `q⁺` then drives the
        // table with no unsaturated lag at all.
        Sy = std::max(ts - soil::waterContent(p, L0), kSyFloor);
    } else {
        // CONSISTENCY, not taste: `Sy` and the handover slab content the
        // column pairs with it must sum to θ_s, or the two zones disagree
        // about how much water a moving table carries and the cell leaks at
        // a rate proportional to |ḣ_g|. The bulk store's own mean content is
        // the self-consistent choice — and it reduces to the textbook
        // θ_s − θ_r once the column has drained.
        theta_bot = std::clamp(state_.hu[u] / std::max(L0, kTiny),
                               state_.theta_r[u], ts);
        Sy = std::max(ts - theta_bot, kSyFloor);
    }
    if (theta_bot_out != nullptr) *theta_bot_out = theta_bot;
    return Sy;
}

double SubsurfaceSolver::specificYieldOf(int cell) const noexcept {
    const auto u = static_cast<std::size_t>(cell);
    if (u >= state_.hg.size()) return 0.0;
    return specificYield(u, paramsOf(cell), state_.theta_s[u],
                         std::max(state_.zs[u] - state_.hg[u], 0.0),
                         static_cast<GwClosure>(state_.closure[u]), nullptr);
}

// Pending outward packets have already been promised to their receivers.
// They cannot be spent again before this cell gathers its next interval.
double SubsurfaceSolver::unreservedDrainableWater(int cell) const noexcept {
    const auto u=std::size_t(cell);
    double reserved=(tr_.n_layers>0?tr_.contact_link_out[u]:std::max(0.,-state_.lacc[u]))+pendingLidUp(cell);
    if(!state_.eacc_L.empty())for(int p=edges_->cell_ptr[u];p<edges_->cell_ptr[u+1];++p) {
        const auto e=std::size_t(edges_->cell_edge[p]);
        reserved+=std::max(0.,-(edges_->cell_sign[p]>0?state_.eacc_L[e]:state_.eacc_R[e]));
    }
    if(!lid_receipts_enabled_)for(int b:cell_beds_[u])
        reserved+=tr_.n_layers>0?tr_.contact_node_out[b]:std::max(0.,state_.nacc[b]);
    return std::max(0.,specificYieldOf(cell)*state_.hg[u]*state_.area[u]-reserved);
}
double SubsurfaceSolver::pendingSaturatedMassExport(int cell,int species) const noexcept {
    const auto u=std::size_t(cell),ne=state_.eacc_L.size();
    double reserved=tr_.lout_mass.empty()?0:std::max(0.,tr_.lout_mass[tr_.idx(species,cell)]);
    if(!tr_.sacc_L.empty())for(int p=edges_->cell_ptr[u];p<edges_->cell_ptr[u+1];++p) {
        const auto e=std::size_t(edges_->cell_edge[p]),k=std::size_t(species)*ne+e;
        reserved+=std::max(0.,-(edges_->cell_sign[p]>0?tr_.sacc_L[k]:tr_.sacc_R[k]));
    }
    return reserved;
}

std::string SubsurfaceSolver::nodeWaterBudget(int cell, double interval, NodeWaterBudget& out) const {
    if (!state_.active || cell<0 || cell>=state_.n_cells || !std::isfinite(interval) || interval<=0)
        return "Invalid aquifer node trial cell/interval.";
    const auto u=static_cast<std::size_t>(cell);
    for(double value:{state_.area[u],state_.zs[u],state_.hg[u],state_.hu[u],state_.et_pending[u],state_.c_loss[u]})
        if(!std::isfinite(value) || value<0) return "Invalid aquifer storage/demand in node trial.";
    if(state_.area[u]<=0 || state_.zs[u]<=0 || state_.hg[u]>state_.zs[u] ||
       !std::isfinite(state_.lacc[u])) return "Invalid aquifer geometry/pending water in node trial.";
    // Wells/live drivers require their interval reservations in the eventual
    // scheduling adapter. A temperature-only row does not change the water
    // budget and is qualified by the paired thermal receipt path; all other
    // transported rows still require their own node adapters.
    const bool unsupported_transport=tr_.active()&&
        (tr_.n_pollut>0||tr_.n_msx>0||tr_.age_row>=0||tr_.temp_row<0);
    if (unsupported_transport || (u<cell_sources_.size() && !cell_sources_[u].empty()) || !runtime_sources_.rows.empty())
        return "Node trials require water-only or temperature-only aquifers without unreserved wells/live drivers.";
    if(!std::isfinite(state_.Ks[u]) || state_.Ks[u]<0 || !std::isfinite(state_.theta_s[u]) ||
       !std::isfinite(state_.theta_r[u]) || state_.theta_r[u]<0 || state_.theta_s[u]<=state_.theta_r[u] ||
       state_.theta_s[u]>1) return "Invalid aquifer material in node trial.";
    if(static_cast<GwClosure>(state_.closure[u])==GwClosure::SIGMA)
        for(int k=0;k<state_.m_layers;++k) {
            const double theta=state_.theta_sigma[k*state_.n_cells+cell];
            if(!std::isfinite(theta) || theta<state_.theta_r[u] || theta>state_.theta_s[u])
                return "Invalid aquifer sigma state in node trial.";
        }
    const double L=std::max(0.0,state_.zs[u]-state_.hg[u]);
    if(static_cast<GwClosure>(state_.closure[u])==GwClosure::ENSLAVED) {
        const double equilibrium=soil::equilibriumStorage(paramsOf(cell),L);
        if(!std::isfinite(equilibrium) || std::abs(state_.hu[u]-equilibrium)>
            1e-12*std::max({1.,equilibrium,state_.hu[u]}))
            return "Enslaved aquifer node trial requires its actual equilibrium storage.";
    }
    double donor=specificYieldOf(cell)*state_.hg[u]*state_.area[u];
    if (static_cast<GwClosure>(state_.closure[u])==GwClosure::ENSLAVED)
        donor=std::max(0.0,state_.theta_s[u]*state_.hg[u]+soil::equilibriumStorage(paramsOf(cell),L)
                         -soil::equilibriumStorage(paramsOf(cell),state_.zs[u]))*state_.area[u];
    double claimed=std::max(0.0,state_.et_pending[u])+std::max(0.0,-state_.lacc[u])+pendingLidUp(cell);
    claimed+=state_.c_loss[u]*state_.hg[u]/std::max(state_.zs[u],kTiny)*state_.area[u]*interval;
    if (!state_.eacc_L.empty())   // PER_SUBCATCH: no lateral accumulators
    for (int p=edges_->cell_ptr[u];p<edges_->cell_ptr[u+1];++p) {
        const auto e=static_cast<std::size_t>(edges_->cell_edge[p]);
        const double pending=edges_->cell_sign[p]>0?state_.eacc_L[e]:state_.eacc_R[e];
        if(!std::isfinite(pending))return "Nonfinite pending lateral node trial water.";
        claimed+=std::max(0.0,-pending);
    }
    for (int b:cell_beds_[u]) {
        if(!std::isfinite(state_.nacc[b]))return "Nonfinite pending node trial water.";
        if(!lid_receipts_enabled_)claimed+=std::max(0.0,state_.nacc[b]);
    }
    NodeWaterBudget proposed{state_.z_bed[u]+state_.hg[u],std::max(0.0,donor-claimed),infiltrationHeadroom(cell)};
    if (!std::isfinite(proposed.head) || !std::isfinite(proposed.donor) || !std::isfinite(proposed.receiver))
        return "Nonfinite aquifer node trial budget.";
    out=proposed;return {};
}

double SubsurfaceSolver::gatherNode(int i) noexcept {
    double v = 0.0;
    // T7.4 fix (2026-09-21): the per-bed split weights are the volumes THIS
    // gather takes, recorded here. They were previously accumulated across a
    // routing step and cleared at its flush — but a cell fires on the LTS
    // ladder, not the routing step, so any firing whose weights had already
    // been cleared found `drew == 0` and dropped its outgoing mass on the
    // floor. The aquifer still booked `lost_node`, so the mass left the
    // aquifer's books and never reached the node's: 75 % of it on the gate
    // deck. Tying the weights to the gather makes them exact by
    // construction, because they ARE the volumes that produced this
    // firing's `node_out`.
    std::fill(bed_gathered_.begin(), bed_gathered_.end(), 0.0);
    for (std::size_t b = 0; b < node_beds_.size(); ++b) {
        if (node_beds_[b].cell != i) continue;
        const double take = consumePending(state_.nacc[b]);
        if(heat_.active())heat_node_gathered_out_[b]=consumePending(heat_node_out_volume_[b]);
        if(lid_receipts_enabled_) {consumePending(node_pending_down_[b]);consumePending(node_pending_up_[b]);}
        if (b < bed_gathered_.size()) bed_gathered_[b] = tr_.n_layers>0?consumePending(tr_.contact_node_out[b]):take;
        v += take;
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
    heat_.clear();heat_link_out_volume_.clear();heat_preset_out_j_.clear();heat_preset_out_water_.clear();heat_node_out_volume_.clear();heat_node_gathered_out_.clear();
    heat_faces_.clear();heat_pending_left_.clear();heat_pending_right_.clear();heat_dt_.clear();
    porewater_preset_out_.clear();
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
        for (int pass = 0; pass < 3; ++pass) for (const auto& r : gw->initial_quality) {
            if (static_cast<int>(r.scope) != pass || r.zone == GwZone::LAYER) continue;
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

    }
    // T7.2: resolve `[GW_TRANSPORT_PARAMS]` and `[GW_SORPTION]` onto cells,
    // `* < TAG < CELL` by pass — the same precedence the `[2D_AQUIFER]`
    // rows use, so one deck's two aquifer sections behave the same way.
    if (gw != nullptr) {
        tr_.dispersion_on = gw->options.dispersion;
        tr_.c_diff = gw->options.c_diff;
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

    if(gw && gw->options.unsat_transport=="LAYERED") {
        if(options_.per_subcatch)throw std::runtime_error("LAYERED groundwater transport requires a spatial SIGMA mesh.");
        for(int i=0;i<state_.n_cells;++i)if(covers(i) &&
            static_cast<GwClosure>(state_.closure[i])!=GwClosure::SIGMA)
            throw std::runtime_error("UNSAT_TRANSPORT LAYERED requires SIGMA on every active aquifer cell.");
        tr_.n_layers=state_.m_layers;
        tr_.layer_mass.assign(std::size_t(tr_.n_species)*tr_.n_layers*tr_.n_cells,0);
        tr_.contact_water.assign(std::size_t(tr_.n_layers)*tr_.n_cells,0);
        tr_.contact_node_out.assign(node_beds_.size(),0);tr_.contact_link_out.assign(tr_.n_cells,0);
        tr_.contact_node_mass.assign(tr_.layer_mass.size(),0);
        tr_.contact_link_mass.assign(tr_.layer_mass.size(),0);
        std::fill(tr_.sat_mass.begin(),tr_.sat_mass.end(),0);
        std::fill(tr_.unsat_mass.begin(),tr_.unsat_mass.end(),0);
        // Resolve scope first, then the baseline before layer overrides at
        // that scope. A global LAYER cannot trump a cell UNSAT baseline.
        for(int pass=0;pass<3;++pass)for(int layers=0;layers<2;++layers)
            for(const auto& row:gw->initial_quality) {
                if(int(row.scope)!=pass || (row.zone==GwZone::LAYER)!=bool(layers))continue;
                if(row.zone==GwZone::LAYER && (row.layer<1 || row.layer>tr_.n_layers))
                    throw std::runtime_error("[GW_INITIAL_QUALITY] LAYER exceeds the SIGMA layer count.");
                const int species=tr_.rowIndex(row.species);if(species<0)continue;
                for(int i=0;i<state_.n_cells;++i)if(covers(i)&&scopeCoversCell(i,row.scope,row.tag,row.cell)) {
                    const double partition=(species==tr_.age_row||species==tr_.temp_row)?0:
                        tr_.kd[tr_.idx(species,i)]*tr_.rho_b[i];
                    if(row.zone==GwZone::SAT) {
                        tr_.sat_mass[tr_.idx(species,i)]=row.value*
                            state_.area[i]*state_.hg[i]*(state_.theta_s[i]+partition);
                    } else for(int j=0;j<tr_.n_layers;++j) {
                        if(row.zone==GwZone::LAYER && row.layer!=j+1)continue;
                        const double dz=(state_.zs[i]-state_.hg[i])/tr_.n_layers;
                        tr_.layer_mass[tr_.layerIdx(species,j,i)]=row.value*dz*state_.area[i]*
                            (state_.theta_sigma[std::size_t(j)*state_.n_cells+i]+partition);
                    }
                }
            }
        for(int species=0;species<tr_.n_species;++species)for(int i=0;i<state_.n_cells;++i)
            tr_.refreshUnsat(species,i);
    }

    if(opts_&&opts_->reconstruction_order==2&&tr_.age_row>=0) {
        tr_.generated_age.assign(tr_.n_species,0);tr_.gained_contact.assign(tr_.n_species,0);
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

std::string SubsurfaceSolver::configureSourceReceipts(const std::vector<int>& cells) {
    if(sourceReceiptOwnershipBound())
        return "Aquifer source receipts already configured.";
    for(int i:cells) {
        if(i<0 || i>=state_.n_cells || state_.xacc_from_surface[i]!=0 || state_.xacc_to_surface[i]!=0)
            return "Source receipt ownership must bind before pending surface exchange.";
        for(int p=0;p<tr_.n_species;++p)
            if(tr_.xacc_from_surface[tr_.idx(p,i)]!=0 || tr_.xacc_to_surface[tr_.idx(p,i)]!=0)
                return "Source receipt ownership must bind before pending mass exchange.";
    }
    for(int i:cells) source_owned_[i]=true;source_species_=tr_.row_names;
    return {};
}
void SubsurfaceSolver::retainSourceReceipt(const SurfaceIntakeReceipt& r) {
    if(source_owned_[r.request.cell] && r.volume>0) source_receipts_[r.request.cell].push_back(r);
}
void SubsurfaceSolver::acknowledgeSourceReturns(double through) {
    std::erase_if(source_returns_,[&](const auto& r){return r.available_at<=through;});
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

double SubsurfaceSolver::infiltrationHeadroom(int cell) const noexcept {
    if (!state_.active || !covers(cell)) return 0.0;
    const auto u = static_cast<std::size_t>(cell);
    const double L = std::max(0.0, state_.zs[u] - state_.hg[u]);
    double col = state_.hu[u];
    const auto cl = static_cast<GwClosure>(state_.closure[u]);
    if (cl == GwClosure::ENSLAVED) col = soil::equilibriumStorage(paramsOf(cell), L);
    if (cl == GwClosure::SIGMA) col = sigma::columnStorage(&state_.theta_sigma[u], state_.m_layers, state_.n_cells, L);
    double deficit = std::max(0.0, (state_.theta_s[u] * L - col) * state_.area[u]);
    // Do not promise future drainage from the explicit top layer. Its actual
    // headroom is renewed only after the column has advanced.
    if (cl == GwClosure::SIGMA)
        deficit = std::min(deficit, std::max(0.0, state_.theta_s[u] - state_.theta_sigma[u]) *
                          L / state_.m_layers * state_.area[u]);
    double pending = std::max(0.0, state_.xacc_from_surface[u]) +
        (tr_.n_layers>0?std::max(0.,state_.lacc[u]+tr_.contact_link_out[u]):std::max(0.0,state_.lacc[u])) + pendingLidDown(cell);
    // PER_SUBCATCH sizes no lateral accumulators: there is no lateral flow.
    if (!state_.eacc_L.empty())
    for (int p = edges_->cell_ptr[u]; p < edges_->cell_ptr[u + 1]; ++p) {
        const auto e = static_cast<std::size_t>(edges_->cell_edge[p]);
        pending += std::max(0.0, edges_->cell_sign[p] > 0 ? state_.eacc_L[e] : state_.eacc_R[e]);
    }
    if(!lid_receipts_enabled_)for (const auto b : cell_beds_[u])
        pending += tr_.n_layers>0?std::max(0.,tr_.contact_node_out[b]-state_.nacc[b]):std::max(0.0,-state_.nacc[b]);
    return std::max(0.0, deficit - pending);
}

void SubsurfaceSolver::publishInfiltration(SurfaceStateData& surf, double interval, double time) {
    if (!state_.active || !(interval > 0.0)) return;
    for (int i = 0; i < state_.n_cells; ++i) publishCellInfiltration(i, surf, interval, time);
}

void SubsurfaceSolver::publishCellInfiltration(int i, SurfaceStateData& surf, double interval, double time) {
        const auto u = static_cast<std::size_t>(i);
        const double A = state_.area[u];
        const double remaining = infiltrationHeadroom(i);
        state_.infil_remaining[u] = remaining;
        const double pond = u < surf.depth.size() ? std::max(0.0, surf.depth[u]) : 0.0;
        double rate = remaining > 0.0 ? sourceInfiltrationCapacity(i, pond, interval) : 0.0;
        if (A > 0.0) rate = std::min(rate, remaining / (A * interval));
        state_.infil_capacity[u] = rate;
        state_.infil_refresh[u] = time; state_.infil_interval[u] = interval;
        if (u < surf.infil_rate.size()) surf.infil_rate[u] = rate;
}

double SubsurfaceSolver::sourceInfiltrationCapacity(int i, double pond, double interval) const noexcept {
        if (!state_.active || !covers(i) ||
            !(interval > 0.0) || !std::isfinite(interval) ||
            !(pond >= 0.0) || !std::isfinite(pond)) return 0.0;
        const auto u = static_cast<std::size_t>(i);
        const double A = state_.area[u], L = std::max(0.0, state_.zs[u] - state_.hg[u]);
        double rate = 0.0;
        if (A > 0.0 && L > 0.0) {
            const auto p = paramsOf(i);
            const auto cl = static_cast<GwClosure>(state_.closure[u]);
            if (cl == GwClosure::SIGMA) {
                const double Se = std::clamp((state_.theta_sigma[u] - p.theta_r) / (p.theta_s - p.theta_r), 1e-6, 1.0);
                const double suction = soil::suctionAtSaturation(p, Se);
                rate = p.Ks * (1.0 + (pond + suction) / (0.5 * L / state_.m_layers));
            } else if (cl == GwClosure::ENSLAVED) {
                rate = p.Ks * (1.0 + pond / L);
            } else {
                const double theta = std::clamp(state_.hu[u] / L, p.theta_r, p.theta_s);
                const double delta = p.theta_s - theta;
                const double Se = std::clamp((theta - p.theta_r) / (p.theta_s - p.theta_r), 1e-6, 1.0);
                const double suction = soil::suctionAtSaturation(p, Se);
                rate = frontIntake(state_.wetting_front[u], (pond+suction)*delta, p.Ks, interval)/interval;
            }
        }
        return std::isfinite(rate) ? std::max(0.0, rate) : 0.0;
}

double SubsurfaceSolver::acceptSurfaceInfiltration(int cell, double requested, bool prescribed) noexcept {
    if (!(requested > 0.0) || !std::isfinite(requested) || !covers(cell) || !state_.active) return 0.0;
    const auto u = static_cast<std::size_t>(cell);
    if (!prescribed && (!(state_.Ks[u] > 0.0) || !(state_.infil_capacity[u] > 0.0) || !std::isfinite(state_.infil_capacity[u]))) return 0.0;
    const double take = std::min(requested, infiltrationHeadroom(cell));
    state_.xacc_from_surface[u] += take;
    state_.wetting_front[u] += take / state_.area[u];
    state_.infil_remaining[u] = infiltrationHeadroom(cell);
    return take;
}

void SubsurfaceSolver::bookInfiltrationFromSurface(int cell,
                                                   double vol_m3) noexcept {
    if (!state_.active || !covers(cell)) return;
    state_.xacc_from_surface[static_cast<std::size_t>(cell)] += vol_m3;
    // Cell-local booking is called concurrently by the surface marcher.
    // settle() does not use the obsolete pending flag; do not race on it.
}

// G-X3 (2026-09-19): conduit seepage lands in the saturated zone of the
// cells the conduit crosses. Booked per routing step, gathered per firing —
// the same cross-cadence accumulator the surface infiltration uses.
void SubsurfaceSolver::bookLinkSeepage(int cell, double vol_m3) noexcept {
    if (!state_.active || !covers(cell)) return;
    state_.lacc[static_cast<std::size_t>(cell)] += vol_m3;
    if(tr_.n_layers>0 && vol_m3<0)tr_.contact_link_out[cell]-=vol_m3;
    accumulators_pending_ = true;
}

int SubsurfaceSolver::contactLayer(int cell,double elevation) const noexcept {
    if(tr_.n_layers<=0 || !covers(cell) || !std::isfinite(elevation))return -1;
    const double table=state_.z_bed[cell]+state_.hg[cell];
    const double length=state_.zs[cell]-state_.hg[cell];
    if(elevation<=table || !(length>0))return -1;
    const double depth=state_.z_bed[cell]+state_.zs[cell]-elevation;
    return std::clamp(int(std::floor(std::max(0.,depth)/length*tr_.n_layers)),0,tr_.n_layers-1);
}

void SubsurfaceSolver::bookLinkContact(int cell,double volume,const std::vector<double>& concentration,
                                       double begin,double end) {
    if(!active() || !covers(cell) || !(volume>0))return;
    if(int(concentration.size())!=tr_.n_species || !std::isfinite(volume) ||
       !std::isfinite(begin) || !std::isfinite(end))
        throw std::invalid_argument("Invalid accepted conduit contact receipt.");
    for(int s=0;s<tr_.n_species;++s)if(!std::isfinite(concentration[s]) ||
        (!tr_.signedRow(s)&&concentration[s]<0))
        throw std::invalid_argument("Invalid accepted conduit contact composition.");
    std::vector<double> cuts{0,1};
    if(tr_.n_layers>0 && begin!=end) {
        const double length=state_.zs[cell]-state_.hg[cell];
        const double ground=state_.z_bed[cell]+state_.zs[cell];
        for(int j=1;j<=tr_.n_layers;++j) {
            const double t=(ground-length*j/tr_.n_layers-begin)/(end-begin);
            if(t>0 && t<1)cuts.push_back(t);
        }
        std::sort(cuts.begin(),cuts.end());
    }
    bookLinkSeepage(cell,volume);
    for(std::size_t i=1;i<cuts.size();++i) {
        const double v=volume*(cuts[i]-cuts[i-1]);
        const int layer=contactLayer(cell,begin+(end-begin)*(.5*(cuts[i]+cuts[i-1])));
        if(layer>=0)tr_.contact_water[std::size_t(layer)*state_.n_cells+cell]+=v;
        for(int s=0;s<tr_.n_species;++s) {
            if(layer>=0)tr_.contact_link_mass[tr_.layerIdx(s,layer,cell)]+=v*concentration[s];
            else bookLinkSeepageMass(cell,s,v*concentration[s]);
        }
    }
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

void SubsurfaceSolver::bookSurfaceEt(int cell,double dt,double rate,double surface) {
    if(!active()||options_.per_subcatch||!covers(cell)||!(dt>0.0))return;
    const auto u=static_cast<std::size_t>(cell);
    const double potential=std::max(0.0,rate)*state_.area[u]*dt;
    const double taken=std::clamp(surface,0.0,potential);
    state_.et_pending[u]+=potential-taken;
    state_.et_potential_cumulative[u]+=potential; state_.et_surface_cumulative[u]+=taken;
    state_.et_surface_last[u]=taken/(state_.area[u]*dt); state_.et_potential_last[u]=std::max(0.0,rate);
    state_.et_refresh[u]=(std::isfinite(state_.et_refresh[u])?state_.et_refresh[u]:0.0)+dt;
}

void SubsurfaceSolver::bookAreaEt(int cell,double dt,double potential,double evaporation,double soil) {
    if(!active()||options_.per_subcatch||!covers(cell)||!(dt>0.0))return;
    if(!std::isfinite(dt)||!std::isfinite(potential)||!std::isfinite(evaporation)||!std::isfinite(soil)||
       potential<0||evaporation<0||soil<0||evaporation+soil>potential+1e-12*std::max(1.0,potential))
        throw std::invalid_argument("Invalid completed atmospheric area budget.");
    const auto u=static_cast<std::size_t>(cell);
    soil=std::min(soil,std::max(0.0,potential-evaporation));
    state_.et_pending[u]+=soil;
    state_.et_potential_cumulative[u]+=potential;state_.et_surface_cumulative[u]+=evaporation;
    state_.et_unused_cumulative[u]+=std::max(0.0,potential-evaporation-soil);
    state_.et_surface_last[u]=evaporation/(state_.area[u]*dt);
    state_.et_potential_last[u]=potential/(state_.area[u]*dt);
    state_.et_refresh[u]=(std::isfinite(state_.et_refresh[u])?state_.et_refresh[u]:0.0)+dt;
}

void SubsurfaceSolver::fireCell(int i, double dt, SurfaceStateData& surf, double time) {
    if(tr_.n_layers>0&&heat_.active())heat_.refreshView(i,tr_);
    const auto u = static_cast<std::size_t>(i);
    const int  n = state_.n_cells;
    const double A  = std::max(state_.area[u], kTiny);
    const double zs = state_.zs[u];
    const double ts = state_.theta_s[u];
    const soil::Params p = paramsOf(i);
    const auto cl = static_cast<GwClosure>(state_.closure[u]);
    const bool sigma_stage=cl==GwClosure::SIGMA&&opts_&&opts_->reconstruction_order==2;

    const double hg0 = state_.hg[u];
    const double hu_pre = state_.hu[u];   // T7.1: the column store this firing starts from
    const double L0  = std::max(zs - hg0, 0.0);

    // Verify the paired queue before any cell/face gather. Unpaired or future
    // receipts cannot be consumed by an owned cell's groundwater firing.
    const bool paired=source_owned_[u];
    if(paired) {
        if(!std::isfinite(time)||time<0||!std::isfinite(dt)||dt<=0||!std::isfinite(time+dt))
            throw std::runtime_error("Invalid completed source groundwater firing interval.");
        if(time<source_cell_end_[u])throw std::runtime_error("Completed source groundwater firing cannot replay.");
        if(source_species_!=tr_.row_names || int(source_species_.size())!=tr_.n_species)
            throw std::runtime_error("Source receipt pollutant layout changed before groundwater firing.");
        double volume=0;std::vector<double> mass(tr_.n_species,0);
        for(const auto& r:source_receipts_[u]) {
            if(int(r.mass.size())!=tr_.n_species)throw std::runtime_error("Source receipt mass layout differs from groundwater.");
            if(r.end>source_firing_end_) throw std::runtime_error("Future source receipt in groundwater firing.");
            volume+=r.volume;for(int p=0;p<tr_.n_species;++p)mass[p]+=r.mass[p];
        }
        const auto close=[](double a,double b){return std::abs(a-b)<=1e-12*std::max({1.0,std::abs(a),std::abs(b)});};
        if(!close(volume,state_.xacc_from_surface[u]))throw std::runtime_error("Unpaired source water in owned groundwater cell.");
        for(int p=0;p<tr_.n_species;++p)if(!close(mass[p],tr_.xacc_from_surface[tr_.idx(p,i)]))
            throw std::runtime_error("Unpaired source mass in owned groundwater cell.");
    }
    // Completed hydraulic receipts become ordinary signed cell water only
    // when this groundwater owner reaches their endpoint. Later receipts
    // keep both directional budgets reserved across earlier cell firings.
    double lid_in_j=0;
    if(lid_receipts_enabled_)for(int b:cell_beds_[u]) {
        auto& pending=lid_receipts_[b];
        for(const auto& r:pending)if(r.end<=time+dt) {
            if(heat_.active()&&!r.heat_applied)
                throw std::runtime_error("Groundwater firing reached a Mesh LID interval before its thermal receipt was applied.");
            lid_in_j+=r.down_j;
        }
        if(!std::isfinite(lid_in_j))throw std::runtime_error("Nonfinite accepted LID heat receipt in groundwater firing.");
        for(const auto& r:pending)if(r.end<=time+dt)state_.nacc[b]+=r.up-r.down;
        std::erase_if(pending,[&](const auto& r){return r.end<=time+dt;});
    }
    // --- gather the accumulated cross-cadence volumes ---------------------
    const double lat_vol  = gatherLateral(i);                 // m³, + in
    const double node_vol = gatherNode(i);                    // m³, + OUT
    const double future_lid_up=pendingLidUp(i);
    double infil_vol = consumePending(state_.xacc_from_surface[u]); // m³, + in
    // G-X3: conduit seepage delivered to this cell since its last firing —
    // a saturated-zone inflow, so it rides the table update with the
    // lateral and node volumes and shares their storage coefficient.
    const double link_vol = consumePending(state_.lacc[u]); // m³, + in
    state_.qlink_last[u] = link_vol / dt;
    state_.led_link += link_vol;
    const double link_out=tr_.n_layers>0?consumePending(tr_.contact_link_out[u]):std::max(0.,-link_vol);
    std::vector<double> contact_depth(tr_.n_layers);
    double contact_volume=0;
    for(int j=0;j<tr_.n_layers;++j) {
        const double v=consumePending(tr_.contact_water[std::size_t(j)*n+u]);
        contact_depth[j]=v/A;contact_volume+=v;
    }
    const double saturated_exchange=lat_vol-node_vol+link_vol-contact_volume;

    const double q_in = infil_vol / (A * dt);                 // m/s at the top
    state_.qplus_last[u] = q_in;
    state_.led_infil_in += infil_vol;

    // Surface-cadence demand has already paid actual surface evaporation.
    const bool et_boundary=(options_.gw_et=="BOUNDARY_ET"||options_.gw_et=="BOTH");
    const bool et_caprise=(options_.gw_et=="CAPILLARY_RISE"||options_.gw_et=="BOTH");
    const double budget=options_.per_subcatch?std::max(0.0,surf.exposedEvaporation(i))*A*dt:consumePending(state_.et_pending[u]);
    double q_et=et_boundary?budget/(A*dt):0.0;
    if(et_boundary && cl!=GwClosure::SIGMA){
        const double Se=L0>kTiny?std::clamp((state_.hu[u]/L0-state_.theta_r[u])/std::max(ts-state_.theta_r[u],kTiny),1.0e-6,1.0):1.0;
        const double psi=cl==GwClosure::ENSLAVED?L0:soil::suctionAtSaturation(p,Se);
        state_.et_stress[u]=soil::feddesStress(psi,options_.wilting_suction);
        q_et*=state_.et_stress[u];
    }

    // --- 1. recharge across the table -------------------------------------
    double q0 = 0.0;      // m/s, + down
    double theta_bot = state_.theta_r[u];
    // G-W1 (2026-09-21): `Sy` comes from the one function that owns it, so
    // that anything CAPPING a withdrawal against the cell's drainable water
    // caps against the same number the column will then use to spend it.
    // `sampleNodeExchange` used the textbook θ_s − θ_r while the column used
    // the floored θ_s − θ_bot — see specificYield().
    double Sy = specificYield(u, p, ts, L0, cl, &theta_bot);

    if (cl == GwClosure::SIGMA && L0 > 0.0) {
        // The physical Darcy flux across the table is the bottom layer's
        // gravity drainage. It is passed INTO the sweep as `q0_phys` and
        // comes back inside `f_bot` together with the handover, which is why
        // the saturated update above books `q0` and not `f_bot`.
        q0 = soil::conductivity(p, soil::suctionAtSaturation(
                 p, std::clamp((theta_bot - state_.theta_r[u]) /
                                   std::max(ts - state_.theta_r[u], kTiny),
                               1.0e-6, 1.0)));
    } else if (cl == GwClosure::ENSLAVED) {
        q0 = q_in - q_et;
    } else if (L0 > 0.0) {
        q0 = rawRecharge(i,p,std::max(L0,kTiny));
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
    if (cl!=GwClosure::ENSLAVED && !et_caprise && q0 < 0.0) q0 = 0.0;   // GW_ET NONE: no capillary rise

    // Availability, the same idiom as every other flux here: a column may not
    // hand down more than it holds above residual, and the table may not hand
    // up more than it holds. Without the first cap a closure-A cell whose
    // equilibrium it cannot reach drains itself past residual and then keeps
    // "delivering" at K̄ forever — a leak that grows without bound and looks,
    // from the ledger, like recharge.
    if (cl != GwClosure::ENSLAVED) {
        if (q0 > 0.0) {
            double give = std::max(state_.hu[u] - state_.theta_r[u] * L0,0.0) / dt;
            if(sigma_stage)give=std::min(give,std::max(0.,theta_bot-state_.theta_r[u])*L0/state_.m_layers/dt);
            q0 = std::min(q0, give);
        } else if (q0 < 0.0) {
            // Already delivered node/pipe withdrawals cannot be refunded
            // from the aquifer alone. Internal rise spends only the water
            // remaining after those committed signed exchanges.
            const double available = std::max(0.0, Sy * hg0 * A
                                              + saturated_exchange-future_lid_up);
            // Leave a few ulps so multiply/divide roundoff cannot make
            // the saturated update refund an already delivered node debit.
            const double take = available / (A * dt) * (1.0 - 8.0 * std::numeric_limits<double>::epsilon());
            q0 = std::max(q0, -take);
        }
    }
    if(cl==GwClosure::CLOSED_FORM)q_et=std::min(q_et,std::max(0.0,state_.hu[u]-state_.theta_r[u]*L0-std::max(0.0,q0)*dt)/dt);
    if(cl==GwClosure::ENSLAVED){
        const double donor=std::max(0.0,ts*hg0+soil::equilibriumStorage(p,L0)-soil::equilibriumStorage(p,zs)
                                       +q_in*dt+(lat_vol-node_vol+link_vol-future_lid_up)/A);
        q_et=std::min(q_et,donor/dt); q0=q_in-q_et;
    }
    state_.q0_last[u] = q0;

    // --- 2. saturated explicit FV update ----------------------------------
    double q_deep = state_.c_loss[u] * hg0 / std::max(zs, kTiny);
    if(lid_receipts_enabled_) {
        const double donor=cl==GwClosure::ENSLAVED
            ? std::max(0.,ts*hg0+soil::equilibriumStorage(p,L0)-soil::equilibriumStorage(p,zs))*A
            : Sy*hg0*A;
        q_deep=std::min(q_deep,std::max(0.,donor+q0*A*dt+saturated_exchange-future_lid_up)/(A*dt));
    }
    state_.qdeep_last[u] = q_deep;
    state_.qlat_last[u]  = lat_vol / dt;
    state_.qnode_last[u] = node_vol / dt;

    // Every saturated source shares one storage coefficient — that is the
    // §sy table's whole content.
    GwSourcePulse source;
    if (u < cell_sources_.size() && !cell_sources_[u].empty()) {
        source.mass.assign(static_cast<std::size_t>(tr_.n_species), 0.0);
        for (const auto& entry : cell_sources_[u]) {
            const auto& authored=sources_[entry.first];
            bool overridden=false;
            if(u<runtime_sources_.cells.size())
                for(auto k:runtime_sources_.cells[u])
                    if(runtime_sources_.rows[k].id==authored.name) { overridden=true; break; }
            if(!overridden) integrateGwSource(authored,time,dt,entry.second,source);
        }
    }
    const bool runtime = u < runtime_sources_.cells.size() && !runtime_sources_.cells[u].empty();
    if (runtime) {
        source.mass.resize(tr_.n_species, 0.0);
        for (auto k : runtime_sources_.cells[u]) {
            auto& s = runtime_sources_.rows[k];
            if (s.boundary_edge>=0 && std::isfinite(s.boundary_head)) {
                // One-sided unconfined Darcy face: arithmetic saturated
                // transmissivity between the cell and the boundary head.
                const int slot=MeshData::slot(i,s.boundary_edge);
                const double outside=std::max(s.boundary_head,state_.z_bed[u]);
                const double height=std::clamp(outside-state_.z_bed[u],0.0,zs);
                const double conductance=state_.Ks[u]*0.5*(hg0+height)*mesh_->edge_length[slot]
                    /mesh_->edge_dist_c[slot];
                s.flow=conductance*(outside-state_.z_bed[u]-hg0);
            }
            const double in = std::max(0.0,s.flow)*dt;
            source.in += in; source.out += std::max(0.0,-s.flow)*dt;
            s.receipt.requested += s.flow*dt;
            for(int r=0;r<tr_.n_species;++r) {
                const double dm = in*s.concentrations[r]+dt*std::max(0.0,s.rates[r]);
                source.mass[r] += dm;
                s.receipt.requested_mass[r] += dm;
                s.receipt.applied_mass[r] += dm;
            }
        }
        for (auto k : runtime_sources_.cells[u]) {
            auto& s=runtime_sources_.rows[k];
            s.receipt.heat_requested += s.heat*dt;
            if (ts*hg0*A+source.in>0.0 && tr_.temp_row>=0) {
                const double dm=s.heat*dt/opts_->water_heat_capacity;
                source.mass[tr_.temp_row] += dm;
                s.receipt.heat_applied += s.heat*dt;
                s.receipt.requested_mass[tr_.temp_row] += dm;
                s.receipt.applied_mass[tr_.temp_row] += dm;
            }
        }
    }
    std::vector<GwBoundaryPulse> boundary_pulses;
    double boundary_in=0,boundary_out=0;
    if(u<cell_boundaries_.size())for(auto index:cell_boundaries_[u]) {
        auto pulse=integrateGwBoundary(boundaries_[index],time,dt,state_.z_bed[u]+hg0,
            state_.z_bed[u],zs,state_.Ks[u],tr_.n_species);
        pulse.boundary=index;
        boundary_in+=pulse.in;boundary_out+=pulse.out;
        boundary_pulses.push_back(std::move(pulse));
    }
    const double other_sat = (q0 - q_deep) * A * dt + saturated_exchange;
    // Wells withdraw only available water after all other signed channels.
    // ENSLAVED uses its exact storage relation; other closures use the same
    // specific yield their table update spends against.
    double drainable = Sy * hg0 * A;
    if (cl == GwClosure::ENSLAVED)
        drainable = std::max(0.0, ts * hg0 + soil::equilibriumStorage(p,L0)
                                  - soil::equilibriumStorage(p,zs)) * A;
    const double requested_source_out = source.out;
    const double combined_out=source.out+boundary_out;
    // A bounded SIGMA sweep can reduce its proposed recharge. External
    // withdrawals cannot spend that unaccepted column-to-table transfer.
    const double saturated_credit=other_sat-(sigma_stage?std::max(0.,q0)*A*dt:0.);
    const double accepted_scale=combined_out>0 ?
        std::min(1.,std::max(0.,drainable+saturated_credit+source.in+boundary_in-future_lid_up)/combined_out):1.;
    source.out*=accepted_scale;boundary_out*=accepted_scale;
    for(auto& pulse:boundary_pulses) {
        auto& boundary=boundaries_[pulse.boundary];
        boundary.requested_in+=pulse.in;boundary.requested_out+=pulse.out;
        pulse.out*=accepted_scale;
        boundary.applied_in+=pulse.in;boundary.applied_out+=pulse.out;
    }
    state_.led_boundary_in+=boundary_in;state_.led_boundary_out+=boundary_out;
    state_.led_lateral+=boundary_in-boundary_out;
    const double runtime_scale = requested_source_out > 0.0 ? source.out/requested_source_out : 1.0;
    if(runtime) for(auto k:runtime_sources_.cells[u]) {
        auto& s=runtime_sources_.rows[k];
        const double in=std::max(0.0,s.flow)*dt, out=std::max(0.0,-s.flow)*dt*runtime_scale;
        s.receipt.applied+=in-out; s.receipt.water_in+=in; s.receipt.water_out+=out;
        s.receipt.last_flow=(in-out)/dt; s.receipt.last_dt=dt;
    }
    state_.led_source_in += source.in;
    state_.led_source_out += source.out;
    const double dV_sat = other_sat + source.in - source.out + boundary_in - boundary_out;
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
    } else if (cl != GwClosure::SIGMA && hg1 > zs) {
        dunne_vol = (hg1 - zs) * Sy * A;
        hg1 = zs;
    } else if (cl != GwClosure::SIGMA && (hg1 < 0.0 || ens_short >= 0.0)) {
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
            // G-W1b (2026-09-23): this is the defect's own signature. The
            // router has ALREADY handed the unrefunded volume to the node,
            // so every count here is water the 1D keeps and the aquifer
            // disowns. With the drain cap measured against the yield the
            // column spends against, it should never fire: ~120 → 0 on the
            // gate deck (124 on Linux, 117 on macOS — the count is
            // platform-dependent, the zero is not). A gate asserting zero
            // fails loudly on the bug and needs no tolerance; it held at 0
            // across three closures, five geometries and 20 min to 12 h,
            // including a deck that clamps 8,635 times.
            ++node_refunds_;
            state_.qnode_last[u] = (node_vol - refund_node) / dt;
        }
    }
    state_.led_recharge += q0 * A * dt;
    if (cl!=GwClosure::ENSLAVED && q0 < 0.0) state_.led_caprise += -q0 * A * dt;
    state_.led_deep += state_.qdeep_last[u] * A * dt;
    state_.led_lateral += lat_vol;
    state_.led_node    += state_.qnode_last[u] * dt;

    // --- 3-4. the unsaturated zone sees the CLAMPED table ------------------
    const double L1 = std::max(zs - hg1, 0.0);
    double rejected = 0.0;

    sigma::ColumnTransfers column_transfers;
    if (cl == GwClosure::SIGMA) {
        // A post-sweep height correction cannot spend a negative surplus
        // when the predicted column collapses to a saturated, zero-yield
        // profile. Settle the table with the actual bounded bottom transfer
        // instead; every trial starts from the same original layer water.
        static thread_local std::vector<double> original, trial;
        original.resize(state_.m_layers);trial.resize(state_.m_layers);
        for(int j=0;j<state_.m_layers;++j) original[j]=state_.theta_sigma[static_cast<std::size_t>(j)*n+u];
        sigma::ColumnStep cs;
        // Smooth RK stages evaluate ALE fluxes at the starting state. The
        // bounded table solve below settles capacity constraints without
        // replacing that Euler velocity by an endpoint-dependent subflow.
        const bool euler=sigma_stage;
        const double grid_rate=-(hg1-hg0)/dt;
        double external=(saturated_exchange+source.in-source.out+boundary_in-boundary_out)/A-state_.qdeep_last[u]*dt;
        const double tolerance=1e-15*std::max({1.0,zs,std::abs(external)});
        const auto evaluate=[&](double h,bool capture=false) {
            trial=original;cs={};cs.L_old=L0;cs.L_new=std::max(0.0,zs-h);cs.dt=dt;
            cs.forward_euler=euler;cs.grid_rate=grid_rate;
            cs.q_in=q_in;cs.q_et=q_et;cs.q0_phys=q0;cs.handover_theta=ts-Sy;
            if(tr_.n_layers>0)cs.layer_inflow=&contact_depth;
            if(capture)cs.transfers=&column_transfers;
            cs.wilting_suction=options_.wilting_suction;cs.capillary=options_.capillary_diff;
            sigma::advanceColumn(p,trial.data(),state_.m_layers,1,cs);
            if(euler)for(double& theta:trial)theta=rk2SigmaView(theta,p.theta_r,p.theta_s);
            // ts * delta_h = external + the actual column-to-table water.
            const double balance = ts*(h-hg0)-external-
                (cs.f_bot+cs.overflow_to_sat-cs.deficit_from_sat)*dt;
            if (!std::isfinite(balance))
                throw std::runtime_error("SIGMA table/column water settlement is non-finite.");
            return balance;
        };
        double lo=0.0,hi=zs;
        hg1=std::clamp(hg1,lo,hi);
        double residual=evaluate(hg1);
        if(std::abs(residual)>tolerance) {
            double lower=evaluate(lo);
            if(lower>tolerance) {
                // No saturated water remains to support this withdrawal.
                // Preserve the existing deep-first refund order, using the
                // settled column balance rather than a floored predictor.
                double short_volume=lower*A;
                const double deep=std::min(short_volume,state_.qdeep_last[u]*A*dt);
                state_.qdeep_last[u]-=deep/(A*dt);state_.led_deep-=deep;
                external+=deep/A;short_volume-=deep;
                const double node=std::min(short_volume,std::max(0.0,state_.qnode_last[u]*dt));
                if(node>0) {
                    ++node_refunds_;state_.qnode_last[u]-=node/dt;state_.led_node-=node;
                    external+=node/A;
                }
                lower=evaluate(lo);
                if(lower>tolerance) throw std::runtime_error("SIGMA saturated withdrawals exceed available column/table water.");
            }
            const double upper=evaluate(hi);
            if(upper<-tolerance) {
                hg1=hi;dunne_vol=-upper*A;
            } else if(std::abs(lower)<=tolerance) {
                hg1=lo;evaluate(hg1);
            } else if(std::abs(upper)<=tolerance) {
                hg1=hi;
            } else {
                // Bounded scalar consistency solve; no forcing or water
                // histories change in a trial, and no future flux is credit.
                for(int iteration=0;iteration<100;++iteration) {
                    hg1=lo+.5*(hi-lo);residual=evaluate(hg1);
                    if(std::abs(residual)<=tolerance) break;
                    if(residual>0) hi=hg1;else lo=hg1;
                }
                if(std::abs(residual)>tolerance)
                    throw std::runtime_error("SIGMA table/column water settlement did not converge.");
            }
        }
        if(tr_.n_layers>0||(heat_.active()&&sigma_stage))evaluate(hg1,true);
        for(int j=0;j<state_.m_layers;++j) state_.theta_sigma[static_cast<std::size_t>(j)*n+u]=trial[j];
        rejected=cs.rejected;state_.et_stress[u]=et_boundary?cs.et_stress:std::numeric_limits<double>::quiet_NaN();
        state_.qet_last[u]=cs.et_taken;state_.led_et+=cs.et_taken*A*dt;
        state_.hu[u]=sigma::columnStorage(trial.data(),state_.m_layers,1,zs-hg1);
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

    if(!options_.per_subcatch){
        const double taken=state_.qet_last[u]*A*dt;
        state_.et_soil_cumulative[u]+=taken;
        state_.et_unused_cumulative[u]+=std::max(0.0,budget-taken);
    }

    // --- 5. Dunne saturation excess (step 12) -----------------------------
    // Two routes reach the surface twin and they are physically distinct:
    // `dunne_vol` is the table itself arriving at the ground (`h_g → z_s`),
    // and `reject_vol` is water the top layer would not take. Both are
    // saturation excess. Ordinary cells use the mesh accumulator; reviewed
    // source cells keep donor refunds distinct from local emergence. Together
    // they ARE the mass balance
    // (draft decision 9), which is why DUNNE defaults ON.
    const double reject_vol = rejected * A * dt;
    const double to_surface = (options_.dunne ? dunne_vol : 0.0) + reject_vol;
    if (to_surface > 0.0 && !paired) {
        state_.xacc_to_surface[u] += to_surface;
        markPendingSurface(i);
    }
    const double physical_return = options_.dunne ? dunne_vol : 0.0;
    state_.led_dunne += physical_return; state_.led_reject += reject_vol;
    state_.dunne_cumulative[u] += physical_return; state_.reject_cumulative[u] += reject_vol;
    state_.dunne_last[u] = physical_return / dt; state_.reject_last[u] = reject_vol / dt;

    // Consume the same fraction as the existing water/mass gathers. Keep
    // residual tuples until the next knot/firing, even across source intervals.
    const double paired_refund=paired ? std::min(reject_vol,infil_vol) : 0;
    std::vector<double> refund_mass,dunne_mass,excess_mass;
    std::vector<SurfaceReturnReceipt> refunds;
    double return_ready=paired ? source_firing_end_ : time+dt;
    if(paired) {
        refund_mass.assign(tr_.n_species,0);dunne_mass.assign(tr_.n_species,0);excess_mass.assign(tr_.n_species,0);
        const double fraction=infil_vol>0 ? paired_refund/infil_vol : 0;
        for(auto& pending:source_receipts_[u]) {
            auto consumed=pending;
            consumed.volume=consumePending(pending.volume);
            for(int p=0;p<tr_.n_species;++p)consumed.mass[p]=consumePending(pending.mass[p]);
            return_ready=std::max(return_ready,consumed.end);
            if(fraction>0 && consumed.volume>0) {
                SurfaceReturnReceipt r;r.kind=SurfaceReturnKind::NUMERICAL_REFUND;r.intake=consumed;
                r.available_at=std::max(time+dt,consumed.end);r.volume=consumed.volume*fraction;
                r.mass.resize(tr_.n_species);
                for(int p=0;p<tr_.n_species;++p) {r.mass[p]=consumed.mass[p]*fraction;refund_mass[p]+=r.mass[p];}
                refunds.push_back(std::move(r));
            }
        }
        std::erase_if(source_receipts_[u],[](const auto& r){return r.volume==0;});
    }

    // --- T7.1: the tuple rides the volumes this firing just decided -------
    // Every number below is the one the water used, read AFTER the refund
    // paths may have rewritten a rate (`qnode_last`, `qdeep_last`, `qet_last`),
    // so mass can never travel on a volume the water gave back.
    if (tr_.active()) {
        CellFlux f;
        if(tr_.n_layers>0||(heat_.active()&&sigma_stage))f.column=&column_transfers;
        f.boundary_in=boundary_in;f.boundary_out=boundary_out;f.boundaries=&boundary_pulses;
        f.hg_final=hg1;f.runtime_dt=dt; f.requested_source_out=requested_source_out;
        f.v_sat0   = hg0 * ts * A;
        f.v_uns0   = hu_pre * A;
        f.lateral  = lat_vol;
        f.node_out = state_.qnode_last[u] * dt;
        if(tr_.n_layers>0) {
            f.node_out=0;
            for(std::size_t b=0;b<node_beds_.size();++b)if(node_beds_[b].cell==i)
                f.node_out+=std::max(0.,bed_gathered_[b]);
            f.node_in=f.node_out-state_.qnode_last[u]*dt;
        }
        f.lid_in_j = lid_in_j;
        f.infil_in = infil_vol;
        f.link     = link_vol;
        f.link_out = link_out;
        f.source_in = source.in; f.source_out = source.out; f.source_mass = &source.mass;
        f.recharge = q0 * A * dt;
        f.et       = state_.qet_last[u] * A * dt;
        f.deep     = state_.qdeep_last[u] * A * dt;
        f.dt       = dt;   // T7.2
        f.dunne    = options_.dunne ? dunne_vol : 0.0;
        f.reject   = reject_vol;
        if(paired) { f.paired_refund=paired_refund;f.refund_mass=&refund_mass;
            f.dunne_mass=&dunne_mass;f.excess_mass=&excess_mass; }
        // What the column's own balance could not have been produced by a
        // flux IS the moving boundary's swap. Reading it off the final
        // store rather than re-deriving `θ_bot·ΔL` keeps it exact under
        // every closure — including the σ column, whose handover lives
        // inside its ALE sweep.
        f.handover = (state_.hu[u] * A - f.v_uns0) - f.infil_in + f.et +
                     f.recharge + f.reject;
        f.grid_volume=A*(hg0-hg1);
        if(opts_&&opts_->reconstruction_order==2&&cl==GwClosure::ENSLAVED&&ens_dunne<0&&ens_short<0&&hg1>0&&hg1<zs) {
            // Equilibrium water is nonlinear in table height. The accepted
            // projection is not an Euler transport velocity: evaluate its
            // tangent at the beginning state, as for the SIGMA ALE stage.
            f.grid_volume=-dV_sat/Sy;f.handover=(ts-Sy)*f.grid_volume;
        }
        fireCellSpecies(i, f);
    }

    if(paired) {
        for(auto& r:refunds)source_returns_.push_back(std::move(r));
        const auto emergence=[&](SurfaceReturnKind kind,double volume,const std::vector<double>& mass) {
            if(volume<=0)return;
            SurfaceReturnReceipt r;r.kind=kind;r.intake.request.cell=i;r.intake.start=time;r.intake.end=time+dt;
            r.available_at=return_ready;r.volume=volume;r.mass=mass;source_returns_.push_back(std::move(r));
        };
        emergence(SurfaceReturnKind::PHYSICAL_DUNNE,physical_return,dunne_mass);
        emergence(SurfaceReturnKind::COLUMN_EXCESS,reject_vol-paired_refund,excess_mass);
    }
    if(paired)source_cell_end_[u]=time+dt;
    state_.hg[u] = hg1;
    cell_completed_[u] = time + dt;
    // Drying recovers the equivalent wetting-front depth in proportion to
    // the newly created column deficit; no second time constant is authored.
    const double deficit_before = std::max(0.0, ts * L0 - hu_pre);
    const double deficit_after = std::max(0.0, ts * std::max(0.0, zs - hg1) - state_.hu[u]);
    const double expected = std::max(0.0, deficit_before - infil_vol / A);
    if (deficit_after > expected && deficit_after > kTiny) {
        if((cl==GwClosure::CLOSED_FORM||cl==GwClosure::ENSLAVED) && opts_ && opts_->reconstruction_order==2) {
            // SSP stages require the drying derivative at the old front and
            // old deficit. The finite ratio used by the first-order held
            // intake route contributes its own O(dt^2) subflow and cannot be
            // composed as a forward-Euler RK stage.
            const double front0=std::max(0.,state_.wetting_front[u]-infil_vol/A);
            const double drying=deficit_after-expected;
            if(deficit_before>kTiny)state_.wetting_front[u]=std::max(0.,state_.wetting_front[u]-front0*drying/deficit_before);
        } else state_.wetting_front[u] *= expected / deficit_after;
    }
    if (state_.infil_interval[u] > 0.0)
        publishCellInfiltration(i, surf, state_.infil_interval[u], time + dt);
}

// ---------------------------------------------------------------------------
// T7.1 — one cell's species pass
// ---------------------------------------------------------------------------

std::string SubsurfaceSolver::initLateralHeat() {
    const auto& o=heat_.config->options;
    if(!o.conduction||!o.lateral_conduction)return {};
    const int n=state_.n_cells,nz=o.thermal_layers;const auto& edges=*edges_;
    heat_faces_.resize(edges.ne);heat_pending_left_.assign(std::size_t(edges.ne)*nz,0);
    heat_pending_right_=heat_pending_left_;heat_dt_.assign(n,1e30);
    std::vector<double> conductance(std::size_t(n)*nz,0);
    for(int e=0;e<edges.ne;++e) {
        const int l=edges.cL[e],r=edges.cR[e];if(!covers(l)||!covers(r))continue;
        auto& face=heat_faces_[e];const auto& a=heat_.cells[l].column;const auto& b=heat_.cells[r].column;
        face.distance_left=mesh_->edge_dist_c[edges.slotL[e]];face.distance_right=mesh_->edge_dist_c[edges.slotR[e]];
        if(!(face.distance_left>0)||!(face.distance_right>0)||!std::isfinite(face.distance_left)||!std::isfinite(face.distance_right))
            return "Lateral heat contact requires positive cell-centroid normal distances on both sides of every face.";
        const double ka=std::max({a.material.grain_conductivity,a.material.water_conductivity,a.material.air_conductivity});
        const double kb=std::max({b.material.grain_conductivity,b.material.water_conductivity,b.material.air_conductivity});
        int j=0,k=0;while(j<nz&&k<nz) {
            const double atop=mesh_->tri_cz[l]-a.depth_edges[j],abot=mesh_->tri_cz[l]-a.depth_edges[j+1];
            const double btop=mesh_->tri_cz[r]-b.depth_edges[k],bbot=mesh_->tri_cz[r]-b.depth_edges[k+1];
            const double low=std::max(abot,bbot),high=std::min(atop,btop);
            if(high>low) {
                face.slices.push_back({j,k,low,high});
                const double g=edges.xi[e]*(high-low)/(face.distance_left/ka+face.distance_right/kb);
                conductance[std::size_t(l)*nz+j]+=g;conductance[std::size_t(r)*nz+k]+=g;
            }
            if(abot>=bbot)++j;if(bbot>=abot)++k;
        }
    }
    // Solid capacity and the largest phase conductivity give a bound that
    // remains safe when theta or the saturated footprint changes between
    // tier rebuilds. The explicit contact sum is at most one half per step.
    for(int i=0;i<n;++i)if(covers(i)) {
        const auto& c=heat_.cells[i].column;const auto& m=c.material;
        for(int j=0;j<nz;++j)if(const double g=conductance[std::size_t(i)*nz+j];g>0) {
            const double capacity=c.area*(c.depth_edges[j+1]-c.depth_edges[j])*(1-m.porosity)*m.grain_density*m.grain_specific_heat;
            heat_dt_[i]=std::min(heat_dt_[i],std::min(.5,o.c_diff)*capacity/g);
        }
    }
    return {};
}
void SubsurfaceSolver::fireLateralHeatFace(int e,double dt) {
    const auto& face=heat_faces_[e];if(face.slices.empty())return;
    const int l=edges_->cL[e],r=edges_->cR[e],nz=heat_.config->options.thermal_layers;
    const auto& a=heat_.cells[l].column;const auto& b=heat_.cells[r].column;
    const double table=std::min(state_.z_bed[l]+state_.hg[l],state_.z_bed[r]+state_.hg[r]);
    for(const auto& s:face.slices) {
        const double height=std::min(s.high,table)-s.low;if(!(height>0))continue;
        const double g=edges_->xi[e]*height/(face.distance_left/a.conductivity[s.left]+face.distance_right/b.conductivity[s.right]);
        const double joules=dt*g*(heat_.activeTemperature(l,s.left)-heat_.activeTemperature(r,s.right));
        if(!std::isfinite(joules))throw std::runtime_error("Nonfinite saturated lateral thermal contact; check material properties and step size.");
        heat_pending_left_[std::size_t(e)*nz+s.left]-=joules;
        heat_pending_right_[std::size_t(e)*nz+s.right]+=joules;
    }
}
void SubsurfaceSolver::gatherLateralHeat(int i) noexcept {
    if(heat_faces_.empty())return;
    const int nz=heat_.config->options.thermal_layers;auto& cell=heat_.cells[i];
    for(int p=edges_->cell_ptr[i];p<edges_->cell_ptr[i+1];++p) {
        const int e=edges_->cell_edge[p];auto& pending=edges_->cell_sign[p]>0?heat_pending_left_:heat_pending_right_;
        for(int j=0;j<nz;++j) {
            auto& receipt=pending[std::size_t(e)*nz+j];cell.column.energy[j]+=receipt;cell.lateral_j+=receipt;receipt=0;
        }
    }
}
double SubsurfaceSolver::pendingHeat(int i) const noexcept {
    if(heat_faces_.empty()||i<0||i>=state_.n_cells)return 0;
    const int nz=heat_.config->options.thermal_layers;double total=0;
    for(int p=edges_->cell_ptr[i];p<edges_->cell_ptr[i+1];++p) {
        const int e=edges_->cell_edge[p];const auto& pending=edges_->cell_sign[p]>0?heat_pending_left_:heat_pending_right_;
        for(int j=0;j<nz;++j)total+=pending[std::size_t(e)*nz+j];
    }
    return total;
}
std::string SubsurfaceSolver::initHeat(SimulationContext& ctx,const GwTransportData& config) {
    transport_units_.clear();
    for(int s=0;s<tr_.n_species;++s) {
        if(s<tr_.n_pollut)transport_units_.push_back("POLLUT:"+std::to_string(int(ctx.pollutants.units[s])));
        else if(s<tr_.n_pollut+tr_.n_msx) {
            const int id=ctx.reactions.find_species(tr_.row_names[s]);
            transport_units_.push_back("MSX:"+ctx.reactions.species_units[id]+":"+std::to_string(int(ctx.reactions.species_is_wall[id])));
        }
        else transport_units_.push_back(s==tr_.age_row?"AGE:s":"TEMPERATURE:C");
    }

    heat_.clear();heat_link_out_volume_.clear();heat_preset_out_j_.clear();heat_preset_out_water_.clear();heat_node_out_volume_.clear();heat_node_gathered_out_.clear();
    heat_faces_.clear();heat_pending_left_.clear();heat_pending_right_.clear();heat_dt_.clear();
    porewater_preset_out_.clear();
    if(config.options.thermal_model!="POROUS_EQUILIBRIUM"||tr_.temp_row<0)return {};
    if(config.options.thermal_model=="POROUS_EQUILIBRIUM" && tr_.temp_row>=0 && options_.per_subcatch)
        return "Porous ground heat currently requires a spatial mesh; PER_SUBCATCH retains water-only heat transport.";
    if(auto error=heat_.initialize(ctx,*mesh_,state_,tr_,config,covered_);!error.empty())return error;
    if(heat_.active()) {
        heat_link_out_volume_.assign(state_.n_cells,0);
        heat_preset_out_j_.assign(std::size_t(state_.n_cells)*config.options.thermal_layers,0);
        heat_preset_out_water_.assign(heat_preset_out_j_.size(),0);
        heat_node_out_volume_.assign(node_beds_.size(),0);heat_node_gathered_out_=heat_node_out_volume_;
        for(int i=0;i<state_.n_cells;++i)heat_.refreshView(i,tr_);
        if(auto error=initLateralHeat();!error.empty())return error;
        refreshDtCell(*mesh_,*edges_);
    }
    return {};
}

void SubsurfaceSolver::bookPresetHeatOut(int i,double volume,double temperature_volume) noexcept {
    if(!heat_.active()||!(volume>0)||i<0||std::size_t(i)>=heat_.cells.size())return;
    auto& cell=heat_.cells[i];auto& c=cell.column;const auto& sat=cell.saturated_water;
    const auto n=c.energy.size();if(!n)return;
    const auto base=std::size_t(i)*heat_.config->options.thermal_layers;
    const double total=std::accumulate(sat.begin(),sat.end(),0.);if(!(total>0))return;
    const double target=heat_.water_capacity*temperature_volume;double sum=0;std::size_t last=0;
    if(cell.reserved_out_water.size()!=n){cell.failed=1;return;}
    const auto receipt=[&](std::size_t j){return heat_.activeTemperature(i,j)*
        (volume*(sat[j]/total)*heat_.water_capacity);};
    for(std::size_t j=0;j<n;++j)if(sat[j]>0) {
        last=j;const double energy=receipt(j);
        const double reserved_water=cell.reserved_out_water[j]+volume*(sat[j]/total);
        const double active_capacity=c.capacity[j]-c.material.water_heat_capacity*reserved_water;sum+=energy;
        if(!std::isfinite(energy)||!std::isfinite(heat_preset_out_j_[base+j]+energy)||
           !std::isfinite(heat_preset_out_water_[base+j]+volume*(sat[j]/total))||
           !std::isfinite(c.energy[j]-energy)||!(active_capacity>0)||!std::isfinite(active_capacity)) {cell.failed=1;return;}
    }
    const double correction=target-sum;
    if(!std::isfinite(target)||!std::isfinite(correction)||
       !std::isfinite(heat_preset_out_j_[base+last]+receipt(last)+correction)||
       !std::isfinite(c.energy[last]-receipt(last)-correction)){cell.failed=1;return;}
    for(std::size_t j=0;j<n;++j)if(sat[j]>0) {
        const double part=receipt(j);
        const double reserved=part+(j==last?correction:0.0);
        // The accepted face/link transfer has already left this cell, even
        // though the hydraulic/thermal column gathers its volume at the next
        // tier firing. Reserve its producer-time sensible energy now so an
        // intervening surface/ground contact cannot spend that packet again.
        heat_preset_out_j_[base+j]+=part;
        heat_preset_out_water_[base+j]+=volume*(sat[j]/total);
        cell.reserved_out_water[j]+=volume*(sat[j]/total);
        cell.column.energy[j]-=reserved;
        cell.advective_j-=reserved;
    }
    heat_preset_out_j_[base+last]+=correction;
}

std::string SubsurfaceSolver::conductGroundWithLids(int i,double dt) {
    if(!heat_.active()||i<0||std::size_t(i)>=heat_.cells.size())return {};
    auto& ground=heat_.cells[std::size_t(i)];auto& soil=ground.column;
    std::vector<int> beds;
    for(std::size_t b=0;b<node_beds_.size();++b)
        if(richards_bed_owned_[b]&&node_beds_[b].cell==i)beds.push_back(int(b));
    if(beds.empty())return heat_.conduct(i,dt);
    if(!heat_.context||soil.energy.empty())
        return "Mesh LID contact has no compatible porous soil thermal column.";
    // MeshHeat/ChannelGround own this same soil column when a surface or open
    // channel finite contact is configured. Their shared solve carries the
    // LID stores from GroundHeatState::lid_owners; never advance the contact
    // again on the groundwater clock.
    if(ground.surface_owned)return {};
    auto& ctx=*heat_.context;
    const auto& options=heat_.config->options;
    const double initial=ctx.heat_config.global_temp[static_cast<int>(HeatSource::INITIAL_STATE)];
    constexpr double ft2_to_m2=.09290304;
    struct LidOwner {int row=-1;double area=0;std::size_t ground_contact=std::numeric_limits<std::size_t>::max();LidNodeThermalGraph graph;};
    std::vector<LidOwner> lids;lids.reserve(beds.size());
    double occupied=0;
    for(const int b:beds) {
        const auto& bed=node_beds_[b];const int row=ctx.node_subtypes.storage_row(bed.node);
        if(row<0)return "Mesh LID thermal contact has no storage node.";
        auto& state=ctx.node_subtypes.storages.lid_state[row];
        if(!state.richards||state.cells.empty())return "Mesh LID heat currently requires a Richards layer stack.";
        LidOwner owner;owner.row=row;
        const double bed_area=bed.area>0?bed.area:soil.area;
        const double lid_area=state.cells.back().area*ft2_to_m2;
        owner.area=std::min({bed_area,lid_area,soil.area});
        if(!(owner.area>0)||!std::isfinite(owner.area))return "Mesh LID/ground thermal contact has no mapped overlap area.";
        if(auto error=buildLidNodeThermalGraph(state,0,heat_.water_capacity,
            ctx.heat_config.conduction,initial,owner.graph);!error.empty())return error;
        if(!ctx.heat_config.layer_conduction)owner.graph.contacts.clear();
        if(!state.thermal_ground_contact_j.empty()&&
           (state.thermal_ground_contact_j.size()!=state.cells.size()||
            std::any_of(state.thermal_ground_contact_j.begin(),state.thermal_ground_contact_j.end(),
                        [](double x){return !std::isfinite(x); })))
            return "Mesh LID ground-contact receipt shape or value is invalid.";
        occupied+=owner.area;
        if(!std::isfinite(occupied)||occupied>soil.area*(1+1e-12))
            return "Mesh LID thermal footprints overlap beyond their groundwater cell area.";
        lids.push_back(std::move(owner));
    }
    const bool soil_conduction=options.conduction;
    const double snow_fraction=heat_.snowContactFraction(i);
    if(occupied>0&&snow_fraction>0)
        return "Mesh LID and reciprocal UEB contacts overlap the same groundwater cell; resolve their surface ownership first.";
    if(occupied/soil.area+snow_fraction>1+1e-12)
        return "Mesh LID and snow contacts exceed their groundwater cell area.";
    if(!ctx.heat_config.layer_conduction&&!soil_conduction)return {};
    if(soil_conduction&&options.surface_thermal_bc=="SURFACE_WATER"&&occupied<soil.area*(1-1e-12))
        return "SURFACE_WATER thermal boundary requires a reciprocal mesh ground-column contact.";

    std::vector<ThermalStore> stores;std::vector<ThermalContact> contacts;
    std::size_t lid_store_count=0;
    for(const auto& owner:lids)lid_store_count+=owner.graph.stores.size();
    stores.reserve(lid_store_count+(soil_conduction?soil.energy.size():0));
    contacts.reserve(lid_store_count+soil.energy.size()+lids.size());
    std::vector<std::size_t> offsets;offsets.reserve(lids.size());
    std::vector<unsigned char> vertical_contact;
    for(const auto& owner:lids) {
        offsets.push_back(stores.size());
        stores.insert(stores.end(),owner.graph.stores.begin(),owner.graph.stores.end());
        for(const auto& c:owner.graph.contacts) {
            contacts.push_back({int(offsets.back())+c.a,int(offsets.back())+c.b,c.conductance});
            vertical_contact.push_back(1);
        }
    }
    std::vector<ThermalBoundary> boundaries;std::size_t top_count=0;int snow_boundary=-1;
    if(soil_conduction) {
        const int ground_offset=int(stores.size());
        for(std::size_t j=0;j<soil.energy.size();++j) {
            const double capacity=heat_.activeCapacity(i,j);
            if(!(capacity>0)||!std::isfinite(capacity))return "Mesh LID contact found a nonpositive active groundwater heat capacity.";
            stores.push_back({capacity,soil.energy[j]});
        }
        for(const auto& c:soil.contacts()) {
            contacts.push_back({ground_offset+c.a,ground_offset+c.b,c.conductance});
            vertical_contact.push_back(1);
        }
        const double exposed=std::max(0.,1-occupied/soil.area-snow_fraction);
        top_count=heat_.boundaries(i,exposed,boundaries);
        if(heat_.appendSnowContact(i,soil.area*snow_fraction,boundaries))snow_boundary=int(boundaries.size()-1);
        if(options.conduction)for(std::size_t k=0;k<lids.size();++k) {
            const auto& owner=lids[k];
            ThermalContact contact;
            if(auto error=buildLidNodeGroundThermalContact(owner.graph,stores.size(),ground_offset,
                owner.area,soil.area,soil.depth_edges[1]-soil.depth_edges[0],soil.conductivity.front(),0,contact);!error.empty())return error;
            contact.a+=int(offsets[k]);lids[k].ground_contact=contacts.size();contacts.push_back(contact);vertical_contact.push_back(0);
        }
        (void)ground_offset;
    } else if(lid_store_count==0)return {};
    if(stores.empty())return {};
    auto solved=solveThermalContacts(stores,contacts,boundaries,dt);
    if(!solved.error.empty())return solved.error;
    // Stage every LID publication first; the ground and snow owners change
    // only after all graph layouts and the shared implicit solve qualify.
    std::vector<std::vector<double>> lid_energy(lids.size());
    std::vector<LidNodeState> lid_states;
    lid_states.reserve(lids.size());
    for(std::size_t k=0;k<lids.size();++k) {
        const auto offset=offsets[k],count=lids[k].graph.cells.size();
        lid_energy[k].assign(solved.energy.begin()+offset,solved.energy.begin()+offset+count);
        lid_states.push_back(ctx.node_subtypes.storages.lid_state[lids[k].row]);
        if(auto error=publishLidNodeThermalEnergy(lid_states.back(),lids[k].graph,lid_energy[k]);!error.empty())return error;
        auto& receipts=lid_states.back().thermal_ground_contact_j;
        if(receipts.empty())receipts.assign(lid_states.back().cells.size(),0.0);
        if(receipts.size()!=lid_states.back().cells.size())return "Mesh LID ground-contact receipt shape differs from its physical layers.";
        if(lids[k].ground_contact!=std::numeric_limits<std::size_t>::max()) {
            if(lids[k].ground_contact>=solved.contact_j.size()||lids[k].graph.cells.empty())
                return "Mesh LID ground-contact receipt does not match the solved contact graph.";
            const auto physical=lids[k].graph.cells.back();
            const double next=receipts[physical]-solved.contact_j[lids[k].ground_contact];
            if(!std::isfinite(next))return "Mesh LID ground-contact receipt overflow.";
            receipts[physical]=next;
        }
    }
    double lid_contact_j=0,vertical_j=0;
    for(std::size_t k=0;k<solved.contact_j.size();++k) {
        if(k<vertical_contact.size()&&vertical_contact[k])vertical_j+=std::abs(solved.contact_j[k]);
        else lid_contact_j+=solved.contact_j[k];
    }
    if(soil_conduction) {
        const std::size_t offset=lid_store_count;
        std::vector<double> next(solved.energy.begin()+offset,solved.energy.end());
        double surface_j=0,deep_j=0;
        for(std::size_t b=0;b<solved.boundary_j.size();++b) {
            const double receipt=solved.boundary_j[b];
            if(int(b)==snow_boundary) {
                if(auto error=heat_.applySnowContactEnergy(i,receipt);!error.empty())return error;
                surface_j+=receipt;
            } else if(b<top_count)surface_j+=receipt;else deep_j+=receipt;
        }
        ground.surface_j+=surface_j;ground.deep_j+=deep_j;
        ground.vertical_absolute_j+=vertical_j;
        ground.lid_contact_j+=lid_contact_j;
        for(std::size_t k=0;k<lids.size();++k) {
            auto& current=ctx.node_subtypes.storages.lid_state[lids[k].row];
            using std::swap;swap(current,lid_states[k]);
        }
        soil.energy=std::move(next);
    } else {
        // LID-only conduction has no external ground boundary or soil receipt.
        ground.vertical_absolute_j+=vertical_j;
        for(std::size_t k=0;k<lids.size();++k) {
            auto& current=ctx.node_subtypes.storages.lid_state[lids[k].row];
            using std::swap;swap(current,lid_states[k]);
        }
    }
    return {};
}

std::string SubsurfaceSolver::groundWaterIncrement(int i,const CellFlux& f,std::vector<double>& output) const {
    const auto& column=heat_.cells[i].column;const double area=state_.area[i];
    const double length=state_.zs[i]-state_.hg[i];
    if(!(length>0)) {
        // The collapsed-column start is a bounded geometric event, not a
        // differentiable stretching coordinate. Retain its accepted water.
        std::vector<double> moisture;
        if(auto error=projectGroundMoisture(state_,i,f.hg_final,column.depth_edges,moisture);!error.empty())return error;
        std::vector<double> next(moisture.size());
        for(std::size_t j=0;j<next.size();++j)
            next[j]=(moisture[j]-column.theta[j])*area*(column.depth_edges[j+1]-column.depth_edges[j]);
        output=std::move(next);return {};
    }
    std::vector<double> old_water,delta_water;
    if(state_.closure[i]==int8_t(GwClosure::SIGMA)) {
        if(!f.column||f.column->old_water.size()!=std::size_t(state_.m_layers)||
           f.column->new_water.size()!=f.column->old_water.size())
            return "RK2 porous heat requires the accepted hydraulic layer transfers.";
        old_water=f.column->old_water;delta_water.resize(old_water.size());
        for(std::size_t j=0;j<old_water.size();++j)delta_water[j]=f.column->new_water[j]-old_water[j];
    } else {
        old_water={f.v_uns0/area};
        delta_water={(f.infil_in-f.reject-f.et-f.recharge+f.handover)/area};
    }
    return projectGroundWaterIncrement(column,length,f.grid_volume/area,old_water,delta_water,output);
}
void SubsurfaceSolver::fireGroundHeatSpecies(int i,const CellFlux& f) noexcept {
    auto& cell=heat_.cells[i];auto& column=cell.column;
    if(column.energy.empty()||cell.failed)return;
    try {
        const bool euler=opts_&&opts_->reconstruction_order==2;
        std::vector<double> donor_temperature;
        if(euler)for(std::size_t j=0;j<column.energy.size();++j)donor_temperature.push_back(heat_.activeTemperature(i,j));
        gatherLateralHeat(i);
        const int r=tr_.temp_row;const auto k=tr_.idx(r,i);const double cp=heat_.water_capacity;
        const double top=heat_.activeTemperature(i,0);
        const double source=f.source_mass&&std::size_t(r)<f.source_mass->size()?(*f.source_mass)[r]:0;
        double edge_mass=0,edge_heat=0;
        if(f.boundaries)for(const auto& pulse:*f.boundaries) {
            edge_mass+=pulse.mass[r];edge_heat+=pulse.flux_in[r]-pulse.flux_out[r];
        }
        const double lateral=gatherLateralMass(i,r),infil=consumePending(tr_.xacc_from_surface[k]);
        const double link_in=consumePending(tr_.lacc_mass[k]),node_in=gatherNodeMass(i,r);
        const bool link_packet=consumePending(heat_link_out_volume_[i])>0;
        const double booked_link=consumePending(tr_.lout_mass[k]);
        const double implicit_link_volume=link_packet?0:std::max(0.,-f.link);
        double node_volume=0;
        for(std::size_t b=0;b<node_beds_.size();++b)if(node_beds_[b].cell==i)node_volume+=heat_node_gathered_out_[b];
        node_volume=std::max(node_volume,std::max(0.,f.node_out));
        const double refund=f.refund_mass?(*f.refund_mass)[r]:0;
        const double excess_volume=f.reject-f.paired_refund;
        double heater_j=0;
        if(std::size_t(i)<runtime_sources_.cells.size())for(auto index:runtime_sources_.cells[i]) {
            const auto& row=runtime_sources_.rows[index];if(f.v_sat0+f.source_in>0)heater_j+=row.heat*f.runtime_dt;
        }
        std::vector<double> moisture,new_sat;
        if(auto error=projectGroundMoisture(state_,i,f.hg_final,column.depth_edges,moisture,&new_sat);!error.empty())throw std::runtime_error(error);
        auto weights=cell.saturated_water;double weight=std::accumulate(weights.begin(),weights.end(),0.);
        if(!(weight>0)){weights=new_sat;weight=std::accumulate(weights.begin(),weights.end(),0.);}
        if(!(weight>0)){std::fill(weights.begin(),weights.end(),0);weights.back()=1;weight=1;}
        const auto base=std::size_t(i)*heat_.config->options.thermal_layers;
        std::vector<double> preset(weights.size()),preset_water(weights.size());double preset_total=0,preset_water_total=0;
        for(std::size_t j=0;j<weights.size();++j) {
            preset[j]=consumePending(heat_preset_out_j_[base+j]);preset_total+=preset[j];
            preset_water[j]=consumePending(heat_preset_out_water_[base+j]);preset_water_total+=preset_water[j];
        }
        const double body_water=f.lateral+f.node_in-f.node_out+f.link+f.source_in-f.source_out+f.boundary_in-f.boundary_out;
        // Already committed face/link exports keep their producer-time layer
        // receipts. New wells/node/top/deep outputs share the implicit donor
        // solution, rather than debiting every layer at a bulk mean.
        double contact_volume=0,contact_node=0,contact_link=0;
        std::vector<double> contact_water(weights.size(),0),contact_j(weights.size(),0);
        if(tr_.n_layers>0 && f.column)for(int layer=0;layer<tr_.n_layers;++layer) {
            const double volume=f.column->contacts[layer]*state_.area[i];contact_volume+=volume;
            const auto q=tr_.layerIdx(r,layer,i);
            const double mn=consumePending(tr_.contact_node_mass[q]),ml=consumePending(tr_.contact_link_mass[q]);
            contact_node+=mn;contact_link+=ml;
            const double top=f.column->old_length*layer/tr_.n_layers,bottom=f.column->old_length*(layer+1)/tr_.n_layers;
            for(std::size_t j=0;j<weights.size();++j) {
                const double overlap=std::max(0.,std::min(bottom,column.depth_edges[j+1])-std::max(top,column.depth_edges[j]));
                const double share=bottom>top?overlap/(bottom-top):0;
                contact_water[j]+=volume*share;contact_j[j]+=cp*(mn+ml)*share;
            }
        }
        const double incoming_j=cp*(source+edge_mass+edge_heat+lateral+link_in-(link_packet?booked_link:0)+node_in)+preset_total+f.lid_in_j;
        std::vector<double> water(weights.size()),energy(weights.size()),withdrawal(weights.size());
        for(std::size_t j=0;j<weights.size();++j) {
            const double share=weights[j]/weight;
            water[j]=(body_water-contact_volume+preset_water_total)*share-preset_water[j]+contact_water[j];
            energy[j]=incoming_j*share+contact_j[j];
            withdrawal[j]=(node_volume+f.source_out+f.boundary_out+implicit_link_volume)*share;
        }
        water.front()+=f.infil_in-f.reject-f.et-f.dunne;energy.front()+=cp*(infil-refund);
        withdrawal.front()+=excess_volume+f.et+f.dunne;water.back()-=f.deep;withdrawal.back()+=f.deep;
        std::vector<double> water_change;
        if(euler)if(auto error=groundWaterIncrement(i,f,water_change);!error.empty())throw std::runtime_error(error);
        const auto advected=euler?advectPorousColumnEuler(column,moisture,water_change,water,energy,withdrawal,donor_temperature):
            advectPorousColumn(column,moisture,water,energy,withdrawal);
        if(!advected.error.empty())throw std::runtime_error(advected.error);
        for(std::size_t j=0;j<weights.size();++j)cell.reserved_out_water[j]=heat_preset_out_water_[base+j];
        double donor=0;for(std::size_t j=0;j<weights.size();++j)donor+=weights[j]/weight*advected.temperature[j];
        const double link_out=link_packet?booked_link:implicit_link_volume*donor;
        const double node_out=node_volume*donor,source_out=f.source_out*donor;
        const double et=f.et*advected.temperature.front(),dunne=f.dunne*advected.temperature.front();
        const double excess=excess_volume*advected.temperature.front(),deep=f.deep*advected.temperature.back();
        const double edge_out=f.boundary_out*donor;
        tr_.boundary_in[r]+=edge_mass;tr_.boundary_out[r]+=edge_out;
        tr_.net_lateral[r]+=edge_mass+edge_heat-edge_out;
        if(f.boundaries)for(const auto& pulse:*f.boundaries) {
            auto& edge=boundaries_[pulse.boundary];
            edge.mass_in[r]+=pulse.mass[r];edge.mass_out[r]+=pulse.out*donor;
            edge.flux_in[r]+=pulse.flux_in[r];edge.flux_out[r]+=pulse.flux_out[r];
            tr_.boundary_flux_in[r]+=pulse.flux_in[r];tr_.boundary_flux_out[r]+=pulse.flux_out[r];
        }
        tr_.gained_source[r]+=source;tr_.net_lateral[r]+=lateral;tr_.gained_infil[r]+=infil;
        tr_.gained_link[r]+=link_in+contact_link;tr_.lost_link[r]+=link_out;tr_.gained_node[r]+=node_in+contact_node;
        tr_.lost_node[r]+=node_out;tr_.lost_deep[r]+=deep;tr_.lost_source[r]+=source_out;
        tr_.lost_et[r]+=et;tr_.lost_dunne[r]+=refund+excess+dunne;
        if(f.dunne_mass){(*f.dunne_mass)[r]=dunne;(*f.excess_mass)[r]=excess;}
        else tr_.xacc_to_surface[k]+=excess+dunne;
        if(node_volume>0&&!tr_.node_out_mass.empty()) {
            const auto nn=tr_.node_out_mass.size()/tr_.n_species;
            for(std::size_t b=0;b<node_beds_.size();++b)if(node_beds_[b].cell==i) {
                const int node=node_beds_[b].node;
                if(node>=0&&std::size_t(node)<nn)tr_.node_out_mass[std::size_t(r)*nn+node]+=donor*heat_node_gathered_out_[b];
            }
        }
        if(std::size_t(i)<runtime_sources_.cells.size())for(auto index:runtime_sources_.cells[i]) {
            auto& row=runtime_sources_.rows[index];const double requested=std::max(0.,-row.flow)*f.runtime_dt;
            row.receipt.requested_mass[r]-=requested*donor;
            if(f.requested_source_out>0)row.receipt.applied_mass[r]-=requested/f.requested_source_out*source_out;
        }
        cell.saturated_water=std::move(new_sat);cell.advective_j+=advected.external_j-heater_j-cp*edge_heat;cell.source_j+=heater_j+cp*edge_heat;
        if(heat_.config->options.soil_et_heat=="LATENT_SURFACE"&&f.et>0) {
            const double latent=-heat_.density*f.et*transport::heat::latentHeatOfVaporization(top);
            column.energy.front()+=latent;cell.latent_j+=latent;
        }
        if(!euler)if(auto error=conductGroundWithLids(i,f.dt);!error.empty())throw std::runtime_error(error);
        if(tr_.n_layers>0)if(auto error=refreshSigmaPorewater(i);!error.empty())throw std::runtime_error(error);
        heat_.refreshView(i,tr_);
    } catch(const std::exception& error) {
        cell.failed=1;try{cell.error=error.what();}catch(...){}
    } catch(...) {cell.failed=1;}
}

void SubsurfaceSolver::fireCellSpecies(int i, const CellFlux& f) {
    if(tr_.n_layers>0) {
        fireLayerSpecies(i,f);
        if(heat_.active()) {
            fireGroundHeatSpecies(i,f);
            if(heat_.cells[i].failed)throw std::runtime_error(heat_.cells[i].error);
        }
        if(!opts_||opts_->reconstruction_order!=2)reactCell(i,f.dt,f.hg_final);
        if(heat_.active()) {
            if(auto error=refreshSigmaPorewater(i);!error.empty())throw std::runtime_error(error);
            heat_.refreshView(i,tr_);
        }
        return;
    }
    if(heat_.active()&&heat_.cells[i].porewater.active())fireGroundPorewaterSpecies(i,f);
    if(heat_.active())fireGroundHeatSpecies(i,f);
    const auto k0 = static_cast<std::size_t>(i);
    (void)k0;
    // Arrivals land before anything is taken, so a cell that receives and
    // gives in the same firing gives at the MIXED concentration — the
    // surface marcher's rule (its face gather lands before the sinks read
    // the cell). The denominators are the post-arrival water volumes; every
    // sink in this firing reads the same pair, because the water update
    // decided all of them simultaneously.
    const bool euler=opts_&&opts_->reconstruction_order==2;
    const double v_sat = f.v_sat0 + f.lateral + std::max(f.link, 0.0) + f.source_in + f.boundary_in;
    const double v_uns = f.v_uns0 + f.infil_in - f.paired_refund;
    const auto   uc = static_cast<std::size_t>(i);
    // T7.2: the water contents retardation is computed against. The
    // saturated zone is at θ_s by definition; the column's mean content is
    // its store over its thickness, floored so a vanishing column does not
    // divide.
    const double ts_cell  = state_.theta_s[uc];
    const double L_cell   = std::max(state_.zs[uc] - state_.hg[uc], kTiny);
    const double theta_uns = std::clamp((euler?f.v_uns0/state_.area[uc]:state_.hu[uc]) / L_cell,
                                        state_.theta_r[uc], ts_cell);

    for (int s = 0; s < tr_.n_species; ++s) {
        if(heat_.active()&&(s==tr_.temp_row||heat_.cells[i].porewater.active()))continue;
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
        const double old_sat=msat,old_uns=muns;
        const double partition=(s==tr_.age_row||s==tr_.temp_row)?0:tr_.kd[k]*tr_.rho_b[i];
        const double handover=euler?f.handover+partition*f.grid_volume:f.handover;
        const double sat_available=std::max(0.,old_sat-pendingSaturatedMassExport(i,s));
        // Simultaneous old-state Euler exports share each beginning donor.
        const double sat_request=old_sat*(
            (std::max(0.,-f.recharge)+f.deep+f.source_out+f.boundary_out+f.dunne)*mob_sat+std::max(0.,handover)*mob_sat)/
            std::max(f.v_sat0,kTiny);
        const double uns_request=old_uns*(
            (std::max(0.,f.recharge)+f.reject)*mob_uns+std::max(0.,-handover)*mob_uns+
            ((s==tr_.age_row||s==tr_.temp_row)?f.et:0))/std::max(f.v_uns0,kTiny);
        const double sat_scale=sat_request>sat_available&&sat_request>0?sat_available/sat_request:1;
        const double uns_scale=uns_request>old_uns&&uns_request>0?old_uns/uns_request:1;
        double sat_remaining=sat_available,uns_remaining=old_uns;
        auto take = [&](double& m, double vol, double v_store,
                        double mob) -> double {
            if (!(vol > 0.0) || !(v_store > 0.0) || m == 0.0) return 0.0;
            double dm = vol * (m / v_store) * mob;
            if(euler) {
                const bool saturated=&m==&msat;const double volume=saturated?f.v_sat0:f.v_uns0;
                auto& remaining=saturated?sat_remaining:uns_remaining;
                dm=volume>0?vol*(saturated?old_sat:old_uns)/volume*mob*
                    (tr_.signedRow(s)?1:(saturated?sat_scale:uns_scale)):0;
                // A signed temperature-volume has no nonnegative mass
                // budget. Water already bounds the physical donor; min on
                // two negatives would empty the whole cold store.
                if(!tr_.signedRow(s)){dm=std::min(dm,remaining);remaining-=dm;}
            }
            if (!tr_.signedRow(s) && dm > m) dm = m;
            m -= dm;
            return dm;
        };

        if (f.source_mass && us < f.source_mass->size()) {
            const double input = (*f.source_mass)[us];
            msat += input;
            tr_.gained_source[us] += input;
        }
        if(f.boundaries)for(const auto& pulse:*f.boundaries) {
            const double mass=pulse.mass[us]+pulse.flux_in[us];
            msat+=mass;tr_.net_lateral[us]+=mass;
            tr_.boundary_in[us]+=pulse.mass[us];tr_.boundary_flux_in[us]+=pulse.flux_in[us];
            auto& boundary=boundaries_[pulse.boundary];
            boundary.mass_in[us]+=pulse.mass[us];boundary.flux_in[us]+=pulse.flux_in[us];
        }
        // (1) lateral Darcy — internal between cells, so the sum over the
        //     mesh is zero on a closed domain and the ledger says exactly
        //     that (the water's `led_lateral` behaves identically).
        const double lat_m = gatherLateralMass(i, s);
        msat += lat_m;
        tr_.net_lateral[us] += lat_m;

        // (2) infiltration the surface handed down, with the mass the water
        //     was carrying when it left the surface cell.
        const double in_m = consumePending(tr_.xacc_from_surface[k]);
        if (in_m != 0.0) {
            const double accepted=f.refund_mass?in_m-(*f.refund_mass)[us]:in_m;
            muns+=(f.refund_mass&&!tr_.signedRow(s))?std::max(0.,accepted):accepted;
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
        const double link_in_m = consumePending(tr_.lacc_mass[k]);
        if (link_in_m != 0.0) {
            msat += link_in_m;
            tr_.gained_link[us] += link_in_m;
        }
        // Review (2026-10-15): a GAINING conduit's mass now reaches the 1D.
        // The router booked what it delivered (at this cell's dissolved
        // saturated concentration when the water left); the cell debits
        // exactly that, so the aquifer gives what the network received.
        const double out_m = tr_.lout_mass.empty() ? 0.0
                                                   : consumePending(tr_.lout_mass[k]);
        if (out_m != 0.0) {
            double dm = out_m;
            if (!tr_.signedRow(s) && dm > msat) {
                tr_.link_gain_shortfall[us] += dm - std::max(msat, 0.0);
                dm = std::max(msat, 0.0);
            }
            msat -= dm;
            tr_.lost_link[us] += dm;
        } else if (f.link < 0.0) {
            tr_.lost_link[us] += take(msat, -f.link, v_sat, mob_sat);
        }

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
        if (handover > 0.0) {
            // The moving boundary carries the water's whole content across,
            // sorbed included: the grains themselves change zone when the
            // table passes them. `mob = 1`.
            const double dm = take(msat, handover, v_sat, euler?mob_sat:1.0);
            muns += dm;
            tr_.internal_recharge[us] -= dm;
        } else if (handover < 0.0) {
            const double dm = take(muns, -handover, v_uns, euler?mob_uns:1.0);
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
                        drew += std::max(bed_gathered_[b], 0.0);
                if (drew > 0.0) {
                    for (std::size_t b = 0; b < node_beds_.size(); ++b) {
                        if (node_beds_[b].cell != i) continue;
                        const double share = std::max(bed_gathered_[b], 0.0) / drew;
                        const auto ni = static_cast<std::size_t>(node_beds_[b].node);
                        if (ni < nn)
                            tr_.node_out_mass[static_cast<std::size_t>(s) * nn + ni] +=
                                dm * share;
                    }
                }
            }
        }
        tr_.lost_deep[us] += take(msat, f.deep, v_sat, mob_sat);
        const double source_conc = v_sat>0.0 ? msat*mob_sat/v_sat : 0.0;
        const double source_export = take(msat, f.source_out, v_sat, mob_sat);
        tr_.lost_source[us] += source_export;
        if(static_cast<std::size_t>(i)<runtime_sources_.cells.size())
            for(auto k:runtime_sources_.cells[i]) {
                auto& row=runtime_sources_.rows[k];
                const double requested=std::max(0.0,-row.flow)*f.runtime_dt;
                row.receipt.requested_mass[us] -= requested*source_conc;
                if(f.requested_source_out>0.0)
                    row.receipt.applied_mass[us] -= source_export*requested/f.requested_source_out;
            }

        // Independent species sinks act on the dissolved fraction of the
        // saturated store, sharing its remaining mobile mass proportionally.
        if(static_cast<std::size_t>(i)<runtime_sources_.cells.size()) {
            double requested=0.0;
            for(auto k:runtime_sources_.cells[i])
                requested+=std::max(0.0,-runtime_sources_.rows[k].rates[us])*f.runtime_dt;
            const double scale=requested>0.0
                ? std::min(1.0,std::max(0.0,msat*mob_sat)/requested) : 1.0;
            for(auto k:runtime_sources_.cells[i]) {
                auto& row=runtime_sources_.rows[k];
                const double wanted=std::max(0.0,-row.rates[us])*f.runtime_dt;
                const double applied=wanted*scale;
                msat-=applied; tr_.lost_source[us]+=applied;
                row.receipt.requested_mass[us]-=wanted;
                row.receipt.applied_mass[us]-=applied;
            }
        }

        if(f.boundaries) {
            const double export_mass=take(msat,f.boundary_out,v_sat,mob_sat);
            double wanted_flux=0;
            for(const auto& pulse:*f.boundaries)wanted_flux+=pulse.flux_out[us];
            const double flux_scale=tr_.signedRow(s)||wanted_flux<=0 ? 1. :
                std::min(1.,std::max(0.,msat*mob_sat)/wanted_flux);
            const double flux_export=wanted_flux*flux_scale;
            msat-=flux_export;
            tr_.net_lateral[us]-=export_mass+flux_export;
            tr_.boundary_out[us]+=export_mass;tr_.boundary_flux_out[us]+=flux_export;
            for(const auto& pulse:*f.boundaries) {
                auto& boundary=boundaries_[pulse.boundary];
                if(f.boundary_out>0)boundary.mass_out[us]+=export_mass*pulse.out/f.boundary_out;
                boundary.flux_out[us]+=pulse.flux_out[us]*flux_scale;
            }
        }

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
        //     unsaturated one. Reviewed refunds bypass mixing and remain in
        //     paired source receipts; ordinary cells use takeToSurfaceMass.
        double up = 0.0;
        if(f.refund_mass) {
            (*f.dunne_mass)[us]=take(msat,f.dunne,v_sat,mob_sat);
            (*f.excess_mass)[us]=take(muns,f.reject-f.paired_refund,v_uns,mob_uns);
            up=(*f.refund_mass)[us]+(*f.dunne_mass)[us]+(*f.excess_mass)[us];
        } else {
            if (f.dunne  > 0.0) up += take(msat, f.dunne,  v_sat, mob_sat);
            if (f.reject > 0.0) up += take(muns, f.reject, v_uns, mob_uns);
            if(up!=0.0)tr_.xacc_to_surface[k] += up;
        }
        tr_.lost_dunne[us] += up;

        if(euler&&s==tr_.age_row) {
            const double production=f.dt*(f.v_sat0+f.v_uns0);
            msat+=f.dt*f.v_sat0;muns+=f.dt*f.v_uns0;tr_.generated_age[us]+=production;
        }
        // (8) T7.2: first-order decay, on the TOTAL mass of both zones —
        //     dissolved and sorbed alike, the usual assumption for a
        //     partitioning solute whose reaction does not care which phase
        //     it is in. Applied last, on what the channels left behind, and
        //     never to age or temperature: `__WATER_AGE__` grows with time
        //     by construction and a temperature does not decay.
        const double kdec = tr_.decay.empty() ? 0.0 : tr_.decay[k];
        if (!euler && kdec > 0.0 && f.dt > 0.0 && s != tr_.age_row && s != tr_.temp_row) {
            const double keep = std::exp(-kdec * f.dt);
            const double gone = (msat + muns) * (1.0 - keep);
            msat *= keep;
            muns *= keep;
            tr_.lost_reaction[us] += gone;
        }
    }
    if(!opts_||opts_->reconstruction_order!=2)reactCell(i,f.dt,f.hg_final);
}

void SubsurfaceSolver::setBoundaries(std::vector<GwResolvedBoundary> rows) {
    boundaries_=std::move(rows);cell_boundaries_.assign(state_.n_cells,{});
    for(std::size_t i=0;i<boundaries_.size();++i)
        cell_boundaries_[boundaries_[i].cell].push_back(i);
    if(mesh_&&edges_)refreshDtCell(*mesh_,*edges_);
}

void SubsurfaceSolver::setSources(std::vector<GwResolvedSource> sources) {
    runtime_sources_ = {};
    sources_ = std::move(sources);
    cell_sources_.assign(static_cast<std::size_t>(state_.n_cells), {});
    for (std::size_t row=0; row<sources_.size(); ++row)
        for (const auto& cell : sources_[row].cells)
            if (cell.first>=0 && cell.first<state_.n_cells)
                cell_sources_[static_cast<std::size_t>(cell.first)].push_back({row,cell.second});
}
void SubsurfaceSolver::fireGwRk2Cells(double dt,SurfaceStateData& surface,double time) {
    source_firing_end_=time+dt;
    if(!active()||!tierHasCells(0))return;
    const auto& cells=cells_by_tier_[0];prepareRechargeBatch(cells);
    for(int cell:cells) {
        fireCell(cell,dt,surface,time);
        if(heat_.active()&&heat_.cells[cell].failed)
            throw std::runtime_error(heat_.cells[cell].error.empty()?"Coupled RK2 thermal stage failed.":heat_.cells[cell].error);
        if(heat_.active()) {
            if(tr_.n_layers)if(auto error=refreshSigmaPorewater(cell);!error.empty())throw std::runtime_error(error);
            heat_.refreshView(cell,tr_);
        }
    }
    if(!tier_firings_.empty())++tier_firings_[0];
}
void SubsurfaceSolver::advanceRk2Heat(double seconds) {
    if(!heat_.active()||!(seconds>0))return;
    if(!std::isfinite(seconds))throw std::runtime_error("Nonfinite coupled thermal interval.");
    heat_.prepare();
    for(int i=0;i<state_.n_cells;++i)if(covers(i)) {
        if(auto error=heat_.conduct(i,seconds,true);!error.empty())throw std::runtime_error(error);
        heat_.refreshView(i,tr_);
    }
}
void SubsurfaceSolver::fireGwCells(int tier, double dt, SurfaceStateData& surf, double time,
                                   double accepted_end) {
    if (state_.active && tierHasCells(tier))
        for (const int i : cells_by_tier_[static_cast<std::size_t>(tier)])
            for (const auto b : cell_beds_[i]) if (richards_bed_owned_[b] && !lid_receipts_enabled_)
                throw std::runtime_error("Richards-owned groundwater cells require joint owner firing acceptance.");
    source_firing_end_=accepted_end>=0 ? accepted_end : time+dt;
    if (!state_.active || !tierHasCells(tier)) return;
    fireGwCellList(cells_by_tier_[static_cast<std::size_t>(tier)],dt,surf,time,accepted_end);
    if (tier < static_cast<int>(tier_firings_.size()))
        ++tier_firings_[static_cast<std::size_t>(tier)];
}
void SubsurfaceSolver::fireGwCellList(const std::vector<int>& cells, double dt, SurfaceStateData& surf,
                                    double time, double accepted_end) {
    prepareRechargeBatch(cells);
    for (const int i : cells) {
        const auto u = static_cast<std::size_t>(i);
        const bool exact=accepted_end>=0 && (source_owned_[u] || lid_receipts_enabled_);
        const double cell_time=exact ? (source_owned_[u]?source_cell_end_[u]:cell_completed_[u]) : time;
        const double cell_dt=exact ? accepted_end-cell_time : dt;
        if ((u >= cell_sources_.size() || cell_sources_[u].empty()) &&
            (u>=cell_boundaries_.size()||cell_boundaries_[u].empty())) {
            fireCell(i, cell_dt, surf, cell_time);
            if(exact)source_cell_end_[u]=accepted_end;
            continue;
        }
        const double end = exact ? accepted_end : time + dt;
        const double start = cell_time;
        std::vector<double> knots{start, end};
        if (u < cell_sources_.size()) for (const auto& entry : cell_sources_[u]) {
            const auto& source = sources_[entry.first];
            auto add = [&](const GwSourceSignal& signal) {
                const auto& times=signal.timePoints();
                for(auto it=std::upper_bound(times.begin(),times.end(),start);it!=times.end()&&*it<end;++it)
                    knots.push_back(*it);
            };
            add(source.flow);
            for (const auto& term : source.terms) add(term.signal);
            const auto& times=source.flow.timePoints();
            const auto& values=source.flow.ordinates();
            const auto first=std::upper_bound(times.begin(),times.end(),start);
            for (std::size_t j=std::max<std::size_t>(1,first-times.begin());j<times.size()&&times[j-1]<end;++j) {
                const double a = values[j-1], b = values[j];
                if ((a < 0 && b > 0) || (a > 0 && b < 0)) {
                    const double t = times[j-1] +
                        (times[j] - times[j-1]) * (-a) / (b-a);
                    if (t > start && t < end) knots.push_back(t);
                }
            }
        }
        if(u<cell_boundaries_.size())for(auto index:cell_boundaries_[u])
            appendGwBoundaryCuts(boundaries_[index],start,end,state_.z_bed[u]+state_.hg[u],
                state_.z_bed[u],state_.zs[u],knots);
        std::sort(knots.begin(), knots.end());
        knots.erase(std::unique(knots.begin(), knots.end()), knots.end());
        for (std::size_t j = 1; j < knots.size(); ++j) {
            // Existing cross-cadence accumulators represent this full firing.
            // Apportion both water and species by elapsed duration, retaining
            // the unused portion until its chronological subinterval.
            gather_fraction_ = j + 1 == knots.size() ? 1.0 :
                (knots[j] - knots[j-1]) / (end - knots[j-1]);
            fireCell(i, knots[j] - knots[j-1], surf, knots[j-1]);
            if(exact)source_cell_end_[u]=knots[j];
        }
        gather_fraction_ = 1.0;
    }
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


void SubsurfaceSolver::prepareRechargeBatch(const std::vector<int>& cells) {
    if(!recharge_backend_.evaluate)return;
    recharge_inputs_.clear();
    for(int cell:cells) {
        recharge_slots_[cell]=-1;
        if(state_.closure[cell]!=int8_t(GwClosure::CLOSED_FORM))continue;
        const double length=state_.zs[cell]-state_.hg[cell];if(!(length>0))continue;
        const auto p=paramsOf(cell);OpenSwmmGwRechargeInputV1 v{};v.struct_size=sizeof(v);v.law=int(p.law);
        v.Ks=p.Ks;v.theta_s=p.theta_s;v.theta_r=p.theta_r;v.alpha=p.alpha;v.psi_b=p.psi_b;
        v.lambda=p.lambda;v.vg_n=p.vg_n;v.vg_L=p.vg_L;v.length=std::max(length,kTiny);v.hu=state_.hu[cell];
        recharge_slots_[cell]=int(recharge_inputs_.size());recharge_inputs_.push_back(v);
    }
    if(recharge_inputs_.empty())return;
    recharge_values_.resize(recharge_inputs_.size());
    const int rc=recharge_backend_.evaluate(recharge_inputs_.data(),int(recharge_inputs_.size()),recharge_values_.data(),opts_?opts_->num_threads:1);
    if(rc!=0||std::any_of(recharge_values_.begin(),recharge_values_.end(),[](double x){return !std::isfinite(x);})) {
        std::fill(recharge_slots_.begin(),recharge_slots_.end(),-1);recharge_backend_.evaluate=nullptr;
        recharge_backend_.diagnostic="Recharge extension failed validation; outputs discarded and CPU computation retained";
        return;
    }
    ++recharge_batches_;recharge_cells_+=recharge_inputs_.size();
}
double SubsurfaceSolver::rawRecharge(int cell,const soil::Params& p,double length) const noexcept {
    const int slot=recharge_backend_.evaluate&&std::size_t(cell)<recharge_slots_.size()?recharge_slots_[cell]:-1;
    // Source/boundary knots can advance a cell more than once in a batch.
    // Only the exact immutable input may consume the cached pure result.
    if(slot>=0&&std::size_t(slot)<recharge_values_.size()&&std::size_t(slot)<recharge_inputs_.size()) {
        const auto& v=recharge_inputs_[slot];
        if(v.length==length&&v.hu==state_.hu[cell]&&v.law==int(p.law)&&v.Ks==p.Ks&&
           v.theta_s==p.theta_s&&v.theta_r==p.theta_r&&v.alpha==p.alpha&&v.psi_b==p.psi_b&&
           v.lambda==p.lambda&&v.vg_n==p.vg_n&&v.vg_L==p.vg_L)return recharge_values_[slot];
    }
    return soil::rechargeQ0(p,length,state_.hu[cell]);
}

std::string SubsurfaceSolver::rk2ProfileError() const {
    if(!active() || options_.per_subcatch ||
       !node_beds_.empty() || lid_receipts_enabled_ || runtime_sources_.active() ||
       std::any_of(source_owned_.begin(),source_owned_.end(),[](bool v){return v;}) ||
       std::any_of(state_.closure.begin(),state_.closure.end(),[](int8_t v){return v!=int8_t(GwClosure::CLOSED_FORM)&&v!=int8_t(GwClosure::ENSLAVED)&&v!=int8_t(GwClosure::SIGMA);}))
        return "Coupled RK2 requires spatial CLOSED_FORM/ENSLAVED/SIGMA cells without borrowed source, network or thermal owners.";
    if(heat_.active()) {
        if(!heat_.config||tr_.n_species<=0||tr_.temp_row<0||tr_.temp_row>=tr_.n_species||
           heat_.cells.size()!=std::size_t(state_.n_cells)||heat_.snow_owner||
           heat_.top_series>=0||heat_.deep_series>=0||
           heat_.config->options.surface_thermal_bc=="AIR"||heat_.config->options.surface_thermal_bc=="SURFACE_WATER"||
           heat_.config->options.soil_et_heat!="SENSIBLE_ONLY"||
           (opts_&&opts_->water_heat_capacity!=heat_.water_capacity))
            return "Coupled RK2 porous heat requires a temperature row, constant boundaries and no borrowed heat owner.";
        for(int i=0;i<state_.n_cells;++i)if(covers(i)) {
            const auto& cell=heat_.cells[i];const auto& column=cell.column;const auto n=column.energy.size();
            if(cell.surface_owned||cell.failed||!n||column.theta.size()!=n||column.capacity.size()!=n||
               column.depth_edges.size()!=n+1||cell.saturated_water.size()!=n||cell.reserved_out_water.size()!=n)
                return "Coupled RK2 porous thermal owner shape or status differs.";
            const auto& pore=cell.porewater;
            if((tr_.n_layers||tr_.n_species>1)&&!pore.active())return "Coupled RK2 porous transport has no canonical porewater owner.";
            if(pore.active()) {
                if(pore.n_species!=tr_.n_species||pore.temp_row!=tr_.temp_row||pore.age_row!=tr_.age_row||
                   pore.mass.size()!=n*tr_.n_species||pore.retention.size()!=pore.mass.size()||
                   pore.reserved_out.size()!=pore.mass.size()||pore.contact_mass.size()!=pore.mass.size()||pore.generated_age.size()!=n)
                    return "Coupled RK2 porous species or ledger shape differs.";
                for(std::size_t k=0;k<pore.mass.size();++k)
                    if(!std::isfinite(pore.mass[k])||pore.mass[k]<0||!std::isfinite(pore.retention[k])||pore.retention[k]<0||
                       !std::isfinite(pore.reserved_out[k])||pore.reserved_out[k]<0||pore.reserved_out[k]>pore.mass[k]||
                       !std::isfinite(pore.contact_mass[k])||(int(k/n)==tr_.temp_row&&(pore.mass[k]!=0||pore.reserved_out[k]!=0)))
                        return "Coupled RK2 porous canonical mass or reservation is inadmissible.";
                for(double v:pore.generated_age)if(!std::isfinite(v)||v<0)return "Coupled RK2 porous age generation is inadmissible.";
                for(double v:pore.sigma.pending)if(v!=0)return "Coupled RK2 has an unstaged porous contact receipt.";
            }
            for(std::size_t j=0;j<n;++j)
                if(!std::isfinite(column.energy[j])||!std::isfinite(heat_.activeCapacity(i,j))||!(heat_.activeCapacity(i,j)>0))
                    return "Coupled RK2 porous energy or active capacity is inadmissible.";
        }
    }
    for(int i=0;i<state_.n_cells;++i)if(covers(i)&&state_.closure[i]==int8_t(GwClosure::ENSLAVED)) {
        const double equilibrium=soil::equilibriumStorage(paramsOf(i),state_.zs[i]-state_.hg[i]);
        if(!std::isfinite(state_.hu[i])||!std::isfinite(equilibrium)||
           std::abs(state_.hu[i]-equilibrium)>1e-12*std::max(1.,std::abs(equilibrium)))
            return "Coupled RK2 ENSLAVED cells require their actual equilibrium storage.";
    }
    for(int i=0;i<state_.n_cells;++i)if(covers(i)&&state_.closure[i]==int8_t(GwClosure::SIGMA)) {
        if(state_.m_layers<=0||state_.theta_sigma.size()!=std::size_t(state_.m_layers)*state_.n_cells)
            return "Coupled RK2 SIGMA layer shape differs.";
        for(int j=0;j<state_.m_layers;++j) {
            const double theta=state_.theta_sigma[std::size_t(j)*state_.n_cells+i];
            if(!std::isfinite(theta)||theta<state_.theta_r[i]||theta>state_.theta_s[i])
                return "Coupled RK2 SIGMA water content is inadmissible.";
        }
        const double water=sigma::columnStorage(&state_.theta_sigma[i],state_.m_layers,state_.n_cells,state_.zs[i]-state_.hg[i]);
        if(!std::isfinite(state_.hu[i])||std::abs(state_.hu[i]-water)>1e-12*std::max(1.,std::abs(water)))
            return "Coupled RK2 SIGMA bulk water differs from its layer inventories.";
    }
    if(tr_.active()) {
        if(tr_.sat_mass.size()!=std::size_t(tr_.n_species)*state_.n_cells||tr_.unsat_mass.size()!=tr_.sat_mass.size()||
           tr_.layer_mass.size()!=std::size_t(tr_.n_layers)*tr_.sat_mass.size())
            return "Coupled RK2 groundwater transport shape differs.";
        for(const auto* mass:{&tr_.sat_mass,&tr_.unsat_mass,&tr_.layer_mass}) {
            const auto stride=std::size_t(tr_.n_cells)*(mass==&tr_.layer_mass?tr_.n_layers:1);
            for(std::size_t k=0;k<mass->size();++k)
                if(!std::isfinite((*mass)[k])||(int(k/stride)!=tr_.temp_row&&(*mass)[k]<0))
                    return "Coupled RK2 groundwater transport inventory is inadmissible.";
        }
        if(std::any_of(tr_.decay.begin(),tr_.decay.end(),[](double v){return !std::isfinite(v)||v<0;}))
            return "Coupled RK2 groundwater decay is inadmissible.";
    }
    for(const auto& source:sources_)for(const auto& term:source.terms)
        if(term.row<0||term.row>=tr_.n_species)return "Coupled RK2 well species layout differs.";
    for(const auto& boundary:boundaries_) {
        for(const auto& term:boundary.terms)if(term.row<0||term.row>=tr_.n_species)
            return "Coupled RK2 boundary species layout differs.";
        for(const auto* values:{&boundary.mass_in,&boundary.mass_out,&boundary.flux_in,&boundary.flux_out})
            if(values->size()!=std::size_t(tr_.n_species))return "Coupled RK2 boundary receipt shape differs.";
    }
    for(const auto* values:{&state_.nacc,&state_.lacc})for(double v:*values)if(v!=0)
        return "Coupled RK2 cannot consume an unstaged external node/link receipt.";
    return {};
}

void SubsurfaceSolver::publishRk2Infiltration(SurfaceStateData& surface,double dt,double time) {
    for(int i=0;i<state_.n_cells;++i)if(covers(i)) {
        const auto p=paramsOf(i);const double length=state_.zs[i]-state_.hg[i];
        const double pond=std::max(0.,surface.depth[i]);double rate=0;
        if(length>0&&state_.closure[i]==int8_t(GwClosure::SIGMA)) {
            rate=sourceInfiltrationCapacity(i,pond,dt);
        } else if(length>0) {
            const double theta=std::clamp(state_.hu[i]/length,p.theta_r,p.theta_s);
            const double saturation=std::clamp((theta-p.theta_r)/(p.theta_s-p.theta_r),1e-6,1.);
            const double suction=state_.closure[i]==int8_t(GwClosure::ENSLAVED)?length:soil::suctionAtSaturation(p,saturation);
            const double drive=(pond+suction)*(p.theta_s-theta);
            // The held-interval Green-Ampt integral is an exact scalar
            // subflow, not a forward-Euler derivative. Smooth SSP stages use
            // its instantaneous rate. At an unadvanced front the derivative
            // is singular; retain the finite integral as a bounded startup,
            // without claiming second order across that singular event.
            const double front=state_.wetting_front[i];
            rate=front>0?p.Ks*(1+drive/front):frontIntake(0,drive,p.Ks,dt)/dt;
        }
        const double room=infiltrationHeadroom(i);
        rate=std::min(rate,room/(state_.area[i]*dt));
        if(!std::isfinite(rate)||rate<0)throw std::runtime_error("Coupled RK2 infiltration rate is inadmissible.");
        state_.infil_capacity[i]=rate;state_.infil_remaining[i]=room;
        state_.infil_refresh[i]=time;state_.infil_interval[i]=dt;surface.infil_rate[i]=rate;
    }
}

void SubsurfaceSolver::blendRk2Water(const SubsurfaceSolver& initial,double end) {
    if(auto error=rk2ProfileError();!error.empty())throw std::runtime_error(error);
    auto& s=state_;const auto& b=initial.state_;
    const auto blend=[](auto& now,const auto& before) {
        if(now.size()!=before.size())throw std::runtime_error("Coupled RK2 groundwater shape changed in a stage.");
        for(std::size_t i=0;i<now.size();++i)now[i]=.5*(before[i]+now[i]);
    };
    if(s.hg.size()!=b.hg.size()||s.hu.size()!=b.hu.size())
        throw std::runtime_error("Coupled RK2 groundwater shape changed in a stage.");
    for(int i=0;i<s.n_cells;++i) {
        if(covers(i)&&s.closure[i]==int8_t(GwClosure::ENSLAVED)) {
            const auto p=paramsOf(i);const double zs=s.zs[i],ts=s.theta_s[i];
            const double target=.5*((ts*b.hg[i]+b.hu[i])+(ts*s.hg[i]+s.hu[i]));
            const auto water=[&](double h){return ts*h+soil::equilibriumStorage(p,zs-h);};
            double lo=0,hi=zs;
            if(!std::isfinite(target))throw std::runtime_error("Nonfinite coupled RK2 equilibrium storage.");
            if(target<=water(lo))hi=lo;
            else if(target>=water(hi))lo=hi;
            else for(int iteration=0;iteration<80;++iteration) {
                const double middle=lo+.5*(hi-lo);
                if(middle==lo||middle==hi)break;
                if(water(middle)>target)hi=middle;else lo=middle;
            }
            // Invert the conserved water, not an arithmetic table-height or
            // moisture average. A flat capillary fringe maps to saturation.
            const double h=std::abs(water(lo)-target)<=std::abs(water(hi)-target)?lo:hi;
            if(std::abs(water(h)-target)>32*std::numeric_limits<double>::epsilon()*std::max(1.,std::abs(target)))
                throw std::runtime_error("Coupled RK2 equilibrium storage inversion failed.");
            s.hg[i]=h;s.hu[i]=soil::equilibriumStorage(p,zs-h);
        } else if(covers(i)&&s.closure[i]==int8_t(GwClosure::SIGMA)) {
            const double old_length=b.zs[i]-b.hg[i],new_length=s.zs[i]-s.hg[i];
            const double height=.5*(b.hg[i]+s.hg[i]),dz=(s.zs[i]-height)/s.m_layers;
            double water=0;
            for(int j=0;j<s.m_layers;++j) {
                const auto k=std::size_t(j)*s.n_cells+i;
                // This is the same conserved-water blend with its common
                // factors cancelled. Avoid dividing by a near-zero length
                // obtained by subtracting the averaged table from zs.
                const double amount=b.theta_sigma[k]*old_length+s.theta_sigma[k]*new_length;
                const double length=old_length+new_length;
                s.theta_sigma[k]=rk2SigmaView(length>0?amount/length:s.theta_s[i],s.theta_r[i],s.theta_s[i]);
                water+=s.theta_sigma[k]*dz;
            }
            s.hg[i]=height;s.hu[i]=water;
        } else {
            // Both CLOSED_FORM stores are linear canonical inventories.
            s.hg[i]=.5*(b.hg[i]+s.hg[i]);s.hu[i]=.5*(b.hu[i]+s.hu[i]);
        }
    }
    blend(s.wetting_front,b.wetting_front);
    blend(s.eacc_L,b.eacc_L);blend(s.eacc_R,b.eacc_R);
    blend(s.xacc_from_surface,b.xacc_from_surface);blend(s.xacc_to_surface,b.xacc_to_surface);
    blend(s.et_pending,b.et_pending);blend(s.reject_cumulative,b.reject_cumulative);
    blend(s.dunne_cumulative,b.dunne_cumulative);blend(s.et_potential_cumulative,b.et_potential_cumulative);
    blend(s.et_surface_cumulative,b.et_surface_cumulative);blend(s.et_soil_cumulative,b.et_soil_cumulative);
    blend(s.et_unused_cumulative,b.et_unused_cumulative);
    for(auto member:{&SubsurfaceState::led_recharge,&SubsurfaceState::led_lateral,&SubsurfaceState::led_deep,
        &SubsurfaceState::led_node,&SubsurfaceState::led_reject,&SubsurfaceState::led_dunne,
        &SubsurfaceState::led_caprise,&SubsurfaceState::led_et,&SubsurfaceState::led_infil_in,
        &SubsurfaceState::led_link,&SubsurfaceState::led_subcatch_in,&SubsurfaceState::led_final_storage,
        &SubsurfaceState::led_source_in,&SubsurfaceState::led_source_out,
        &SubsurfaceState::led_boundary_in,&SubsurfaceState::led_boundary_out}) {
        s.*member=.5*(b.*member+s.*member);
        if(!std::isfinite(s.*member))throw std::runtime_error("Coupled RK2 groundwater ledger is nonfinite.");
    }
    if(boundaries_.size()!=initial.boundaries_.size())throw std::runtime_error("Coupled RK2 boundary shape changed in a stage.");
    for(std::size_t i=0;i<boundaries_.size();++i)for(auto member:{&GwResolvedBoundary::requested_in,&GwResolvedBoundary::requested_out,
        &GwResolvedBoundary::applied_in,&GwResolvedBoundary::applied_out}) {
        auto& value=boundaries_[i].*member;value=.5*(initial.boundaries_[i].*member+value);
        if(!std::isfinite(value)||value<0)throw std::runtime_error("Coupled RK2 boundary receipt is inadmissible.");
    }
    auto& t=tr_;const auto& old=initial.tr_;
    for(auto member:{&SubsurfaceTransportState::sat_mass,&SubsurfaceTransportState::unsat_mass,
        &SubsurfaceTransportState::layer_mass,&SubsurfaceTransportState::sacc_L,&SubsurfaceTransportState::sacc_R,
        &SubsurfaceTransportState::xacc_from_surface,&SubsurfaceTransportState::xacc_to_surface,
        &SubsurfaceTransportState::nacc_mass,&SubsurfaceTransportState::lacc_mass,&SubsurfaceTransportState::lout_mass,
        &SubsurfaceTransportState::node_out_mass,&SubsurfaceTransportState::lost_deep,&SubsurfaceTransportState::lost_node,
        &SubsurfaceTransportState::lost_link,&SubsurfaceTransportState::lost_dunne,&SubsurfaceTransportState::lost_et,
        &SubsurfaceTransportState::gained_infil,&SubsurfaceTransportState::gained_node,&SubsurfaceTransportState::gained_link,
        &SubsurfaceTransportState::net_lateral,&SubsurfaceTransportState::gained_source,&SubsurfaceTransportState::lost_source,
        &SubsurfaceTransportState::boundary_in,&SubsurfaceTransportState::boundary_out,
        &SubsurfaceTransportState::boundary_flux_in,&SubsurfaceTransportState::boundary_flux_out,
        &SubsurfaceTransportState::lost_reaction,&SubsurfaceTransportState::generated_age,
        &SubsurfaceTransportState::gained_contact,&SubsurfaceTransportState::internal_recharge})blend(t.*member,old.*member);
    for(int species=0;species<t.n_species;++species)for(int i=0;i<t.n_cells;++i)
        if(t.n_layers)t.refreshUnsat(species,i);
    for(std::size_t i=0;i<boundaries_.size();++i)for(auto member:{&GwResolvedBoundary::mass_in,&GwResolvedBoundary::mass_out,
        &GwResolvedBoundary::flux_in,&GwResolvedBoundary::flux_out})blend(boundaries_[i].*member,initial.boundaries_[i].*member);
    if(heat_.active()) {
        const auto scalar=[](double& now,double before) {
            now=.5*before+.5*now;
            if(!std::isfinite(now))throw std::runtime_error("Coupled RK2 thermal inventory or receipt is nonfinite.");
        };
        const auto values=[&](auto& now,const auto& before) {
            if(now.size()!=before.size())throw std::runtime_error("Coupled RK2 thermal shape changed in a stage.");
            for(std::size_t k=0;k<now.size();++k)scalar(now[k],before[k]);
        };
        for(auto member:{&SubsurfaceSolver::heat_link_out_volume_,&SubsurfaceSolver::heat_preset_out_j_,
            &SubsurfaceSolver::heat_preset_out_water_,&SubsurfaceSolver::heat_node_out_volume_,
            &SubsurfaceSolver::heat_node_gathered_out_,&SubsurfaceSolver::porewater_preset_out_,
            &SubsurfaceSolver::heat_pending_left_,&SubsurfaceSolver::heat_pending_right_})values(this->*member,initial.*member);
        for(int i=0;i<s.n_cells;++i)if(covers(i)) {
            auto& cell=heat_.cells[i];const auto& before=initial.heat_.cells[i];
            values(cell.column.energy,before.column.energy);values(cell.reserved_out_water,before.reserved_out_water);
            auto& pore=cell.porewater;
            if(pore.active()) {
                if(!t.n_layers){values(pore.mass,before.porewater.mass);values(pore.reserved_out,before.porewater.reserved_out);}
                values(pore.contact_mass,before.porewater.contact_mass);values(pore.generated_age,before.porewater.generated_age);
            }
            for(auto member:{&GroundThermalCell::advective_j,&GroundThermalCell::source_j,&GroundThermalCell::surface_j,
                &GroundThermalCell::deep_j,&GroundThermalCell::latent_j,&GroundThermalCell::lateral_j,
                &GroundThermalCell::water_contact_j,&GroundThermalCell::channel_contact_j,&GroundThermalCell::lid_contact_j,
                &GroundThermalCell::vertical_absolute_j})scalar(cell.*member,before.*member);
            std::vector<double> moisture,saturated;
            if(auto error=projectGroundMoisture(s,i,s.hg[i],cell.column.depth_edges,moisture,&saturated);!error.empty())throw std::runtime_error(error);
            if(auto error=cell.column.setMoisture(moisture);!error.empty())throw std::runtime_error(error);
            cell.saturated_water=std::move(saturated);
            if(t.n_layers)if(auto error=refreshSigmaPorewater(i);!error.empty())throw std::runtime_error(error);
            heat_.refreshView(i,t);
        }
    }
    pending_surface_.clear();std::fill(pending_flag_.begin(),pending_flag_.end(),0);
    for(int i=0;i<s.n_cells;++i)if(covers(i)) {
        if(!std::isfinite(s.hg[i]) || !std::isfinite(s.hu[i]) || !std::isfinite(s.xacc_to_surface[i]) ||
           !std::isfinite(s.xacc_from_surface[i]) || s.hg[i]<0 || s.hg[i]>s.zs[i] || s.hu[i]<0 ||
           s.hu[i]>s.theta_s[i]*(s.zs[i]-s.hg[i])+1e-12)
            throw std::runtime_error("Coupled RK2 groundwater state is inadmissible.");
        cell_completed_[i]=end;
        if(s.xacc_to_surface[i]>0)markPendingSurface(i);
    }
}

}  // namespace openswmm::twoD
