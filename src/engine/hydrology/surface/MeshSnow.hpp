// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "MeshSnowConfig.hpp"
#include "UebHeld.hpp"
#include "DegreeDaySnow.hpp"
namespace openswmm {struct SimulationContext;}
namespace openswmm::surface {
struct MeshSnowGeometry {
    std::string tag;
    double area=0,slope=0,aspect=0; // horizontal weather footprint m2; degrees
};
struct MeshSnowLedger {
    double initial=0,precipitation=0,raw_precipitation=0,liquid=0,sublimation=0,stored=0; // m3
    double net_energy=0,initial_energy=0,stored_energy=0; // kJ; point cells have no depletion remapping
    double prescribed_ground_energy=0,ground_contact_energy=0; // kJ; signed positive into UEB
    double energyResidual() const {return initial_energy+net_energy-stored_energy;}
    double residual() const {return initial+precipitation-liquid-sublimation-stored;}
};
// Point snow on the remaining mesh weather footprint. Named pack PERVIOUS
// initial values seed each cell; mesh snow does not inherit subcatch plowing
// or ADC. UEB and degree-day call the same kernels as native subcatchments.
class MeshSnow {
public:
    std::string initialize(SimulationContext&,const std::vector<MeshSnowGeometry>&);
    std::string advance(SimulationContext&,double start,double end,
        const std::vector<double>& precipitation_ms,const std::vector<double>& snow_catch);
    std::string setGroundContactMask(const std::vector<unsigned char>&);
    std::string applyGroundContactEnergy(int cell,double soil_j);
    std::string saveCheckpoint() const;
    std::string restoreCheckpoint(const std::string&,bool seed=false);
    bool active() const {return !active_cells_.empty();}
    bool hasUeb() const {for(int cell:active_cells_)if(kind_[cell]==SnowKind::Ueb)return true;return false;}
    bool owns(int cell) const {return cell>=0&&cell<int(pack_.size())&&pack_[cell]>=0;}
    double completedEnd() const {return completed_;}
    double lastSeconds() const {return last_seconds_;}
    SnowKind kind(int cell) const {return kind_.at(cell);}
    const std::vector<double>& liquidRates() const {return liquid_rate_;}
    const std::vector<double>& exposedFractions() const {return exposed_;}
    const std::string& configurationIdentity() const {return identity_;}
    const std::vector<MeshSnowGeometry>& geometry() const {return geometry_;}
    const std::vector<SnowOutputs>& outputs() const {return output_;}
    const std::vector<MeshSnowLedger>& ledgers() const {return ledger_;}
    bool groundContactEnabled(int cell) const {return cell>=0&&std::size_t(cell)<ground_contact_.size()&&ground_contact_[cell]!=0;}
    double groundContactFraction(int cell) const {return groundContactEnabled(cell)?1.-exposed_.at(cell):0.;}
    double groundContactSurfaceTemperature(int cell) const {return output_.at(cell).Ts_C;}
    const std::vector<ueb::HeldState>& columns() const {return held_;}
private:
    std::string advanceInPlace(SimulationContext&,double,double,const std::vector<double>&,const std::vector<double>&);
    std::string identity_;
    ueb::Parameters parameters_;
    std::array<double,4> forcing_{};
    double completed_=0,last_seconds_=0,step_=3600;
    std::vector<int> active_cells_,pack_;
    std::vector<SnowKind> kind_;
    std::vector<MeshSnowGeometry> geometry_;
    std::vector<ueb::Site> site_;
    std::vector<ueb::HeldState> held_;
    std::vector<double> precipitation_,catch_,liquid_rate_,exposed_;
    std::vector<unsigned char> ground_contact_;
    std::vector<SnowOutputs> output_;
    std::vector<MeshSnowLedger> ledger_;
    snow::DegreeDaySnow degree_day_;
};
} // namespace openswmm::surface
