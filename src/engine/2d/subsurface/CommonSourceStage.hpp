// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "SurfaceExchange.hpp"
#include "SurfaceOwnership.hpp"
#include "../../hydrology/SourceWaterDriver.hpp"

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
/// remaining weather footprint, with matching pollutant IDs. Age, heat, MSX,
/// mesh coupling donors are not qualified. Paired refunds and locally owned
/// emergence are consumed once in the next eligible completed interval.
/// Whole-context snapshots are qualification scaffolding, not runtime activation.
class CommonSourceStage {
public:
    std::string initialize(const MeshData&, SurfaceStateData&, const SolverOptions2D&, SubsurfaceSolver&,
                           runoff::SourceWaterDriver&, const SurfaceOwnershipPreview&);
    bool matches(const MeshData* m,const SurfaceStateData* s,const SubsurfaceSolver* g) const {
        return mesh_==m && surface_==s && gw_==g && mesh_;
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
    std::string loadCheckpointBytes(std::string bytes);
    std::string checkpointBoundary() const;
    std::string checkpointModel() const;
    std::string checkpoint_model_;
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
