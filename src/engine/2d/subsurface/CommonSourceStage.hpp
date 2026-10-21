// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "SurfaceExchange.hpp"
#include "SurfaceOwnership.hpp"
#include "../coupling/RuntimeCouplingCheckpoint.hpp"
#include "../../hydrology/SourceWaterDriver.hpp"
#include <functional>

namespace openswmm { struct SimulationContext; }
namespace openswmm::twoD {
struct MeshData; struct SurfaceStateData; struct SolverOptions2D; class SubsurfaceSolver;
enum class SurfaceEtOwner { MESH, PERVIOUS, IMPERVIOUS, LID };
struct SurfaceEtReceipt {
    SurfaceEtOwner owner = SurfaceEtOwner::MESH;
    int source=-1,type=-1,unit=-1,cell=-1;
    double start=0,end=0,area=0; // s, m2
    double potential=0,evaporation=0,soil_demand=0,unused=0; // m3
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
    std::string initialize(const MeshData&, SurfaceStateData&, const SolverOptions2D&, SubsurfaceSolver&,
                           runoff::SourceWaterDriver&, const SurfaceOwnershipPreview&, bool mesh_only = false,
                           const std::string& runtime_water_identity = {}, bool runtime_boundaries = false,
                           bool runtime_catchment_forcing = false);
    bool matches(const MeshData* m,const SurfaceStateData* s,const SubsurfaceSolver* g) const {
        return mesh_==m && surface_==s && mesh_ &&
            (gw_==g || (standalone_local_bed_ && !g && gw_));
    }
    std::string advance(double start, double end);
    bool ownsCell(int cell) const { return cell >= 0 && cell < int(owned_.size()) && owned_[cell]; }
    const std::vector<int>& cells() const { return cells_; }
    const std::vector<SurfaceIntakeReceipt>& receipts() const { return exchange_.receipts(); }
    const std::vector<SurfaceReturnReceipt>& returns() const { return returns_; }
    /// Last committed interval; diagnostic provenance, not restart/result history.
    const std::vector<SurfaceEtReceipt>& etReceipts() const { return et_receipts_; }
    double completedEnd() const { return exchange_.completedEnd(); }
    long intervals() const { return intervals_; }
    /// Qualification guard against running private and live subcatchment kernels twice.
    const runoff::SourceWaterDriver& sources() const { return *sources_; }
    std::string creditOutfallReturns(double start,double end,const std::vector<double>& volumes) {
        return sources_ ? sources_->creditOutfallReturns(start,end,volumes) : "Source stage is not attached.";
    }
    bool meshOnly() const { return sources_ && sources_->clocks().groups().empty() && donors_.empty(); }
    bool meshOnlyGroundQuality() const { return mesh_only_ground_quality_; }
    /// Read-only canonical layout check shared by managed preflight and attachment.
    static bool supportsMeshGroundQuality(const SimulationContext&,const SurfaceStateData&,const SubsurfaceSolver&);
    /// Surface-only LOCAL_STORAGE bed profile used to persist mesh heat state
    /// when no groundwater receiver exists. It owns synchronization only;
    /// ordinary mesh source kernels continue to own rainfall and evaporation.
    bool standaloneLocalBed() const { return standalone_local_bed_; }
    void synchronizeSurfaceLedgers(const ::openswmm::SimulationContext&);
    // Initial internal qualification profile. External program state is not
    // recovered by its immutable replay identity. No authored/public activation.
    bool runtimeWaterEnabled() const { return !runtime_water_identity_.empty(); }
    bool runtimeBoundariesEnabled() const { return runtime_boundaries_; }
    bool runtimeCatchmentForcingEnabled() const { return runtime_catchment_forcing_; }
    const std::string& runtimeWaterIdentity() const { return runtime_water_identity_; }
    std::string validateRuntimeWater(const RuntimeSources&,const RuntimeSources&,const RuntimeForcings&,double time,const BoundaryData* boundary=nullptr) const;
    /// Internal component checkpoint, not a production engine hotstart.
    std::string saveCheckpoint(const std::string& path) const;
    std::string loadCheckpoint(const std::string& path);
    double meshRain() const { return mesh_rain_; }
    double meshEvaporation() const { return mesh_evap_; }
    double meshReturn() const { return mesh_return_; }
    double meshWeatherArea(int cell) const { return weather_.at(cell); }
private:
    friend class SourceResultHistory;
    friend class ExplicitInertialSolver;
    std::string checkpointBytes(std::string& bytes) const;
    std::string loadCheckpointBytes(std::string bytes,const std::function<std::string(const CommonSourceStage&)>* inspect=nullptr);
    // Validate a captured earlier source boundary on private source/GW/mesh
    // copies. Never install it into the current accepted engine state.
    std::string inspectCheckpointBytes(std::string,const std::function<std::string(const CommonSourceStage&)>&) const;
    std::string checkpointBoundary() const;
    std::string checkpointModel() const;
    std::string checkpoint_model_;
    RuntimeWaterCheckpointBinding runtimeWaterBinding(double time) const;
    std::string runtime_water_identity_;
    bool runtime_boundaries_=false;
    bool runtime_catchment_forcing_=false;
    bool standalone_local_bed_=false;
    bool mesh_only_ground_quality_=false;
    std::vector<std::vector<std::pair<int,double>>> forcing_contacts_; // source -> cell, m2
    BoundaryData authored_boundary_;
    struct Donor {
        int source=-1, type=-1, unit=-1, identity=-1;
        double area=0;
        std::vector<std::pair<int,double>> contacts;
    };
    const MeshData* mesh_=nullptr;
    SurfaceStateData* surface_=nullptr;
    const SolverOptions2D* options_=nullptr;
    SubsurfaceSolver* gw_=nullptr;
    runoff::SourceWaterDriver* sources_=nullptr;
    SurfaceExchange exchange_;
    std::vector<Donor> donors_;
    std::vector<std::vector<std::pair<int,double>>> et_contacts_;
    std::vector<SurfaceEtReceipt> et_receipts_;
    std::vector<SurfaceReturnReceipt> returns_;
    std::vector<int> cells_;
    std::vector<bool> owned_;
    std::vector<double> weather_;
    long intervals_=0;
    double mesh_rain_=0,mesh_evap_=0,mesh_return_=0;
};
}
