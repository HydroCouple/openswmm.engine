/**
 * @file QualityHandler.cpp
 * @brief Section handlers for [POLLUTANTS], [LANDUSES], [COVERAGES],
 *        [BUILDUP], [WASHOFF], [TREATMENT], and [LOADINGS].
 *
 * ### [POLLUTANTS] format
 * ```
 * ;; Name  Units  Crain  Cgw  Crdii  Kdecay  SnowOnly  CoPollut  CoFrac  Cdwf  Cinit
 * TSS       MG/L   0.0    0.0  0.0    0.0     NO        *         0.0     0.0   0.0
 * Lead      UG/L   0.0    0.0  0.0    0.0     NO        TSS       0.2     0.0   0.0
 * ```
 *
 * ### [LANDUSES] format
 * ```
 * ;; Name  SweepInterval  SweepRemoval  SweepDays0
 * Residential  0  0  0
 * ```
 *
 * ### [COVERAGES] format
 * ```
 * ;; Subcatch  LandUse  Percent
 * S1           Residential  60.0
 * ```
 *
 * ### [BUILDUP] format
 * ```
 * ;; Landuse  Pollutant  Type  C1  C2  C3  PerUnit
 * Residential  TSS  POW  100  0.5  1  AREA
 * ```
 *
 * ### [WASHOFF] format
 * ```
 * ;; Landuse  Pollutant  Type  C1  C2  SweepEffic  BMPEffic
 * Residential  TSS  EMC  50  0  0  0
 * ```
 *
 * ### [TREATMENT] format
 * ```
 * ;; Node  Pollutant  Result = Expression
 * J1       TSS        R = 0.5 * exp(-0.1 * DT)
 * ```
 *
 * ### [LOADINGS] format
 * ```
 * ;; Subcatch  Pollutant  InitBuildup
 * S1           TSS        100.0
 * ```
 *
 * @see Legacy reference: src/solver/input.c — readPollutant(), readLanduse(), etc.
 * @ingroup engine_input
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#include "QualityHandler.hpp"

#include "../Tokenizer.hpp"
#include "../SectionParser.hpp"
#include "../../core/SimulationContext.hpp"
#include "../../data/PollutantData.hpp"
#include "../../data/QualityData.hpp"

#include "../InputParseUtils.hpp"

#include <charconv>
#include <string>

namespace openswmm::input {

// ============================================================================
// handle_pollutants()
// ============================================================================

void handle_pollutants(SimulationContext& ctx, const std::vector<std::string>& lines) {
    const auto parsed = parse_section(lines);

    // First pass: register all pollutant names so co-pollutant lookup works
    for (const auto& pl : parsed) {
        auto tok = Tokenizer::tokenize(pl.data);
        if (tok.size() < 2) continue;

        const std::string& name = tok[0];
        if (ctx.pollutant_names.find(name) < 0) {
            ctx.pollutant_names.add(name);
        }
    }

    // Resize pollutant arrays to accommodate all pollutants
    const int n = ctx.pollutant_names.size();
    ctx.pollutants.resize_pollutants(n);

    // Second pass: populate pollutant properties and comments
    for (const auto& pl : parsed) {
        auto tok = Tokenizer::tokenize(pl.data);
        if (tok.size() < 2) continue;
        // Name  Units  Crain  Cgw  Crdii  Kdecay  SnowOnly  CoPollut  CoFrac  Cdwf  Cinit

        const int idx = ctx.pollutant_names.find(tok[0]);
        if (idx < 0) continue;

        // Units
        const std::string units_str = Tokenizer::to_upper(tok[1]);
        if (units_str == "UG/L")
            ctx.pollutants.units[idx] = MassUnits::UG_PER_L;
        else if (units_str == "#/L")
            ctx.pollutants.units[idx] = MassUnits::COUNTS_PER_L;
        else
            ctx.pollutants.units[idx] = MassUnits::MG_PER_L;

        if (tok.size() > 2) ctx.pollutants.c_rain[idx]  = to_double(tok[2]);
        if (tok.size() > 3) ctx.pollutants.c_gw[idx]    = to_double(tok[3]);
        if (tok.size() > 4) ctx.pollutants.c_rdii[idx]  = to_double(tok[4]);
        if (tok.size() > 5) ctx.pollutants.k_decay[idx] = to_double(tok[5]);

        // SnowOnly
        if (tok.size() > 6) {
            const std::string snow = Tokenizer::to_upper(tok[6]);
            ctx.pollutants.snow_only[idx] = (snow == "YES" || snow == "1");
        }

        // CoPollut
        if (tok.size() > 7) {
            if (tok[7] == "*" || tok[7] == "-") {
                ctx.pollutants.co_pollut[idx] = -1;
            } else {
                ctx.pollutants.co_pollut[idx] = ctx.pollutant_names.find(tok[7]);
            }
        }

        // CoFrac
        if (tok.size() > 8)  ctx.pollutants.co_frac[idx]   = to_double(tok[8]);

        // Cdwf (skip tok[9] — not stored in PollutantData)

        // Cinit
        if (tok.size() > 10) ctx.pollutants.init_conc[idx]  = to_double(tok[10]);

        if (!pl.comment.empty())
            ctx.pollutants.comments[static_cast<std::size_t>(idx)] = pl.comment;
    }
}

// ============================================================================
// handle_landuses()
// ============================================================================

void handle_landuses(SimulationContext& ctx, const std::vector<std::string>& lines) {
    for (const auto& pl : parse_section(lines)) {
        auto tok = Tokenizer::tokenize(pl.data);
        if (tok.empty()) continue;
        // Name  [SweepInterval]  [SweepRemoval]  [SweepDays0]

        const std::string& name = tok[0];
        int idx = ctx.landuse_names.find(name);
        if (idx < 0) idx = ctx.landuse_names.add(name);

        // Ensure capacity
        const auto n = static_cast<std::size_t>(idx + 1);
        if (ctx.landuses.sweep_interval.size() < n) {
            ctx.landuses.sweep_interval.resize(n, 0.0);
            ctx.landuses.sweep_removal.resize(n, 0.0);
            ctx.landuses.last_swept.resize(n, 0.0);
            ctx.landuses.comments.resize(n, std::string{});
        }

        if (tok.size() > 1) ctx.landuses.sweep_interval[idx] = to_double(tok[1]);
        if (tok.size() > 2) ctx.landuses.sweep_removal[idx]  = to_double(tok[2]);
        if (tok.size() > 3) ctx.landuses.last_swept[idx]     = to_double(tok[3]);
        if (!pl.comment.empty())
            ctx.landuses.comments[static_cast<std::size_t>(idx)] = pl.comment;
    }
}

// ============================================================================
// handle_coverages()
// ============================================================================

void handle_coverages(SimulationContext& ctx, const std::vector<std::string>& lines) {
    const int n_landuses = ctx.landuse_names.size();
    if (n_landuses <= 0) return;

    const int n_subcatches = ctx.subcatch_names.size();
    if (n_subcatches <= 0) return;

    // Allocate flat coverage array if needed
    const auto total = static_cast<std::size_t>(n_subcatches * n_landuses);
    if (ctx.subcatches.coverage.size() < total) {
        ctx.subcatches.coverage.assign(total, 0.0);
    }
    ctx.subcatches.coverage_n_landuses = n_landuses;

    for (const auto& line : lines) {
        auto tok = Tokenizer::tokenize(line);
        if (tok.size() < 3) continue;
        // Subcatch  LandUse  Percent

        const int sub_idx = ctx.subcatch_names.find(tok[0]);
        if (sub_idx < 0) continue;

        const int lu_idx = ctx.landuse_names.find(tok[1]);
        if (lu_idx < 0) continue;

        ctx.subcatches.coverage[sub_idx * n_landuses + lu_idx] = to_double(tok[2]);
    }
}

// ============================================================================
// handle_buildup()
// ============================================================================

void handle_buildup(SimulationContext& ctx, const std::vector<std::string>& lines) {
    const int n_landuses   = ctx.landuse_names.size();
    const int n_pollutants = ctx.pollutant_names.size();
    if (n_landuses <= 0 || n_pollutants <= 0) return;

    // Ensure buildup arrays are sized
    if (ctx.buildup.n_landuses != n_landuses || ctx.buildup.n_pollutants != n_pollutants) {
        ctx.buildup.resize(n_landuses, n_pollutants);
    }

    for (const auto& line : lines) {
        auto tok = Tokenizer::tokenize(line);
        if (tok.size() < 5) continue;
        // Landuse  Pollutant  Type  C1  C2  C3  PerUnit

        const int lu_idx = ctx.landuse_names.find(tok[0]);
        if (lu_idx < 0) continue;

        const int p_idx = ctx.pollutant_names.find(tok[1]);
        if (p_idx < 0) continue;

        const auto flat = static_cast<std::size_t>(lu_idx * n_pollutants + p_idx);

        // Type: NONE=0, POW=1, EXP=2, SAT=3, EXT=4
        const std::string type_str = Tokenizer::to_upper(tok[2]);
        if      (type_str == "POW") ctx.buildup.func_type[flat] = 1;
        else if (type_str == "EXP") ctx.buildup.func_type[flat] = 2;
        else if (type_str == "SAT") ctx.buildup.func_type[flat] = 3;
        else if (type_str == "EXT") ctx.buildup.func_type[flat] = 4;
        else                        ctx.buildup.func_type[flat] = 0;

        ctx.buildup.coeff1[flat] = to_double(tok[3]);
        ctx.buildup.coeff2[flat] = to_double(tok[4]);

        if (tok.size() > 5) {
            // Gap #35: EXT buildup uses a time series name in tok[5], not a number.
            // Resolve to table index; other types store a numeric exponent.
            if (ctx.buildup.func_type[flat] == 4) {  // EXTERNAL
                int ts_idx = ctx.table_names.find(tok[5]);
                ctx.buildup.coeff3[flat] = static_cast<double>(ts_idx);
            } else {
                ctx.buildup.coeff3[flat] = to_double(tok[5]);
            }
        }

        // Normalizer: AREA=0, CURB=1
        if (tok.size() > 6) {
            const std::string norm = Tokenizer::to_upper(tok[6]);
            ctx.buildup.normalizer[flat] = (norm == "CURB") ? 1 : 0;
        }
    }
}

// ============================================================================
// handle_washoff()
// ============================================================================

void handle_washoff(SimulationContext& ctx, const std::vector<std::string>& lines) {
    const int n_landuses   = ctx.landuse_names.size();
    const int n_pollutants = ctx.pollutant_names.size();
    if (n_landuses <= 0 || n_pollutants <= 0) return;

    // Ensure washoff arrays are sized
    if (ctx.washoff.n_landuses != n_landuses || ctx.washoff.n_pollutants != n_pollutants) {
        ctx.washoff.resize(n_landuses, n_pollutants);
    }

    for (const auto& line : lines) {
        auto tok = Tokenizer::tokenize(line);
        if (tok.size() < 5) continue;
        // Landuse  Pollutant  Type  C1  C2  [SweepEffic]  [BMPEffic]

        const int lu_idx = ctx.landuse_names.find(tok[0]);
        if (lu_idx < 0) continue;

        const int p_idx = ctx.pollutant_names.find(tok[1]);
        if (p_idx < 0) continue;

        const auto flat = static_cast<std::size_t>(lu_idx * n_pollutants + p_idx);

        // Type: NONE=0, EXP=1, RC=2, EMC=3
        const std::string type_str = Tokenizer::to_upper(tok[2]);
        if      (type_str == "EXP") ctx.washoff.func_type[flat] = 1;
        else if (type_str == "RC")  ctx.washoff.func_type[flat] = 2;
        else if (type_str == "EMC") ctx.washoff.func_type[flat] = 3;
        else                        ctx.washoff.func_type[flat] = 0;

        ctx.washoff.coeff[flat] = to_double(tok[3]);
        ctx.washoff.expon[flat] = to_double(tok[4]);

        if (tok.size() > 5) ctx.washoff.sweep_effic[flat] = to_double(tok[5]);
        if (tok.size() > 6) ctx.washoff.bmp_effic[flat]   = to_double(tok[6]);
    }
}

// ============================================================================
// handle_treatment()
// ============================================================================

void handle_treatment(SimulationContext& ctx, const std::vector<std::string>& lines) {
    const int n_pollutants = ctx.pollutant_names.size();
    const int n_nodes      = ctx.node_names.size();
    if (n_pollutants <= 0 || n_nodes <= 0) return;

    // Ensure treatment arrays are sized
    if (ctx.treatment.n_nodes != n_nodes || ctx.treatment.n_pollutants != n_pollutants) {
        ctx.treatment.resize(n_nodes, n_pollutants);
    }

    for (const auto& line : lines) {
        auto tok = Tokenizer::tokenize(line);
        if (tok.size() < 3) continue;
        // Node  Pollutant  Result = Expression

        const int node_idx = ctx.node_names.find(tok[0]);
        if (node_idx < 0) continue;

        const int p_idx = ctx.pollutant_names.find(tok[1]);
        if (p_idx < 0) continue;

        const auto flat = static_cast<std::size_t>(node_idx * n_pollutants + p_idx);

        // Reconstruct the expression from remaining tokens (e.g. "R = 0.5 * ...")
        std::string expr;
        for (std::size_t i = 2; i < tok.size(); ++i) {
            if (i > 2) expr += ' ';
            expr += tok[i];
        }
        ctx.treatment.expressions[flat] = expr;
    }
}

// ============================================================================
// handle_loadings()
// ============================================================================

void handle_loadings(SimulationContext& ctx, const std::vector<std::string>& lines) {
    const int n_pollutants = ctx.pollutant_names.size();
    if (n_pollutants <= 0) return;

    for (const auto& line : lines) {
        auto tok = Tokenizer::tokenize(line);
        if (tok.size() < 3) continue;
        // Subcatch  Pollutant  InitBuildup

        const int sub_idx = ctx.subcatch_names.find(tok[0]);
        if (sub_idx < 0) continue;

        const int p_idx = ctx.pollutant_names.find(tok[1]);
        if (p_idx < 0) continue;

        const auto flat = static_cast<std::size_t>(sub_idx * n_pollutants + p_idx);
        if (flat < ctx.subcatches.conc.size()) {
            ctx.subcatches.conc[flat] = to_double(tok[2]);
        }
    }
}

} /* namespace openswmm::input */
