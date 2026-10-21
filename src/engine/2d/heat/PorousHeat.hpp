// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <string>
#include <vector>
namespace openswmm::twoD {
struct SubsurfaceState;

// Local thermal equilibrium: theta is water per bulk volume. Solids do not
// disappear when the water table moves or a hydraulic layer collapses.
struct ThermalMaterial {
    double porosity=.4,grain_density=2650,grain_specific_heat=880;
    double grain_conductivity=2,water_conductivity=.606,air_conductivity=.025;
    double water_heat_capacity=4184000;
    bool geometric=false;
    std::string validate() const;
    double volumetricCapacity(double theta) const;
    double conductivity(double theta) const;
};
struct ThermalStore {double capacity=0,energy=0;};
struct ThermalContact {int a=-1,b=-1;double conductance=0;};
struct ThermalBoundary {int store=-1;double conductance=0,temperature=0,power=0;};
struct ThermalSolution {
    std::string error;
    bool interval_limit=false; // bounded work, distinct from invalid physics
    std::vector<double> energy,temperature,contact_j,boundary_j;
    double algebraic_residual=0;
    int iterations=0;
};
// Backward Euler for a reciprocal thermal contact network. All stores are
// finite; all contact receipts enter one store and leave the other. Inputs
// are never modified, including on numerical refusal.
ThermalSolution solveThermalContacts(const std::vector<ThermalStore>&,
    const std::vector<ThermalContact>&,const std::vector<ThermalBoundary>&,double seconds);
// Implicit midpoint with conservative receipts and bounded monotone
// subdivision for stiff contact graphs. Boundary data is held for seconds.
// This does not advance moisture, advection or a borrowed thermal owner.
ThermalSolution solveThermalContactsRk2(const std::vector<ThermalStore>&,
    const std::vector<ThermalContact>&,const std::vector<ThermalBoundary>&,double seconds);

struct PorousHeatColumn {
    ThermalMaterial material;
    double area=0;
    std::vector<double> depth_edges,theta,capacity,energy,initial_energy,conductivity;
    std::string initialize(double area_m2,const std::vector<double>& fixed_depth_edges,
        const ThermalMaterial&,const std::vector<double>& moisture,const std::vector<double>& initial_celsius);
    // Changing theta changes capacity, never energy. Physical water transfers
    // must add/remove their own sensible heat before calling this method.
    std::string setMoisture(const std::vector<double>&);
    std::vector<ThermalStore> stores() const;
    std::vector<ThermalContact> contacts() const;
};
struct ThermalAdvection {
    std::string error;
    std::vector<double> vertical_water,vertical_j,temperature,withdrawn_j;
    double external_j=0,water_residual=0;
};
// Fixed-grid flux reconstruction from accepted physical external transfers
// and overlap-projected hydraulic water contents. Geometric sigma handover
// is not an external transfer. The interior flux is the unique conservative
// net flux satisfying each fixed layer's water balance (piston attribution
// for a bulk closure). External energy is the actual paired donor receipt.
ThermalAdvection advectPorousColumn(PorousHeatColumn&,const std::vector<double>& new_moisture,
    const std::vector<double>& external_water_m3,const std::vector<double>& external_j,
    const std::vector<double>& implicit_withdrawal_m3={});
// Forward-Euler energy derivative for shared RK stages. water_change is
// the beginning-state fixed-grid water tangent integrated over the stage;
// endpoint moisture only derives the next capacity. Donor temperatures
// exclude already reserved outgoing water and are returned for paired
// withdrawal receipts. No column mutation on refusal.
ThermalAdvection advectPorousColumnEuler(PorousHeatColumn&,const std::vector<double>& new_moisture,
    const std::vector<double>& water_change_m3,const std::vector<double>& external_water_m3,
    const std::vector<double>& external_j,const std::vector<double>& withdrawal_m3,
    const std::vector<double>& donor_temperature);
// Tangent of the overlap projection from equal stretching hydraulic layers
// to fixed physical layers. Water depths are per hydraulic layer. Moving
// interface jumps belong to the receiving fixed layer at an aligned edge.
std::string projectGroundWaterIncrement(const PorousHeatColumn&,double unsaturated_length,
    double length_change,const std::vector<double>& old_water_depth,
    const std::vector<double>& water_depth_change,std::vector<double>& fixed_water_change);
// Conservative geometric projection. Bulk closures use uniform unsaturated
// theta; SIGMA uses overlaps of accepted stretching hydraulic layers. The
// thermal depth grid itself never moves. Optional sat_water is SI m3/layer.
std::string projectGroundMoisture(const SubsurfaceState&,int cell,double saturated_thickness,
    const std::vector<double>& fixed_depth_edges,std::vector<double>& theta,
    std::vector<double>* sat_water=nullptr);
} // namespace openswmm::twoD
