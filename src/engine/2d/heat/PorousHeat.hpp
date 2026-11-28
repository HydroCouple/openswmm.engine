// SPDX-License-Identifier: Apache-2.0

/**
 * @file      PorousHeat.hpp
 * @brief     Local thermal equilibrium in porous ground layers.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#pragma once
#include <string>
#include <vector>
namespace openswmm::twoD {
struct SubsurfaceState;

// Local thermal equilibrium: theta is water per bulk volume. Solids do not
// disappear when the water table moves or a hydraulic layer collapses.
struct ThermalMaterial {
    double porosity = .4, grain_density = 2650, grain_specific_heat = 880;
    double grain_conductivity = 2, water_conductivity = .606, air_conductivity = .025;
    double water_heat_capacity = 4184000;
    bool geometric = false;
    /**
     * @brief Check finite porosity and positive phase thermal properties.
     * @return Empty string when porosity is in [0,1) and each phase density, heat capacity and
     * conductivity is finite and positive; otherwise the first diagnostic. No fields are
     * changed.
     */
    std::string validate() const;
    /**
     * @brief Compute local-equilibrium bulk heat capacity without an air heat-capacity term.
     * @param[in] theta Water volume per bulk volume, normally finite and within [0,porosity].
     * @return (1-porosity)*grain_density*grain_specific_heat + theta*water_heat_capacity in
     * J/(m3 K). No validation or clipping occurs; validate() and moisture bounds belong to the
     * caller.
     */
    double volumetricCapacity(double theta) const;
    /**
     * @brief Mix solid, water and air conductivities for a prescribed water fraction.
     * @param[in] theta Water volume per bulk volume, normally within [0,porosity].
     * @return Effective conductivity in W/(m K), using the geometric mean when geometric is true
     * and otherwise the volume-weighted arithmetic mean. Valid positive phase properties are
     * required; no validation, moisture clipping or numerical fallback occurs here.
     */
    double conductivity(double theta) const;
};
struct ThermalStore {
    double capacity = 0, energy = 0;
};
struct ThermalContact {
    int a = -1, b = -1;
    double conductance = 0;
};
struct ThermalBoundary {
    int store = -1;
    double conductance = 0, temperature = 0, power = 0;
};
struct ThermalSolution {
    std::string error;
    bool interval_limit = false; // bounded work, distinct from invalid physics
    std::vector<double> energy, temperature, contact_j, boundary_j;
    double algebraic_residual = 0;
    int iterations = 0;
};
// Caller-owned workspaces: inputs must not alias writable workspace/result storage.
// One active call owns each workspace. Borrowed mutable results expire on reuse;
// owning overloads below retain independent value semantics.
struct ThermalContactScratch {
    ThermalSolution result;
    std::vector<double> diag, rhs, x, r, z, p, ap, energy;
};
struct ThermalMidpointScratch {
    ThermalSolution result;
    ThermalContactScratch implicit;
    std::vector<long double> rate;
    std::vector<ThermalStore> current;
    std::vector<double> energy_correction, contact_correction, boundary_correction;
};
ThermalSolution& solveThermalContacts(const std::vector<ThermalStore>&,
                                      const std::vector<ThermalContact>&,
                                      const std::vector<ThermalBoundary>&, double seconds,
                                      ThermalContactScratch&);
ThermalSolution& solveThermalContactsRk2(const std::vector<ThermalStore>&,
                                         const std::vector<ThermalContact>&,
                                         const std::vector<ThermalBoundary>&, double seconds,
                                         ThermalMidpointScratch&);
// Backward Euler for a reciprocal thermal contact network. All stores are
// finite; all contact receipts enter one store and leave the other. Inputs
// are never modified, including on numerical refusal.
/**
 * @brief Solve reciprocal heat contacts by backward Euler and diagonal-preconditioned CG.
 *
 * @par Declaration arguments
 * - first store vector (in): Finite heat stores: positive finite capacity J/K and finite energy
 * J. Inputs are read-only; temperatures are energy/capacity in degrees C relative to the energy
 * datum.
 * - second contact vector (in): Reciprocal connections with distinct in-range store indices and
 * finite nonnegative conductance W/K. Positive returned contact receipts move from a to b.
 * - third boundary vector (in): In-range store indices, finite nonnegative conductance W/K,
 * finite prescribed temperature degrees C and finite signed power W. Positive receipts enter the
 * store.
 * - seconds (in): Finite strictly positive interval in seconds; boundaries remain fixed
 * throughout.
 * @return Owned solution with empty error on success, final energy J/temperature degrees C,
 * contact and boundary receipts J in input order, iteration count and equation residual J. On
 * numerical/physical refusal error is nonempty; other fields may be partial and must not be
 * published. Allocation failures propagate. All inputs remain unchanged.
 * @note Conjugate-gradient work is bounded by min(4096,4*n+64) iterations, with a 1e-13 relative
 * preconditioned-norm scale. Energies are updated from equal/opposite receipts, then
 * temperatures are recovered from those conservative energies; algebraic_residual measures their
 * solve defect.
 */
ThermalSolution solveThermalContacts(const std::vector<ThermalStore>&,
                                     const std::vector<ThermalContact>&,
                                     const std::vector<ThermalBoundary>&, double seconds);
// Implicit midpoint with conservative receipts and bounded monotone
// subdivision for stiff contact graphs. Boundary data is held for seconds.
// This does not advance moisture, advection or a borrowed thermal owner.
/**
 * @brief Solve reciprocal heat contacts by bounded implicit-midpoint subdivisions.
 *
 * @par Declaration arguments
 * - first store vector (in): Finite heat stores: positive finite capacity J/K and finite energy
 * J. Inputs are read-only; temperatures are energy/capacity in degrees C relative to the energy
 * datum.
 * - second contact vector (in): Reciprocal connections with distinct in-range store indices and
 * finite nonnegative conductance W/K. Positive returned contact receipts move from a to b.
 * - third boundary vector (in): In-range store indices, finite nonnegative conductance W/K,
 * finite prescribed temperature degrees C and finite signed power W. Positive receipts enter the
 * store.
 * - seconds (in): Finite strictly positive interval in seconds; boundaries remain fixed
 * throughout.
 * @return Owned solution with empty error on success, final energy J/temperature degrees C,
 * contact and boundary receipts J in input order, iteration count and equation residual J. On
 * numerical/physical refusal error is nonempty; other fields may be partial and must not be
 * published. Allocation failures propagate. All inputs remain unchanged.
 * @note Subdivision enforces dt_sub*sum(conductance)/(2*capacity) <= 1. More than 65536 required
 * subintervals returns interval_limit=true; other refusals leave it false. Each midpoint solve
 * uses backward Euler over half a subinterval, and doubled receipts update energy
 * conservatively. This method advances neither moisture nor external thermal owners.
 */
ThermalSolution solveThermalContactsRk2(const std::vector<ThermalStore>&,
                                        const std::vector<ThermalContact>&,
                                        const std::vector<ThermalBoundary>&, double seconds);

struct PorousHeatColumn {
    ThermalMaterial material;
    double area = 0;
    std::vector<double> depth_edges, theta, capacity, energy, initial_energy, conductivity;
    /**
     * @brief Build a validated candidate physical heat column and publish it on success.
     *
     * @par Declaration arguments
     * - area_m2 (in): Footprint in m2, finite and positive.
     * - fixed_depth_edges (in): Fixed depths in m, beginning at zero and strictly increasing,
     * moisture.size()+1.
     * - unnamed material (in): Material with finite porosity in [0,1) and positive phase
     * properties.
     * - moisture (in): Nonempty water fractions per layer, finite in [0,material porosity].
     * - initial_celsius (in): Finite degrees C per layer, with moisture.size() entries.
     * @return Empty on replacement success; diagnostic on validation refusal with this
     * unchanged. Allocation errors propagate before publication.
     */
    std::string initialize(double area_m2, const std::vector<double>& fixed_depth_edges,
                           const ThermalMaterial&, const std::vector<double>& moisture,
                           const std::vector<double>& initial_celsius);
    // Changing theta changes capacity, never energy. Physical water transfers
    // must add/remove their own sensible heat before calling this method.
    /**
     * @brief Recompute moisture-dependent heat capacity and conductivity without changing
     * energy.
     *
     * @par Declaration arguments
     * - unnamed moisture vector (in): Finite per-layer water fraction in [0,material.porosity],
     * with one fewer entry than depth_edges. Geometry/material must already describe this
     * column.
     * @return Empty on success; diagnostic for shape, bounds or invalid derived properties.
     * Validation and temporary allocation precede field replacement; allocation errors
     * propagate.
     * @note Water receipts must carry their own heat before this call. Energy and initial_energy
     * are untouched; changing capacity therefore changes the implied temperature.
     */
    std::string setMoisture(const std::vector<double>&);
    /**
     * @brief Copy canonical layer capacity and energy into finite-network stores.
     * @return Owned vector in layer order, capacity J/K and energy J. The prepared column must
     * have capacity.size() >= energy.size(); no shape or property validation is performed. May
     * allocate.
     */
    std::vector<ThermalStore> stores() const;
    void stores(std::vector<ThermalStore>& result) const;
    /**
     * @brief Construct conductive contacts between consecutive fixed layers.
     * @return Owned contacts joining j-1 to j, with W/K conductance equal to area divided by the
     * sum of the two half-layer resistances. Prepared positive conductivity and valid increasing
     * depth edges are required; energy.size() controls the layer count. No validation occurs;
     * may allocate.
     */
    std::vector<ThermalContact> contacts() const;
    void contacts(std::vector<ThermalContact>& result) const;
};
struct ThermalAdvection {
    std::string error;
    std::vector<double> vertical_water, vertical_j, temperature, withdrawn_j;
    double external_j = 0, water_residual = 0;
};
// Fixed-grid flux reconstruction from accepted physical external transfers
// and overlap-projected hydraulic water contents. Geometric sigma handover
// is not an external transfer. The interior flux is the unique conservative
// net flux satisfying each fixed layer's water balance (piston attribution
// for a bulk closure). External energy is the actual paired donor receipt.
/**
 * @brief Reconstruct conservative layer water fluxes and apply implicit upwind heat advection.
 *
 * @par Declaration arguments
 * - unnamed column (in,out): Prepared nonempty physical column; replaced only after successful
 * solve and finite energy checks. Material water heat capacity is J/(m3 K), energy J and area
 * m2.
 * - new_moisture (in): Endpoint water fractions per layer; same layer count and valid moisture
 * bounds.
 * - external_water_m3 (in): Signed accepted external water volumes m3 per layer, excluding
 * purely geometric hydraulic-grid remapping. The reconstructed column water balance must close.
 * - external_j (in): Signed paired external sensible energy J per layer.
 * - implicit_withdrawal_m3 (in): Optional nonnegative finite implicit outgoing volumes m3 per
 * layer; empty means none. Their donor heat is solved at the new implicit layer temperature and
 * subtracted.
 * @return Owned result with empty error on success, n-1 interior downward-positive water/energy
 * receipts m3/J, n donor temperatures degrees C and outgoing energies J, net external energy J
 * and bottom water closure residual m3. On refusal error is nonempty and column is unchanged;
 * other result fields may be partial. Allocation exceptions propagate without publishing the
 * candidate.
 * @note The prepared column's existing arrays/geometry must have consistent layer extents.
 * Closure tolerance is 1e-10 times total absolute external/change volume plus 1e-12 m3. This is
 * an integrated accepted-transfer update, not a rate calculation or conductive heat step.
 */
ThermalAdvection advectPorousColumn(PorousHeatColumn&, const std::vector<double>& new_moisture,
                                    const std::vector<double>& external_water_m3,
                                    const std::vector<double>& external_j,
                                    const std::vector<double>& implicit_withdrawal_m3 = {});
// Forward-Euler energy derivative for shared RK stages. water_change is
// the beginning-state fixed-grid water tangent integrated over the stage;
// endpoint moisture only derives the next capacity. Donor temperatures
// exclude already reserved outgoing water and are returned for paired
// withdrawal receipts. No column mutation on refusal.
/**
 * @brief Apply a conservative explicit heat-advection tangent for one shared RK stage.
 *
 * @par Declaration arguments
 * - unnamed column (in,out): Beginning-state fixed column, published only after all checks pass.
 * - new_moisture (in): Endpoint water fractions, used to compute next capacity and conductivity.
 * - water_change_m3 (in): Integrated beginning-state physical water tangent m3 per layer; used
 * for flux reconstruction rather than inferring it from endpoint moisture.
 * - external_water_m3 (in): Signed finite accepted external water volume m3 per layer.
 * - external_j (in): Signed finite paired external energy J per layer.
 * - withdrawal_m3 (in): Optional nonnegative finite outgoing volume m3 per layer; empty means
 * none.
 * - donor_temperature (in): Finite beginning-state donor temperatures degrees C per layer,
 * excluding already reserved outgoing water. These are copied into the returned
 * donor-temperature vector.
 * @return Owned receipts with empty error on success; interior n-1 downward-positive flux
 * volumes m3 and energy J, per-layer withdrawal J, net external energy J and closure residual
 * m3. A nonempty error means refusal with column unchanged and no publishable receipts.
 * @note All nonoptional vectors have n=column.theta.size() entries; n must be positive. Existing
 * column geometry must be consistent. The water closure tolerance is 1e-10*absolute volume scale
 * plus 1e-12 m3. Allocation failures propagate; moisture, conduction and outside owners are not
 * advanced independently. Supplied donor temperatures are trusted rather than recomputed.
 */
ThermalAdvection advectPorousColumnEuler(PorousHeatColumn&, const std::vector<double>& new_moisture,
                                         const std::vector<double>& water_change_m3,
                                         const std::vector<double>& external_water_m3,
                                         const std::vector<double>& external_j,
                                         const std::vector<double>& withdrawal_m3,
                                         const std::vector<double>& donor_temperature);
// Tangent of the overlap projection from equal stretching hydraulic layers
// to fixed physical layers. Water depths are per hydraulic layer. Moving
// interface jumps belong to the receiving fixed layer at an aligned edge.
/**
 * @brief Project a moving hydraulic-layer water tangent onto the fixed thermal grid.
 *
 * @par Declaration arguments
 * - unnamed column (in): Prepared column with positive finite area m2, finite porosity in [0,1),
 * and strictly increasing fixed depth edges starting at zero.
 * - unsaturated_length (in): Current unsaturated length m, finite positive and no greater than
 * grid depth.
 * - length_change (in): Integrated change of unsaturated length m, finite and signed.
 * - old_water_depth (in): Nonempty accepted water depths m per equal stretching hydraulic layer;
 * derived water fraction may exceed [0,porosity] only by the 1e-12 roundoff tolerance.
 * - water_depth_change (in): Finite water-depth tangent m per hydraulic layer; old_water.size()
 * entries.
 * - fixed_water_change (out): Replaced on success with integrated water changes m3 per fixed
 * layer. Remains unchanged on validation refusal or allocation failure.
 * @return Empty on success; diagnostic for invalid geometry, hydraulic moisture or overflow.
 * @note Includes moving-interface jump terms. At an aligned fixed-grid boundary, the receiving
 * layer is chosen from movement direction; no heat inventory or input column is changed.
 */
std::string projectGroundWaterIncrement(const PorousHeatColumn&, double unsaturated_length,
                                        double length_change,
                                        const std::vector<double>& old_water_depth,
                                        const std::vector<double>& water_depth_change,
                                        std::vector<double>& fixed_water_change);
// Conservative geometric projection. Bulk closures use uniform unsaturated
// theta; SIGMA uses overlaps of accepted stretching hydraulic layers. The
// thermal depth grid itself never moves. Optional sat_water is SI m3/layer.
/**
 * @brief Project accepted hydraulic moisture into a fixed physical thermal depth grid.
 *
 * @par Declaration arguments
 * - unnamed subsurface owner (in): Stable prepared subsurface owner with per-cell arrays sized
 * through n_cells and, for SIGMA, theta_sigma sized m_layers*n_cells in layer-major order.
 * - cell (in): Cell index in [0,s.n_cells).
 * - saturated_thickness (in): Accepted saturated thickness m, finite within [0,s.zs[i]].
 * - fixed_depth_edges (in): Finite increasing fixed depths m, from zero to aquifer depth within
 * 1e-12 relative tolerance, with at least two entries. Cell area and depth must be positive.
 * - theta (out): On success, replaced by one volumetric water fraction per fixed layer;
 * unchanged on refusal. Fractions outside phase bounds by at most 1e-12 are clipped for
 * roundoff.
 * - sat_water (out): Optional distinct output for saturated pore-water volume m3 per fixed
 * layer; untouched on failure. It must not alias output if both results are needed.
 * @return Empty on success; diagnostic for invalid target/grid, missing SIGMA layers or moisture
 * outside finite physical bounds. Allocation failures propagate without output publication.
 * @note Bulk closures distribute unsaturated water uniformly; SIGMA integrates overlaps of its
 * accepted stretching layers. No hydraulic or heat inventory is changed.
 */
std::string projectGroundMoisture(const SubsurfaceState&, int cell, double saturated_thickness,
                                  const std::vector<double>& fixed_depth_edges,
                                  std::vector<double>& theta,
                                  std::vector<double>* sat_water = nullptr);
} // namespace openswmm::twoD
