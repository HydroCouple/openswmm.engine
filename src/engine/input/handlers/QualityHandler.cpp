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
 * @file      QualityHandler.cpp
 * @brief     Section handlers for [POLLUTANTS], [LANDUSES], [COVERAGES],
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
 * ;; Subcatch  LandUse  Percent  [LandUse  Percent ...]
 * S1           Residential  60.0  Commercial  40.0
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
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include "QualityHandler.hpp"

#include "../Tokenizer.hpp"
#include "../SectionParser.hpp"
#include "../../core/SimulationContext.hpp"
#include "../../data/PollutantData.hpp"
#include "../../data/QualityData.hpp"

#include "../InputParseUtils.hpp"
#include "../../core/Constants.hpp"

#include <charconv>
#include <initializer_list>
#include <string>

namespace openswmm::input {

namespace {

/**
 * @brief Find the first nonempty exact-case catalogue token that prefixes input.
 *
 * Accepts trailing characters after a matched keyword; empty catalogue words never match.
 *
 * @param[in] token Borrowed tokenizer-normalized input; no local case folding or trimming.
 * @param[in] words Borrowed list of non-NULL NUL-terminated prefixes; list length fits int.
 * @return First zero-based prefix match or -1.
 */
int findmatch_prefix(const std::string& token, std::initializer_list<const char*> words) {
    int i = 0;
    for (const char* w : words) {
        // The tokenizer has already stripped the leading blanks legacy's
        // match() skips, so this is a plain prefix test.
        const std::size_t n = std::char_traits<char>::length(w);
        if (n > 0 && token.size() >= n && token.compare(0, n, w) == 0)
            return i;
        ++i;
    }
    return -1;
}

} // namespace

// ============================================================================
// handle_pollutants()
// ============================================================================

/**
 * @brief Register pollutant names and populate definition columns in two passes.
 *
 * Name pass permits forward co-pollutant references and records duplicates. Then resize_pollutants
 * resets every existing definition to defaults before parsing all rows; duplicate rows still
 * repopulate the original index in the second pass. Unknown unit text defaults mg/L; decay converts
 * 1/day to 1/second. Stores concentration, snow flag, co-pollutant/fraction and comments without
 * complete finite/range validation; no network concentration matrices are sized.
 *
 * @param[in,out] ctx Context receiving authored quality stores and errors; caller serializes
 *     input-phase mutations and provides coherent name/dimension counts.
 * @param[in] lines Borrowed section rows, tokenized/copied synchronously. Numeric quantities retain
 *     their documented authored units except explicit conversions below.
 */
void handle_pollutants(SimulationContext& ctx, const std::vector<std::string>& lines) {
    const auto parsed = parse_section(lines);

    // First pass: register all pollutant names so co-pollutant lookup works
    for (const auto& pl : parsed) {
        auto tok = Tokenizer::tokenize(pl.data);
        if (tok.size() < 2)
            continue;

        const std::string& name = tok[0];
        add_unique(ctx.pollutant_names, name, ctx.errors);
        // duplicate ID (any case spelling) pushes ERR 207 and is skipped;
        // the second pass keys by find(), so it repopulates the original.
    }

    // Resize pollutant arrays to accommodate all pollutants
    const int n = ctx.pollutant_names.size();
    ctx.pollutants.resize_pollutants(n);

    // Second pass: populate pollutant properties and comments
    for (const auto& pl : parsed) {
        auto tok = Tokenizer::tokenize(pl.data);
        if (tok.size() < 2)
            continue;
        // Name  Units  Crain  Cgw  Crdii  Kdecay  SnowOnly  CoPollut  CoFrac  Cdwf  Cinit

        const int idx = ctx.pollutant_names.find(tok[0]);
        if (idx < 0)
            continue;

        // Units
        const std::string units_str = Tokenizer::to_upper(tok[1]);
        if (units_str == "UG/L")
            ctx.pollutants.units[idx] = MassUnits::UG_PER_L;
        else if (units_str == "#/L")
            ctx.pollutants.units[idx] = MassUnits::COUNTS_PER_L;
        else
            ctx.pollutants.units[idx] = MassUnits::MG_PER_L;

        if (tok.size() > 2)
            ctx.pollutants.c_rain[idx] = to_double(tok[2]);
        if (tok.size() > 3)
            ctx.pollutants.c_gw[idx] = to_double(tok[3]);
        if (tok.size() > 4)
            ctx.pollutants.c_rdii[idx] = to_double(tok[4]);
        // The Kdecay column is 1/day; store 1/sec, exactly as legacy
        // does (landuse.c divides by SECperDAY at parse) — KD1.
        if (tok.size() > 5)
            ctx.pollutants.k_decay[idx] = to_double(tok[5]) / constants::SEC_PER_DAY;

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
        if (tok.size() > 8)
            ctx.pollutants.co_frac[idx] = to_double(tok[8]);

        // Cdwf — default dry weather sanitary flow concentration
        if (tok.size() > 9)
            ctx.pollutants.c_dwf[idx] = to_double(tok[9]);

        // Native extension after the legacy Cinit column.
        if (tok.size() > 11)
            ctx.pollutants.mwt[idx] = to_double(tok[11]);

        // Cinit
        if (tok.size() > 10)
            ctx.pollutants.init_conc[idx] = to_double(tok[10]);

        if (!pl.comment.empty())
            ctx.pollutants.comments[static_cast<std::size_t>(idx)] = pl.comment;
    }
}

// ============================================================================
// handle_landuses()
// ============================================================================

/**
 * @brief Append named land uses with optional sweep parameters/comments.
 *
 * Duplicate names report ERR_DUP_NAME and skip; existing other rows persist. Defaults zero
 * interval/removal/days-since-sweep, stores supplied values permissively in authored days/fraction
 * conventions. Does not allocate buildup/washoff/coverage matrices, validate sweep ranges or
 * execute sweeping.
 *
 * @param[in,out] ctx Context receiving authored quality stores and errors; caller serializes
 *     input-phase mutations and provides coherent name/dimension counts.
 * @param[in] lines Borrowed section rows, tokenized/copied synchronously. Numeric quantities retain
 *     their documented authored units except explicit conversions below.
 */
void handle_landuses(SimulationContext& ctx, const std::vector<std::string>& lines) {
    for (const auto& pl : parse_section(lines)) {
        auto tok = Tokenizer::tokenize(pl.data);
        if (tok.empty())
            continue;
        // Name  [SweepInterval]  [SweepRemoval]  [SweepDays0]

        const std::string& name = tok[0];
        int idx = add_unique(ctx.landuse_names, name, ctx.errors);
        if (idx < 0)
            continue; // duplicate ID (ERR 207, legacy input.c parity)

        // Ensure capacity
        const auto n = static_cast<std::size_t>(idx + 1);
        if (ctx.landuses.sweep_interval.size() < n) {
            ctx.landuses.sweep_interval.resize(n, 0.0);
            ctx.landuses.sweep_removal.resize(n, 0.0);
            ctx.landuses.last_swept.resize(n, 0.0);
            ctx.landuses.comments.resize(n, std::string{});
        }

        if (tok.size() > 1)
            ctx.landuses.sweep_interval[idx] = to_double(tok[1]);
        if (tok.size() > 2)
            ctx.landuses.sweep_removal[idx] = to_double(tok[2]);
        if (tok.size() > 3)
            ctx.landuses.last_swept[idx] = to_double(tok[3]);
        if (!pl.comment.empty())
            ctx.landuses.comments[static_cast<std::size_t>(idx)] = pl.comment;
    }
}

// ============================================================================
// handle_coverages()
// ============================================================================

/**
 * @brief Assign land-use percentages for each named subcatchment.
 *
 * No-op unless both name registries are nonempty; undersized coverage/sweep arrays are replaced
 * with zeros and coverage stride updated. Each row accepts multiple LANDUSE PERCENT pairs, last
 * duplicate wins. Missing items/names/bad complete numeric tokens append errors and stop the
 * remaining pairs after any earlier assignments. Values remain percentages, without [0,100] or
 * sum-to100 validation; caller counts/products must fit signed arithmetic.
 *
 * @param[in,out] ctx Context receiving authored quality stores and errors; caller serializes
 *     input-phase mutations and provides coherent name/dimension counts.
 * @param[in] lines Borrowed section rows, tokenized/copied synchronously. Numeric quantities retain
 *     their documented authored units except explicit conversions below.
 */
void handle_coverages(SimulationContext& ctx, const std::vector<std::string>& lines) {
    const int n_landuses = ctx.landuse_names.size();
    if (n_landuses <= 0)
        return;

    const int n_subcatches = ctx.subcatch_names.size();
    if (n_subcatches <= 0)
        return;

    // Allocate flat coverage array if needed
    const auto total = static_cast<std::size_t>(n_subcatches * n_landuses);
    if (ctx.subcatches.coverage.size() < total) {
        ctx.subcatches.coverage.assign(total, 0.0);
    }
    // BW-MSX round (2026-09-19), pre-existing defect: the per-(subcatchment,
    // land use) last-swept counters were sized only by the C API
    // (resize_coverage) and the GeoPackage reader — never by this handler —
    // so street sweeping silently never fired on an .inp deck (the A7 loop
    // skips any index past the empty array). Size them with the coverage
    // matrix, as resize_coverage does.
    if (ctx.subcatches.sweep_last_swept.size() < total) {
        ctx.subcatches.sweep_last_swept.assign(total, 0.0);
    }
    ctx.subcatches.coverage_n_landuses = n_landuses;

    for (const auto& line : lines) {
        auto tok = Tokenizer::tokenize(line);
        if (tok.empty())
            continue;
        // Subcatch  LandUse  Percent  [LandUse  Percent ...]
        // Legacy subcatch_readLanduseParams (subcatch.c): every pair is read,
        // the last value for a repeated land use wins, and an unknown name,
        // a land use without a percent or a bad number is an error.
        if (tok.size() < 3) {
            ctx.errors.push_back(format_error(ERR_ITEMS, ""));
            continue;
        }

        const int sub_idx = ctx.subcatch_names.find(tok[0]);
        if (sub_idx < 0) {
            ctx.errors.push_back(format_error(ERR_NAME, tok[0]));
            continue;
        }

        for (std::size_t k = 1; k < tok.size(); k += 2) {
            const int lu_idx = ctx.landuse_names.find(tok[k]);
            if (lu_idx < 0) {
                ctx.errors.push_back(format_error(ERR_NAME, tok[k]));
                break;
            }
            if (k + 1 >= tok.size()) {
                ctx.errors.push_back(format_error(ERR_ITEMS, ""));
                break;
            }
            double pct = 0.0;
            if (!parse_double_strict(tok[k + 1], pct)) {
                ctx.errors.push_back(format_error(ERR_NUMBER, tok[k + 1]));
                break;
            }
            ctx.subcatches.coverage[sub_idx * n_landuses + lu_idx] = pct;
        }
    }
}

// ============================================================================
// handle_buildup()
// ============================================================================

/**
 * @brief Assign pollutant buildup functions or defer nonpollutant species rows.
 *
 * Requires existing land uses; resizes pollutant parameter matrices only when dimensions differ,
 * discarding them in that case. Known pollutant rows overwrite supplied parameters, using legacy
 * keyword prefixes and unknown=>NONE; EXT parameter3 looks up a timeseries immediately.
 * Nonpollutant names append deferred MSX rows with retained series name. Coefficients retain
 * function-specific authored mass/area/curb/time units; no full range, reference or species
 * validation.
 *
 * @param[in,out] ctx Context receiving authored quality stores and errors; caller serializes
 *     input-phase mutations and provides coherent name/dimension counts.
 * @param[in] lines Borrowed section rows, tokenized/copied synchronously. Numeric quantities retain
 *     their documented authored units except explicit conversions below.
 */
void handle_buildup(SimulationContext& ctx, const std::vector<std::string>& lines) {
    const int n_landuses = ctx.landuse_names.size();
    const int n_pollutants = ctx.pollutant_names.size();
    if (n_landuses <= 0)
        return;

    // Ensure buildup arrays are sized
    if (n_pollutants > 0 &&
        (ctx.buildup.n_landuses != n_landuses || ctx.buildup.n_pollutants != n_pollutants)) {
        ctx.buildup.resize(n_landuses, n_pollutants);
    }

    for (const auto& line : lines) {
        auto tok = Tokenizer::tokenize(line);
        if (tok.size() < 5)
            continue;
        // Landuse  Pollutant  Type  C1  C2  C3  PerUnit

        const int lu_idx = ctx.landuse_names.find(tok[0]);
        if (lu_idx < 0)
            continue;

        const int p_idx = ctx.pollutant_names.find(tok[1]);
        if (p_idx < 0) {
            // BW-MSX: not a pollutant — park the row by name for the
            // reactions component's species, resolved after the component
            // is applied (msxsurf::resolve). Same deferral as [INITIAL_QUALITY].
            MsxSurfaceRows::Buildup r;
            r.landuse = tok[0];
            r.species = tok[1];
            const std::string ts = Tokenizer::to_upper(tok[2]);
            const int mbt = findmatch_prefix(ts, {"NONE", "POW", "EXP", "SAT", "EXT"});
            r.func_type = (mbt >= 0) ? mbt : 0;
            r.c1 = to_double(tok[3]);
            r.c2 = to_double(tok[4]);
            if (tok.size() > 5) {
                if (r.func_type == 4)
                    r.ts_name = tok[5];
                else
                    r.c3 = to_double(tok[5]);
            }
            if (tok.size() > 6)
                r.normalizer = (Tokenizer::to_upper(tok[6]) == "CURB") ? 1 : 0;
            ctx.reactions.surface.rows.buildup.push_back(std::move(r));
            continue;
        }
        if (n_pollutants <= 0)
            continue;

        const auto flat = static_cast<std::size_t>(lu_idx * n_pollutants + p_idx);

        // Type: NONE=0, POW=1, EXP=2, SAT=3, EXT=4 — legacy BuildupTypeWords,
        // matched by PREFIX so the decks' POWER / EXPON / SATUR / EXTERNAL
        // reach their stems (see findmatch_prefix).
        const std::string type_str = Tokenizer::to_upper(tok[2]);
        const int bt = findmatch_prefix(type_str, {"NONE", "POW", "EXP", "SAT", "EXT"});
        ctx.buildup.func_type[flat] = (bt >= 0) ? bt : 0;

        ctx.buildup.coeff1[flat] = to_double(tok[3]);
        ctx.buildup.coeff2[flat] = to_double(tok[4]);

        if (tok.size() > 5) {
            // Gap #35: EXT buildup uses a time series name in tok[5], not a number.
            // Resolve to table index; other types store a numeric exponent.
            if (ctx.buildup.func_type[flat] == 4) { // EXTERNAL
                int ts_idx = ctx.find_timeseries(tok[5]);
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

/**
 * @brief Assign pollutant washoff functions or defer nonpollutant species rows.
 *
 * Requires existing land uses and known land-use ID; short/unknown-land-use rows skip. Resizes
 * dimension-changed pollutant matrices; known pollutants overwrite supplied
 * coefficients/efficiencies, others append MSX authoring rows by name. Types match legacy prefixes
 * (unknown=>NONE); coefficients/exponents and sweep/BMP percentages are stored permissively without
 * finite/range conversion or runtime load computation.
 *
 * @param[in,out] ctx Context receiving authored quality stores and errors; caller serializes
 *     input-phase mutations and provides coherent name/dimension counts.
 * @param[in] lines Borrowed section rows, tokenized/copied synchronously. Numeric quantities retain
 *     their documented authored units except explicit conversions below.
 */
void handle_washoff(SimulationContext& ctx, const std::vector<std::string>& lines) {
    const int n_landuses = ctx.landuse_names.size();
    const int n_pollutants = ctx.pollutant_names.size();
    if (n_landuses <= 0)
        return;

    // Ensure washoff arrays are sized
    if (n_pollutants > 0 &&
        (ctx.washoff.n_landuses != n_landuses || ctx.washoff.n_pollutants != n_pollutants)) {
        ctx.washoff.resize(n_landuses, n_pollutants);
    }

    for (const auto& line : lines) {
        auto tok = Tokenizer::tokenize(line);
        if (tok.size() < 5)
            continue;
        // Landuse  Pollutant  Type  C1  C2  [SweepEffic]  [BMPEffic]

        const int lu_idx = ctx.landuse_names.find(tok[0]);
        if (lu_idx < 0)
            continue;

        const int p_idx = ctx.pollutant_names.find(tok[1]);
        if (p_idx < 0) {
            // BW-MSX: park by name (see handle_buildup).
            MsxSurfaceRows::Washoff r;
            r.landuse = tok[0];
            r.species = tok[1];
            const std::string ts = Tokenizer::to_upper(tok[2]);
            const int mwt = findmatch_prefix(ts, {"NONE", "EXP", "RC", "EMC"});
            r.func_type = (mwt >= 0) ? mwt : 0;
            r.coeff = to_double(tok[3]);
            r.expon = to_double(tok[4]);
            if (tok.size() > 5)
                r.sweep_effic = to_double(tok[5]);
            if (tok.size() > 6)
                r.bmp_effic = to_double(tok[6]);
            ctx.reactions.surface.rows.washoff.push_back(std::move(r));
            continue;
        }
        if (n_pollutants <= 0)
            continue;

        const auto flat = static_cast<std::size_t>(lu_idx * n_pollutants + p_idx);

        // Type: NONE=0, EXP=1, RC=2, EMC=3 — legacy WashoffTypeWords, matched
        // by PREFIX so a deck spelling EXPON reaches EXP (see
        // findmatch_prefix).
        const std::string type_str = Tokenizer::to_upper(tok[2]);
        const int wt = findmatch_prefix(type_str, {"NONE", "EXP", "RC", "EMC"});
        ctx.washoff.func_type[flat] = (wt >= 0) ? wt : 0;

        ctx.washoff.coeff[flat] = to_double(tok[3]);
        ctx.washoff.expon[flat] = to_double(tok[4]);

        if (tok.size() > 5)
            ctx.washoff.sweep_effic[flat] = to_double(tok[5]);
        if (tok.size() > 6)
            ctx.washoff.bmp_effic[flat] = to_double(tok[6]);
    }
}

// ============================================================================
// handle_treatment()
// ============================================================================

/**
 * @brief Store authored treatment expressions by node/pollutant key.
 *
 * No-op without nodes or pollutants. Dimension changes resize/reset treatment authored storage;
 * known-key rows reconstruct tokens from the third onward with single spaces and overwrite their
 * slot. Unknown/short rows skip. Does not compile expressions, set has_treatment, resolve
 * dependency cycles or execute removal.
 *
 * @param[in,out] ctx Context receiving authored quality stores and errors; caller serializes
 *     input-phase mutations and provides coherent name/dimension counts.
 * @param[in] lines Borrowed section rows, tokenized/copied synchronously. Numeric quantities retain
 *     their documented authored units except explicit conversions below.
 */
void handle_treatment(SimulationContext& ctx, const std::vector<std::string>& lines) {
    const int n_pollutants = ctx.pollutant_names.size();
    const int n_nodes = ctx.node_names.size();
    if (n_pollutants <= 0 || n_nodes <= 0)
        return;

    // Ensure treatment arrays are sized
    if (ctx.treatment.n_nodes != n_nodes || ctx.treatment.n_pollutants != n_pollutants) {
        ctx.treatment.resize(n_nodes, n_pollutants);
    }

    for (const auto& line : lines) {
        auto tok = Tokenizer::tokenize(line);
        if (tok.size() < 3)
            continue;
        // Node  Pollutant  Result = Expression

        const int node_idx = ctx.node_names.find(tok[0]);
        if (node_idx < 0)
            continue;

        const int p_idx = ctx.pollutant_names.find(tok[1]);
        if (p_idx < 0)
            continue;

        const auto flat = static_cast<std::size_t>(node_idx * n_pollutants + p_idx);

        // Reconstruct the expression from remaining tokens (e.g. "R = 0.5 * ...")
        std::string expr;
        for (std::size_t i = 2; i < tok.size(); ++i) {
            if (i > 2)
                expr += ' ';
            expr += tok[i];
        }
        ctx.treatment.expressions[flat] = expr;
    }
}

// ============================================================================
// handle_loadings()
// ============================================================================

/**
 * @brief Assign initial pollutant surface buildup and defer MSX loading rows.
 *
 * Nonpollutant names append raw MSX loading rows first, even without pollutant definitions;
 * species/subcatchment validation defers. Known pollutant/subcatchment pairs overwrite init_loading
 * in authored mass per unit area, not runoff concentration. Can resize/zero quality matrices if
 * pollutant shape differs. Short/unknown rows otherwise skip and numeric parsing is permissive; no
 * runtime buildup seeding or unit conversion.
 *
 * @param[in,out] ctx Context receiving authored quality stores and errors; caller serializes
 *     input-phase mutations and provides coherent name/dimension counts.
 * @param[in] lines Borrowed section rows, tokenized/copied synchronously. Numeric quantities retain
 *     their documented authored units except explicit conversions below.
 */
void handle_loadings(SimulationContext& ctx, const std::vector<std::string>& lines) {
    const int n_pollutants = ctx.pollutant_names.size();
    // BW-MSX: rows naming a non-pollutant are parked by name for the
    // reactions component's species (msxsurf::resolve); a deck with no
    // [POLLUTANTS] at all may still carry MSX loadings.
    for (const auto& line : lines) {
        auto tok = Tokenizer::tokenize(line);
        if (tok.size() < 3)
            continue;
        if (ctx.pollutant_names.find(tok[1]) >= 0)
            continue;
        MsxSurfaceRows::Loading r;
        r.subcatch = tok[0];
        r.species = tok[1];
        r.value = to_double(tok[2]);
        ctx.reactions.surface.rows.loadings.push_back(std::move(r));
    }
    if (n_pollutants <= 0)
        return;

    // Size the quality arrays now (iteration 4): they used to be sized only
    // by PostParseResolver AFTER the handlers ran, so the `flat < size()`
    // guard below silently dropped every [LOADINGS] row.
    const int n_sub = ctx.subcatch_names.size();
    if (n_sub > 0 && (ctx.subcatches.conc_n_pollutants != n_pollutants ||
                      static_cast<int>(ctx.subcatches.conc.size()) != n_sub * n_pollutants)) {
        ctx.subcatches.resize_quality(n_pollutants);
    }

    for (const auto& line : lines) {
        auto tok = Tokenizer::tokenize(line);
        if (tok.size() < 3)
            continue;
        // Subcatch  Pollutant  InitBuildup

        const int sub_idx = ctx.subcatch_names.find(tok[0]);
        if (sub_idx < 0)
            continue;

        const int p_idx = ctx.pollutant_names.find(tok[1]);
        if (p_idx < 0)
            continue;

        const auto flat = static_cast<std::size_t>(sub_idx * n_pollutants + p_idx);
        // The row is an initial surface BUILDUP per unit area, not a runoff
        // concentration — see SubcatchData::init_loading.
        if (flat < ctx.subcatches.init_loading.size()) {
            ctx.subcatches.init_loading[flat] = to_double(tok[2]);
        }
    }
}

// ============================================================================
// handle_initial_quality()
// ============================================================================
// [INITIAL_QUALITY] format:
//   ;;Scope   Element   Constituent      Value
//   NODE      J1        TSS              12.5
//   NODE      ST1       __WATER_AGE__    6.0        ; hours
//   LINK      C3        __TEMPERATURE__  18.5       ; degC
//
// Per-element initial values overriding the global [POLLUTANTS] Cinit /
// sidecar INITIAL_STATE seeds. Element names and constituents may be defined
// in later sections, so both resolution and constituent classification are
// deferred to PostParseResolver (legacy two-pass parsing is order-
// independent); only row shape is validated here, loudly.
// ============================================================================

/**
 * @brief Append per-node/link initial-quality rows or record a CSV sidecar reference.
 *
 * FILE stores its path token (last wins) for later loading without IO. Inline scope must be
 * NODE/LINK with >=4 tokens and complete from_chars_double value; malformed rows append errors.
 * Element/constituent resolution defers and raw value remains concentration units, age hours or
 * temperature degC. Does not reject negative/nonfinite values solely via a separate policy check,
 * apply seeds, clear prior rows or set error_code.
 *
 * @param[in,out] ctx Context receiving authored quality stores and errors; caller serializes
 *     input-phase mutations and provides coherent name/dimension counts.
 * @param[in] lines Borrowed section rows, tokenized/copied synchronously. Numeric quantities retain
 *     their documented authored units except explicit conversions below.
 */
void handle_initial_quality(SimulationContext& ctx, const std::vector<std::string>& lines) {
    for (const auto& line : lines) {
        auto tok = Tokenizer::tokenize(line);
        if (tok.empty())
            continue;
        // U2: `FILE <path>` names a CSV sidecar (scope,element,constituent,
        // value per line; header optional) read at open once the .inp
        // directory is known (SWMMEngine::open → load_initial_quality_file).
        if (Tokenizer::to_upper(tok[0]) == "FILE") {
            if (tok.size() < 2) {
                ctx.errors.push_back("[INITIAL_QUALITY] FILE needs a path.");
                continue;
            }
            ctx.initial_quality.file = tok[1];
            continue;
        }
        if (tok.size() < 4) {
            ctx.errors.push_back("[INITIAL_QUALITY] row needs NODE|LINK element constituent "
                                 "value: '" +
                                 line + "'.");
            continue;
        }

        const std::string scope = Tokenizer::to_upper(tok[0]);
        bool link = false;
        if (scope == "LINK") {
            link = true;
        } else if (scope != "NODE") {
            ctx.errors.push_back("[INITIAL_QUALITY] scope must be NODE or LINK: '" + tok[0] + "'.");
            continue;
        }

        // Value must be fully numeric — a lenient default here would turn a
        // typo into a silent 0.0 initial condition.
        const std::string& vs = tok[3];
        double v = 0.0;
        auto [p, ec] = openswmm::from_chars_double(vs.data(), vs.data() + vs.size(), v);
        if (ec != std::errc{} || p != vs.data() + vs.size()) {
            ctx.errors.push_back("[INITIAL_QUALITY] bad value '" + vs + "' for '" + tok[2] +
                                 "' at " + scope + " '" + tok[1] + "'.");
            continue;
        }

        ctx.initial_quality.add(link, tok[1], tok[2], v);
    }
}

} /* namespace openswmm::input */
