// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin
#pragma once
#include "PassiveStorageAdmission.hpp"
#include "PassiveStorageExchange.hpp"
#include <memory>

namespace openswmm::twoD { struct SurfaceStateData; }
namespace openswmm { class SWMMEngine; namespace twoD { class SurfaceRouter2D; } }
namespace openswmm::quality {
class PeerPassiveStorageProducer;
struct PeerFundingToken;
struct PeerRecipientBinding;
class PeerFundingRecipient {
public:
    PeerFundingRecipient(const PeerFundingRecipient&) = delete;
    PeerFundingRecipient& operator=(const PeerFundingRecipient&) = delete;
private:
    friend class PeerPassiveStorageFunding;
    friend class PeerPassiveStorageProducer;
    explicit PeerFundingRecipient(std::shared_ptr<PeerRecipientBinding> binding) : binding_(std::move(binding)) {}
    std::shared_ptr<PeerRecipientBinding> binding_;
};
struct PeerFundingView {
    PassiveStorageExchange::FundingAllowance allowance;
    std::vector<PassiveStorageExchange::HydraulicPoint> points;
    // Native node columns sampled by the issuer; the consumer must already
    // have this published boundary. These are observations, not another solver.
    std::vector<double> head, volume, depth, full_volume, rpt_full_volume, ponded_area;
    std::vector<std::string> nodes;
    double si_to_native = 0;
    bool operator==(const PeerFundingView&) const = default;
};
// These native capsules cannot be copied or constructed from a diagnostic view.
// Their process-local single-use state is not a portable packet or wire authority.
class PeerFundingOffer {
public:
    PeerFundingOffer(const PeerFundingOffer&) = delete;
    PeerFundingOffer& operator=(const PeerFundingOffer&) = delete;
    const PeerFundingView& view() const noexcept { return view_; }
private:
    friend class PeerFundingReply;
    friend class PeerPassiveStorageFunding;
    friend class PeerPassiveStorageProducer;
    explicit PeerFundingOffer(std::shared_ptr<PeerFundingToken> token);
    PeerFundingView view_;
    std::shared_ptr<PeerFundingToken> token_;
};
class PeerFundingReply {
public:
    PeerFundingReply(const PeerFundingReply&) = delete;
    PeerFundingReply& operator=(const PeerFundingReply&) = delete;
    const PeerFundingView& view() const noexcept { return view_; }
    const PassiveStorageExchange::LiveProof& proof() const noexcept { return proof_; }
    const std::vector<PassiveStorageExchange::Row>& rows() const noexcept { return rows_; }
    const std::vector<NativeAmountSum>& spend() const noexcept { return spent_; }
private:
    friend class PeerPassiveStorageCheckpoint;
    friend class PeerPassiveStorageFunding;
    friend class PeerPassiveStorageProducer;
    explicit PeerFundingReply(const PeerFundingOffer& offer);
    PeerFundingView view_;
    PassiveStorageExchange::LiveProof proof_;
    std::vector<PassiveStorageExchange::Row> rows_;
    std::vector<NativeAmountSum> spent_;
    std::shared_ptr<PeerFundingToken> token_;
};
class PeerFundingAcknowledgement {
public:
    PeerFundingAcknowledgement(const PeerFundingAcknowledgement&) = delete;
    PeerFundingAcknowledgement& operator=(const PeerFundingAcknowledgement&) = delete;
private:
    friend class PeerPassiveStorageFunding;
    friend class PeerPassiveStorageProducer;
    explicit PeerFundingAcknowledgement(std::shared_ptr<PeerFundingToken> token) : token_(std::move(token)) {}
    std::shared_ptr<PeerFundingToken> token_;
};

// Private peer owner protocol. The network owns all inventory; the
// producer has no pointer/reference/callback to its context or receiving book.
// Engine attachment supplies native phase scheduling; portable packets and
// paired recovery remain gated.
// Caller precondition: one exclusive issuer for this network context for the
// entire session, including unresolved work. Nonces/capabilities do not arbitrate
// two independently constructed books over the same stock. Engine attachment
// enforces this ownership for engine callers.
class PeerPassiveStorageFunding {
public:
    using Identity = PassiveStorageExchange::PeerIdentity;
    using Phase = PassiveStorageExchange::Phase;
    static void validateProfile(const SimulationContext& context);
    PeerPassiveStorageFunding(SimulationContext& network, std::uint64_t issuer, std::uint64_t recipient,
        std::vector<PassiveStorageExchange::HydraulicPoint> points, double factor, std::size_t capacity = 4096);
    ~PeerPassiveStorageFunding();
    PeerPassiveStorageFunding(const PeerPassiveStorageFunding&) = delete;
    Identity identity() const noexcept { return identity_; }
    std::unique_ptr<PeerFundingRecipient> bindRecipient(const SimulationContext& surface);
    void freeze(double dt);
    void observeDrain();
    void receive(QualitySolver& solver, double dt);
    std::unique_ptr<PeerFundingOffer> issue(Phase phase, double start_ms, double end_ms);
    void accept(std::unique_ptr<PeerFundingReply> reply);
    void validateWaterBooking(const std::vector<double>& queue,const std::vector<double>& direct,double remaining) const;
    std::unique_ptr<PeerFundingAcknowledgement> confirmWater();
    void fail(const char* reason) noexcept;
    bool failed() const noexcept;
    bool unresolved() const noexcept;
    const char* error() const noexcept { return failure_ ? failure_ : "Peer funding receiving failed."; }
    const PassiveStorageExchange& receipts() const noexcept { return book_; }
    const PeerFundingReply* returnedReply() const noexcept { return reply_.get(); }
    std::array<double,2> lastWindow() const noexcept;
private:
    friend class PeerPassiveStorageCheckpoint;
    void ready() const;
    SimulationContext* context_;
    Identity identity_;
    std::vector<PassiveStorageExchange::HydraulicPoint> points_;
    double factor_;
    std::size_t capacity_;
    PassiveStorageExchange book_{PassiveStorageArithmetic::RETAIN_EXACT_SLICES,
        PassiveStorageLaw::CHANGING_VOLUME,PassiveStorageWaterArithmetic::RETAIN_AGGREGATES};
    std::shared_ptr<PeerFundingToken> token_;
    std::shared_ptr<PeerRecipientBinding> recipient_;
    std::unique_ptr<PeerFundingReply> reply_;
    const char* failure_ = nullptr;
    std::size_t committed_legs_ = 0;
};

// Restricted synchronous window handoff; authority never enters copyable frames.
class PeerPassiveStorageWindow {
public:
    virtual ~PeerPassiveStorageWindow() = default;
    virtual std::unique_ptr<PeerFundingOffer> ready(PassiveStorageExchange::Phase phase,
        double start_ms,double end_ms) = 0;
    virtual void returned(std::unique_ptr<PeerFundingReply> reply) = 0;
};

class PeerPassiveStorageProducer final : public PassiveStorageAdmission {
public:
    using Identity = PassiveStorageExchange::PeerIdentity;
    using Leg = PassiveStorageExchange::AppliedLeg;
    PeerPassiveStorageProducer(const SimulationContext& surface, const twoD::SurfaceStateData& state,
        std::unique_ptr<PeerFundingRecipient> recipient, double factor, std::size_t capacity = 4096);
    ~PeerPassiveStorageProducer() override;
    PeerPassiveStorageProducer(const PeerPassiveStorageProducer&) = delete;
    void begin(std::unique_ptr<PeerFundingOffer> offer);
    bool admit(std::size_t point, int cell, int node, double water, double mass,
        double before, double volume_before, double offset, double duration, double withdrawal = 0) noexcept override;
    void applied(double actual_after, double volume_after) noexcept override;
    std::unique_ptr<PeerFundingReply> seal(const std::vector<double>& hydraulic_exchange_si);
    void acknowledge(std::unique_ptr<PeerFundingAcknowledgement> acknowledgement);
    void fail(const char* reason) noexcept override;
    bool failed() const noexcept override;
    const char* error() const noexcept override { return failure_ ? failure_ : "Peer passive session failed."; }
    bool owns(const twoD::SurfaceStateData* state) const noexcept override { return state==state_; }
    const Leg& leg(std::size_t index) const { return legs_.at(index); }
    std::size_t legCount() const noexcept { return count_; }
    const Leg& attempted() const noexcept { return attempted_; }
    bool awaitingAcknowledgement() const noexcept { return awaiting_ack_; }
    const std::vector<NativeAmountSum>& spend() const noexcept { return spent_; }
private:
    friend class PeerPassiveStorageCheckpoint;
    friend class openswmm::twoD::SurfaceRouter2D;
    const SimulationContext* context_; // surface only
    const twoD::SurfaceStateData* state_;
    Identity identity_;
    double factor_;
    std::vector<Leg> legs_;
    std::size_t count_ = 0, first_ = 0;
    std::uint64_t last_ordinal_ = 0;
    std::unique_ptr<PeerFundingOffer> offer_;
    std::shared_ptr<PeerFundingToken> token_;
    std::shared_ptr<PeerRecipientBinding> recipient_;
    std::vector<NativeAmountSum> spent_;
    Leg attempted_;
    bool reserved_ = false, open_ = false, awaiting_ack_ = false;
    const char* failure_ = nullptr;
};

// Private CF7 prerequisite: receipt/funding owners only. This does not restore
// hydraulics, CPU/mesh inventories, scheduling or output, and confers no remote
// authority. A paired engine coordinator must prepare those domains separately.
class PeerPassiveStorageCheckpoint {
public:
    class Prepared {
    public:
        Prepared(Prepared&&) noexcept;
        Prepared& operator=(Prepared&&) noexcept;
        ~Prepared();
        // Call immediately before the numerical transaction installs anything.
        void validateTarget() const;
        // One-shot, on the exact pinned slots. Caller exclusively owns both slots
        // and address-stable contexts/state through preparation and installation;
        // all numerical state must already be validated and installed without throws.
        void install() noexcept;
    private:
        friend class PeerPassiveStorageCheckpoint;
        friend class openswmm::SWMMEngine;
        const PeerPassiveStorageProducer& producer() const;
        const PeerPassiveStorageFunding& funding() const;
        void rekey(std::uint64_t issuer, std::uint64_t recipient,
                   std::uint64_t archived_issuer, std::uint64_t archived_recipient);
        struct State;
        explicit Prepared(std::unique_ptr<State> state);
        std::unique_ptr<State> state_;
    };
    static std::string save(const PeerPassiveStorageFunding&, const PeerPassiveStorageProducer&);
    static Prepared prepare(std::unique_ptr<PeerPassiveStorageFunding>& fresh_issuer,
        std::unique_ptr<PeerPassiveStorageProducer>& fresh_producer,
        const SimulationContext& incoming_network, const SimulationContext& incoming_surface,
        const twoD::SurfaceStateData& incoming_state, const std::string& bytes);
private:
    friend class openswmm::SWMMEngine;
    static std::uint64_t recoveryIdentity();
    static void boundary(const PeerPassiveStorageFunding&, const PeerPassiveStorageProducer&);
    static void fresh(const PeerPassiveStorageFunding&, const PeerPassiveStorageProducer&);
    struct Journal;
    static Journal journal(const PeerPassiveStorageFunding&);
    static void surface(const PeerPassiveStorageFunding&, const SimulationContext&,
        const SimulationContext&, const twoD::SurfaceStateData&);
};
}
