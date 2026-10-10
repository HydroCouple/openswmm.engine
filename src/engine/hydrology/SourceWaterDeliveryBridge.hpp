// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin
#pragma once
#include "SourceWaterDriver.hpp"
#include <cstdint>
#include <optional>

namespace openswmm::runoff {
struct QueuedSourceDelivery {
    uint64_t ordinal = 0;
    double available = 0; // accepted receiving boundary, seconds
    SourceWaterDelivery water; // original completed source interval and ft3
};
struct SourceNodeDelivery {
    double runoff = 0, drain = 0; // ft3, kept separate for hydraulic continuity
    double rate = 0; // total ft3/s over the staged receiving interval
};

/// Internal water-only qualification boundary between completed spatial source
/// graphs and hydraulic routing. Ownership covers whole weakly connected graphs.
/// Capture publishes completed outlet volumes after an accepted receiving step;
/// stage offers them once to the next step, at volume / receiving duration.
/// Commit must follow hydraulic acceptance; cancel leaves every receipt pending.
/// No legacy runoff/LID ownership is changed here. Live engine attachment still
/// requires suppression of those duplicate source kernels and paired continuity.
/// This whole-copy bridge does not qualify production scheduling/performance.
class SourceWaterDeliveryBridge {
public:
    std::string initialize(const SourceWaterDriver&);
    bool ownsSubcatch(int source) const;
    const std::vector<int>& sourceGroups() const { return source_groups_; }
    std::string validateReceiver(const SimulationContext&) const;
    std::string capture(const SourceWaterDriver&, double accepted_end);
    std::string stage(double start, double end);
    std::string commit();
    void cancel() { trial_.reset(); }
    const std::vector<SourceNodeDelivery>& receipts() const;
    const std::vector<QueuedSourceDelivery>& pending() const { return state_.pending; }
    const std::vector<SourceNodeDelivery>& received() const { return state_.received; }
    const std::vector<SourceNodeDelivery>& delivered() const { return state_.delivered; }
    double receiverEnd() const { return state_.receiver_end; }
    uint64_t capturedCount() const { return state_.count; }
    /// Private same-platform IO against the already prepared matching source.
    /// Restore on a private bridge copy, finish the containing archive, then
    /// publish both prepared components together. No production restart caller.
    void checkpoint(detail::CompletedSourceArchive&, bool model, const SourceWaterDriver&);
private:
    struct State {
        uint64_t count = 0, digest = 14695981039346656037ULL;
        double receiver_end = 0;
        std::vector<double> source_ends;
        std::vector<QueuedSourceDelivery> pending;
        std::vector<SourceNodeDelivery> received, delivered;
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
    void validate(const State&, const SourceWaterDriver&) const;
};
}
