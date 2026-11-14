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
 * @file      QualityData.hpp
 * @brief     SoA stores for land uses, buildup, washoff, and treatment.
 *
 * @details These stores persist across read/write for .inp round-trip fidelity.
 *          They are separate from the computational Landuse/Treatment modules
 *          which hold runtime state.
 *
 * @ingroup engine_data
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_QUALITY_DATA_HPP
#define OPENSWMM_ENGINE_QUALITY_DATA_HPP

#include <vector>
#include <string>
#include "../quality/Treatment.hpp"

namespace openswmm {

// ============================================================================
// Land use definitions
// ============================================================================

struct LanduseData {
    /**
     * @brief Count land-use rows from the sweep interval column.
     * @return sweep_interval.size() converted to int; no parallel-column alignment check.
     */
    int count() const { return static_cast<int>(sweep_interval.size()); }

    std::vector<double> sweep_interval; ///< Days between sweeps
    std::vector<double> sweep_removal;  ///< Max removal fraction (0-100)
    std::vector<double> last_swept;     ///< Days since last swept

    /**
     * @brief Object comment from the INP file (';'-prefixed lines immediately
     *        above this landuse's data row), joined by literal "\\n".
     *        Empty string means no comment.
     */
    std::vector<std::string> comments;

    /**
     * @brief Replace all land-use parameter/comment rows with defaults.
     *
     * Sets interval/removal/last-swept values to zero and comments empty; caller maps land-use names
     * separately. Independent assignments can fail partway.
     *
     * @param[in] n Nonnegative land-use count; no guard before conversion to size_t.
     */
    void resize(int n) {
        auto un = static_cast<std::size_t>(n);
        sweep_interval.assign(un, 0.0);
        sweep_removal.assign(un, 0.0);
        last_swept.assign(un, 0.0);
        comments.assign(un, std::string{});
    }

    /**
     * @brief Request capacity reduction for the owned parameter columns.
     *
     * Preserves sizes/values. Standard vector requests need not reduce capacity and may allocate/throw;
     * any reallocation invalidates borrowed elements.
     */
    void shrink_to_fit() {
        sweep_interval.shrink_to_fit();
        sweep_removal.shrink_to_fit();
        last_swept.shrink_to_fit();
        comments.shrink_to_fit();
    }
};

// ============================================================================
// Buildup function per (landuse x pollutant)
// ============================================================================

struct BuildupData {
    /// Index: [landuse * n_pollutants + pollutant]
    std::vector<int> func_type; ///< 0=NONE,1=POW,2=EXP,3=SAT,4=EXT
    std::vector<double> coeff1;
    std::vector<double> coeff2;
    std::vector<double> coeff3;
    std::vector<int> normalizer; ///< 0=PER_AREA, 1=PER_CURB

    int n_landuses = 0;
    int n_pollutants = 0;

    /**
     * @brief Replace the land-use-major pollutant parameter matrix.
     *
     * Stores counts then sets function NONE, coefficients zero and normalizer PER_AREA. Discards
     * existing parameters; no negative/overflow checks or rollback on allocation failure.
     *
     * @param[in] nlu Nonnegative land-use count.
     * @param[in] npoll Nonnegative pollutants per land use; size_t product must fit.
     */
    void resize(int nlu, int npoll) {
        n_landuses = nlu;
        n_pollutants = npoll;
        auto total = static_cast<std::size_t>(nlu) * static_cast<std::size_t>(npoll);
        func_type.assign(total, 0);
        coeff1.assign(total, 0.0);
        coeff2.assign(total, 0.0);
        coeff3.assign(total, 0.0);
        normalizer.assign(total, 0);
    }

    /**
     * @brief Request capacity reduction for the owned parameter columns.
     *
     * Preserves sizes/values. Standard vector requests need not reduce capacity and may allocate/throw;
     * any reallocation invalidates borrowed elements.
     */
    void shrink_to_fit() {
        func_type.shrink_to_fit();
        coeff1.shrink_to_fit();
        coeff2.shrink_to_fit();
        coeff3.shrink_to_fit();
        normalizer.shrink_to_fit();
    }
};

// ============================================================================
// Washoff function per (landuse x pollutant)
// ============================================================================

struct WashoffData {
    /// Index: [landuse * n_pollutants + pollutant]
    std::vector<int> func_type; ///< 0=NONE,1=EXP,2=RC,3=EMC
    std::vector<double> coeff;
    std::vector<double> expon;
    std::vector<double> sweep_effic; ///< 0-100
    std::vector<double> bmp_effic;   ///< 0-100

    int n_landuses = 0;
    int n_pollutants = 0;

    /**
     * @brief Replace the land-use-major pollutant parameter matrix.
     *
     * Stores counts then sets function NONE and all coefficients/exponents/efficiencies zero. Discards
     * existing parameters; no negative/overflow checks or rollback on allocation failure.
     *
     * @param[in] nlu Nonnegative land-use count.
     * @param[in] npoll Nonnegative pollutants per land use; size_t product must fit.
     */
    void resize(int nlu, int npoll) {
        n_landuses = nlu;
        n_pollutants = npoll;
        auto total = static_cast<std::size_t>(nlu) * static_cast<std::size_t>(npoll);
        func_type.assign(total, 0);
        coeff.assign(total, 0.0);
        expon.assign(total, 0.0);
        sweep_effic.assign(total, 0.0);
        bmp_effic.assign(total, 0.0);
    }

    /**
     * @brief Request capacity reduction for the owned parameter columns.
     *
     * Preserves sizes/values. Standard vector requests need not reduce capacity and may allocate/throw;
     * any reallocation invalidates borrowed elements.
     */
    void shrink_to_fit() {
        func_type.shrink_to_fit();
        coeff.shrink_to_fit();
        expon.shrink_to_fit();
        sweep_effic.shrink_to_fit();
        bmp_effic.shrink_to_fit();
    }
};

// ============================================================================
// Treatment expression per (node x pollutant)
// ============================================================================

struct TreatmentData {
    /// Index: [node * n_pollutants + pollutant]
    std::vector<std::string> expressions; ///< e.g. "R = 0.5 * exp(-0.1 * DT)"

    int n_nodes = 0;
    int n_pollutants = 0;

    /// Compiled treatment expressions (same indexing as expressions[])
    std::vector<openswmm::treatment::TreatExpr> compiled;

    /// Per-node flag: true if any pollutant has a treatment expression
    std::vector<bool> has_treatment;

    /// Per-node inflow concentrations (size = n_pollutants, reused each timestep)
    std::vector<double> cin;

    /// Per-node removal fractions (size = n_pollutants, reused each timestep)
    /// -1.0 = not computed, 0-1 = computed, > 1.0 = currently being evaluated (cycle detect)
    std::vector<double> removal;

    /**
     * @brief Resize node-major treatment storage and reset authored expressions/workspaces.
     *
     * Empties every authored expression, resets has_treatment=false, inflow workspace zero and
     * removal=-1. Compiled expressions use resize, retaining any existing prefix rather than clearing
     * it; caller recompiles or otherwise synchronizes them. Counts change before allocations; no
     * rollback on failure.
     *
     * @param[in] nn Nonnegative node count.
     * @param[in] npoll Nonnegative pollutants per node; nn*npoll must fit signed int before conversion
     *     to size_t.
     */
    void resize(int nn, int npoll) {
        n_nodes = nn;
        n_pollutants = npoll;
        auto total = static_cast<std::size_t>(nn * npoll);
        expressions.assign(total, "");
        compiled.resize(total);
        has_treatment.assign(static_cast<std::size_t>(nn), false);
        cin.assign(static_cast<std::size_t>(npoll), 0.0);
        removal.assign(static_cast<std::size_t>(npoll), -1.0);
    }

    /**
     * @brief Request capacity reduction for authored and compiled expression vectors.
     *
     * Does not compact has_treatment, cin or removal. Values persist; vector relocation can invalidate
     * references and allocation can throw.
     */
    void shrink_to_fit() {
        expressions.shrink_to_fit();
        compiled.shrink_to_fit();
    }

    /**
     * @brief Test whether any authored treatment expression is nonempty.
     *
     * Does not inspect compiled validity or has_treatment flags.
     * @return True if any expressions string has bytes; whitespace alone counts.
     */
    bool hasAny() const {
        for (const auto& e : expressions)
            if (!e.empty())
                return true;
        return false;
    }
};

} // namespace openswmm

#endif // OPENSWMM_ENGINE_QUALITY_DATA_HPP
