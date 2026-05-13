/**
 * @file FilesHandler.hpp
 * @brief [FILES] section handler — secondary file references.
 *
 * @details Parses the legacy SWMM5 `[FILES]` section into
 *          `SimulationContext::files`.  Row format:
 *
 * @code
 * [FILES]
 * SAVE/USE   FileType   "path"   [date  time]
 * @endcode
 *
 *          Where FileType ∈ {RAINFALL, RUNOFF, RDII, INFLOWS, OUTFLOWS,
 *          HOTSTART}.  The optional date/time tokens apply only to
 *          `SAVE HOTSTART` and are stored on the `hotstart_save_datetime`
 *          field as a fractional-day SWMM date.
 *
 *          Multi-row `SAVE HOTSTART` (legacy supported up to 10) is not
 *          yet honoured: each new SAVE row overwrites the previous slot.
 *          Lift to a vector when a real workflow needs it.
 *
 * @see src/engine/core/SimulationContext.hpp — `FilesSpec` struct.
 * @see src/legacy/engine/iface.c — legacy parser this mirrors.
 * @ingroup engine_input
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_ENGINE_FILES_HANDLER_HPP
#define OPENSWMM_ENGINE_FILES_HANDLER_HPP

#include <vector>
#include <string>

namespace openswmm { struct SimulationContext; }

namespace openswmm::input {

/**
 * @brief Parse the `[FILES]` section into `ctx.files`.
 *
 * @param ctx    Simulation context to populate.
 * @param lines  Non-comment, non-empty lines from the section.
 */
void handle_files(SimulationContext& ctx, const std::vector<std::string>& lines);

} /* namespace openswmm::input */

#endif /* OPENSWMM_ENGINE_FILES_HANDLER_HPP */
