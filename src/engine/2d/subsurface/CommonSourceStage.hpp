// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "SurfaceExchange.hpp"
#include "SurfaceOwnership.hpp"
#include "../../hydrology/SourceWaterDriver.hpp"

namespace openswmm::twoD {
struct MeshData; struct SurfaceStateData; struct SolverOptions2D; class SubsurfaceSolver;

/// Internal water-only source-phase adapter. Bind fully inside reviewed areas;
/// invoke after accepted face volumes land and before managed source debits.
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
    double completedEnd() const { return exchange_.completedEnd(); }
    long intervals() const { return intervals_; }
    double meshRain() const { return mesh_rain_; }
    double meshEvaporation() const { return mesh_evap_; }
private:
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
    std::vector<int> cells_;
    std::vector<bool> owned_;
    std::vector<double> weather_;
    long intervals_=0;
    double mesh_rain_=0,mesh_evap_=0;
};
}
