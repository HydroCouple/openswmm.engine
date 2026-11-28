// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin

/**
 * @file      SourceWaterDeliveryBridge.hpp
 * @brief     Delivery of accepted source water to coupled recipients.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#pragma once
#include "SourceWaterDriver.hpp"
#include <cstdint>
#include <optional>

namespace openswmm::runoff {
struct QueuedSourceDelivery {
    uint64_t ordinal = 0;
    double available = 0;      // accepted receiving boundary, seconds
    SourceWaterDelivery water; // original completed source interval and ft3
};
struct SourceNodeDelivery {
    double runoff = 0, drain = 0; // ft3, kept separate for hydraulic continuity
    double rate = 0;              // total ft3/s over the staged receiving interval
};

/// Internal water-only qualification boundary between completed spatial source
/// graphs and hydraulic routing. Ownership covers whole weakly connected graphs.
/// Capture publishes completed outlet volumes after an accepted receiving step;
/// stage offers them once to the next step, at volume / receiving duration.
/// Commit must follow hydraulic acceptance; cancel leaves every receipt pending.
/// The explicit internal engine attachment suppresses duplicate ordinary
/// runoff/LID kernels and pairs the bridge with engine/surface/source restart.
/// This whole-copy bridge does not qualify production scheduling/performance.
class SourceWaterDeliveryBridge {
public:
    /** @brief Attach whole-source ownership before any source/receiving advancement.
     * @param source Compatible initialized source driver with zero completed group clocks,
     * no pending source trial or delivery history, named hydraulic nodes and resolved
     * outlets for selected components. This bridge rejects pollutant-bearing sources;
     * other source qualification remains the driver's responsibility.
     * @return Empty after publishing a prepared owned snapshot, otherwise refusal or
     * caught standard-exception text, retaining the prior bridge. Reattachment is
     * refused. Copies model-archive identity, node names, ownership and zero ledgers;
     * retains no source pointer. Source identity/order must remain compatible later.
     * Whole copies/hash validation are intentional qualification costs, not a claim of
     * production scheduling performance. Serialize all bridge/source operations. */
    std::string initialize(const SourceWaterDriver& source);
    /** @brief Test whether a model subcatchment belongs to a captured source component.
     * @param source Zero-based original subcatchment index.
     * @return True only when initialized and the stored mapping is nonnegative; false
     * for invalid/unselected indices. Does not inspect current source water or clocks. */
    bool ownsSubcatch(int source) const;
    /** @brief Borrow the original-subcatchment-to-source-group mapping.
     * @return Owned vector, -1 for unowned rows; empty before initialization. Valid
     * until destruction; initialization/replacement invalidates elements. Caller keeps
     * source model indices compatible and serializes access with state replacement. */
    const std::vector<int>& sourceGroups() const { return source_groups_; }
    /** @brief Check receiver node count and exact ordered node-name identity.
     * @param context Borrowed initialized receiving context; node-name table matches count.
     * @return Empty on exact compatibility, otherwise diagnostic. Does not validate
     * hydraulic geometry, physical state, flow/quality options or source ownership;
     * no receiver/bridge mutation occurs. */
    std::string validateReceiver(const SimulationContext& context) const;
    /** @brief Queue newly completed source outlet records at an accepted receiver boundary.
     * @param source Compatible original driver with immutable previously captured prefix,
     * no pending group trials and completed source horizons no later than accepted_end
     * except relative 1e-12 roundoff. All records must carry positive finite ft3 water,
     * valid owned source/node/interval and no pollutant mass.
     * @param accepted_end Finite elapsed seconds exactly equal receiverEnd(); a receiving
     * trial must not be staged. Capture publishes availability for the next interval,
     * not consumption in the already accepted step.
     * @return Empty on success, otherwise refusal/caught standard exception retaining
     * bridge state. Copies/validates a candidate, recomputes model identity, complete
     * prefix digest and per-node totals; does not consume pending or change receiver
     * clock/source water. Repeated capture of an unchanged prefix adds no duplicates. */
    std::string capture(const SourceWaterDriver& source, double accepted_end);
    /** @brief Offer every pending receipt once over the next contiguous receiving interval.
     * @param start Finite elapsed seconds exactly equal accepted receiverEnd().
     * @param end Finite later elapsed seconds with representable finite difference.
     * @return Empty after installing private per-node trial receipts and projected
     * delivered totals, otherwise refusal/caught standard exception retaining accepted
     * state. Rejects an existing trial. Runoff/drain remain separate ft3 volumes; total
     * rate is volume/(end-start) in cfs. Does not mutate receiving hydraulics or consume
     * pending. Caller applies these held rates, commits only after acceptance, or cancels. */
    std::string stage(double start, double end);
    /** @brief Accept the staged receiving interval and consume its pending source water.
     * @return Empty after moving trial receipts into last, advancing receiverEnd and
     * cumulative delivered totals, clearing pending/trial; refusal without an active
     * trial. Does not verify hydraulic acceptance or book receiver water itself; caller
     * must settle the corresponding hydraulic interval first. Invalidates trial and
     * pending element borrows. No source advancement/capture is performed here. */
    std::string commit();
    /** @brief Drop a receiving trial without consuming any accepted pending receipt.
     * Leaves accepted clocks/totals/last receipts unchanged; safe without a trial.
     * Invalidates receipts() borrows. Caller owns rollback of any private hydraulic trial. */
    void cancel() { trial_.reset(); }
    /** @brief Borrow staged per-node water offers in receiver index order.
     * @return Trial vector of separate runoff/drain ft3 and total cfs, valid until
     * cancel/commit/stage replacement/destruction. Never modify/free; copy as needed.
     * @throws std::logic_error No active receiving trial. Does not consume/commit water. */
    const std::vector<SourceNodeDelivery>& receipts() const;
    /** @brief Borrow the ordered captured-but-unconsumed delivery suffix.
     * @return Owned queue with original source interval/ft3, ordinal and availability
     * seconds. Capture/commit/checkpoint/replacement can invalidate elements. Empty
     * before initial capture or after successful consumption; read requires serialization. */
    const std::vector<QueuedSourceDelivery>& pending() const { return state_.pending; }
    /** @brief Borrow cumulative captured volumes per receiver node.
     * @return Runoff/drain ft3 totals, rate fields zero. Capture/checkpoint/replacement
     * can change storage; not staged/accepted hydraulic rates. Copy for a stable snapshot. */
    const std::vector<SourceNodeDelivery>& received() const { return state_.received; }
    /** @brief Borrow cumulative volumes consumed by accepted receiving intervals.
     * @return Per-node runoff/drain ft3 totals with rate zero. Excludes staged offers
     * until commit. Commit/checkpoint/replacement may invalidate element references. */
    const std::vector<SourceNodeDelivery>& delivered() const { return state_.delivered; }
    /** @brief Borrow exact receipts of the preceding accepted receiving interval.
     * @return Per-node runoff/drain ft3 and held total cfs over [lastStart(),receiverEnd()].
     * Initially zero rows after initialization. Commit/checkpoint/replacement invalidates
     * old elements; caller uses these for accepted held-rate continuity/report integration. */
    const std::vector<SourceNodeDelivery>& lastReceipts() const { return state_.last; }
    /** @brief Read the previous accepted receiving interval's opening time.
     * @return Elapsed seconds, zero initially. Meaningful with lastReceipts/receiverEnd;
     * staging/cancel alone do not change it. */
    double lastStart() const { return state_.last_start; }
    /** @brief Read the accepted receiving frontier.
     * @return Elapsed seconds, zero initially and advanced only by commit/restoration.
     * A staged end is not exposed as accepted by this accessor. */
    double receiverEnd() const { return state_.receiver_end; }
    /** @brief Read the length of the source delivery prefix captured by this bridge.
     * @return Original record count, including pending and already consumed records;
     * not volume, node count or number of receiving steps. Does not inspect live source. */
    uint64_t capturedCount() const { return state_.count; }
    /** @brief Stream private same-platform identity or accepted delivery restart state.
     * @param[in,out] archive Matching native CompletedSourceArchive reader/writer; byte
     * order/schema are not portable interchange. Caller finishes archive validation
     * before publishing restored engine/surface/source/bridge ownership together.
     * @param model True writes version-two model identity/mapping only; reading model
     * identity is refused. False streams version-two accepted state into a temporary.
     * @param source Already prepared matching source with every completed outlet
     * captured and every source group clock published at the bridge boundary.
     * @throws std::runtime_error Uninitialized/staged bridge, wrong version/model/prefix,
     * clock/order/shape/water closure or archive error. A failed read retains accepted
     * bridge state; the archive cursor/output bytes can be partially consumed/written.
     * Restore on a private bridge copy, validate the containing archive then publish
     * all paired owners together. No receiving/source water is booked by this method. */
    void checkpoint(detail::CompletedSourceArchive& archive, bool model,
                    const SourceWaterDriver& source);

private:
    struct State {
        uint64_t count = 0, digest = 14695981039346656037ULL;
        double receiver_end = 0;
        std::vector<double> source_ends;
        std::vector<QueuedSourceDelivery> pending;
        std::vector<SourceNodeDelivery> received, delivered, last;
        uint64_t last_begin = 0;
        double last_start = 0;
    } state_;
    struct Trial {
        double end = 0;
        std::vector<SourceNodeDelivery> receipts, delivered;
    };
    bool initialized_ = false;
    std::string source_model_;
    std::vector<std::string> node_names_;
    std::vector<int> source_groups_;
    std::optional<Trial> trial_;
    /** @brief Check complete ownership, immutable source prefix and water-ledger closure.
     * @param state Candidate accepted bridge state, with no independently staged trial.
     * @param source Compatible prepared driver with closed groups and append-only outlet
     * history; may contain uncaptured suffix except at the stricter checkpoint boundary.
     * @throws std::runtime_error Model/name/prefix/digest, count/clock/interval/order,
     * water-only positive record, availability or received=delivered+pending mismatch.
     * Reconstructs received/pending/last per-node totals and last rates, comparing with
     * relative finite tolerances. Re-serializes model and full prefix without mutating
     * either owner; allocation/archive exceptions propagate to the calling boundary. */
    void validate(const State& state, const SourceWaterDriver& source) const;
};
}
