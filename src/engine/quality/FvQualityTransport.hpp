// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin
#pragma once
#include "FvTransportWater.hpp"
#include <string>
namespace openswmm { struct SimulationContext; }
namespace openswmm::quality {
class FvQualityTransport {
public:
    FvTransportWater water;
    bool execute(SimulationContext& ctx,double dt);
    bool accepted() const noexcept {return accepted_;}
    bool hasRetainedDryMass() const noexcept {
        for(const auto& row:held_)for(double mass:row)if(mass!=0.0)return true;
        return false;
    }
    // Classic native restart extension: only disconnected dry scalar inventory.
    // Hydraulic subgrid history is outside this record's contract.
    std::string dryRestartState(const SimulationContext& ctx) const;
    std::string restoreDryRestart(SimulationContext& ctx,const std::string& blob,bool apply);
    std::vector<double> retainedPollutantMass(const SimulationContext& ctx) const;
    std::vector<double> retainedSpeciesMass(const SimulationContext& ctx) const;
    void beginWindow() noexcept {accepted_=false;water.ready=false;}
    void applyPollutantDecay(SimulationContext& ctx,double dt);
private:
    ConservativeMixing solute_mixer_,thermal_mixer_;
    std::vector<std::vector<double>> held_;
    std::vector<MixedScalar> results_;
    std::vector<MixingScalar> inputs_;
    MixingWater thermal_water_;
    bool accepted_=false;
};
}
