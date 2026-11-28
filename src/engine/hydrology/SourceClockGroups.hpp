// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin

/**
 * @file      SourceClockGroups.hpp
 * @brief     Scheduling of water sources with common integration clocks.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_SOURCE_CLOCK_GROUPS_HPP
#define OPENSWMM_SOURCE_CLOCK_GROUPS_HPP
#include "Runoff.hpp"
#include "Climate.hpp"
#include "../data/ForcingData.hpp"
#include <array>
#include <string>
#include <vector>

namespace openswmm::detail {
class CompletedSourceArchive;
}

namespace openswmm::runoff {

/** @brief Piecewise-constant forcing over [start,end) in elapsed seconds.
 * month is zero-based; infil_factor/recovery_factor are monthly dimensionless
 * multipliers. sources follow the owning group's ascending subcatchment order;
 * rain/pet/raw_precipitation are internal ft/s, not project rainfall units. */
struct SourceForcingInterval {
    double start = 0.0, end = 0.0;
    int month = 0;
    double infil_factor = 1.0, recovery_factor = 1.0;
    std::vector<RunoffSourceForcing> sources;
};

/** @brief One weakly connected runoff/LID/snow-transfer component.
 * subcatches/lid_usages contain model indices; completed_end is accepted elapsed
 * seconds. pending contains staged forcing only, never committed water state. */
struct SourceClockGroup {
    std::vector<int> subcatches, lid_usages;
    double completed_end = 0.0;
    std::vector<SourceForcingInterval> pending;
};

/// Internal completed-interval clock/forcing adapter. Seeds select whole weakly
/// connected runoff/LID-drain graphs. Copied source records, not the global
/// gage/climate cursor, supply group rates through SourceWaterDriver.
/// Stage changes no source water. The driver must trial every enabled store,
/// settle intake, install accepted source states, then commit the group marker.
/// Cancellation restores no water: uncommitted source trials must stay private.
class SourceClockGroups {
public:
    /** @brief Build selected connected source groups and snapshot their forcing inputs.
     * @param ctx Compatible initialized model with aligned subcatchment/gage/LID arrays,
     * resolved names, valid flow units, calendar dates and forcing persistence arrays.
     * Reads graph, rainfall/PET tables and monthly factors without advancing cursors.
     * @param seeds Zero-based subcatchment indices. Every whole weakly connected
     * component touched by a seed is selected; duplicates are harmless, empty selects
     * none. Runoff, LID drains and enabled snow plowing all connect components.
     * @param hydraulic_returns True permits qualified hydraulic outfall returns and
     * validates their indices; false rejects returns into selected components.
     * @return Empty on success, otherwise a refusal message. Builds a temporary then
     * moves it into this adapter, retaining old state on normal failure. Reinitializing
     * an adapter with pending intervals or a positive committed clock is refused.
     * Selected groups support persistent prescriptions, nonnegative ordered rainfall
     * and constant/monthly/time-series PET; unsupported forcing/precomputed runoff is
     * refused. With no selected groups those weather qualifications are skipped.
     * No borrowed context data is retained; later model/forcing edits require a fresh
     * compatible initialization. Caller serializes all adapter operations. */
    std::string initialize(const SimulationContext& ctx, const std::vector<int>& seeds,
                           bool hydraulic_returns = false);
    /** @brief Resolve and stage contiguous forcing from a group's accepted boundary.
     * @param group Zero-based initialized group index with no existing pending interval.
     * @param end Finite requested end in elapsed seconds, strictly after completed_end
     * and within simulation duration (relative tolerance 1e-12).
     * @param completed_through Finite elapsed-second frontier at least end; supplied by
     * the driver as proof the forcing interval is completed, not future prediction.
     * @return Empty on success, otherwise a refusal without installing a partial pending
     * list. Splits at month, PET-record, rain-record and rain-expiry boundaries. Resolves
     * monthly adjustments and persistent overrides/additions, dry-only evaporation,
     * and subcatchment scales in internal ft/s. Invalid resolved rates refuse staging.
     * No source water or accepted clock changes. A successful stage requires driver
     * trial/settlement before commit(); cancel() only drops forcing. Allocation and
     * calendar exceptions propagate; dates must be representable by DateTime helpers. */
    std::string stage(int group, double end, double completed_through);
    /** @brief Advance a group's accepted clock to the last staged forcing endpoint.
     * @param group Valid zero-based group with a nonempty pending list.
     * @return Empty after updating completed_end and clearing pending, otherwise a
     * refusal. Does not install/verify source water; the driver must first accept all
     * corresponding source states. Borrowed pending element references are invalidated. */
    std::string commit(int group);
    /** @brief Drop staged forcing without changing the accepted clock or water stores.
     * @param group Zero-based group index; an invalid index is a no-op.
     * Invalidates references to that group's pending elements. Source trials and any
     * water rollback remain the driver's responsibility. */
    void cancel(int group);
    /** @brief Borrow the group membership, accepted clocks and pending forcing.
     * @return Const owned vector valid until destruction; initialization/checkpoint
     * replacement can invalidate elements, and stage/commit/cancel mutate their pending
     * storage. Caller copies data needed across such changes and serializes access. */
    const std::vector<SourceClockGroup>& groups() const { return groups_; }
    /** @brief Look up selected component membership without scanning the graph.
     * @param source Zero-based model subcatchment index.
     * @return Group index or -1 for invalid/unselected sources (including before init).
     * The mapping describes the last successful initialization/model restoration. */
    int groupForSubcatch(int source) const;
    /** @brief Stream model forcing/membership or accepted source-clock restart state.
     * @param[in,out] archive Matching CompletedSourceArchive in read or write mode;
     * serialization order/schema and companion model are supplied by the caller.
     * @param model True streams forcing snapshot and group membership; false streams
     * only completed_end for each group and requires no pending intervals.
     * @throws std::runtime_error Pending restart state, invalid restored/written clock,
     * or archive shape/bounds/IO errors. Clocks must be finite, nonnegative and within
     * the stored duration tolerance. Read can partially mutate before a later refusal;
     * use an isolated candidate when transactional restoration is required. Model-mode
     * loading alone is not comprehensive validation of all stored forcing values. */
    void checkpoint(detail::CompletedSourceArchive& archive,
                    bool model); // internal accepted-boundary IO

private:
    struct RainSeries {
        std::vector<double> times, rates;
        double period = 0.0, override_rate = -1.0;
        ForcingMode mode = ForcingMode::NONE;
        double prescribed = 0.0;
    };
    bool initialized_ = false, ignore_rain_ = false, dry_only_ = false;
    double start_date_ = 0.0, duration_ = 0.0, rain_ucf_ = 1.0, evap_ucf_ = 1.0;
    climate::EvapMethod evap_method_ = climate::EvapMethod::CONSTANT;
    ForcingData forcing_;
    std::array<double, 12> monthly_evap_{}, adjust_evap_{}, adjust_rain_{}, infil_{}, recovery_{};
    std::vector<double> evap_times_, evap_values_, rain_scale_;
    std::vector<int> source_gage_, source_group_;
    std::vector<RainSeries> gages_;
    std::vector<SourceClockGroup> groups_;
};
}
#endif
