// SPDX-License-Identifier: Apache-2.0

/**
 * @file      CommonSourceStage.hpp
 * @brief     Shared source intervals and stage bookkeeping for coupled water owners.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#pragma once
#include "SurfaceExchange.hpp"
#include "SurfaceOwnership.hpp"
#include "../coupling/RuntimeCouplingCheckpoint.hpp"
#include "../../hydrology/SourceWaterDriver.hpp"
#include <functional>

namespace openswmm {
struct SimulationContext;
}
namespace openswmm::twoD {
struct MeshData;
struct SurfaceStateData;
struct SolverOptions2D;
class SubsurfaceSolver;
enum class SurfaceEtOwner { MESH, PERVIOUS, IMPERVIOUS, LID };
struct SurfaceEtReceipt {
    SurfaceEtOwner owner = SurfaceEtOwner::MESH;
    int source = -1, type = -1, unit = -1, cell = -1;
    double start = 0, end = 0, area = 0;                                // s, m2
    double potential = 0, evaporation = 0, soil_demand = 0, unused = 0; // m3
};

/// Internal completed source-phase adapter. Bind reviewed inside/outside uniform areas;
/// invoke after accepted face volumes land and before managed source debits.
/// Conservative pollutants pair actual non-LID/LID withdrawals and the mesh's
/// remaining weather footprint, with matching pollutant IDs. A separately
/// qualified mesh-only matched profile carries optional age and BULK MSX rows with
/// WATER_ONLY or paired porous heat owners.
/// Private source MSX and mesh coupling donors remain guarded. Paired refunds and locally owned
/// emergence are consumed once in the next eligible completed interval.
/// Whole-context snapshots are qualification scaffolding, not runtime activation.
class CommonSourceStage {
public:
    // mesh_only explicitly assigns the whole mesh weather footprint with no
    // private subcatchment groups; default reviewed-source behavior is unchanged.
    std::string initialize(const MeshData&, SurfaceStateData&, const SolverOptions2D&,
                           SubsurfaceSolver&, runoff::SourceWaterDriver&,
                           const SurfaceOwnershipPreview&, bool mesh_only = false,
                           const std::string& runtime_water_identity = {},
                           bool runtime_boundaries = false, bool runtime_catchment_forcing = false);
    /**
     * @brief Compare the exact borrowed mesh/surface/receiver addresses of this attachment.
     * @param[in] m Expected mesh pointer; must equal the stored nonnull mesh to match.
     * @param[in] s Expected surface state pointer, compared without dereferencing.
     * @param[in] g Expected receiver pointer. A standalone local bed additionally accepts null
     * here when its stored placeholder receiver exists.
     * @return True on those pointer/standalone conditions; does not validate current model/state
     * or prove clocks are synchronized. No ownership transfer or pointer retention from this
     * query.
     */
    bool matches(const MeshData* m, const SurfaceStateData* s, const SubsurfaceSolver* g) const {
        return mesh_ == m && surface_ == s && mesh_ &&
               (gw_ == g || (standalone_local_bed_ && !g && gw_));
    }
    std::string advance(double start, double end);
    /**
     * @brief Test this stage's reviewed receiving-aquifer ownership mask.
     * @param[in] cell Zero-based mesh cell; negative/out-of-range returns false.
     * @return Stored owned flag. Standalone local-bed synchronization cells may appear in
     * cells() without receiver ownership here. Does not activate a model or inspect current
     * aquifer depth.
     */
    bool ownsCell(int cell) const { return cell >= 0 && cell < int(owned_.size()) && owned_[cell]; }
    /**
     * @brief Borrow the stable ordered cells handled by this source/synchronization stage.
     * @return Const recorder-owned cell-index vector; initialized standalone local beds can list
     * cells without ownsCell() receiver ownership. Valid until stage
     * replacement/restore/destruction; no copy or allocation. Caller keeps stage stable while
     * iterating.
     */
    const std::vector<int>& cells() const { return cells_; }
    /**
     * @brief Borrow last committed interval intake receipts.
     * @return Const stage-owned vector. Diagnostic provenance for the last interval, not an
     * append-only history; next advance, restore, replacement or destruction may invalidate
     * elements. Times are elapsed s, water m3, area m2 and species amounts in receiver-native
     * inventory units where present. No copy, queue consumption or synchronization occurs.
     */
    const std::vector<SurfaceIntakeReceipt>& receipts() const { return exchange_.receipts(); }
    /**
     * @brief Borrow last adopted return receipts.
     * @return Const stage-owned vector. Diagnostic provenance for the last interval, not an
     * append-only history; next advance, restore, replacement or destruction may invalidate
     * elements. Times are elapsed s, water m3, area m2 and species amounts in receiver-native
     * inventory units where present. No copy, queue consumption or synchronization occurs.
     */
    const std::vector<SurfaceReturnReceipt>& returns() const { return returns_; }
    /**
     * @brief Last committed interval; diagnostic provenance, not restart/result history.
     *
     * @details Borrow last committed interval evaporation/soil-demand receipts.
     * @return Const stage-owned vector. Diagnostic provenance for the last interval, not an
     * append-only history; next advance, restore, replacement or destruction may invalidate
     * elements. Times are elapsed s, water m3, area m2 and species amounts in receiver-native
     * inventory units where present. No copy, queue consumption or synchronization occurs.
     */
    const std::vector<SurfaceEtReceipt>& etReceipts() const { return et_receipts_; }
    /**
     * @brief Read the accepted source-completion horizon.
     * @return Elapsed simulation seconds from the exchange owner, initially zero. A planned but
     * uncommitted interval does not advance this value; no receiver clock synchronization
     * occurs.
     */
    double completedEnd() const { return exchange_.completedEnd(); }
    /**
     * @brief Read the number of accepted common-source intervals.
     * @return Stored nonnegative count after successful initialization/continuation, initially
     * zero. This is independent of hydraulic report or whole-engine step counts; no mutation
     * occurs.
     */
    long intervals() const { return intervals_; }
    /**
     * @brief Qualification guard against running private and live subcatchment kernels twice.
     *
     * @return Borrowed const source driver; requires a successfully attached live driver. The
     * stage retains no ownership and this unchecked dereference is invalid before attachment or
     * after driver destruction. Access changes no physics; callbacks must not outlive the
     * owner's stable state.
     */
    const runoff::SourceWaterDriver& sources() const { return *sources_; }
    /**
     * @brief Credit one accepted water-only hydraulic outfall-return interval to the source
     * owner.
     * @param[in] start Finite elapsed interval start s, exactly the driver's previous
     * hydraulic-return end.
     * @param[in] end Finite later elapsed s, within simulation duration and no earlier than
     * completed source groups; groups must have no unsettled pending intervals.
     * @param[in] volumes Finite nonnegative returned water ft3 per source/subcatchment, exactly
     * the driver's receiver vector length. Positive amounts require an owned physical receiving
     * footprint.
     * @return Empty after validating/copying the whole pending queue and publishing credits/end
     * cursor; otherwise driver refusal text, or an unattached-stage diagnostic. Allocation
     * errors may propagate before driver publication. No input storage is retained.
     * @note This wrapper deliberately forwards the 1D driver's ft3 convention rather than the
     * stage's usual SI receipt convention. Updates return credit/provenance, not common-source
     * completion.
     */
    std::string creditOutfallReturns(double start, double end, const std::vector<double>& volumes) {
        return sources_ ? sources_->creditOutfallReturns(start, end, volumes)
                        : "Source stage is not attached.";
    }
    /**
     * @brief Test whether the attached driver has no private source groups or donor records.
     * @return True only with an attached sources_ pointer, empty clock groups and empty donors_.
     * May include a standalone local-bed profile; does not imply any particular quality/runtime
     * capability.
     */
    bool meshOnly() const {
        return sources_ && sources_->clocks().groups().empty() && donors_.empty();
    }
    /**
     * @brief Read the qualified matched mesh/groundwater quality-profile flag.
     * @return Flag resolved during attachment; this does not rerun layout checks or activate
     * private MSX/age/heat owners. False includes uninitialized and conventional private-source
     * attachments.
     */
    bool meshOnlyGroundQuality() const { return mesh_only_ground_quality_; }
    /// Read-only canonical layout check shared by managed preflight and attachment.
    static bool supportsMeshGroundQuality(const SimulationContext&, const SurfaceStateData&,
                                          const SubsurfaceSolver&);
    /**
     * @brief Surface-only LOCAL_STORAGE bed profile used to persist mesh heat state when no
     * groundwater receiver exists. It owns synchronization only; ordinary mesh source kernels
     * continue to own rainfall and evaporation.
     *
     * @return Stored standalone-local-bed attachment flag; synchronization only, not aquifer
     * receiver ownership. Ordinary mesh kernels retain rainfall/evaporation ownership in that
     * profile.
     */
    bool standaloneLocalBed() const { return standalone_local_bed_; }
    /**
     * @brief Synchronize standalone-local-bed cumulative water diagnostics from accepted mesh
     * ledgers.
     * @param[in] ctx Stable simulation context supplying SI mass_balance_2d rainfall_in, evap_out and
     * aquifer_in volumes m3.
     * @note Copies these three totals only for standalone_local_bed_; otherwise does nothing. Does
     * not change physical water, source intervals, receipt queues or hydraulic reporting clocks.
     * Caller orders this after the appropriate accepted surface ledger publication.
     */
    void synchronizeSurfaceLedgers(const ::openswmm::SimulationContext& ctx);
    // Initial internal qualification profile. External program state is not
    // recovered by its immutable replay identity. No authored/public activation.
    /**
     * @brief Test presence of an explicit copied-prescription replay identity.
     * @return True when runtime_water_identity_ is nonempty. It enables only the internally
     * qualified water profile; no provider state is sampled, scheduled or externally restored by
     * this query.
     */
    bool runtimeWaterEnabled() const { return !runtime_water_identity_.empty(); }
    /**
     * @brief Read whether the qualified runtime hydraulic-boundary profile was attached.
     * @return Stored boundary capability flag; does not inspect current rows or validate a
     * checkpoint. No model activation or borrowed boundary ownership change occurs.
     */
    bool runtimeBoundariesEnabled() const { return runtime_boundaries_; }
    /**
     * @brief Read the qualified private-catchment provider-forcing capability flag.
     * @return Flag fixed at attachment. Does not advance providers, set weather or validate snow
     * phase compatibility; these checks occur when the profile is attached/advanced.
     */
    bool runtimeCatchmentForcingEnabled() const { return runtime_catchment_forcing_; }
    /**
     * @brief Borrow the immutable replay identity recorded for the copied water-provider
     * profile.
     * @return Const stage-owned string, empty when disabled. Valid until stage
     * replacement/restore or destruction; no copy/allocation. External program state is not
     * represented by this identity alone.
     */
    const std::string& runtimeWaterIdentity() const { return runtime_water_identity_; }
    std::string validateRuntimeWater(const RuntimeSources&, const RuntimeSources&,
                                     const RuntimeForcings&, double time,
                                     const BoundaryData* boundary = nullptr) const;
    /// Internal component checkpoint, not a production engine hotstart.
    std::string saveCheckpoint(const std::string& path) const;
    std::string loadCheckpoint(const std::string& path);
    /**
     * @brief Read cumulative remaining mesh-owned rainfall input booked by this stage.
     * @return Cumulative water m3, initially zero. Standalone local beds synchronize from
     * accepted surface mass-balance ledgers; other profiles update at common-source acceptance.
     * This is a cumulative diagnostic, not an interval increment or rate. No mutation or
     * synchronization.
     */
    double meshRain() const { return mesh_rain_; }
    /**
     * @brief Read cumulative remaining mesh-owned evaporation loss booked by this stage.
     * @return Cumulative water m3, initially zero. Standalone local beds synchronize from
     * accepted surface mass-balance ledgers; other profiles update at common-source acceptance.
     * This is a cumulative diagnostic, not an interval increment or rate. No mutation or
     * synchronization.
     */
    double meshEvaporation() const { return mesh_evap_; }
    /**
     * @brief Read cumulative aquifer-to-mesh returned water booked by this stage.
     * @return Cumulative water m3, initially zero. Standalone local beds synchronize from
     * accepted surface mass-balance ledgers; other profiles update at common-source acceptance.
     * This is a cumulative diagnostic, not an interval increment or rate. No mutation or
     * synchronization.
     */
    double meshReturn() const { return mesh_return_; }
    /**
     * @brief Read the remaining mesh-owned weather footprint for one cell.
     * @param[in] cell Zero-based index into the initialized weather-area vector.
     * @return Area m2 left to mesh weather after reviewed uniform source ownership.
     * @throws std::out_of_range For an invalid index, including negative cell converted by
     * vector::at. Does not recompute geometry or change weather/source partitioning.
     */
    double meshWeatherArea(int cell) const { return weather_.at(cell); }

private:
    friend class SourceResultHistory;
    friend class ExplicitInertialSolver;
    std::string checkpointBytes(std::string& bytes) const;
    std::string loadCheckpointBytes(
        std::string bytes,
        const std::function<std::string(const CommonSourceStage&)>* inspect = nullptr);
    // Validate a captured earlier source boundary on private source/GW/mesh
    // copies. Never install it into the current accepted engine state.
    std::string
    inspectCheckpointBytes(std::string,
                           const std::function<std::string(const CommonSourceStage&)>&) const;
    std::string checkpointBoundary() const;
    std::string checkpointModel() const;
    std::string checkpoint_model_;
    RuntimeWaterCheckpointBinding runtimeWaterBinding(double time) const;
    std::string runtime_water_identity_;
    bool runtime_boundaries_ = false;
    bool runtime_catchment_forcing_ = false;
    bool standalone_local_bed_ = false;
    bool mesh_only_ground_quality_ = false;
    std::vector<std::vector<std::pair<int, double>>> forcing_contacts_; // source -> cell, m2
    BoundaryData authored_boundary_;
    struct Donor {
        int source = -1, type = -1, unit = -1, identity = -1;
        double area = 0;
        std::vector<std::pair<int, double>> contacts;
    };
    const MeshData* mesh_ = nullptr;
    SurfaceStateData* surface_ = nullptr;
    const SolverOptions2D* options_ = nullptr;
    SubsurfaceSolver* gw_ = nullptr;
    runoff::SourceWaterDriver* sources_ = nullptr;
    SurfaceExchange exchange_;
    std::vector<Donor> donors_;
    std::vector<std::vector<std::pair<int, double>>> et_contacts_;
    std::vector<SurfaceEtReceipt> et_receipts_;
    std::vector<SurfaceReturnReceipt> returns_;
    std::vector<int> cells_;
    std::vector<bool> owned_;
    std::vector<double> weather_;
    long intervals_ = 0;
    double mesh_rain_ = 0, mesh_evap_ = 0, mesh_return_ = 0;
};
}
