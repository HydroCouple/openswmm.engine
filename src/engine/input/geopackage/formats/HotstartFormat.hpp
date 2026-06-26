/**
 * @file HotstartFormat.hpp
 * @brief Legacy HSF v4 binary hot-start file — parser / materialiser.
 *
 * @details Slice IO-6e. Implements the routing-state portion of the
 *          EPA-SWMM hot-start file (`saveRouting`/`readRouting` in
 *          `src/legacy/engine/hotstart.c`):
 *
 *          - Header: "SWMM5-HOTSTART4" + `{nSubcatch, nLandUses, nNodes,
 *            nLinks, nPollut, flowUnits}` (six int32 fields).
 *          - Per-node state: float depth, lateral_inflow + nPollut quality
 *            floats. **`overflow` is not in the legacy HSF**; it is
 *            preserved through the GPKG schema only and round-trips
 *            through HSF as 0.0.
 *          - Per-link state: float flow, depth, setting + nPollut quality
 *            floats. The other GPKG `hotstart_link_state` fields
 *            (`volume`, `target_setting`, `time_open`, `time_closed`)
 *            have no HSF counterpart and round-trip through HSF as 0.0.
 *
 *          The subcatchment-runoff portion of the legacy file
 *          (saveRunoff/readRunoff) carries conditional fields whose
 *          presence depends on per-subcatchment groundwater / snowpack
 *          configuration. That portion is **out of scope for IO-6e**;
 *          this slice covers the routing-state half so the GeoPackage
 *          materialiser can round-trip a USE-direction hot-start used to
 *          seed routing-only restart scenarios. A follow-up slice
 *          extends to runoff state when the subcatchment editor lands.
 *
 *          Endianness: HSF is written little-endian on all SWMM5
 *          platforms.  We rely on the SWMM5 convention (no byte-swapping)
 *          and document the assumption.
 *
 * @ingroup engine_geopackage
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_GEOPACKAGE_HOTSTART_FORMAT_HPP
#define OPENSWMM_GEOPACKAGE_HOTSTART_FORMAT_HPP

#include "FormatTypes.hpp"

#include <string>

namespace openswmm::gpkg::formats {

/**
 * @brief Parse an HSF v4 file into a `HotstartSnapshot`.
 *
 * @details Populates `header.{num_*,flow_units,file_version,file_stamp}`,
 *          `node_state[]`, and `link_state[]`. `subcatch_state[]` is left
 *          empty (see header doc — runoff state is out of scope for this
 *          slice).
 *
 * @param path      Filesystem path to the HSF file.
 * @param snapshot  Output snapshot (existing content is overwritten).
 */
FormatResult parseHotstartHsf(const std::string&  path,
                               HotstartSnapshot&  snapshot);

/**
 * @brief Write a `HotstartSnapshot` to an HSF v4 file at `path`.
 *
 * @details Header counts must already match the lengths of the per-object
 *          state vectors; mismatches return a `FormatResult` with `ok=false`
 *          rather than writing a corrupt file. Subcatchment runoff state
 *          is not written — the slice's scope is routing-only round-trip.
 */
FormatResult writeHotstartHsf(const std::string&        path,
                               const HotstartSnapshot&   snapshot);

} // namespace openswmm::gpkg::formats

#endif // OPENSWMM_GEOPACKAGE_HOTSTART_FORMAT_HPP
