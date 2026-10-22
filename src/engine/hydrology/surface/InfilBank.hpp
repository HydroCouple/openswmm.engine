// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin

/**
 * @file      InfilBank.hpp
 * @brief     Infiltration state and operations for surface water owners.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#pragma once
#include "../Infiltration.hpp"
#include <vector>

namespace openswmm::detail {
class CompletedSourceArchive;
}

namespace openswmm {
struct SimulationContext;
}
namespace openswmm::surface {

/// One state owner and one dispatch for subcatchments and mesh cells. Kernels
/// retain their legacy feet arithmetic; the SI boundary converts only once.
/// Initialize size then configure every BANK-owned entry before evaluation.
/// Mutations are immediate and shared use must be serialized; evaluate a private
/// copy for rejected/accepted trials. Kernels do not debit an external water store.
class InfilBank {
public:
    enum class Owner { BANK, EXTERNAL };
    struct Factors {
        double infiltration = 1.0, recovery = 1.0;
    };
    /** @brief Clear all ownership/history and allocate n default method entries.
     * @param n Nonnegative entry count with allocatable storage, unchecked conversion.
     * Methods default Horton, owners BANK, constants/cumulative zero; per-model state
     * arrays remain empty until setMethod(). Calling a default Horton rate before its
     * configuration is unsupported. Allocation can leave partially initialized arrays;
     * no physical validation or external state mutation occurs. */
    void init(int n);
    /** @brief Select a method and convert its authored parameters for one entry.
     * @param e Valid initialized entry index, unchecked.
     * @param method Supported InfilModel; stored even for an unknown enum, which later
     * dispatches no kernel and yields zero.
     * @param p Readable five-double authored tuple. Horton: f0/fmin project rainfall
     * rate, decay per hour, drying days, max inches/mm. Green-Ampt: suction inches/mm,
     * Ks project rainfall rate, moisture fraction; final slots unused. Curve Number:
     * CN in slot zero, drying days slot two; other slots unused. CONSTANT: project
     * rainfall rate slot zero. Arrays are borrowed only during the call.
     * @param options Valid project flow-unit options for native ft/ft/s conversion.
     * Lazily allocates a full-sized method state bank. Does not reset ownership,
     * cumulative metres or all previously retained per-method history (e.g. Horton Fmh,
     * Green-Ampt T follow native initializer behavior). Caller validates parameters and
     * uses a fresh bank for fresh state; allocation failures can follow method assignment. */
    void setMethod(int e, InfilModel method, const double p[5], const SimulationOptions& options);
    /** @brief Set the exclusive infiltration-state owner for one entry.
     * @param e Valid initialized index, unchecked.
     * @param owner BANK enables local kernel updates; EXTERNAL suppresses rate/history
     * and pack/unpack payloads. Does not transfer physical water or clear existing history.
     * Caller serializes this ownership change with external solver and local evaluations. */
    void setOwner(int e, Owner owner) { owners_[e] = owner; }
    /** @brief Read the stored state-owner marker.
     * @param e Valid initialized index, unchecked.
     * @return BANK/EXTERNAL as last set, without qualifying another solver or timestep. */
    Owner owner(int e) const { return owners_[e]; }
    /** @brief Advance one bank-owned kernel through the SI boundary.
     * @param e Valid configured index with matching method state and stable ownership.
     * @param precip_m_s Finite nonnegative precipitation m/s.
     * @param runon_m_s Finite nonnegative runon m/s; CN treats it as ponded supply.
     * @param depth_m Finite nonnegative available ponded metres.
     * @param dt Finite positive seconds; nonpositive returns zero without kernel advance.
     * @param factors Finite physical conductivity/recovery multipliers; Green-Ampt
     * requires a positive conductivity factor for finite upper-zone division behavior.
     * @return Nonnegative m/s converted from rateFeet. EXTERNAL entries return zero.
     * Advances owned model history and cumulative infiltrated metres immediately; does
     * not debit a donor/store or provide rollback. Caller supplies accepted/trial water
     * and validates numerical inputs; flooring the returned SI value is not state repair. */
    double rate(int e, double precip_m_s, double runon_m_s, double depth_m, double dt,
                Factors factors);
    // Avoid a feet -> SI -> feet roundtrip on the existing runoff path.
    /** @brief Advance one configured kernel without a legacy feet-to-SI-to-feet round trip.
     * @param e Valid configured entry index and method state, unchecked.
     * @param precip Finite nonnegative rainfall ft/s.
     * @param runon Finite nonnegative incoming ft/s; CN adds runon*dt to ponded feet.
     * @param depth Finite nonnegative available feet.
     * @param dt Finite positive seconds; nonpositive suppresses all updates.
     * @param factors Conductivity/recovery multipliers with native-kernel admissible ranges.
     * @return Native kernel ft/s, unaltered; zero if EXTERNAL/nonpositive dt. Adds
     * max(0,rate*0.3048)*dt to cumulative metres even on this feet API. No explicit
     * finite validation, rollback, physical-water debit or failure diagnostic. */
    double rateFeet(int e, double precip, double runon, double depth, double dt, Factors factors);
    /** @brief Export one legacy dynamic-state payload without changing history.
     * @param e Entry index; invalid yields method zero and six zeros.
     * @param[out] method Stored InfilModel integer for valid entry, including EXTERNAL.
     * @param[out] state Required writable six-double array, always zero-filled first.
     * Horton slots tp/Fe/Fmh; Green-Ampt IMD/F/Fu/T/saturated(0 or1); CN S/Se/P/F/f/T.
     * Native dimensions are seconds/feet/ft/s/fractions as the model fields define.
     * EXTERNAL/CONSTANT/unknown methods leave zero payload. Assumes method state exists;
     * parameters, cumulative metres and ownership are not encoded. noexcept is not
     * bounds or buffer validation; caller provides compatible initialized storage. */
    void pack(int e, int& method, double state[6]) const noexcept;
    /** @brief Restore one matching bank-owned legacy dynamic-state payload.
     * @param e Entry index; invalid is a no-op.
     * @param method Must equal this entry's initialized method, otherwise no-op.
     * @param state Readable six-double payload in pack() slot/native-unit order. Any
     * nonzero Green-Ampt saturation slot means true; no finite/range validation occurs.
     * EXTERNAL entries ignore input; CONSTANT/unknown dispatch nothing. Parameters,
     * method, cumulative metres and owner remain unchanged. Caller validates a matching
     * model/buffer and serializes restoration; noexcept does not supply rollback/checks. */
    void unpack(int e, int method, const double state[6]) noexcept;
    /** @brief Borrow per-entry integrated infiltration depth in metres.
     * @return Owned vector, updated by either rate API and reset by init/clear. Not an
     * infiltrated volume or a physical-store debit. Resizing/move/checkpoint may invalidate
     * elements; serialize reading and copy snapshots needed across mutation. */
    const std::vector<double>& cumulative() const noexcept { return cumulative_; }
    /** @brief Release all method/owner/parameter/history/cumulative entries.
     * Invalidates all element borrows; no external solver/water store is reset. Safe
     * for an empty bank. Call init/setMethod before further indexed rate/owner access. */
    void clear();
    /** @brief Stream private native infiltration model or runtime state.
     * @param[in,out] archive Matching CompletedSourceArchive reader/writer at the paired
     * schema position; can partially mutate/emit before an archive error.
     * @param model True streams method/owner/constant arrays only; false streams fixed
     * cumulative metres and model-specific native parameter/history sequences.
     * Does not itself qualify the loaded model/state. Restore an isolated compatible
     * bank, call validateCheckpoint(), finish the outer archive, then publish paired
     * owners. No accepted clock/store change or portable interchange guarantee. */
    void checkpoint(detail::CompletedSourceArchive& archive,
                    bool model); // internal accepted-boundary IO
    /** @brief Check restored method/material identity and selected physical state ranges.
     * @param model Compatible independently qualified original bank. Requires equal
     * methods/owners/constants and per-method vector sizes, exact parameter identity;
     * checks finite nonnegative cumulative values, Horton/CN states and selected
     * Green-Ampt moisture/depth constraints. Green-Ampt timer may be negative.
     * @throws std::runtime_error Any checked mismatch. Does not change either bank or
     * validate every cross-array length, owner enum/physical model relationship; cumulative
     * vector length equality is not checked here. Outer archive owns shape validation. */
    void validateCheckpoint(const InfilBank& model) const;

private:
    std::vector<InfilModel> methods_;
    std::vector<Owner> owners_;
    std::vector<HortonState> horton_;
    std::vector<GreenAmptState> grnampt_;
    std::vector<CurveNumState> curvenum_;
    std::vector<double> constant_, cumulative_;
    /** @brief Dispatch one native ft/s kernel with local pattern factors.
     * @param e Valid configured entry with compatible allocated per-method state; unchecked.
     * @param precip Finite nonnegative rainfall ft/s.
     * @param runon Finite nonnegative incoming ft/s; added to rainfall except CN, where
     * runon*dt augments ponded depth while precip remains the event rainfall input.
     * @param depth Finite nonnegative ponded feet.
     * @param dt Finite positive seconds; no local step/owner guard.
     * @param factors Physical infiltration/recovery multipliers. Horton temporarily scales
     * f0/fmin/regen then restores bases; Green-Ampt applies factors inside its kernel;
     * CN uses recovery only; CONSTANT scales capacity.
     * @return Native ft/s, zero for unknown method. Advances model histories but not
     * cumulative metres (rateFeet owns that). No finite validation/rollback is provided. */
    double evaluate(int e, double precip, double runon, double depth, double dt, Factors factors);
};
/// Global monthly factors at the mesh evaluation time. The runoff climate
/// cache may already describe a later runoff interval.
/** @brief Resolve monthly global factors at the actual independent surface clock.
 * @param ctx Compatible initialized options, twelve hydcon adjustments and optional
 * resolved recovery-pattern factor rows; borrowed without changing climate cache.
 * @param elapsed_s Finite elapsed simulation seconds yielding a supported calendar
 * month when added to start_date. No separate finite/range validation here.
 * @return Global conductivity adjustment and available recovery-pattern month value,
 * otherwise recovery one. Does not apply subcatchment-specific infiltration patterns,
 * validate factor signs, advance time or reuse possibly later runoff climate factors. */
InfilBank::Factors infiltrationFactors(const SimulationContext& ctx, double elapsed_s);
} // namespace openswmm::surface
