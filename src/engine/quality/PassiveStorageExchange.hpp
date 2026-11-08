// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin
#ifndef OPENSWMM_PASSIVE_STORAGE_EXCHANGE_HPP
#define OPENSWMM_PASSIVE_STORAGE_EXCHANGE_HPP

#include "../data/PollutantData.hpp"
#include "NativeAmountSum.hpp"
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace openswmm {
struct SimulationContext;
namespace quality {
class QualitySolver;
class PeerPassiveStorageFunding;
enum class PassiveStorageLaw : std::uint8_t { FIXED_VOLUME, CHANGING_VOLUME };
enum class PassiveStorageWaterArithmetic : std::uint8_t { EXACT, RETAIN_AGGREGATES };
enum class PassiveStorageArithmetic : std::uint8_t { EXACT, RETAIN_ROUNDOFF, RETAIN_AGGREGATES, RETAIN_POINT_AGGREGATES, RETAIN_EXACT_HALVES, RETAIN_EXACT_SLICES };
inline bool retainsPointAggregates(PassiveStorageArithmetic policy) noexcept {
    return policy == PassiveStorageArithmetic::RETAIN_POINT_AGGREGATES || policy == PassiveStorageArithmetic::RETAIN_EXACT_HALVES ||
        policy == PassiveStorageArithmetic::RETAIN_EXACT_SLICES;
}
inline bool retainsMaterialAggregates(PassiveStorageArithmetic policy) noexcept {
    return policy == PassiveStorageArithmetic::RETAIN_AGGREGATES || retainsPointAggregates(policy);
}

/**
 * @brief Funded native receiving input for isolated passive storage, one pollutant.
 *
 * One exclusive context/quality owner retains this noncopyable book until all
 * receipts settle. reserve() precedes the staged surface credit. Its caller
 * supplies native accepted drain amounts authenticated by the surface donor;
 * this book funds only the gross network spill. Water is accepted metadata in
 * ft3; amounts are native pollutant concentration*ft3, with no unit conversion.
 * Native node-summed drain/spill water must agree exactly. The existing quality
 * solver is the sole inventory writer. No clock or hydraulic state is advanced.
 * Gross/material aggregates must be exactly representable native doubles;
 * unsupported aggregate arithmetic is refused without a comparison tolerance.
 * Stored increments and gross slice partitions must also be representable.
 * Ordinary concentration division/product roundoff is bounded by two machine
 * epsilons of the target amount; an entire nonzero increment may not disappear.
 * The explicitly selected RETAIN_ROUNDOFF research policy owns the exact local
 * stored rounding difference instead of refusing an inexact stored addition.
 * It never corrects the mixer amount or supplies additional gross donor mass.
 * RETAIN_AGGREGATES also retains exact node material sums across represented
 * rows, correctly rounding only the physical mixer input. Point totals and
 * water/partial partitions remain strict. RETAIN_POINT_AGGREGATES additionally
 * retains authenticated exact point totals across actual firings. Multipart
 * material requires a whole native flush. RETAIN_EXACT_HALVES additionally
 * permits one authenticated equal-half partition per original native leg.
 * Prior policies retain their behavior.
 *
 * CHANGING_VOLUME is a separate explicitly selected law with exact-half
 * arithmetic. Every isolated mapped pool remains bound, including empty cohorts.
 * The sole quality writer mixes settled C*V plus the rounded material slice over
 * the observed new hydraulic volume. The exact difference stays in R; gross
 * spill uses settled physical and accountable inventory. Native water assembly
 * and ordered producer booking have separate bounded observation histories.
 * The separate RETAIN_AGGREGATES water policy retains exact directional totals
 * from authenticated native legs without changing hydraulic queue arithmetic.
 *
 * Staged and privately authenticated live inputs share this native receiving
 * book. Live proofs preserve actual SI donor operands and one native conversion;
 * they do not enter the legacy queue or component packets. Pending receipts cannot
 * be copied, discarded for retry, or used by another context. A failed receiving execution
 * is terminal; pending rows and the attempted native interval remain inspectable.
 */
class PassiveStorageExchange {
public:
    enum class Phase : std::uint8_t { SETTLED, ADVANCE, REPORT, FINAL };
    struct Sample {
        double volume, depth, concentration;
        bool operator==(const Sample&) const = default;
    };
    struct Publication {
        Phase phase = Phase::SETTLED;
        double capture_ms = 0, batch_start_ms = 0, batch_end_ms = 0;
        double eligible_ms = 0, remaining = 0;
        std::uint64_t generation = 0;
        bool operator==(const Publication&) const = default;
    };
    struct Row {
        std::uint64_t point = 0;
        int node = -1;
        std::string pollutant;
        MassUnits units = MassUnits::MG_PER_L;
        double drain_water = 0.0, spill_water = 0.0;
        double drain_mass = 0.0, spill_mass = 0.0;
        // Empty supplement: the scalar is authoritative. Multipart supplement:
        // canonical exact total is authoritative and scalar must be zero.
        std::vector<double> drain_mass_parts, spill_mass_parts;
        std::vector<double> drain_water_parts, spill_water_parts;
        NativeAmountSum materialAmount(bool drain) const;
        void setMaterialAmount(bool drain, const NativeAmountSum& amount);
        NativeAmountSum waterAmount(bool drain) const;
        void setWaterAmount(bool drain, const NativeAmountSum& amount);
        bool operator==(const Row&) const = default;
    };
    struct AppliedLeg {
        std::uint64_t point = 0;
        int cell = -1, node = -1;
        bool drain = false, applied = false;
        double water_si = 0, mass_si = 0, water_native = 0, mass_native = 0;
        double before = 0, after = 0, sample = 0;
        double batch_start_ms = 0, batch_end_ms = 0;
        Phase phase = Phase::ADVANCE;
        double volume_before = 0, volume_after = 0, offset_s = 0, duration_s = 0, withdrawal_si = 0;
        bool operator==(const AppliedLeg&) const = default;
    };
    struct HydraulicPoint {
        std::uint64_t point = 0;
        int cell = -1, node = -1;
        double exchange_si = 0;
        bool operator==(const HydraulicPoint&) const = default;
    };
    struct LiveProof {
        double si_to_native = 0;
        std::vector<AppliedLeg> legs;
        std::vector<HydraulicPoint> points;
        bool operator==(const LiveProof&) const = default;
    };
    struct Receipt {
        std::uint64_t id;
        std::vector<Row> accepted, remaining;
        std::vector<Sample> samples;
        Publication publication;
        bool authenticated = false;
        LiveProof proof;
        std::uint8_t half_state = 0; // retained after settlement; reconstructed through delivery replay
        // Current remainders in immutable proof-leg order; owned only by exact slices.
        std::vector<double> remaining_water_legs, remaining_mass_legs;
        bool operator==(const Receipt&) const = default;
    };
    struct Attempt {
        double start_ms = 0.0, dt = 0.0, remaining_start = 0.0;
    };
    // Private native authority; read-only diagnostics are not a transferable grant.
    struct FundingPool {
        int node;
        Sample settled;
        double published_concentration;
        NativeAmountSum roundoff, sealed, granted, spent;
        bool operator==(const FundingPool&) const = default;
    };
    struct PeerIdentity {
        std::uint64_t issuer = 0, recipient = 0, session = 0;
        bool operator==(const PeerIdentity&) const = default;
    };
    struct FundingAllowance {
        std::uint64_t ordinal, receipt, generation;
        Phase phase;
        double capture_ms, start_ms, end_ms, remaining;
        std::string pollutant;
        MassUnits units;
        std::size_t leg_limit;
        std::vector<FundingPool> pools;
        std::vector<AppliedLeg> spills; // bounded, expected operands in native firing order
        bool reconciled = false;
        PeerIdentity peer;
        bool peer_reported = false; // receipt information, never a local admission witness
        bool operator==(const FundingAllowance&) const = default;
    };
    const FundingAllowance* fundingAllowance() const noexcept { return funding_ ? &*funding_ : nullptr; }
    bool materialSealPrepared() const noexcept { return seal_.has_value(); }

    explicit PassiveStorageExchange(PassiveStorageArithmetic arithmetic = PassiveStorageArithmetic::EXACT,
                                    PassiveStorageLaw law = PassiveStorageLaw::FIXED_VOLUME,
                                    PassiveStorageWaterArithmetic water = PassiveStorageWaterArithmetic::EXACT)
        : arithmetic_(arithmetic), law_(law), water_arithmetic_(water) {}
    PassiveStorageExchange(const PassiveStorageExchange&) = delete;
    PassiveStorageExchange& operator=(const PassiveStorageExchange&) = delete;
    PassiveStorageExchange(PassiveStorageExchange&&) = delete;
    PassiveStorageExchange& operator=(PassiveStorageExchange&&) = delete;

    /// Preflight a whole point batch, then reserve gross spill atomically.
    /// Validation/funding refusal leaves prior receipts and native state intact.
    void reserve(const SimulationContext& ctx, std::uint64_t id, std::vector<Row> rows);
    /// Actual post-quality REPORT/FINAL publication at an accepted boundary.
    void reserveAtBoundary(const SimulationContext& ctx, std::uint64_t id, std::vector<Row> rows,
                           Phase phase, double batch_start_ms, double batch_end_ms);
    /// Freeze BEFORE native lateral assembly; an empty first cohort is valid.
    void freezeDelivery(const SimulationContext& ctx, double dt);
    /// Capture the actual post-drain span BEFORE any current producer fires.
    void observeDeliveryDrain(const SimulationContext& ctx);
    /// Pre-quality ADVANCE publication; eligible at the frozen interval's end.
    /// This bounded input cannot reset/extend the observed native water span.
    void reserveNext(const SimulationContext& ctx, std::uint64_t id, std::vector<Row> rows,
                     double batch_start_ms, double batch_end_ms);
    bool deliveryFrozen() const noexcept { return frozen_.has_value(); }
    bool deliveryDrainObserved() const noexcept { return frozen_ && frozen_->observed; }
    /// Sealed owner archive only. Does not save/restore native water/quality
    /// itself or attach a full-engine checkpoint. Restore requires a pristine
    /// owner and matching restored native state; refusal is atomic/reusable.
    /// Empty bytes mean absence of a bound owner/archive, not a saved policy;
    /// restoring them preserves the destination's explicitly selected policy.
    std::string checkpoint(const SimulationContext& ctx) const;
    void restore(const SimulationContext& ctx, const std::string& bytes);
    const std::vector<Receipt>& receipts() const noexcept { return receipts_; }
    double pendingDrain(int node) const;
    double pendingSpill(int node) const;
    NativeAmountSum pendingDrainAmount(int node) const;
    NativeAmountSum pendingSpillAmount(int node) const;
    PassiveStorageArithmetic arithmetic() const noexcept { return arithmetic_; }
    PassiveStorageLaw law() const noexcept { return law_; }
    PassiveStorageWaterArithmetic waterArithmetic() const noexcept { return water_arithmetic_; }
    bool retainsWaterAggregates() const noexcept { return water_arithmetic_ == PassiveStorageWaterArithmetic::RETAIN_AGGREGATES; }
    bool exactSlices() const noexcept { return arithmetic_ == PassiveStorageArithmetic::RETAIN_EXACT_SLICES; }
    bool changingVolume() const noexcept { return law_ == PassiveStorageLaw::CHANGING_VOLUME; }
    /// Bind every mapped pool before the first changing-volume delivery. Its
    /// settled inventory remains owned through empty cohorts and hydraulic tails.
    void bindChangingVolumePools(const SimulationContext& ctx, const std::vector<int>& nodes);
    const NativeAmountSum& storedRoundoff(int node) const noexcept;
    const Sample* settledPool(int node) const noexcept {
        const auto it = pools_.find(node); return it == pools_.end() ? nullptr : &it->second;
    }
    /// Both physical and exact accountable inventory must fund this gross sum.
    bool canFund(int node, double physical_mass, double gross_spill) const noexcept;
    bool canFund(int node, double physical_mass, const NativeAmountSum& gross_spill) const noexcept;
    bool failed() const noexcept { return failed_; }
    const Attempt& attempt() const noexcept { return attempt_; }

private:
    friend class QualitySolver;
    friend class LivePassiveStorageExchange;
    friend class PeerPassiveStorageFunding;
    friend class PeerPassiveStorageCheckpoint;
    // Only the acknowledged peer coordinator may bypass the standalone peer guard.
    std::string checkpointSealed(const SimulationContext& ctx) const;
    void restoreSealed(const SimulationContext& ctx, const std::string& bytes,
                       const std::map<int, Sample>* closed_initial);
    using Pool = Sample;
    // Columnar observations of native hydraulics. These are inputs to material
    // replay, not a second hydraulic solver or a claim of historical authenticity.
    struct WaterState {
        std::vector<double> volume, old_volume, depth, old_depth, inflow, outflow,
            old_net, old_lateral, old_inflow, rate, queue;
        bool operator==(const WaterState&) const = default;
    };
    struct Delivery {
        double start_ms, dt, remaining_start, remaining_after;
        std::uint64_t cutoff;
        bool frozen;
        double drained_span = 0;
        WaterState before, assembled, received;
        bool operator==(const Delivery&) const = default;
    };
    struct Frozen {
        double start_ms, dt, end_ms, remaining_start, remaining_after;
        std::uint64_t cutoff;
        bool observed = false;
        WaterState before, assembled;
    };
    struct WaterPublication {
        Publication publication;
        std::uint64_t receipt = 0;
        double dt = 0, remaining_after = 0, si_to_native = 0;
        std::vector<HydraulicPoint> points;
        std::vector<double> before, after;
        std::vector<NativeAmountSum> booking_roundoff;
        bool operator==(const WaterPublication&) const = default;
    };
    struct Prepared {
        std::vector<Receipt> receipts;
        std::vector<double> amount, concentration;
        std::vector<double> volume, depth;
        std::vector<unsigned char> affected;
        std::map<int, NativeAmountSum> roundoff;
        double end_ms, remaining;
        Delivery delivery;
    };
    struct PreparedSeal {
        std::vector<Receipt> receipts;
        std::map<int, Pool> pools;
        std::vector<std::string> node_ids, pollutant_ids;
        std::uint64_t last_id;
        double next_start_ms, remaining;
    };
    void grantFunding(const SimulationContext& ctx, std::uint64_t receipt, Phase phase,
                      double start_ms, double end_ms, std::size_t leg_capacity);
    bool spendFunding(const SimulationContext& ctx, const AppliedLeg& leg) noexcept;
    void reconcileFunding(const SimulationContext& ctx, const LiveProof& proof,
                          Phase phase, double start_ms, double end_ms) const;
    void reserveImpl(const SimulationContext& ctx, std::uint64_t id, std::vector<Row> rows,
                     Publication publication, const LiveProof* proof = nullptr);
    void reserveAuthenticated(const SimulationContext& ctx, std::uint64_t id, std::vector<Row> rows,
                              Phase phase, double batch_start_ms, double batch_end_ms,
                              const LiveProof& proof);
    void validateProof(const SimulationContext& ctx, const std::vector<Row>& rows,
                       const Publication& publication, const LiveProof& proof) const;
    void restoreImpl(const SimulationContext& ctx, const std::string& bytes,
                     const std::map<int, Sample>* closed_initial);
    void validateChangingReplay(const SimulationContext& ctx) const;
    struct ReplaySize {
        std::size_t deliveries = 0, receipts = 0, rows = 0, nodes = 0, proof_items = 0, material_parts = 0, proof_legs = 0,
            water_publications = 0, water_points = 0, water_parts = 0;
    };
    ReplaySize replaySize(std::size_t deliveries) const;
    static void checkReplaySize(const SimulationContext& ctx, const ReplaySize& size,
                                PassiveStorageArithmetic arithmetic);
    void validate(const SimulationContext& ctx) const;
    void validatePool(const SimulationContext& ctx, int node, const Pool& pool,
                      bool assembled) const;
    WaterState waterState(const SimulationContext& ctx) const;
    void installWaterState(SimulationContext& ctx, const WaterState& state) const;
    void validateWaterTransition(const SimulationContext& ctx) const;
    void beginWaterPublication(const SimulationContext& ctx, const LiveProof& proof,
                               Phase phase, double start_ms, double end_ms, double dt);
    void finishWaterPublication(const SimulationContext& ctx);
    void begin(const SimulationContext& ctx, double dt);
    std::vector<Row> exactHalfRows(const Receipt& receipt, double span, double dt) const;
    std::vector<Row> exactSliceRows(Receipt& receipt, double span, double dt) const;
    void prepare(const SimulationContext& ctx, double dt);
    void commit(const SimulationContext& ctx);
    double receivingConcentration(const Pool& pool, double amount, NativeAmountSum& next_roundoff) const;
    double receivingConcentration(const Pool& pool, const NativeAmountSum& amount,
                                  NativeAmountSum& next_roundoff, double& mixer_amount) const;
    double changingConcentration(const Pool& pool, double volume, const NativeAmountSum& amount,
                                 NativeAmountSum& next_roundoff, double& mixer_amount) const;
    void fail() noexcept { failed_ = true; }

    const SimulationContext* owner_ = nullptr;
    std::vector<std::string> node_ids_, pollutant_ids_;
    MassUnits units_ = MassUnits::MG_PER_L;
    std::vector<Receipt> receipts_;
    std::map<int, Pool> pools_;
    std::map<int, NativeAmountSum> roundoff_;
    PassiveStorageArithmetic arithmetic_;
    PassiveStorageLaw law_;
    PassiveStorageWaterArithmetic water_arithmetic_;
    std::map<int, Pool> initial_pools_;
    std::vector<WaterPublication> water_publications_;
    std::optional<WaterPublication> pending_water_;
    std::optional<FundingAllowance> funding_;
    std::optional<PreparedSeal> seal_;
    std::optional<PeerIdentity> peer_binding_; // private session; paired recovery is not qualified
    std::optional<LiveProof> peer_received_;
    std::optional<Prepared> prepared_;
    std::optional<Frozen> frozen_;
    std::vector<Delivery> deliveries_;
    std::uint64_t last_id_ = 0;
    double next_start_ms_ = 0.0, remaining_ = 0.0;
    bool failed_ = false;
    Attempt attempt_;
};
} // namespace quality
} // namespace openswmm
#endif
