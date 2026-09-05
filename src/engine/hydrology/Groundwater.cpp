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
 * @file Groundwater.cpp
 * @brief Two-zone groundwater — matching legacy gwater.c via RKF45 ODE solver.
 * @ingroup new_engine
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include "Groundwater.hpp"
#include "../core/Constants.hpp"
#include "../core/DateTime.hpp"
#include "../core/SimulationContext.hpp"
#include "../core/UnitConversion.hpp"
#include "../math/OdeSolver.hpp"
#include <cmath>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>
#include <vector>

namespace openswmm {
namespace groundwater {

// Use global constants
using constants::ode::GWTOL;
using constants::XTOL;

void GWSoA::resize(int n) {
    n_subcatch = n;
    auto un = static_cast<std::size_t>(n);

    porosity.assign(un, 0.4);
    field_cap.assign(un, 0.2);
    wilt_point.assign(un, 0.1);
    k_sat.assign(un, 0.0);
    k_slope.assign(un, 0.0);
    tension_slope.assign(un, 0.0);
    upper_evap_frac.assign(un, 0.0);
    upper_evap_pat.assign(un, -1);
    lower_evap_depth.assign(un, 0.0);
    lower_loss_coeff.assign(un, 0.0);
    total_depth.assign(un, 0.0);

    a1.assign(un, 0.0); b1.assign(un, 0.0);
    a2.assign(un, 0.0); b2.assign(un, 0.0);
    a3.assign(un, 0.0);
    h_star.assign(un, 0.0);

    theta.assign(un, 0.2);
    lower_depth.assign(un, 0.0);
    old_flow.assign(un, 0.0);

    gw_flow.assign(un, 0.0);
    upper_evap.assign(un, 0.0);
    lower_evap.assign(un, 0.0);
    deep_loss.assign(un, 0.0);
    max_infil_vol.assign(un, 0.0);

    lateral_expr.resize(un);
    deep_expr.resize(un);
}

void GWSolver::init(int n_subcatch) {
    soa_.resize(n_subcatch);
}

// ---------------------------------------------------------------------------
// Per-subcatchment context for ODE callback (matching legacy static vars)
// ---------------------------------------------------------------------------
struct GWContext {
    // Aquifer properties
    double porosity, field_cap, wilt_point;
    double k_sat, k_slope, tension_slope;
    double upper_evap_frac, lower_evap_depth, lower_loss_coeff;
    double total_depth;
    // GW flow coefficients
    double a1, b1, a2, b2, a3, h_star;
    // External inputs
    double infil;          // infiltration rate (ft/sec)
    double avail_evap;     // available evaporation (ft/sec)
    double max_evap;       // max evaporation rate (ft/sec)
    double sw_head;        // surface water head (ft above aquifer bottom)
    double dt;             // time step (sec)
    double area;           // subcatchment area (project units, for 'A' variable)
    // Unit conversion factors
    double ucf_length;     // UCF(LENGTH) — 1.0 for US
    double ucf_gwflow;     // UCF(GWFLOW) — 43560.0 for US (cfs/acre → ft/sec)
    double ucf_rainfall;   // UCF(RAINFALL) — 43200.0 for US (in/hr → ft/sec)
    double ucf_landarea;   // UCF(LANDAREA) — 2.2956e-5 for US (ac → ft²)
    // Flow limits (set before ODE integration)
    double max_upper_perc;
    double max_gw_flow_pos;
    double max_gw_flow_neg;
    // Output fluxes (set by getFluxes, read after integration)
    double upper_evap, lower_evap, upper_perc, deep_loss, gw_flow;
    // Custom flow expressions (nullptr if not set)
    const mathexpr::Expression* lateral_expr = nullptr;
    const mathexpr::Expression* deep_expr    = nullptr;
};

// ---------------------------------------------------------------------------
// getFluxes — matching legacy getFluxes() exactly
// ---------------------------------------------------------------------------
static void getFluxes(GWContext& c, double theta, double lower_depth) {
    lower_depth = std::max(lower_depth, 0.0);
    lower_depth = std::min(lower_depth, c.total_depth);
    double upper_depth = c.total_depth - lower_depth;

    // --- Evaporation (matching legacy getEvapRates) ---
    c.upper_evap = 0.0;
    c.lower_evap = 0.0;
    if (c.infil <= 0.0) {
        // Upper zone evap (only if above wilting point)
        if (theta > c.wilt_point) {
            c.upper_evap = c.upper_evap_frac * c.max_evap;
            c.upper_evap = std::min(c.upper_evap, c.avail_evap);
        }
        // Lower zone evap
        if (c.lower_evap_depth > 0.0) {
            double frac = (c.lower_evap_depth - upper_depth) / c.lower_evap_depth;
            frac = std::max(0.0, std::min(frac, 1.0));
            c.lower_evap = frac * (1.0 - c.upper_evap_frac) * c.max_evap;
            c.lower_evap = std::min(c.lower_evap, c.avail_evap - c.upper_evap);
        }
    }

    // --- Upper zone percolation (matching legacy getUpperPerc) ---
    c.upper_perc = 0.0;
    if (upper_depth > 0.0 && theta > c.field_cap) {
        double delta = theta - c.porosity;
        double hydcon = c.k_sat * std::exp(delta * c.k_slope);
        double delta2 = theta - c.field_cap;
        double dhdz = 1.0 + c.tension_slope * 2.0 * delta2 / upper_depth;
        c.upper_perc = hydcon * dhdz;
    }
    c.upper_perc = std::min(c.upper_perc, c.max_upper_perc);

    // --- Deep percolation (matching legacy getFluxes) ---
    // Custom [GWF] DEEP expression replaces the standard formula if present.
    if (c.deep_expr && c.deep_expr->valid) {
        // Build variable array in GW_VAR_NAMES order for evaluate_fast
        double vars[GWV_MAX];
        vars[GWV_HGW]   = lower_depth * c.ucf_length;
        vars[GWV_HSW]   = c.sw_head * c.ucf_length;
        vars[GWV_HCB]   = c.h_star * c.ucf_length;
        vars[GWV_HGS]   = c.total_depth * c.ucf_length;
        vars[GWV_KS]    = c.k_sat * c.ucf_rainfall;
        vars[GWV_K]     = c.upper_perc > 0.0 ? c.upper_perc * c.ucf_rainfall : 0.0;
        vars[GWV_THETA] = theta;
        vars[GWV_PHI]   = c.porosity;
        vars[GWV_FI]    = c.infil * c.ucf_rainfall;
        vars[GWV_FU]    = c.upper_perc * c.ucf_rainfall;
        vars[GWV_A]     = c.area * c.ucf_landarea;
        c.deep_loss = mathexpr::evaluate_fast(*c.deep_expr, vars) / c.ucf_rainfall;
    } else {
        c.deep_loss = (c.total_depth > 0.0)
            ? c.lower_loss_coeff * lower_depth / c.total_depth : 0.0;
    }
    c.deep_loss = std::min(c.deep_loss, lower_depth / c.dt);

    // --- Lateral GW flow (matching legacy getGWFlow) ---
    // Legacy applies UCF(LENGTH) to head differences before pow(),
    // then divides result by UCF(GWFLOW) to get ft/sec.
    // For US: UCF(LENGTH)=1.0, UCF(GWFLOW)=43560.0
    c.gw_flow = 0.0;
    if (lower_depth > c.h_star) {
        double ucf_len = c.ucf_length;
        double hdiff1 = (lower_depth - c.h_star) * ucf_len;
        double t1;
        if (c.b1 == 0.0)       t1 = c.a1;
        else if (c.b1 == 1.0)  t1 = c.a1 * hdiff1;
        else                    t1 = c.a1 * std::pow(hdiff1, c.b1);
        // Legacy: if b2==0 then t2=a2 always (constant surface water term),
        //         else if Hsw>Hstar then t2=a2*pow(...), else t2=0
        double t2;
        if (c.b2 == 0.0)
            t2 = c.a2;
        else if (c.sw_head > c.h_star) {
            double hdiff2 = (c.sw_head - c.h_star) * ucf_len;
            t2 = (c.b2 == 1.0) ? c.a2 * hdiff2
                                : c.a2 * std::pow(hdiff2, c.b2);
        }
        else
            t2 = 0.0;
        double t3 = c.a3 * lower_depth * c.sw_head * ucf_len * ucf_len;
        c.gw_flow = (t1 - t2 + t3) / c.ucf_gwflow;
        if (c.gw_flow < 0.0 && c.a3 != 0.0) c.gw_flow = 0.0;
    }
    // Custom [GWF] LATERAL expression adds to the standard formula if present.
    if (c.lateral_expr && c.lateral_expr->valid) {
        double vars[GWV_MAX];
        vars[GWV_HGW]   = lower_depth * c.ucf_length;
        vars[GWV_HSW]   = c.sw_head * c.ucf_length;
        vars[GWV_HCB]   = c.h_star * c.ucf_length;
        vars[GWV_HGS]   = c.total_depth * c.ucf_length;
        vars[GWV_KS]    = c.k_sat * c.ucf_rainfall;
        vars[GWV_K]     = c.upper_perc > 0.0 ? c.upper_perc * c.ucf_rainfall : 0.0;
        vars[GWV_THETA] = theta;
        vars[GWV_PHI]   = c.porosity;
        vars[GWV_FI]    = c.infil * c.ucf_rainfall;
        vars[GWV_FU]    = c.upper_perc * c.ucf_rainfall;
        vars[GWV_A]     = c.area * c.ucf_landarea;
        c.gw_flow += mathexpr::evaluate_fast(*c.lateral_expr, vars) / c.ucf_gwflow;
    }
    // Apply flow limits
    if (c.gw_flow >= 0.0)
        c.gw_flow = std::min(c.gw_flow, c.max_gw_flow_pos);
    else
        c.gw_flow = std::max(c.gw_flow, c.max_gw_flow_neg);
}

// ---------------------------------------------------------------------------
// getDxDt — ODE derivative callback matching legacy getDxDt()
// ---------------------------------------------------------------------------
static void getDxDt(GWContext& c, double /*t*/, const double* x, double* dxdt) {
    double theta_val = x[0];
    double lower_d   = x[1];

    getFluxes(c, theta_val, lower_d);

    // dθ/dt = (Infil - UpperEvap - UpperPerc) / upperDepth
    double upper_d = c.total_depth - lower_d;
    dxdt[0] = (upper_d > 0.0)
        ? (c.infil - c.upper_evap - c.upper_perc) / upper_d : 0.0;

    // dH/dt = (UpperPerc - DeepLoss - LowerEvap - GWFlow) / (φ - θ)
    double denom = c.porosity - theta_val;
    dxdt[1] = (denom > 0.0)
        ? (c.upper_perc - c.deep_loss - c.lower_evap - c.gw_flow) / denom : 0.0;
}

// ============================================================================
// Execute — per-subcatchment RKF45 integration matching legacy exactly
// ============================================================================

void GWSolver::execute(SimulationContext& ctx, double dt, double max_evap,
                       const double* infil_rate, const double* sw_head,
                       const double* frac_perv, const double* perv_evap_rate) {
    int n = soa_.n_subcatch;
    if (n == 0) return;

    for (int i = 0; i < n; ++i) {
        auto ui = static_cast<std::size_t>(i);

        // Skip subcatches without groundwater
        if (soa_.total_depth[ui] <= 0.0) {
            soa_.gw_flow[ui] = 0.0;
            soa_.upper_evap[ui] = 0.0;
            soa_.lower_evap[ui] = 0.0;
            soa_.deep_loss[ui] = 0.0;
            continue;
        }

        // --- Build per-subcatchment context (matching legacy shared vars) ---
        GWContext c;
        c.porosity         = soa_.porosity[ui];
        c.field_cap        = soa_.field_cap[ui];
        c.wilt_point       = soa_.wilt_point[ui];
        c.k_sat            = soa_.k_sat[ui];
        c.k_slope          = soa_.k_slope[ui];
        c.tension_slope    = soa_.tension_slope[ui];
        // Apply monthly evaporation pattern adjustment (matching legacy getEvapRates)
        double evap_frac = soa_.upper_evap_frac[ui];
        int pat_idx = soa_.upper_evap_pat[ui];
        if (pat_idx >= 0 && pat_idx < ctx.patterns.count()) {
            int month = datetime::monthOfYear(ctx.current_date);
            auto upat = static_cast<std::size_t>(pat_idx);
            const auto& facs = ctx.patterns.factors[upat];
            if (month >= 1 && month <= static_cast<int>(facs.size()))
                evap_frac *= facs[static_cast<std::size_t>(month - 1)];
        }
        c.upper_evap_frac  = evap_frac;
        c.lower_evap_depth = soa_.lower_evap_depth[ui];
        c.lower_loss_coeff = soa_.lower_loss_coeff[ui];
        c.total_depth      = soa_.total_depth[ui];
        c.a1 = soa_.a1[ui]; c.b1 = soa_.b1[ui];
        c.a2 = soa_.a2[ui]; c.b2 = soa_.b2[ui];
        c.a3 = soa_.a3[ui]; c.h_star = soa_.h_star[ui];
        c.infil            = infil_rate[i];
        // Match legacy: MaxEvap = Evap.rate * FracPerv
        // AvailEvap = max(MaxEvap - pervEvapRate, 0)
        // A prescribed PET forcing on this subcatchment replaces/augments
        // the broadcast climate rate (DRY_ONLY does not apply to GW evap).
        double fp = frac_perv[i];
        c.max_evap         = ctx.forcing.effective_evap_rate(ui, max_evap) * fp;
        c.avail_evap       = std::max(c.max_evap - perv_evap_rate[i], 0.0);
        c.sw_head          = sw_head[i];
        c.dt               = dt;
        c.area             = ctx.subcatches.area[ui];
        int unit_sys = ucf::getUnitSystem(static_cast<int>(ctx.options.flow_units));
        c.ucf_length       = ucf::Ucf[ucf::LENGTH][unit_sys];
        c.ucf_gwflow       = ucf::Ucf[ucf::GWFLOW][unit_sys];
        c.ucf_rainfall     = ucf::Ucf[ucf::RAINFALL][unit_sys];
        c.ucf_landarea     = ucf::Ucf[ucf::LANDAREA][unit_sys];
        // Custom expressions (nullptr if not set for this subcatch)
        c.lateral_expr = soa_.lateral_expr[ui].valid ? &soa_.lateral_expr[ui] : nullptr;
        c.deep_expr    = soa_.deep_expr[ui].valid    ? &soa_.deep_expr[ui]    : nullptr;

        // --- Set flow limits (matching legacy gwater_getGroundwater) ---
        double theta_0 = soa_.theta[ui];
        double lower_d_0 = soa_.lower_depth[ui];
        double upper_d_0 = c.total_depth - lower_d_0;

        // Max upper percolation: available water above field capacity
        double v_upper = upper_d_0 * (theta_0 - c.field_cap);
        v_upper = std::max(0.0, v_upper);
        c.max_upper_perc = v_upper / dt;

        // Max positive GW outflow: all water in lower zone
        c.max_gw_flow_pos = lower_d_0 * c.porosity / dt;

        // Max negative GW inflow: min of upper zone capacity and available node flow
        // (matching legacy gwater.c: MaxGWFlowNeg = -MIN(capacity, nodeFlow))
        double gw_capacity_neg = (c.total_depth - lower_d_0) * (c.porosity - theta_0) / dt;
        int gw_node = ctx.subcatches.gw_node[ui];
        if (gw_node < 0) gw_node = ctx.subcatches.outlet_node[ui];
        double node_flow = 0.0;
        if (gw_node >= 0 && gw_node < ctx.n_nodes()) {
            auto un = static_cast<std::size_t>(gw_node);
            double area_ft2 = ctx.subcatches.area[ui] * ucf::ACRES_TO_FT2;
            if (area_ft2 > 0.0)
                node_flow = (ctx.nodes.inflow[un] + ctx.nodes.volume[un] / dt) / area_ft2;
        }
        c.max_gw_flow_neg = -std::min(gw_capacity_neg, node_flow);

        // --- Integrate ODEs via RKF45 ---
        double x[2] = { theta_0, lower_d_0 };

        // Legacy ignores ODE return code; match that behavior
        ode::integrate(x, 2, 0.0, dt, GWTOL, dt,
            [&c](double t, const double* y, double* dydx) {
                getDxDt(c, t, y, dydx);
            });

        // --- Bound state variables (matching legacy) ---
        x[0] = std::max(x[0], c.wilt_point);
        if (x[0] >= c.porosity) {
            x[0] = c.porosity - XTOL;
            x[1] = c.total_depth - XTOL;
        }
        x[1] = std::max(x[1], 0.0);
        if (x[1] >= c.total_depth)
            x[1] = c.total_depth - XTOL;

        // --- Save updated state ---
        soa_.theta[ui]       = x[0];
        soa_.lower_depth[ui] = x[1];

        // Gap #40: compute max infiltration volume the upper zone can accept
        // in the next timestep (matching legacy gwater.c line ~597).
        // = (total_depth - new_lower_depth) * (porosity - new_theta) / frac_perv
        double fp_gw = frac_perv[i];
        if (fp_gw > 0.0 && c.total_depth > 0.0) {
            soa_.max_infil_vol[ui] =
                (c.total_depth - x[1]) * (c.porosity - x[0]) / fp_gw;
            soa_.max_infil_vol[ui] = std::max(soa_.max_infil_vol[ui], 0.0);
        } else {
            soa_.max_infil_vol[ui] = 0.0;
        }

        // --- Compute final fluxes at new state ---
        getFluxes(c, x[0], x[1]);
        soa_.old_flow[ui]    = soa_.gw_flow[ui]; // save previous
        soa_.gw_flow[ui]     = c.gw_flow;
        soa_.upper_evap[ui]  = c.upper_evap;
        soa_.lower_evap[ui]  = c.lower_evap;
        soa_.deep_loss[ui]   = c.deep_loss;
    }

    // --- Accumulate GW mass balance totals (matching legacy updateMassBal) ---
    auto& mb = ctx.mass_balance;
    for (int i = 0; i < n; ++i) {
        auto ui = static_cast<std::size_t>(i);
        if (soa_.total_depth[ui] <= 0.0) continue;

        double area = ctx.subcatches.area[ui] * ucf::ACRES_TO_FT2;
        double ft2sec = area * dt;

        mb.gw_infil        += infil_rate[i] * ft2sec;
        mb.gw_upper_evap   += soa_.upper_evap[ui] * ft2sec;
        mb.gw_lower_evap   += soa_.lower_evap[ui] * ft2sec;
        mb.gw_lower_perc   += soa_.deep_loss[ui] * ft2sec;
        // Trapezoidal averaging of GW flow (matching legacy)
        mb.gw_lateral_flow += 0.5 * (soa_.old_flow[ui] + soa_.gw_flow[ui]) * ft2sec;
    }
}

// ============================================================================
// [GWF] expression validation — diagnostic peer of mathexpr::parse
// ============================================================================
// mathexpr::parse is deliberately lenient (unknown characters are skipped,
// unknown identifiers evaluate to 0.0), so an editor cannot use "does it
// parse" as its verdict. This is a strict tokenizer + recursive-descent
// grammar check that never compiles or stores anything.

namespace {

enum class GwfTok { NUM, VAR, FUNC, OP, LPAREN, RPAREN, COMMA, END };

struct GwfToken {
    GwfTok      kind;
    std::string text;   // as typed (identifiers keep the user's case)
    int         col;
};

struct GwfFail {
    std::string msg;
    int         col;
};

bool gwf_is_variable(const std::string& upper) {
    for (const char* v : GW_VAR_NAMES)
        if (upper == v) return true;
    return false;
}

bool gwf_is_function(const std::string& lower) {
    for (const auto& f : mathexpr::function_names())
        if (lower == f) return true;
    return false;
}

std::vector<GwfToken> gwf_tokenize(const std::string& s) {
    std::vector<GwfToken> out;
    std::size_t i = 0;
    while (i < s.size()) {
        const char c = s[i];
        if (std::isspace(static_cast<unsigned char>(c))) { ++i; continue; }
        const int col = static_cast<int>(i);

        if (std::isdigit(static_cast<unsigned char>(c)) || c == '.') {
            const std::size_t start = i;
            while (i < s.size() &&
                   (std::isdigit(static_cast<unsigned char>(s[i])) ||
                    s[i] == '.' || s[i] == 'e' || s[i] == 'E' ||
                    ((s[i] == '+' || s[i] == '-') && i > 0 &&
                     (s[i-1] == 'e' || s[i-1] == 'E'))))
                ++i;
            const std::string word = s.substr(start, i - start);
            char* end = nullptr;
            std::strtod(word.c_str(), &end);
            if (end == word.c_str() || *end != '\0')
                throw GwfFail{"malformed number '" + word + "'", col};
            out.push_back({GwfTok::NUM, word, col});
            continue;
        }

        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            const std::size_t start = i;
            while (i < s.size() &&
                   (std::isalnum(static_cast<unsigned char>(s[i])) || s[i] == '_'))
                ++i;
            const std::string word = s.substr(start, i - start);
            std::string lower = word, upper = word;
            for (auto& ch : lower) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            for (auto& ch : upper) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
            if (gwf_is_function(lower))      out.push_back({GwfTok::FUNC, word, col});
            else if (gwf_is_variable(upper)) out.push_back({GwfTok::VAR, word, col});
            else throw GwfFail{"unknown variable '" + word + "'", col};
            continue;
        }

        switch (c) {
            case '+': case '-': case '*': case '/': case '^':
                out.push_back({GwfTok::OP, std::string(1, c), col}); break;
            case '(': out.push_back({GwfTok::LPAREN, "(", col}); break;
            case ')': out.push_back({GwfTok::RPAREN, ")", col}); break;
            case ',': out.push_back({GwfTok::COMMA, ",", col}); break;
            default:
                throw GwfFail{std::string("unexpected character '") + c + "'", col};
        }
        ++i;
    }
    out.push_back({GwfTok::END, "", static_cast<int>(s.size())});
    return out;
}

// expr   := term (('+'|'-') term)*
// term   := unary (('*'|'/') unary)*
// unary  := '-' unary | power
// power  := primary ('^' unary)?
// primary:= NUM | VAR | FUNC '(' expr (',' expr)* ')' | '(' expr ')'
struct GwfParser {
    const std::vector<GwfToken>& t;
    std::size_t p = 0;
    int paren_depth = 0;   // > 0 while inside any '(' — decides ')' diagnostics
    int func_depth  = 0;   // > 0 while inside a function's argument list

    const GwfToken& cur() const { return t[p]; }
    bool is_op(const char* s) const { return cur().kind == GwfTok::OP && cur().text == s; }

    void expr() {
        term();
        while (is_op("+") || is_op("-")) { ++p; term(); }
    }
    void term() {
        unary();
        while (is_op("*") || is_op("/")) { ++p; unary(); }
    }
    void unary() {
        if (is_op("-")) { ++p; unary(); return; }
        power();
    }
    void power() {
        primary();
        if (is_op("^")) { ++p; unary(); }
    }
    void primary() {
        const GwfToken& k = cur();
        switch (k.kind) {
            case GwfTok::NUM:
            case GwfTok::VAR:
                ++p;
                return;
            case GwfTok::FUNC: {
                ++p;
                if (cur().kind != GwfTok::LPAREN)
                    throw GwfFail{"function '" + k.text + "' must be followed by '('",
                                  cur().col};
                const GwfToken& open = cur();
                ++p; ++paren_depth; ++func_depth;
                int nargs = 1;
                expr();
                while (cur().kind == GwfTok::COMMA) { ++p; ++nargs; expr(); }
                close_paren(open);
                --paren_depth; --func_depth;
                std::string lower = k.text;
                for (auto& ch : lower) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                const int want = (lower == "min" || lower == "max") ? 2 : 1;
                if (nargs != want)
                    throw GwfFail{"function '" + k.text + "' takes " +
                                  std::to_string(want) +
                                  (want == 1 ? " argument" : " arguments"), k.col};
                return;
            }
            case GwfTok::LPAREN: {
                const GwfToken& open = cur();
                ++p; ++paren_depth;
                expr();
                if (cur().kind == GwfTok::COMMA)
                    throw GwfFail{"unexpected ','", cur().col};
                close_paren(open);
                --paren_depth;
                return;
            }
            case GwfTok::OP:
                throw GwfFail{"missing operand before '" + k.text + "'", k.col};
            case GwfTok::RPAREN:
                if (paren_depth == 0) throw GwfFail{"unbalanced parenthesis", k.col};
                throw GwfFail{"missing operand before ')'", k.col};
            case GwfTok::COMMA:
                if (func_depth == 0) throw GwfFail{"unexpected ','", k.col};
                throw GwfFail{"missing operand before ','", k.col};
            case GwfTok::END: {
                const GwfToken& prev = t[p - 1];
                if (prev.kind == GwfTok::LPAREN)
                    throw GwfFail{"unbalanced parenthesis", prev.col};
                throw GwfFail{"missing operand after '" + prev.text + "'", prev.col};
            }
        }
    }
    // After an expression inside '(': the next token must close it.
    void close_paren(const GwfToken& open) {
        if (cur().kind == GwfTok::RPAREN) { ++p; return; }
        if (cur().kind == GwfTok::END)
            throw GwfFail{"unbalanced parenthesis", open.col};
        throw GwfFail{"missing operator before '" + cur().text + "'", cur().col};
    }
};

} // namespace

int gwf_validate(const std::string& expr, std::string& msg, int& col) {
    msg.clear();
    col = -1;
    try {
        const auto tokens = gwf_tokenize(expr);
        if (tokens.front().kind == GwfTok::END)
            throw GwfFail{"expression is empty", 0};

        GwfParser parser{tokens};
        parser.expr();
        const GwfToken& rest = parser.cur();
        if (rest.kind != GwfTok::END) {
            if (rest.kind == GwfTok::RPAREN) throw GwfFail{"unbalanced parenthesis", rest.col};
            if (rest.kind == GwfTok::COMMA)  throw GwfFail{"unexpected ','", rest.col};
            throw GwfFail{"missing operator before '" + rest.text + "'", rest.col};
        }

        // Drift guard: the production parser must accept what we accept.
        mathexpr::Expression probe;
        if (mathexpr::parse(expr, probe) != 0 || !probe.valid)
            throw GwfFail{"internal: expression rejected by the engine parser", -1};
    } catch (const GwfFail& f) {
        msg = f.msg;
        col = f.col;
        return -1;
    } catch (...) {
        msg = "internal: expression rejected by the engine parser";
        col = -1;
        return -1;
    }
    return 0;
}

} // namespace groundwater
} // namespace openswmm
