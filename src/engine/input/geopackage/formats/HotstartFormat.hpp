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
 * @file      HotstartFormat.hpp
 * @brief     Legacy HSF v4 binary hot-start file — parser / materialiser.
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
 *          - The subcatchment-runoff section (saveRunoff/readRunoff),
 *            whose length depends on the model (`HsfLayout`), and anything
 *            after the link records (an OpenSWMM trailer) are carried
 *            verbatim, so a file round-trips byte for byte. A v4 file's
 *            per-storage-node residence time float is decoded.
 *
 *          Endianness: HSF is written little-endian on all SWMM5
 *          platforms.  We rely on the SWMM5 convention (no byte-swapping)
 *          and document the assumption.
 *
 * @ingroup engine_geopackage
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_GEOPACKAGE_HOTSTART_FORMAT_HPP
#define OPENSWMM_GEOPACKAGE_HOTSTART_FORMAT_HPP

#include "FormatTypes.hpp"

#include <string>

namespace openswmm::gpkg::formats {

/**
 * @brief Read legacy-layout hot-start records and preserve raw runoff/trailer bytes.
 *
 * Reads a 15-byte SWMM5 stamp and six native int32 counts in the v4-compatible layout regardless of
 * parsed suffix. Caller supplies nonnegative representable counts/size products and matching
 * storage layout; the parser has no global count/memory or finite-value cap. Native floats store
 * feet/cfs and concentrations; runoff/trailer retained verbatim, subcatch_state not decoded. No
 * checksum/model-fingerprint validation or whole-destination rollback; allocation/callback
 * exceptions can escape.
 *
 * @param[in] path UTF-8 filesystem path; caller supplies parent directories and owns
 *     destination/source lifetime.
 * @param[in,out] snapshot Destination overwritten incrementally, with prior/partial fields retained
 *     on failure; caller normally starts fresh.
 * @param[in] layout Borrowed model-dependent runoff-length callback and storage-node mask; correct
 *     aligned dimensions required to locate routing records.
 * @return Failure on guarded open/stamp/count/read/layout refusal; otherwise success.
 */
FormatResult parseHotstartHsf(const std::string& path, HotstartSnapshot& snapshot,
                              const HsfLayout& layout = {});

/**
 * @brief Write retained legacy hot-start routing records and opaque raw sections.
 *
 * Checks node/link vector counts and pollutant widths before opening. Writes v2..v4 stamp according
 * to file_version, otherwise v4, using the same six-count native layout; empty runoff writes
 * subcatch count zero. Values narrow to native floats; HRT is written whenever has_hrt even if
 * version is older, so caller supplies compatible flags. Ignores derived overflow/volume/timing
 * fields. Direct truncation, no atomic stage, finite validation or CRC; trailer preserved.
 *
 * @param[in] path UTF-8 filesystem path; caller supplies parent directories and owns
 *     destination/source lifetime.
 * @param[in] snapshot Coherent header/counts and node/link concentration arrays, with matching
 *     runoff bytes and storage HRT flags.
 * @return Failure on guarded shape/open/write refusal; success after attempted close (close errors
 *     ignored).
 */
FormatResult writeHotstartHsf(const std::string& path, const HotstartSnapshot& snapshot);

} // namespace openswmm::gpkg::formats

#endif // OPENSWMM_GEOPACKAGE_HOTSTART_FORMAT_HPP
