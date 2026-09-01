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
 * @file BedExchange.cpp
 * @brief Plan H6b — implementation. See BedExchange.hpp for the derivation.
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include "BedExchange.hpp"

#include <cmath>
#include <cstddef>

#include "SurfaceExchange.hpp"
#include "../../../core/SimulationContext.hpp"
#include "../../../data/TableData.hpp"
#include "../../../hydraulics/XSectBatch.hpp"

namespace openswmm::transport::heat {

namespace {

/// phi1(z) = expm1(z)/z, with the removable singularity taken.
double phi1(double z) noexcept {
    if (std::fabs(z) < 1e-8) return 1.0 + 0.5 * z;   // series, exact to 1e-16
    return std::expm1(z) / z;
}

/// phi2(z) = (e^z - 1 - z)/z^2, with the removable singularity taken.
/// Used only on the near-degenerate branch, where the spectral formula's
/// 1/(lambda1 - lambda2) is not usable.
double phi2(double z) noexcept {
    if (std::fabs(z) < 1e-5) return 0.5 + z / 6.0;
    return (std::expm1(z) - z) / (z * z);
}

/// Cross-section parameters for a link. The same shape `HeatFluxes.cpp`
/// builds; kept local rather than exported because widening that file's
/// helper to a public seam is a bigger change than repeating ten field
/// copies, and CLAUDE.md §3 says touch only what must be touched.
XSectParams buildXsp(const LinkData& links, std::size_t uk) {
    XSectParams xs{};
    const auto ls = links.xsect_shape[uk];
    xs.type   = (ls == XsectShape::DUMMY) ? 0 : static_cast<int>(ls) + 1;
    xs.y_full = links.xsect_y_full[uk];
    xs.a_full = links.xsect_a_full[uk];
    xs.w_max  = links.xsect_w_max[uk];
    xs.r_full = links.xsect_r_full[uk];
    xs.s_full = links.xsect_s_full[uk];
    xs.s_max  = links.xsect_s_max[uk];
    xs.y_bot  = links.xsect_y_bot[uk];
    xs.a_bot  = links.xsect_a_bot[uk];
    xs.s_bot  = links.xsect_s_bot[uk];
    xs.r_bot  = links.xsect_r_bot[uk];
    return xs;
}

}  // namespace

// ---------------------------------------------------------------------------
// The coupled stepper
// ---------------------------------------------------------------------------
PairStep relaxPair(const BedCoupling& g, double t_w, double t_b,
                   double j0, double j1, double h, double area_m2,
                   double dt) noexcept {
    PairStep out{};
    if (!g.viable() || !(dt > 0.0)) return out;

    // Surface flux slope, W/m²/°C. `h <= 0` or a non-increasing flux means
    // there is no usable linearization; treat the surface term as a constant
    // forcing, which is exactly `relaxT`'s own fallback and keeps the two
    // functions agreeing about when the probe is trusted.
    double djdt = 0.0;
    if (h > 0.0) {
        const double s = (j1 - j0) / h;
        if (s > 0.0 && std::isfinite(s)) djdt = s;
    }

    // du/dt = M u + c, u = (T_w - t_w, T_b - t_b), u(0) = 0.
    const double a11 = -(area_m2 * djdt + g.g_wb) / g.c_w;
    const double a12 = g.g_wb / g.c_w;
    const double a21 = g.g_wb / g.c_b;
    const double a22 = -(g.g_wb + g.g_bg) / g.c_b;

    const double c1 = (-area_m2 * j0 + g.g_wb * (t_b - t_w)) / g.c_w;
    const double c2 = (g.g_wb * (t_w - t_b) + g.g_bg * (g.t_gr - t_b)) / g.c_b;

    // Eigenvalues. The discriminant is (a11-a22)^2 + 4*a12*a21 with both
    // off-diagonals >= 0, so it is never negative: the pair relaxes, it never
    // oscillates, and there is no dt at which this overshoots.
    const double tr   = a11 + a22;
    const double diff = a11 - a22;
    const double disc = diff * diff + 4.0 * a12 * a21;
    const double s    = std::sqrt(disc > 0.0 ? disc : 0.0);

    // phi1(M*dt) = alpha*M + beta*I, by the spectral formula for a 2x2.
    double alpha = 0.0, beta = 0.0;
    if (s * dt > 1e-8) {
        const double l1 = 0.5 * (tr + s);
        const double l2 = 0.5 * (tr - s);
        const double p1 = phi1(l1 * dt);
        const double p2 = phi1(l2 * dt);
        alpha = (p1 - p2) / s;
        beta  = (p2 * l1 - p1 * l2) / s;
    } else {
        // Near-degenerate: the spectral formula divides by s. The confluent
        // form is the limit and needs no division —
        //   phi1(M dt) = phi1(l dt) I + dt phi2(l dt) (M - l I),
        // and since phi1(z) = 1 + z phi2(z) identically, the identity
        // coefficient collapses to exactly 1.
        const double l = 0.5 * tr;
        alpha = dt * phi2(l * dt);
        beta  = 1.0;
    }

    // u = dt * phi1(M dt) * c = dt * (alpha * M c + beta * c)
    const double mc1 = a11 * c1 + a12 * c2;
    const double mc2 = a21 * c1 + a22 * c2;
    const double dw  = dt * (alpha * mc1 + beta * c1);
    const double db  = dt * (alpha * mc2 + beta * c2);

    // A non-finite result means the linearization produced something the
    // caller must not absorb. Refusing to move is the safe answer and the
    // one `relaxT` also takes.
    if (!std::isfinite(dw) || !std::isfinite(db)) return out;
    out.dt_w = dw;
    out.dt_b = db;
    return out;
}

SolutePairStep exchangePair(double c_w, double c_b, double vol_w,
                            double vol_b, double q_exch, double dt) noexcept {
    SolutePairStep out{};
    if (!(vol_w > 0.0) || !(vol_b > 0.0) || !(q_exch > 0.0) || !(dt > 0.0))
        return out;

    // Two bodies exchanging with nothing else: total mass is invariant and
    // the DIFFERENCE decays exponentially. Solving in those coordinates
    // makes conservation STRUCTURAL — there is no separate ledger step that
    // could be forgotten. It is not bit-exact, because `vol_w*(vol_b*share)`
    // and `vol_b*(vol_w*share)` associate differently; the residual is
    // relative round-off and the gate asserts that bound. See the header.
    const double vt = vol_w + vol_b;
    const double mu = q_exch * vt / (vol_w * vol_b);   // 1/s
    const double d0 = c_w - c_b;
    const double dd = d0 * std::expm1(-mu * dt);       // d(t) - d0

    const double share = dd / vt;
    if (!std::isfinite(share)) return out;
    out.dc_w =  vol_b * share;
    out.dc_b = -vol_w * share;
    return out;
}

// ---------------------------------------------------------------------------
// Configuration and geometry
// ---------------------------------------------------------------------------
bool bedExchangeEnabled(const SimulationContext& ctx) noexcept {
    return ctx.options.heat_transport && ctx.heat_config.sediment_exchange;
}

double groundTemperature(SimulationContext& ctx) noexcept {
    const auto& sc = ctx.heat_config.sediment;
    if (sc.ground_ts_index >= 0 &&
        sc.ground_ts_index < static_cast<int>(ctx.tables.count()))
        return table_tseries_lookup_cursor(ctx.tables[sc.ground_ts_index],
                                           ctx.current_date);
    return sc.ground_temp;
}

double linkBedAreaM2(const SimulationContext& ctx, int link) noexcept {
    const auto uj = static_cast<std::size_t>(link);
    const int  cr = ctx.link_subtypes.conduit_row(link);
    if (cr < 0) return 0.0;                  // regulators have no bed
    const auto ucr = static_cast<std::size_t>(cr);
    const auto& CD = ctx.link_subtypes.conduits;

    double length = CD.length[ucr];
    if (!(length > 0.0)) length = CD.mod_length[ucr];
    if (!(length > 0.0)) return 0.0;

    const double depth = ctx.links.depth[uj];
    if (!(depth > 0.0)) return 0.0;

    // Contact area = wetted perimeter x length x barrels. P = A/R, the
    // engine's own identity — NOT the reference's top width, which is zero
    // for a full pipe. See BedZoneData.hpp divergence 1.
    const auto   xs   = buildXsp(ctx.links, uj);
    const double area = xsect::getAofY(xs, depth);
    const double rhyd = xsect::getRofY(xs, depth);
    if (!(area > 0.0) || !(rhyd > 0.0)) return 0.0;
    const double perim_ft = area / rhyd;

    const double m2 =
        perim_ft * length * static_cast<double>(CD.barrels[ucr]) * kSqFtToSqM;
    return (m2 > 0.0) ? m2 : 0.0;
}

double linkFreeSurfaceFt2(const SimulationContext& ctx, int link) noexcept {
    const auto uj = static_cast<std::size_t>(link);
    const int  cr = ctx.link_subtypes.conduit_row(link);
    if (cr < 0) return 0.0;
    if (!xsect::isOpen(ctx.links.xsect_batch_shape[uj])) return 0.0;
    const auto ucr = static_cast<std::size_t>(cr);
    const auto& CD = ctx.link_subtypes.conduits;

    double length = CD.length[ucr];
    if (!(length > 0.0)) length = CD.mod_length[ucr];
    const double depth = ctx.links.depth[uj];
    if (!(length > 0.0) || !(depth > 0.0)) return 0.0;
    const auto xs = buildXsp(ctx.links, uj);
    const double top_width = xsect::getWofY(xs, depth);
    if (!(top_width > 0.0)) return 0.0;
    return top_width * length * static_cast<double>(CD.barrels[ucr]);
}

double bedExchangeQ(const SedimentConfig& cfg, double bed_m2) noexcept {
    if (!(bed_m2 > 0.0) || !(cfg.bed_thickness > 0.0)) return 0.0;
    return cfg.solute_diffusivity * bed_m2 / cfg.bed_thickness +
           cfg.hyporheic_velocity * bed_m2;
}

BedCoupling bedCouplingFromContact(const SimulationContext& ctx,
                                   double bed_m2, double vol_ft3,
                                   double t_gr) noexcept {
    BedCoupling g{};
    if (!(bed_m2 > 0.0) || !(vol_ft3 > 0.0)) return g;

    const auto&  sc       = ctx.heat_config.sediment;
    const double rho_cp_s = sc.sed_density * sc.sed_specific_heat;  // J/m3/K
    const double y_bed    = sc.bed_thickness;
    const double y_gr     = sc.ground_depth;
    if (!(y_bed > 0.0)) return g;

    // Conduction: alpha * rho_s * c_s has units W/m/K, so dividing by a
    // length and multiplying by an area gives W/K. This is the reference's
    // grouping (`element.cpp:132`) rather than a conductivity k, because
    // alpha is the parameter the reference exposes and re-deriving k here
    // would silently invent a second material description.
    const double k_eff  = sc.thermal_diffusivity * rho_cp_s;        // W/m/K
    const double g_cond = k_eff * bed_m2 / y_bed;
    const double g_adv  = ctx.options.water_density *
                          ctx.options.water_specific_heat *
                          sc.hyporheic_velocity * bed_m2;           // W/K

    g.g_wb = g_cond + g_adv;
    g.g_bg = (y_gr > 0.0) ? (k_eff * bed_m2 / y_gr) : 0.0;
    g.c_w  = ctx.options.water_density * ctx.options.water_specific_heat *
             vol_ft3 * kCuFtToCuM;
    g.c_b  = rho_cp_s * bed_m2 * y_bed;
    g.t_gr = t_gr;
    return g;
}

BedCoupling bedCouplingForLink(const SimulationContext& ctx, int link,
                               double vol_ft3, double t_gr) noexcept {
    if (!(vol_ft3 > 0.0)) return BedCoupling{};
    return bedCouplingFromContact(ctx, linkBedAreaM2(ctx, link), vol_ft3,
                                  t_gr);
}

// ---------------------------------------------------------------------------
// Bindings
// ---------------------------------------------------------------------------
void seedBedTemperature(SimulationContext& ctx) {
    auto& bed = ctx.bed_state;
    const int nl = ctx.n_links();
    const auto& sc = ctx.heat_config.sediment;
    const double t0 = sc.has_initial_temp ? sc.initial_temp : sc.ground_temp;

    // Temperature and solute extents are sized INDEPENDENTLY. A single
    // `resize` would have to know the species count, and the heat binding
    // runs first and does not — so a combined resize would either wipe the
    // solute store on every heat step or force the heat step to guess a
    // dimension that belongs to the transport engine.
    if (!bed.sized(nl)) {
        bed.link_temp.assign(
            static_cast<std::size_t>(nl > 0 ? nl : 0), t0);
        bed.seeded = false;
    }
    if (!bed.seeded) {
        // The bed starts at the configured initial temperature, falling back
        // to the ground temperature — the only number in the configuration
        // that describes the subsurface. It deliberately does NOT start at
        // the water's INITIAL_STATE: a bed that starts at the channel
        // temperature exchanges nothing for the first steps and would hide a
        // mis-wired coupling behind a plausible flat line.
        for (auto& t : bed.link_temp) t = t0;
        bed.seeded = true;
    }
}

void applyBedSoluteExchange(SimulationContext& ctx, double* link_conc,
                            int n_arr, int offset, int n_total, double dt) {
    if (!bedExchangeEnabled(ctx) || !(dt > 0.0) || link_conc == nullptr) return;
    if (n_arr <= 0 || offset < 0 || n_total < offset + n_arr) return;
    seedBedTemperature(ctx);

    auto& bed = ctx.bed_state;
    const int nl_all = ctx.n_links();
    const auto need = static_cast<std::size_t>(nl_all > 0 ? nl_all : 0) *
                      static_cast<std::size_t>(n_total);
    if (bed.n_species != n_total || bed.link_conc.size() != need) {
        // First solute step, or the species count changed. Seeding the bed
        // at ZERO is the honest default: the reference has no initial-bed
        // concentration input either, and inventing one would put mass into
        // the system that no deck asked for.
        bed.link_conc.assign(need, 0.0);
        bed.n_species = n_total;
    }

    const auto&  sc = ctx.heat_config.sediment;
    const int    nl = ctx.n_links();
    const auto   na = static_cast<std::size_t>(n_arr);
    const auto   nt = static_cast<std::size_t>(n_total);
    const auto   off = static_cast<std::size_t>(offset);
    // One lookup per STEP, not per link — the ground temperature is GLOBAL
    // scope and a per-link lookup would advance the series cursor N times.
    // (Solutes do not read t_gr; the seed inside seedBedTemperature does.)
    (void)sc;

    for (int j = 0; j < nl; ++j) {
        const auto uj = static_cast<std::size_t>(j);
        if (uj >= bed.link_temp.size()) break;

        const double vol_ft3 = ctx.links.volume[uj];
        if (!(vol_ft3 > 0.0)) continue;
        const double bed_m2 = linkBedAreaM2(ctx, j);
        if (!(bed_m2 > 0.0)) continue;

        const double vol_w  = vol_ft3 * kCuFtToCuM;
        const double vol_b  = bed_m2 * ctx.heat_config.sediment.bed_thickness;
        const double q_exch = bedExchangeQ(ctx.heat_config.sediment, bed_m2);
        if (!(q_exch > 0.0) || !(vol_b > 0.0)) continue;

        for (std::size_t s = 0; s < na; ++s) {
            double& cw = link_conc[uj * na + s];
            double& cb = bed.link_conc[uj * nt + off + s];
            const SolutePairStep st =
                exchangePair(cw, cb, vol_w, vol_b, q_exch, dt);
            cw += st.dc_w;
            cb += st.dc_b;
        }
    }
}

}  // namespace openswmm::transport::heat
