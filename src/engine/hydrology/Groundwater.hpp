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
 * @file      Groundwater.hpp
 * @brief     Two-zone groundwater model — batch-oriented ODE integration.
 *
 * @details Each subcatchment has an independent aquifer with upper (unsaturated)
 *          and lower (saturated) zones. The ODE system is:
 *
 *            dθ/dt = (Infil - UpperEvap - UpperPerc) / upperDepth
 *            dH/dt = (UpperPerc - DeepPerc - LowerEvap - GWFlow) / (φ - θ)
 *
 *          Since each subcatchment is independent, the ODE integration is
 *          embarrassingly parallel — vectorise over subcatchments.
 *
 *          The lateral GW flow formula:
 *            Q = a1*(H-H*)^b1 - a2*(Hsw-H*)^b2 + a3*H*Hsw
 *          is arithmetic-only and vectorisable when b1/b2 are uniform.
 *
 * @note Legacy reference: src/legacy/engine/gwater.c
 * @ingroup new_engine
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_GROUNDWATER_HPP
#define OPENSWMM_GROUNDWATER_HPP

#ifndef OPENSWMM_RESTRICT
#if defined(_MSC_VER)
#define OPENSWMM_RESTRICT __restrict
#else
#define OPENSWMM_RESTRICT __restrict__
#endif
#endif

#include <vector>
#include "../math/MathExpr.hpp"

namespace openswmm {

struct SimulationContext;

namespace groundwater {

/// GW expression variable indices (matching legacy GWvariables enum).
enum GWVar : int {
    GWV_HGW = 0,   ///< Water table height
    GWV_HSW = 1,   ///< Surface water head
    GWV_HCB = 2,   ///< Channel bottom height (h_star)
    GWV_HGS = 3,   ///< Ground surface height (total_depth)
    GWV_KS = 4,    ///< Saturated conductivity
    GWV_K = 5,     ///< Unsaturated conductivity
    GWV_THETA = 6, ///< Upper zone moisture content
    GWV_PHI = 7,   ///< Porosity
    GWV_FI = 8,    ///< Surface infiltration rate
    GWV_FU = 9,    ///< Upper zone percolation rate
    GWV_A = 10,    ///< Subcatchment area
    GWV_MAX = 11
};

/// Variable name table for bind_variables().
static const char* GW_VAR_NAMES[] = {"HGW",   "HSW", "HCB", "HGS", "KS", "K",
                                     "THETA", "PHI", "FI",  "FU",  "A"};

/// Short English gloss per GW_VAR_NAMES entry (editor tooltips / completers).
static const char* GW_VAR_DESCRIPTIONS[] = {
    "Water table height above aquifer bottom (ft or m)",
    "Surface-water head at the receiving node above aquifer bottom (ft or m)",
    "Channel bottom height above aquifer bottom (Hstar) (ft or m)",
    "Ground surface height above aquifer bottom (ft or m)",
    "Saturated hydraulic conductivity (in/hr or mm/hr)",
    "Unsaturated hydraulic conductivity (in/hr or mm/hr)",
    "Upper-zone moisture content (fraction)",
    "Soil porosity (fraction)",
    "Surface infiltration rate (in/hr or mm/hr)",
    "Upper-zone percolation rate (in/hr or mm/hr)",
    "Subcatchment area (ac or ha)"};

/** @brief Validate a GWF expression and probe the production parser without storing it.
 * @param expr Borrowed arithmetic text; empty/whitespace-only is invalid. Supports
 * numbers, variable identifiers, known functions, parentheses, comma and + - * / ^.
 * Identifiers are case insensitive; legacy variable names may be prefixes of a
 * longer identifier (e.g. HGWW resolves HGW, Area resolves A).
 * @param[out] msg Cleared on entry/success, otherwise human-readable rejection.
 * @param[out] col Zero-based byte position or -1 for unattributed/internal failure;
 * not a Unicode code-point column. Text/token positions must fit int.
 * @return Zero when strict grammar and a production parse both accept, -1 otherwise.
 * Checks function arity (min/max two, others one), operator sequencing, unary minus
 * and balanced parentheses. Unary plus is rejected. Numeric overflow to infinity
 * is not explicitly refused by tokenization. Does not evaluate, check units/physics,
 * bind variables or retain the compiled probe. Catches parser/allocation exceptions
 * into an internal diagnostic where possible. */
int gwf_validate(const std::string& expr, std::string& msg, int& col);

// ============================================================================
// Per-subcatchment GW state (SoA for vectorization)
// ============================================================================

struct GWSoA {
    int n_subcatch = 0;

    // Aquifer properties (set at init, constant during sim)
    std::vector<double> porosity;
    std::vector<double> field_cap;
    std::vector<double> wilt_point;
    std::vector<double> k_sat;   ///< Saturated conductivity (ft/sec)
    std::vector<double> k_slope; ///< Exponential decay slope
    std::vector<double> tension_slope;
    std::vector<double> upper_evap_frac;
    std::vector<int> upper_evap_pat; ///< Pattern index for monthly evap adjustment (-1 = none)
    std::vector<double> lower_evap_depth;
    std::vector<double> lower_loss_coeff; ///< Deep percolation coeff
    std::vector<double> total_depth;      ///< Aquifer thickness (ft)
    std::vector<double>
        bottom_elev; ///< Effective aquifer bottom elevation (ft): the [GROUNDWATER] row's Ebot, else the aquifer's

    // Lateral flow coefficients
    std::vector<double> a1, b1; ///< GW outflow
    std::vector<double> a2, b2; ///< Surface water interaction
    std::vector<double> a3;     ///< Cross-interaction
    std::vector<double> h_star; ///< Threshold water table height (ft)

    // State variables (updated each timestep)
    std::vector<double> theta;       ///< Upper zone moisture content (0-φ)
    std::vector<double> lower_depth; ///< Lower zone depth (ft)

    // State
    std::vector<double> old_flow; ///< Previous step GW flow (for trapezoidal avg)

    // Outputs
    std::vector<double> gw_flow;    ///< Lateral GW flux over full subcatchment area (ft/s)
    std::vector<double> upper_evap; ///< Upper-zone evaporation flux (ft/s over full area)
    std::vector<double> lower_evap; ///< Lower-zone evaporation flux (ft/s over full area)
    std::vector<double> deep_loss;  ///< Deep percolation flux (ft/s over full area)
    /// Gap #40: max infiltration volume upper zone can accept in next step (ft).
    /// = (total_depth - lower_depth) * (porosity - theta) / frac_perv
    /// Used by Runoff to cap infiltration rate: max_infil_rate = max_infil_vol / dt.
    std::vector<double> max_infil_vol;

    // Custom flow expressions (from [GWF] section)
    /// Per-subcatch compiled lateral flow expression (added to standard formula).
    std::vector<mathexpr::Expression> lateral_expr;
    /// Per-subcatch compiled deep percolation expression (replaces standard formula).
    std::vector<mathexpr::Expression> deep_expr;

    /** @brief Resize/reset all scalar groundwater rows while retaining expression prefixes.
     * @param n Nonnegative subcatchment count with allocatable vectors; unchecked before
     * conversion to size_t. Sets n_subcatch, assigns physical/history/output defaults,
     * but uses resize for lateral/deep expression arrays, preserving overlapping entries.
     * Invalidates element borrows as storage changes. Allocation failures propagate and
     * can leave partially reset state; caller completes model qualification before stepping. */
    void resize(int n);
};

// ============================================================================
// GW solver
// ============================================================================

/** @brief Mutable two-zone groundwater state with independent scalar per-subcatchment RKF45.
 * One owning simulation thread serializes init/execute/state edits. Initialization
 * supplies defaults only; model loading must populate physical aquifer parameters
 * and compile/bind optional expressions before stepping. No transactional rollback
 * or per-call integration failure result is exposed. */
class GWSolver {
public:
    /** @brief Initialize the owned SoA through GWSoA::resize().
     * @param n_subcatch Nonnegative model count with representable/allocatable rows.
     * Resets scalar parameters/history/output to defaults but can retain overlapping
     * compiled expression entries from prior state. Does not load aquifers from a
     * context, compile expressions, update clocks or validate a physical model. */
    void init(int n_subcatch);

    /** @brief Integrate each active two-zone aquifer and book its groundwater water balance.
     * @param[in,out] ctx Compatible initialized context with at least state().n_subcatch
     * aligned area/receiving-node/forcing rows, valid units/patterns and node hydraulic
     * state. Uses project area to derive ft2 for balance booking. Mass balance totals
     * are incremented; subcatchment/node hydraulic arrays are not directly assigned.
     * @param dt Finite positive seconds; divides by dt without a guard.
     * @param max_evap Finite nonnegative climate potential ft/s, resolved through each
     * subcatchment's forcing then multiplied by its pervious fraction. DRY_ONLY is not
     * applied to groundwater evaporation.
     * @param infil_rate Required readable n-row ft/s over full subcatchment area.
     * @param sw_head Required readable n-row surface-water head feet above aquifer bottom.
     * @param frac_perv Required readable n-row pervious fraction in [0,1]; nonpositive
     * skips ODE/state update, retaining previous outputs. The later balance loop still
     * visits any positive-thickness aquifer, so caller inputs must be consistent.
     * @param perv_evap_rate Required readable n-row ft/s over full area already removed
     * by the surface, subtracted from available groundwater ET.
     * @param evap_pattern_month One-based runoff endpoint month; positive uses it, zero
     * or negative uses ctx.current_date. Pattern factors apply only for an available row.
     * Arrays may be null when n==0; otherwise must not alias owned mutable state.
     * Uses RKF45 with legacy tolerance, ignores its status, bounds moisture/head and
     * computes final endpoint rates plus next-step infiltration capacity. Lateral water
     * balance uses old/new trapezoid, other losses endpoint rates. Zero-thickness rows
     * zero four outputs; no general physical/finite validation or rollback. All stored
     * GW/ET/deep outputs are ft/s, multiplied by full area for balance volumes. Custom
     * GWF K uses computed upper-percolation rate; A is passed as ctx.subcatches.area
     * multiplied by LANDAREA conversion as implemented, not independently normalized.
     * Large batches may use the configured OpenMP team for independent aquifers;
     * small batches and one-worker requests bypass parallel regions. Integration
     * workspace is thread-local; the balance fold retains original subcatchment order.
     * Caller owns acceptable model/expression units and accepted-step invocation. */
    void execute(SimulationContext& ctx, double dt, double max_evap, const double* infil_rate,
                 const double* sw_head, const double* frac_perv, const double* perv_evap_rate,
                 int evap_pattern_month = 0);

    /** @brief Borrow mutable aquifer parameters, history, outputs and compiled expressions.
     * @return Owned SoA valid until destruction; init/resizing invalidates elements.
     * Keep all rows aligned/physically qualified and bound expressions compatible with
     * GW_VAR_NAMES before execute. Serialize edits with integration and other borrows. */
    GWSoA& state() { return soa_; }
    /** @brief Borrow immutable aquifer state and latest endpoint outputs.
     * @return Owned SoA reference valid until destruction, with element storage invalidated
     * by init/resize. Output flux units are ft/s over full area, not cfs. Caller serializes
     * reading against execute and copies snapshots needed across mutation. */
    const GWSoA& state() const { return soa_; }

private:
    GWSoA soa_;

    /** @brief Reserved batch-percolation declaration; no maintained implementation/call exists.
     * @param theta Intended readable count-row moisture fractions.
     * @param field_cap Intended readable count-row field-capacity fractions.
     * @param k_sat Intended readable count-row saturated conductivity ft/s.
     * @param k_slope Intended readable count-row exponential slopes.
     * @param[out] perc Intended writable count-row percolation fluxes ft/s.
     * @param count Intended nonnegative row count; restrict-qualified arrays are disjoint.
     * No runtime operation is provided by this private declaration; execute() uses
     * scalar getFluxes() instead. Calling it would require an out-of-line definition. */
    static void batchUpperPerc(const double* OPENSWMM_RESTRICT theta,
                               const double* OPENSWMM_RESTRICT field_cap,
                               const double* OPENSWMM_RESTRICT k_sat,
                               const double* OPENSWMM_RESTRICT k_slope,
                               double* OPENSWMM_RESTRICT perc, int count);

    /** @brief Reserved batch-flow declaration; no maintained implementation/call exists.
     * @param lower_depth Intended readable count-row saturated depths feet.
     * @param h_star Intended readable count-row threshold depths feet.
     * @param a1 Intended readable outflow coefficients.
     * @param b1 Intended readable outflow exponents.
     * @param a2 Intended readable surface-water interaction coefficients.
     * @param b2 Intended readable interaction exponents.
     * @param a3 Intended readable cross-interaction coefficients.
     * @param sw_head Intended readable count-row receiving-water heads feet.
     * @param[out] gw_flow Intended writable count-row lateral flux output.
     * @param count Intended nonnegative rows; restrict-qualified arrays are disjoint.
     * No callable behavior/unit-conversion implementation is supplied; execute() uses
     * scalar getFluxes() and caller-compiled expression data instead. */
    static void batchGWFlow(const double* OPENSWMM_RESTRICT lower_depth,
                            const double* OPENSWMM_RESTRICT h_star,
                            const double* OPENSWMM_RESTRICT a1, const double* OPENSWMM_RESTRICT b1,
                            const double* OPENSWMM_RESTRICT a2, const double* OPENSWMM_RESTRICT b2,
                            const double* OPENSWMM_RESTRICT a3,
                            const double* OPENSWMM_RESTRICT sw_head,
                            double* OPENSWMM_RESTRICT gw_flow, int count);
};

} // namespace groundwater
} // namespace openswmm

#endif // OPENSWMM_GROUNDWATER_HPP
