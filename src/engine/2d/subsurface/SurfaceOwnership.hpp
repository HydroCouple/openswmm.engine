// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin
#ifndef OPENSWMM_SURFACE_OWNERSHIP_HPP
#define OPENSWMM_SURFACE_OWNERSHIP_HPP
#include <functional>
#include <string>
#include <vector>
namespace openswmm { struct SimulationContext; }
namespace openswmm::twoD {
struct MeshData; struct SolverOptions2D; struct SubsurfaceConfig;
struct SurfaceOwnerRecord {
    std::string subcatch;
    bool operator==(const SurfaceOwnerRecord& other) const { return subcatch==other.subcatch; }
};
struct SurfaceOwnerObject {
    int subcatch=-1;
    std::string name,tag,reason;
    bool reviewed=false,lumped=false;
    double declared_area=0,polygon_area=0,lid_area=0,pervious_area=0,impervious_area=0,
           native_lid_area=0,inside_area=0,outside_area=0;
    int status=0; // 0 outside, 1 unreviewed, 2 reviewed, 3 invalid/unmapped
};
struct SurfaceOwnerShare {
    int subcatch=-1,cell=-1;
    double weather_area=0,pervious_area=0,impervious_area=0,lid_area=0,native_lid_area=0;
};
struct SurfaceOwnershipPreview {
    std::vector<SurfaceOwnerObject> objects;
    std::vector<SurfaceOwnerShare> shares;
    std::vector<double> mesh_weather_area;
    std::vector<std::string> errors;
    std::string token;
    bool cancelled=false;
    bool valid() const { return errors.empty(); }
};
// Geometry is read-only. The same resolver serves authoring and initialization.
SurfaceOwnershipPreview resolveSurfaceOwnership(const SimulationContext&,const MeshData&,
    const SolverOptions2D&,const SubsurfaceConfig&,const std::vector<SurfaceOwnerRecord>&,
    const std::function<bool(int,int)>& progress={});
std::string parseSurfaceOwnerLine(const std::vector<std::string>&,std::vector<SurfaceOwnerRecord>&);
}
#endif
