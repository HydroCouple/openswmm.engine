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
 * @file      FvOptionRows.hpp
 * @brief     The finite volume solver's [OPTIONS] rows, as key and value text.
 *
 * @details One list for every writer that persists the finite volume options:
 *          the input file writer prints the rows into [OPTIONS], and the
 *          GeoPackage writer stores them in its options table, where the
 *          reader hands them back to the [OPTIONS] parser. A key added here
 *          reaches both formats; a key added in one writer only would be lost
 *          by the other.
 *
 * @ingroup engine_core
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_CORE_FV_OPTION_ROWS_HPP
#define OPENSWMM_ENGINE_CORE_FV_OPTION_ROWS_HPP

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "SimulationOptions.hpp"
#include "UnitConversion.hpp"

namespace openswmm {

/**
 * @brief List the finite volume option rows a native model file carries.
 *
 * Values are the tokens the [OPTIONS] parser reads. Doubles carry 17 significant digits, so a
 * value read back is the value written. Keys that are written only when they differ from their
 * default keep that rule here; an absent key reads as its default.
 *
 * @param[in] o Model options; the slot celerity is written in the model's length units.
 * @return Rows in the order the input file writer has always printed them.
 */
inline std::vector<std::pair<std::string, std::string>> fvOptionRows(const SimulationOptions& o) {
    static const char* sRiemann[] = {"HLL", "HLLC"};
    static const char* sLimiter[] = {"MINMOD", "VANLEER", "SUPERBEE"};
    static const char* sScalar[] = {"UPWIND", "MUSCL", "QUICKEST_ULTIMATE"};
    static const char* sTime[] = {"EULER", "RK2"};
    static const char* sBackend[] = {"CPU", "AUTO", "OMP", "CUDA", "HIP", "SYCL"};
    auto real = [](double v) {
        char b[32];
        std::snprintf(b, sizeof(b), "%.17g", v);
        return std::string(b);
    };
    const auto& fvo = o.fv;
    std::vector<std::pair<std::string, std::string>> rows;
    rows.emplace_back("FV_CELL_LENGTH", real(fvo.cell_length));
    rows.emplace_back("FV_MIN_CELLS", std::to_string(fvo.min_cells));
    rows.emplace_back("FV_CFL", real(fvo.cfl));
    rows.emplace_back("FV_RIEMANN", sRiemann[static_cast<int>(fvo.riemann)]);
    rows.emplace_back("FV_ORDER", std::to_string(fvo.order));
    rows.emplace_back("FV_LIMITER", sLimiter[static_cast<int>(fvo.limiter)]);
    rows.emplace_back("FV_SCALAR_SCHEME", sScalar[static_cast<int>(fvo.scalar_scheme)]);
    rows.emplace_back("FV_TIME_INTEGRATION", sTime[static_cast<int>(fvo.time_integration)]);
    rows.emplace_back("FV_SLOT_CELERITY",
                      real(fv::slotCelerityProject(fvo.slot_celerity, ucf::UCF(ucf::LENGTH, o))));
    if (fvo.pressurized_implicit)
        rows.emplace_back("FV_PRESSURIZED_IMPLICIT", "YES");
    if (fvo.node_feedback_dt) // non-default only
        rows.emplace_back("FV_NODE_FEEDBACK_DT", "YES");
    if (fvo.pressure_closure == 1) // issue #156: non-default only
        rows.emplace_back("FV_PRESSURE_CLOSURE", "TPA");
    if (fvo.dispersion > 0.0)
        rows.emplace_back("FV_DISPERSION", real(fvo.dispersion));
    if (fvo.structure_coupling != fv::StructureCoupling::SUBSTEP)
        rows.emplace_back("FV_STRUCTURE_COUPLING", "ROUTING_STEP");
    if (!fvo.compaction)
        rows.emplace_back("FV_COMPACTION", "NO");
    rows.emplace_back("FV_BACKEND", sBackend[static_cast<int>(fvo.backend)]);
    rows.emplace_back("FV_MIN_PARALLEL_CELLS", std::to_string(fvo.min_parallel_cells));
    if (!fvo.lts)
        rows.emplace_back("FV_LTS", "NO");
    rows.emplace_back("FV_LTS_MAX_TIERS", std::to_string(fvo.lts_max_tiers));
    if (fvo.cfl_census_interval != 1)
        rows.emplace_back("FV_CFL_CENSUS_INTERVAL", std::to_string(fvo.cfl_census_interval));
    return rows;
}

} // namespace openswmm

#endif // OPENSWMM_ENGINE_CORE_FV_OPTION_ROWS_HPP
