/**
 * @file ExternalContentReader.hpp
 * @brief Slice IO-8 — hydrate `SimulationContext` external-file slots from
 *        the Part D GeoPackage tables and materialise scratch files in
 *        the legacy on-disk format so legacy `fopen` sites keep working.
 *
 * @details Companion to `ExternalContentWriter`. For each role with rows
 *          present in the Part D tables:
 *
 *            1. Group rows by owner (gage, timeseries, node-id, hot-start
 *               slot, etc.) and feed them through the matching IO-6
 *               materialiser to write a scratch file under
 *               `<gpkg-stem>.scratch/`.
 *            2. Populate the matching slot's `FilePathPair`:
 *               - `.absolute` ← scratch file path (ready for `fopen`).
 *               - `.original` ← diagnostic sentinel of the form
 *                 `"<gpkg:role:owner>"` so `InpWriter` does not emit a
 *                 stale path token if the model is later saved as `.inp`.
 *
 *          Scratch directory sits **next to** the `.gpkg` rather than in
 *          `$TMPDIR` so a reviewer can inspect what was fed to the
 *          engine (CLAUDE.md §4 transparent-IO directive).
 *
 *          On engine close, the orchestrator deletes the scratch
 *          directory; that hookup arrives with the C-API/engine
 *          lifecycle slice and is not part of IO-8.
 *
 * @ingroup engine_geopackage
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_GEOPACKAGE_EXTERNAL_CONTENT_READER_HPP
#define OPENSWMM_GEOPACKAGE_EXTERNAL_CONTENT_READER_HPP

#include <string>

struct sqlite3;

namespace openswmm {
    struct SimulationContext;
}

namespace openswmm::gpkg {

/**
 * @brief Hydrate every external-file slot on `ctx` from the Part D tables
 *        and materialise scratch files in `scratch_dir`.
 *
 * @details `scratch_dir` is created if missing. When `scratch_dir` is
 *          empty the call is a no-op — the function is then equivalent
 *          to skipping external-content hydration entirely (useful in
 *          read_model callers that don't have a `.gpkg` path context).
 *
 * @throws GpkgError on materialiser failures.
 */
void read_external_content(sqlite3*                          db,
                            openswmm::SimulationContext&      ctx,
                            const std::string&                simulation_id,
                            const std::string&                scratch_dir);

/**
 * @brief Compute the canonical scratch-dir path for a given `.gpkg` file.
 *
 * @details Sibling directory named `<basename>.scratch/`. Pure string
 *          transformation — no filesystem touch.
 */
std::string scratchDirFor(const std::string& gpkg_path);

} // namespace openswmm::gpkg

#endif // OPENSWMM_GEOPACKAGE_EXTERNAL_CONTENT_READER_HPP
