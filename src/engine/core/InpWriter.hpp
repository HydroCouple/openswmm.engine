/**
 * @file InpWriter.hpp
 * @brief Write a SimulationContext to a SWMM .inp file.
 *
 * @details Serialises all model data from SoA arrays into standard SWMM
 *          input file format. Supports all sections from the legacy format
 *          plus new sections ([USER_FLAGS], [PLUGINS]).
 *
 * @ingroup engine_core
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_INP_WRITER_HPP
#define OPENSWMM_INP_WRITER_HPP

#include <string>

namespace openswmm {

struct SimulationContext;

namespace inp_writer {

/**
 * @brief Write the full model to a SWMM .inp file.
 *
 * @details Sections written (in order):
 *   [TITLE], [OPTIONS],
 *   [EVAPORATION], [TEMPERATURE], [SNOWPACKS], [ADJUSTMENTS],
 *   [RAINGAGES], [SUBCATCHMENTS], [SUBAREAS],
 *   [INFILTRATION], [JUNCTIONS], [OUTFALLS], [STORAGE], [DIVIDERS],
 *   [CONDUITS], [PUMPS], [ORIFICES], [WEIRS], [OUTLETS],
 *   [XSECTIONS], [TRANSECTS], [LOSSES], [CONTROLS], [REPORT],
 *   [INFLOWS], [DWF], [RDII],
 *   [POLLUTANTS], [LANDUSES], [BUILDUP], [WASHOFF], [TREATMENT],
 *   [TIMESERIES], [CURVES], [PATTERNS],
 *   [USER_FLAGS], [PLUGINS],
 *   [MAP], [COORDINATES], [VERTICES], [Polygons], [SYMBOLS]
 *
 * @param ctx   Simulation context with all model data.
 * @param path  Output file path.
 * @returns 0 on success, -1 on file error.
 */
int writeInpFile(const SimulationContext& ctx, const std::string& path);

} // namespace inp_writer
} // namespace openswmm

#endif // OPENSWMM_INP_WRITER_HPP
