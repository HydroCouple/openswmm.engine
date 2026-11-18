// SPDX-License-Identifier: Apache-2.0

/**
 * @file      UebSubcatchments.hpp
 * @brief     Utah Energy Balance snow processing for subcatchments.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#pragma once
#include "UebHeld.hpp"
#include <vector>
namespace openswmm {
struct SimulationContext;
namespace snow {
struct SnowSoA;
}
namespace surface::ueb {
/** @brief Owned per-surface UEB predictions, accepted outputs and whole-model ledgers.
 * Three surface columns per subcatchment; rain/snow caches are per subcatchment.
 * Held water/energy use m and kJ/m2; global water/energy ledgers use m3 and kJ.
 * Sample/completed clocks are elapsed seconds. last_seconds is the last call span;
 * enabled column clocks can differ when source groups advance independently.
 * Report arrays live in SnowSoA; one serialized owner coordinates both stores. */
struct NativeState {
    std::array<double, 4> forcing{}; // modes and values of runtime climate overrides
    std::vector<HeldState> columns;
    std::vector<LumpedFluxes> last;
    std::vector<double> rain, snow;
    std::vector<double> sample_end, sample_seconds; // per column: independent owner clocks
    bool active = false;
    double sublimation = 0, precipitation = 0, liquid = 0, throughfall = 0;
    double net_energy = 0, geometry_energy = 0, plowed_energy = 0, initial_energy = 0;
    double initial_water = 0, removed = 0, transferred_water = 0;
    double last_seconds = 0, completed_seconds = 0;
};
/** @brief Initialize privately staged UEB columns from authored native snow assignments.
 * @param[in,out] context Initialized snowpacks/configuration, compatible subcatchment
 * areas/pack indices and unit factors. Replaces only snowpacks.runtime on success.
 * @param[in,out] snow Initialized three-surface native geometry/history arrays with
 * valid extents and assignments. Replaces external-owner flags and publishes UEB
 * water/cover/cold reports on success; zero-duration publication retains flux fields.
 * @return Empty on success, or removal-fraction/lumped-initialization diagnostic
 * before publication. Marks UEB assignments active even when surface area is zero.
 * Validates removal fractions individually and their sum, initializes positive-area
 * columns using authored free water and latent energy plus pack initial kJ/m2, and
 * computes initial inventory over non-LID horizontal area. Does not check every
 * context index/physical parameter or apply global ignore-snow gating. Allocation/
 * copy exceptions propagate; serialize access to runtime and native arrays. */
std::string initializeSubcatchments(SimulationContext& context, snow::SnowSoA& snow);
/** @brief Trial accepted UEB snow and plowing over one source/runoff interval.
 * @param[in,out] context Matching initialized weather/gage/snow configuration. On
 * success replaces UEB runtime and books signed sublimation in runoff balance;
 * weather/table cursors and radiation outputs may change even if trial fails.
 * @param[in,out] native Compatible native state/owner flags. Enabled external surfaces
 * are advanced; success replaces staged histories/reports, including DD receivers
 * of plowed UEB snow. Caller supplies consistent report and accepted water pools.
 * @param date Start of interval in native OLE days; must correspond to elapsed.
 * @param elapsed Start seconds since simulation origin, in the owning source schedule.
 * @param seconds Positive accepted span, no longer than snow/hour/gage/transient/
 * simulation boundary; caller qualifies finite time and coherent column clocks.
 * @param rain Borrowed per-subcatchment finite nonnegative, already partitioned and
 * catch-corrected liquid ft/s; array has at least native.n_subcatch entries.
 * @param snow Same-layout solid precipitation ft/s. Native drift/catch must already
 * be applied: this adapter forces site drift and snow catch to one.
 * @return Empty on accepted publication or inactive no-op, otherwise diagnostic
 * retaining owned snow/report state. Copies owner/native arrays, invalidates held
 * predictions on override/rate changes or incoming plowed water, resolves held
 * forcing once per subcatchment, consumes only requested shares and then plows.
 * Precipitation, physical/cover-remap energy and transferred/plowed inventories stay
 * separate. Context weather side effects are outside rollback; exceptions propagate.
 * Does not advance global simulation clocks or debit external soil/source owners. */
std::string advanceSubcatchments(SimulationContext& context, snow::SnowSoA& native, double date,
                                 double elapsed, double seconds, const double* rain,
                                 const double* snow);
/** @brief Shorten a proposed span to the next native UEB held-forcing boundary.
 * @param context Initialized snow runtime/configuration with finite positive step
 * and valid start date; global ignore-snow or inactive runtime bypasses shortening.
 * @param elapsed Current finite elapsed seconds in the source/runoff schedule.
 * @param proposed Proposed finite nonnegative duration seconds.
 * @return Minimum of proposed, next configured snow grid boundary (1e-7 s landing
 * bias), and next clock hour when hourly meteorology exists (1e-5 s bias). Does not
 * limit to gage changes, transient override span or simulation end; caller does so.
 * No state mutation, physical solve or argument validation. */
double nextSnowBoundary(const SimulationContext& context, double elapsed, double proposed) noexcept;
} // namespace surface::ueb
} // namespace openswmm
