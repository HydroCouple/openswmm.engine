// SPDX-License-Identifier: Apache-2.0

/**
 * @file      MeshSnow.hpp
 * @brief     Snow state and advancement for mesh cells.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#pragma once
#include "MeshSnowConfig.hpp"
#include "UebHeld.hpp"
#include "DegreeDaySnow.hpp"
#include <array>
namespace openswmm {
struct SimulationContext;
}
namespace openswmm::surface {
struct MeshSnowGeometry {
    std::string tag;
    double area = 0, slope = 0, aspect = 0; // horizontal weather footprint m2; degrees
};
struct MeshSnowLedger {
    double initial = 0, precipitation = 0, raw_precipitation = 0, liquid = 0, sublimation = 0,
           stored = 0; // m3
    double net_energy = 0, geometry_energy = 0, initial_energy = 0,
           stored_energy = 0; // kJ; geometry is UEB cover/depth remapping
    double prescribed_ground_energy = 0, ground_contact_energy = 0; // kJ; signed positive into UEB
    /** @brief Compute signed cumulative represented-energy closure.
     * @return initial_energy+net_energy+geometry_energy-stored_energy in kJ. No tolerance,
     * finite validation or correction; contact/prescribed fields are diagnostic subsets. */
    double energyResidual() const {
        return initial_energy + net_energy + geometry_energy - stored_energy;
    }
    /** @brief Compute signed cumulative physical water closure.
     * @return initial+precipitation-liquid-sublimation-stored in m3. raw_precipitation
     * is diagnostic before catch/drift partition and is not added a second time. */
    double residual() const { return initial + precipitation - liquid - sublimation - stored; }
};
// Point snow on the remaining mesh weather footprint. Named pack PERVIOUS
// initial values seed each cell; UEB uses the project pervious ADC curve but
// mesh snow does not inherit subcatch plowing or snow state.
/** @brief Owned mixed degree-day/UEB snow on remaining mesh weather footprints.
 * Geometry area is horizontal m2; cell slope/aspect are degrees. Water accounts
 * are m3, energy kJ, point energy kJ/m2 and rates m/s. One owner serializes all
 * mutation, borrowing and checkpoint/contact calls. Trial advance protects this
 * snow owner, while shared context weather cursors/diagnostics can still change.
 * Physical ground heat, cover-geometry energy and prescribed energy stay distinct. */
class MeshSnow {
public:
    /** @brief Build a candidate mixed mesh snow owner from resolved authored assignment scopes.
     * @param context Compatible initialized global/mesh snow, named pack/pervious rows,
     * weather identity and valid project units; read here without advancing climate time.
     * @param geometry One indexed horizontal weather footprint m2 per mesh cell, with tag
     * and slope/aspect degrees. Count/products must fit int. Assigned nonzero-area cells
     * require finite positive area, slope[0,90) and aspect[0,360]; skipped/unassigned cells
     * are not fully geometry-qualified. Tag/default/cell rows can override slope/aspect.
     * @return Empty on replacement or diagnostic retaining the old owner. Ignore-snow,
     * mode NO or no assignments installs an empty inactive owner; YES requires an active
     * assigned footprint. Invalid explicit cell indices/pack names are refused. Copies
     * parameters/site/geometry/identity, inherits positive mesh step or shared SNOW_STEP,
     * uses named PERVIOUS initial values without subcatchment plowing/state. UEB initial
     * SWE includes authored free water (uncapped here), adds its latent energy and uses
     * project pervious depletion curve; DD caps free water and retains native default
     * zero-SI/full-cover depletion until melt-out. DD thermal outputs and initial UEB
     * albedo are NaN. Ground contact defaults off, clocks zero, rates zero and exposure
     * matches initial cover. Does not fully validate all physical/weather parameters;
     * initialization/copy/archive exceptions propagate before owner replacement. */
    std::string initialize(SimulationContext& context,
                           const std::vector<MeshSnowGeometry>& geometry);
    /** @brief Trial a contiguous interval, publishing internal snow state only after success.
     * @param[in,out] context Matching initialized weather/configuration/forcing; shared
     * lookup cursors and climate_state.radiation may change even if snow trial fails.
     * Does not update global simulation time or book external mesh water/thermal stores.
     * @param start Finite elapsed seconds exactly equal completedEnd().
     * @param end Finite elapsed endpoint strictly after start, within weather/simulation coverage.
     * @param precipitation_ms One finite nonnegative held raw precipitation m/s per mesh
     * cell; active entries checked. Snow catch/UEB drift can change effective volume.
     * @param snow_catch Same-sized finite nonnegative catch factors; active entries checked.
     * @return Empty on accepted internal replacement, otherwise diagnostic retaining this
     * owner. Inactive returns immediately without validating inputs. A span<=1e-9seconds
     * only reanchors completedEnd, retaining prior outputs/rates/exposure/lastSeconds.
     * Ordinary work copies this owner, splits held windows at snow/hour/transient boundaries,
     * then checks per-cell water balance. Copy/solver exceptions can propagate; context
     * or caller side effects are outside rollback. External source bookkeeping follows
     * only after accepted return. Serialize changes to forcing/configuration and contact. */
    std::string advance(SimulationContext& context, double start, double end,
                        const std::vector<double>& precipitation_ms,
                        const std::vector<double>& snow_catch);
    /** @brief Install reciprocal ground-contact ownership for assigned UEB cells.
     * @param mask One byte per mesh cell, each zero/one; one requires owns(cell) and UEB.
     * @return Empty after copy, otherwise shape/value/ownership refusal without change.
     * Does not close held predictions, change energy/clocks or validate a ground solver;
     * caller coordinates transitions at an accepted thermal boundary. Allocation may throw. */
    std::string setGroundContactMask(const std::vector<unsigned char>& mask);
    /** @brief Apply an accepted reciprocal soil energy receipt to one covered-point owner.
     * @param cell Valid assigned UEB cell with enabled ground-contact mask; checked here.
     * @param soil_j Finite signed joules into soil; snow receives -soil_j. Zero is a no-op
     * after owner validation. Uses horizontal area times accepted cover (one if snow-free)
     * to convert to point kJ/m2; no water/cover change.
     * @return Empty on accepted energy/temperature/history/ledger publication, otherwise
     * diagnostic retaining state. Checks finite storage, new bulk/surface temperatures,
     * contact/net/stored totals and the implemented initial+net-stored residual. That
     * receipt check does not include geometry_energy, so callers must account for this
     * stricter condition when cover remapping has occurred. Updates last thermal sample,
     * outputs and ground-contact/net/stored kJ; closes a pending prediction on success.
     * Does not itself debit soil energy; caller pairs both owners transactionally.
     * Point-history copy allocations can throw before publication. */
    std::string applyGroundContactEnergy(int cell, double soil_j);
    /** @brief Copy this owner and encode a native version-four accepted/predicted snapshot.
     * @return Empty bytes when inactive, otherwise OpenSWMM mesh snow4 archive containing
     * configuration identity, exact clocks/forcing, contact mask, held windows, DD history,
     * outputs and water/physical/geometry/contact energy ledgers. Does not mutate owner,
     * finish an external paired checkpoint or provide portable endian/schema interchange.
     * Copy/archive exceptions propagate; caller serializes snapshot access. */
    std::string saveCheckpoint() const;
    /** @brief Validate a native mesh-snow archive into a private candidate then publish it.
     * @param bytes Matching native version4/3/2 archive. Identity/model/geometry must
     * match; old legacy identity is allowed only with all-one depletion curve. Version3/4
     * contact mask must match, and version2 cannot restore an enabled contact owner.
     * Empty bytes clear owned inventory using the existing assignments and force seeding.
     * @param seed True preserves restored water/thermal state but clears ledgers to new
     * initial inventory, zeroes clocks/last flux/rates, closes future predictions and
     * resets cached rain/catch. False restores exact clocks and pending held predictions.
     * @return Empty on success or caught standard-exception/compatibility/shape/clock/
     * finite inventory/thermal/closure diagnostic retaining the prior owner. Inactive
     * accepts only empty bytes. Checks entire archive via finish(); no partial normal
     * publication. Does not prepare weather, rebuild geometry, debit soil/source stores
     * or independently qualify the containing engine/surface restart. Caller publishes
     * all paired owners after complete surrounding validation. */
    std::string restoreCheckpoint(const std::string& bytes, bool seed = false);
    /** @brief Test whether any nonzero assigned snow weather footprint is owned.
     * @return True exactly when active_cells_ is nonempty; not a test for nonempty pack
     * water, pending predictions or whether every mesh cell is assigned. */
    bool active() const { return !active_cells_.empty(); }
    /** @brief Scan active cells for at least one UEB model owner.
     * @return True for any active UEB assignment even if its water inventory is zero;
     * false for inactive/DD-only models. Does not resolve current thermal readiness. */
    bool hasUeb() const {
        for (int cell : active_cells_)
            if (kind_[cell] == SnowKind::Ueb)
                return true;
        return false;
    }
    /** @brief Test resolved snowpack ownership for one indexed mesh cell.
     * @param cell Zero-based index; invalid returns false.
     * @return True when pack_[cell]>=0. Does not require stored snow, UEB model or contact mask. */
    bool owns(int cell) const { return cell >= 0 && cell < int(pack_.size()) && pack_[cell] >= 0; }
    /** @brief Read the accepted snow-source frontier.
     * @return Elapsed seconds, initially zero. Advance/reanchor/restore can change it;
     * predicted held end states are not exposed as accepted time. */
    double completedEnd() const { return completed_; }
    /** @brief Read the latest ordinary accepted advance duration.
     * @return Seconds, initially/seed zero. A sub-nanosecond reanchor or explicit clock
     * reanchor retains it, so it need not equal the last completedEnd increment. */
    double lastSeconds() const { return last_seconds_; }
    /** @brief Read stored model kind for one allocated cell row.
     * @param cell Zero-based row, checked through vector::at().
     * @return Stored enum; meaningful only for owned cells (unassigned rows are defaulted).
     * @throws std::out_of_range Invalid row. No pack/water/contact validation. */
    SnowKind kind(int cell) const { return kind_.at(cell); }
    /** @brief Borrow owned snow metadata/state without copying it.
     * @return Copied UEB pervious-area ten-point depletion curve; DD uses its own native state.
     * Reference valid until destruction/replacement; initialize/advance/restore/move can
     * invalidate elements or buffers and contact calls mutate relevant contents. Serialize
     * reads with mutation and copy any snapshot needed across publication. Never free. */
    const std::array<double, 10>& depletionCurve() const { return depletion_curve_; }
    /** @brief Borrow owned snow metadata/state without copying it.
     * @return Per-cell accepted liquid (melt+throughfall) m/s over lastSeconds(), zero initially.
     * Reference valid until destruction/replacement; initialize/advance/restore/move can
     * invalidate elements or buffers and contact calls mutate relevant contents. Serialize
     * reads with mutation and copy any snapshot needed across publication. Never free. */
    const std::vector<double>& liquidRates() const { return liquid_rate_; }
    /** @brief Borrow owned snow metadata/state without copying it.
     * @return Per-cell interval-average snow-free fraction for ET; useAcceptedExposure can replace it with endpoint exposure.
     * Reference valid until destruction/replacement; initialize/advance/restore/move can
     * invalidate elements or buffers and contact calls mutate relevant contents. Serialize
     * reads with mutation and copy any snapshot needed across publication. Never free. */
    const std::vector<double>& exposedFractions() const { return exposed_; }
    // Hold accepted cover during a split source operator, without advancing it.
    /** @brief Replace ET exposure for active cells by one minus accepted endpoint cover.
     * Does not advance snow or alter output cover/water/predictions. Used during split
     * source operators where the previously stored interval-average exposure is unsuitable;
     * inactive/unowned rows remain unchanged. Serialize with source/thermal evaluation. */
    void useAcceptedExposure();
    // Adopt only the driver's representable endpoint after an ulp-sized tail.
    /** @brief Adopt only an ulp-sized driver endpoint change without advancing physics.
     * @param end Finite nonnegative elapsed seconds within8*epsilon*max(1,abs(end),abs(old)).
     * @throws std::invalid_argument Larger/incompatible landing. On success changes only
     * completed_, retaining lastSeconds, histories, held predictions, outputs and ledgers. */
    void reanchorCompletedEnd(double end);
    /** @brief Borrow owned snow metadata/state without copying it.
     * @return Native binary configuration/geometry/forcing identity, not display text or a portable cryptographic hash.
     * Reference valid until destruction/replacement; initialize/advance/restore/move can
     * invalidate elements or buffers and contact calls mutate relevant contents. Serialize
     * reads with mutation and copy any snapshot needed across publication. Never free. */
    const std::string& configurationIdentity() const { return identity_; }
    /** @brief Borrow owned snow metadata/state without copying it.
     * @return Resolved tag/horizontal m2/slope-and-aspect-degree geometry after authored overrides.
     * Reference valid until destruction/replacement; initialize/advance/restore/move can
     * invalidate elements or buffers and contact calls mutate relevant contents. Serialize
     * reads with mutation and copy any snapshot needed across publication. Never free. */
    const std::vector<MeshSnowGeometry>& geometry() const { return geometry_; }
    /** @brief Borrow owned snow metadata/state without copying it.
     * @return Per-cell accepted output: integrated liquid depths m, SWE m, cover and available Celsius/kJ/m2 fields; DD thermal values can be NaN.
     * Reference valid until destruction/replacement; initialize/advance/restore/move can
     * invalidate elements or buffers and contact calls mutate relevant contents. Serialize
     * reads with mutation and copy any snapshot needed across publication. Never free. */
    const std::vector<SnowOutputs>& outputs() const { return output_; }
    /** @brief Borrow owned snow metadata/state without copying it.
     * @return Cumulative physical m3 and kJ accounts, including separate cover-remap and reciprocal/prescribed ground-energy diagnostics.
     * Reference valid until destruction/replacement; initialize/advance/restore/move can
     * invalidate elements or buffers and contact calls mutate relevant contents. Serialize
     * reads with mutation and copy any snapshot needed across publication. Never free. */
    const std::vector<MeshSnowLedger>& ledgers() const { return ledger_; }
    /** @brief Test the stored per-cell reciprocal contact bit.
     * @param cell Zero-based row; invalid returns false.
     * @return Nonzero stored mask byte. Does not independently recheck UEB/pack validity;
     * setGroundContactMask owns that qualification. */
    bool groundContactEnabled(int cell) const {
        return cell >= 0 && std::size_t(cell) < ground_contact_.size() &&
               ground_contact_[cell] != 0;
    }
    // Contact on the next thermal interval follows the accepted end-state
    // cover. exposed_ is an interval average for the ET source, so using it
    // here would retain contact after a melt-out at the interval boundary.
    /** @brief Read accepted endpoint cover for the next reciprocal thermal interval.
     * @param cell Cell index; disabled/invalid contact returns zero before output lookup.
     * @return Endpoint cover when contact enabled, not interval-average ET exposure.
     * Does not predict future coverage or impose a separate snow-water threshold. */
    double groundContactFraction(int cell) const {
        return groundContactEnabled(cell) ? output_.at(cell).cover_frac : 0.;
    }
    /** @brief Read the latest stored cell snow surface temperature.
     * @param cell Zero-based allocated output index, checked by vector::at().
     * @return Celsius, potentially NaN for DD/uninitialized outputs. Does not check
     * contact enabled, model, ownership or current accepted clock.
     * @throws std::out_of_range Invalid index. */
    double groundContactSurfaceTemperature(int cell) const { return output_.at(cell).Ts_C; }
    /** @brief Borrow owned snow metadata/state without copying it.
     * @return Per-cell reciprocal-contact ownership bits, not predicted snow cover.
     * Reference valid until destruction/replacement; initialize/advance/restore/move can
     * invalidate elements or buffers and contact calls mutate relevant contents. Serialize
     * reads with mutation and copy any snapshot needed across publication. Never free. */
    const std::vector<unsigned char>& groundContactMask() const { return ground_contact_; }
    /** @brief Borrow owned snow metadata/state without copying it.
     * @return UEB accepted/begin/private-predicted states and consumed-window clocks; unowned/DD rows are not active UEB columns.
     * Reference valid until destruction/replacement; initialize/advance/restore/move can
     * invalidate elements or buffers and contact calls mutate relevant contents. Serialize
     * reads with mutation and copy any snapshot needed across publication. Never free. */
    const std::vector<ueb::HeldState>& columns() const { return held_; }

private:
    /** @brief Mutate a private owner over one qualified completed interval.
     * @param[in,out] context Matching initialized weather configuration; shared cursors/
     * radiation output can change and are not part of internal candidate rollback.
     * @param start Finite exact old completed seconds.
     * @param end Finite later endpoint within forcing/simulation coverage.
     * @param precipitation_ms Valid held nonnegative m/s vector, one row per cell.
     * @param snow_catch Valid nonnegative catch vector in the same layout.
     * @return Empty after per-active-cell solve, ledger/ET exposure/rate publication and
     * clock update; diagnostic can follow partial private mutation. Clears held predictions
     * when runtime forcing/rain/catch changes, or reciprocal contact requires accepted
     * coverage. UEB consumes window shares; DD point stepping uses pervious index and
     * calendar coefficient. Accumulates effective and raw water, physical and geometric
     * energy separately, then checks per-cell water closure. Caller discards candidate
     * on failure and pairs external water/energy stores only after public advance succeeds. */
    std::string advanceInPlace(SimulationContext& context, double start, double end,
                               const std::vector<double>& precipitation_ms,
                               const std::vector<double>& snow_catch);
    std::string identity_;
    std::string legacy_identity_;
    ueb::Parameters parameters_;
    std::array<double, 10> depletion_curve_{};
    std::array<double, 4> forcing_{};
    double completed_ = 0, last_seconds_ = 0, step_ = 3600;
    std::vector<int> active_cells_, pack_;
    std::vector<SnowKind> kind_;
    std::vector<MeshSnowGeometry> geometry_;
    std::vector<ueb::Site> site_;
    std::vector<ueb::HeldState> held_;
    std::vector<double> precipitation_, catch_, liquid_rate_, exposed_;
    std::vector<unsigned char> ground_contact_;
    std::vector<SnowOutputs> output_;
    std::vector<MeshSnowLedger> ledger_;
    snow::DegreeDaySnow degree_day_;
};
} // namespace openswmm::surface
