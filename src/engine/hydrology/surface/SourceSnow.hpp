// SPDX-License-Identifier: Apache-2.0

/**
 * @file      SourceSnow.hpp
 * @brief     Snow processes associated with hydrological source owners.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#pragma once
#include "NativeSnow.hpp"
#include "../Runoff.hpp"
#include <vector>
namespace openswmm::detail {
class CompletedSourceArchive;
}
namespace openswmm::surface {
// Snow owns solid/liquid pack water. The source water ledger receives only
// completed liquid delivery; together their two balances recover precipitation.
struct SourceSnowLedger {
    double end = 0, initial = 0, precipitation = 0, liquid = 0, sublimation = 0, removed = 0,
           stored = 0; // seconds, ft3
    /** @brief Compute this snow ledger's signed water-balance residual.
     * @return initial+precipitation-liquid-sublimation-removed-stored in ft3. No finite
     * validation, tolerance test, clamping or mutation; sublimation may represent a
     * signed atmospheric exchange under the underlying model. */
    double residual() const {
        return initial + precipitation - liquid - sublimation - removed - stored;
    }
};
/** @brief Per-source-group native/UEB snow ownership and cumulative water balance.
 * Liquid source delivery, stored pack water and atmospheric losses are separate
 * ledgers. Caller supplies a private context/trial and serializes all operations;
 * mutations are immediate and exceptions do not roll back a partially advanced
 * snow owner. SourceWaterDriver provides the enclosing accepted/trial boundary. */
class SourceSnow {
public:
    /** @brief Build source-group snow ownership and initial pack inventory.
     * @param[in,out] context Compatible initialized source model with qualified native/
     * UEB parameters, aligned source/gage arrays and resolved radiation inputs. A private
     * copy masks unowned packs; successful native setup transfers its runtime owner
     * into this context. Other model identities/arrays remain caller-owned.
     * @param groups Ordered nonoverlapping groups of valid subcatchment indices; same
     * membership/ordering must be retained for later advance/checkpoint calls.
     * @param nonlid_area One finite nonnegative physical non-LID ft2 per original source,
     * matching native pack fractional areas; indices/shape are assumed qualified here.
     * @return Empty on accepted replacement, otherwise water-only/native-setup refusal.
     * Ignore-snowmelt or empty groups installs an inactive owner. With snow enabled,
     * nonempty groups are active even without assigned packs, because rain/snow catch
     * factors still apply; active source pollutants are refused unless ignored.
     * Normal setup refusal retains this owner; copy/allocation/setup exceptions can
     * propagate, and a late failure can follow context runtime publication. Initial
     * stored and initial ledger volumes are equal; conservative plowing is enabled.
     * Does not advance source clocks or offer an in-place hot-start transaction. */
    std::string initialize(SimulationContext& context, const std::vector<std::vector<int>>& groups,
                           const std::vector<double>& nonlid_area);
    /** @brief Advance one exact closed snow-owner interval and expose liquid forcing.
     * @param[in,out] context Matching private source context with prepared native/UEB
     * weather, runtime pack state, finite scale/catch/drift factors and valid unit enums.
     * Writes runtime snow/UEB state and source snow-net/depth scratch; weather table
     * cursors may advance. Does not advance global context current_time/date.
     * @param group Valid zero-based owner group, bounds checked by ledger/group access.
     * @param start Finite elapsed seconds exactly equal that ledger's accepted end.
     * @param end Finite later elapsed seconds, with end-start in (0,86400] for weather
     * resolution and respecting held snow subdivisions selected by the caller.
     * @param[in,out] forcing Complete unique matching group cohort with valid source
     * indices, internal ft/s weather and positive segment semantics. Splits raw
     * precipitation by interval temperature, UEB transition thresholds/division mode,
     * gage snow catch/drift and source rain/snow scales, then prescriptions. Writes
     * rain to combined rain+snow for LID footprints, plus exposed impervious/pervious
     * PET; native non-LID packs separately expose completed liquid runoff.
     * @throws std::runtime_error Discontinuous clock, radiation/UEB refusal, invalid
     * precipitation or snow conservation failure. Inactive owner is a no-op without
     * validating arguments. Active mutation/ledger changes can precede failure; discard
     * the enclosing private trial. Plowing, melt, sublimation and stored inventory feed
     * the separate ft3 balance with 1e-10 relative tolerance. No receiver commit occurs. */
    void advance(SimulationContext& context, int group, double start, double end,
                 std::vector<runoff::RunoffSourceForcing>& forcing);
    /** @brief Stream private snow model identity or runtime owner/ledger state.
     * @param[in,out] archive Matching native archive reader/writer; caller qualifies
     * schema/order and completes outer archive validation before publication.
     * @param model True streams snow identity, group/area and gage catch metadata;
     * false streams a native snow checkpoint followed by exact group ledgers.
     * @param[in,out] context Matching private source model/runtime owner, mutated by
     * restoration and underlying native/UEB checkpoint helpers.
     * @throws std::runtime_error Native/UEB restore, invalid finite/nonnegative ledger,
     * stored-inventory or water-closure mismatch. Sublimation must be finite but is
     * not required nonnegative. Inactive owner emits/consumes no bytes. A failed read
     * can partially mutate state/context; restore a private candidate. This routine
     * does not publish a production hot start or independently check all source clocks. */
    void checkpoint(detail::CompletedSourceArchive& archive, bool model,
                    SimulationContext& context);
    /** @brief Overlay owned accepted snow rows into a compatible native restart view.
     * @param source Matching accepted private context holding this owner's UEB runtime.
     * @param[in,out] target Compatible independently prepared native context; copies UEB
     * columns/last samples/end/duration only for owned externally solved pack rows.
     * @param[in,out] output Compatible pre-sized SnowSoA; overwrites selected native
     * history/flux/coverage/energy rows for owned assigned packs, three surfaces each.
     * Does not rebuild geometry, change model parameters, clear global removed totals,
     * reset flux ledgers or resize the destination. Unowned/no-pack rows are retained.
     * Inactive owner is a no-op. Index/allocation errors can follow partial destination
     * writes; caller uses private targets for transactional export and validates the
     * paired restart before publication. Does not mutate this accepted snow owner. */
    void exportAccepted(const SimulationContext& source, SimulationContext& target,
                        snow::SnowSoA& output) const;
    /** @brief Read whether snow-enabled source weather ownership is installed.
     * @return Stored flag, false initially/when ignored or no groups; can be true with
     * no physical pack because catch factors still apply to group precipitation. */
    bool active() const { return active_; }
    /** @brief Borrow the native owner arrays without checking active/initialization.
     * @return Owned solver state, empty/default before setup. initialize/move/destruction
     * can invalidate elements and advance/checkpoint change contents. Serialize reads
     * and copy any snapshot needed across source trial publication. */
    const snow::SnowSoA& state() const { return solver_.state(); }
    /** @brief Borrow per-source-group elapsed endpoints and cumulative snow water accounts.
     * @return Owned ordered rows, elapsed seconds plus ft3 volumes. Changes under
     * initialize/advance/checkpoint/move; copy for a stable snapshot and serialize access.
     * Liquid delivered into SourceWaterDriver is represented separately from pack storage. */
    const std::vector<SourceSnowLedger>& ledgers() const { return ledger_; }

private:
    bool active_ = false;
    snow::SnowSolver solver_;
    std::vector<std::vector<int>> groups_;
    std::vector<double> area_;
    std::vector<SourceSnowLedger> ledger_;
    /** @brief Sum the current physical native/UEB pack water for one owner group.
     * @param group Zero-based group index, checked with groups_.at(); member source and
     * area/state indices must remain compatible with initialization.
     * @return ft3 from nativeSnowVolume over matching non-LID areas. Excludes LID/source
     * liquid stores and pending delivery water; no ledger mutation or tolerance check. */
    double storage(int group) const;
};
}
