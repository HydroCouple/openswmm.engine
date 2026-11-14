// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin
#ifndef OPENSWMM_LIVE_PASSIVE_STORAGE_EXCHANGE_HPP
#define OPENSWMM_LIVE_PASSIVE_STORAGE_EXCHANGE_HPP

#include "PassiveStorageExchange.hpp"
#include "PassiveStorageAdmission.hpp"
#include <cstddef>
#include <memory>

namespace openswmm::twoD {
struct CouplingPoint;
struct SurfaceStateData;
class SurfaceRouter2D;
}
namespace openswmm::quality {
/** Private, explicitly attached first-order CPU owner. No public packet or SDK dependency.
 * A bounded journal is allocated before marching. Admission reserves gross spill
 * before any native leg effect; an unsuccessful window is terminal and inspectable.
 * The fixed-volume law accepts only exactly zero-net, representable windows.
 * The changing-volume law separately authenticates actual native water booking
 * and retains settled funding through pre-quality hydraulic volume changes.
 * Paired physical restart uses the closed no-source native profile. Output
 * continuation, public hotstarts and component activation remain separate gates.
 */
class LivePassiveStorageExchange : public PassiveStorageAdmission {
public:
    using Phase = PassiveStorageExchange::Phase;
    using Leg = PassiveStorageExchange::AppliedLeg;
    LivePassiveStorageExchange(const SimulationContext& ctx,
                               const twoD::SurfaceStateData& state,
                               const std::vector<twoD::CouplingPoint>& points,
                               double si_to_native, std::size_t capacity = 4096,
                               PassiveStorageArithmetic arithmetic = PassiveStorageArithmetic::EXACT, bool preflight_history = false,
                               PassiveStorageLaw law = PassiveStorageLaw::FIXED_VOLUME,
                               PassiveStorageWaterArithmetic water = PassiveStorageWaterArithmetic::EXACT);
    LivePassiveStorageExchange(const LivePassiveStorageExchange&) = delete;
    LivePassiveStorageExchange& operator=(const LivePassiveStorageExchange&) = delete;
    /// Opt-in conservative archive admission. A refusal preserves the accepted owner.
    /// Reserve one possible producer window, including changing-volume empty publications.
    bool preflightsHistory() const noexcept { return preflight_history_; }
    bool preflightHistory(const SimulationContext& ctx, bool next_delivery) noexcept;
    const std::string& historyPreflightError() const noexcept { return history_error_; }
    void freeze(const SimulationContext& ctx, double dt);
    void observeDrain(const SimulationContext& ctx);
    void receive(SimulationContext& ctx, QualitySolver& solver, double dt);
    void beginWindow(const SimulationContext& ctx, const twoD::SurfaceStateData& state,
                     double start_ms, double end_ms, Phase phase);
    /// Same actual operands the native writer will use. Allocation-free, before all leg effects.
    bool admit(std::size_t point, int cell, int node, double signed_water_si,
               double actual_mass_si, double before, double volume_before,
               double offset_s, double duration_s, double withdrawal_si = 0) noexcept override;
    /// Called only after the native leg was applied. Allocation-free and nonthrowing.
    void applied(double actual_after, double volume_after) noexcept override;
    void seal(const SimulationContext& ctx, const std::vector<double>& hydraulic_exchange_si);
    void observeWaterBooking(const SimulationContext& ctx);
    void fail(const char* reason) noexcept override;
    bool failed() const noexcept override { return failure_ != nullptr || book_.failed(); }
    const char* error() const noexcept override { return failure_ ? failure_ : "Live passive exchange receiving failed."; }
    bool windowOpen() const noexcept { return open_; }
    std::size_t legCount() const noexcept { return count_; }
    const Leg& leg(std::size_t index) const { return legs_.at(index); }
    const Leg& attempted() const noexcept { return attempted_; }
    const PassiveStorageExchange& receipts() const noexcept { return book_; }
    std::string checkpoint(const SimulationContext& ctx, const twoD::SurfaceStateData& state) const;
    std::unique_ptr<LivePassiveStorageExchange> prepareRestore(
        const SimulationContext& incoming, const twoD::SurfaceStateData& surface,
        const std::string& bytes) const;
    /// Internal staged setup, before the first live window only; shares the same funding owner.
    void reserveInitial(const SimulationContext& ctx, std::vector<PassiveStorageExchange::Row> rows);
private:
    friend class twoD::SurfaceRouter2D;
    struct Point { int cell, node; };
    struct Budget { int node; double volume, depth, concentration; NativeAmountSum sealed, open; };
    void requireReady() const;
    void validateSurface(const twoD::SurfaceStateData& state) const;
    void rebind(const SimulationContext& ctx) noexcept { context_ = &ctx; if (book_.owner_) book_.owner_ = &ctx; }
    void installFrom(LivePassiveStorageExchange& candidate, const SimulationContext& ctx) noexcept;
    const SimulationContext* context_;
    std::vector<Point> points_;
    std::vector<Budget> budgets_;
    std::map<int, PassiveStorageExchange::Sample> initial_pools_;
    std::vector<Leg> legs_;
    std::size_t count_ = 0, first_ = 0;
    double factor_, start_ms_ = 0, end_ms_ = 0;
    Phase phase_ = Phase::ADVANCE;
    bool open_ = false, reserved_ = false, ever_opened_ = false;
    const char* failure_ = nullptr;
    Leg attempted_;
    std::uint64_t next_id_ = 1;
    double initial_loss_ = 0, initial_gain_ = 0, initial_total_ = 0, attached_ms_ = 0;
    PassiveStorageExchange book_;
    bool preflight_history_ = false;
    std::string history_error_; // transient diagnostic, outside physical checkpoint state
};
}
#endif
