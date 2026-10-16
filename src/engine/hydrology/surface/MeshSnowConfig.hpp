// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "UebConfig.hpp"

namespace openswmm::surface {
struct MeshSnowRow {
    std::string pack;
    std::optional<double> forest,drift,ground_heat,extinction_depth,slope,aspect;
};
struct MeshSnowConfig {
    enum class Mode { Auto, Yes, No };
    Mode mode=Mode::Auto;
    double step=0; // inherit the shared SNOW_STEP
    std::map<std::string,MeshSnowRow> defaults; // * whole mesh, otherwise mesh tag
    std::map<int,MeshSnowRow> cells; // zero-based cell index
    bool assigned() const {return !defaults.empty()||!cells.empty();}
};
std::string setMeshSnowOption(MeshSnowConfig&,const std::vector<std::string>&);
std::string setMeshSnowRow(MeshSnowConfig&,const std::vector<std::string>&,bool cell);
std::vector<std::string> meshSnowOptionRows(const MeshSnowConfig&);
std::vector<std::string> meshSnowRows(const MeshSnowConfig&,bool cell);
MeshSnowRow resolveMeshSnowRow(const MeshSnowConfig&,int cell,const std::string& tag);
} // namespace openswmm::surface
