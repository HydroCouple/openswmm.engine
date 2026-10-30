// SPDX-License-Identifier: Apache-2.0

/**
 * @file      GwBoundaryForcing.hpp
 * @brief     Groundwater boundary forcing series and evaluation.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_GW_BOUNDARY_FORCING_HPP
#define OPENSWMM_GW_BOUNDARY_FORCING_HPP
#include "GwSourceForcing.hpp"
#include <cstdint>
namespace openswmm {
struct SimulationContext;
}
namespace openswmm::twoD {
struct MeshData;
struct SubsurfaceConfig;
struct SubsurfaceState;
struct SubsurfaceTransportState;
struct GwTransportData;
struct GwBoundaryTerm {
    int row = -1;
    bool flux = false;
    double scale = 1;
    GwSourceSignal signal;
};
struct GwResolvedBoundary {
    int cell = -1, edge = -1;
    bool head = false;
    double length = 0, distance = 0;
    GwSourceSignal signal;
    std::vector<GwBoundaryTerm> terms;
    double requested_in = 0, requested_out = 0, applied_in = 0, applied_out = 0;
    std::vector<double> mass_in, mass_out, flux_in, flux_out;
};
struct GwBoundaryPulse {
    std::size_t boundary = 0;
    double in = 0, out = 0;
    std::vector<double> mass, flux_in, flux_out;
};
/**
 * @brief Resolve unique active exterior hydraulic edges and their explicit transport
 * composition.
 * - coverage (in): Active-aquifer mask, at least the prepared state cell count.
 * - heat_capacity (in): Positive finite water heat capacity J/(m3 K) for HEATFLUX conversion.
 * @return Owned SI boundary descriptors with zero receipts; throws on invalid rows, composition,
 * geometry or signal resolution. No partial result is returned.
 * @par Positional dependencies The unnamed arguments supply simulation context, prepared SI
 * mesh, authored aquifer config, quality config, prepared hydraulic state and transport
 * identity, in that order. They must remain stable and internally consistent. Context signal
 * resolution can perform table I/O; all other inputs are read-only. Full field units and refusal
 * conditions are documented at the definition.
 */
std::vector<GwResolvedBoundary>
resolveGwBoundaries(SimulationContext&, const MeshData&, const SubsurfaceConfig&,
                    const GwTransportData&, const SubsurfaceState&, const SubsurfaceTransportState&,
                    const std::vector<uint8_t>& coverage, double heat_capacity);
/**
 * @brief Append unsorted interior knot and crossing times for a boundary integration interval.
 * @par Arguments in declaration order Resolved stable boundary; finite start/end elapsed
 * seconds; held interior head m; aquifer bottom elevation m; nonnegative aquifer thickness m;
 * mutable vector receiving additional cut times. No input state changes, interval checks or
 * deduplication occur. Allocation failure may leave a partially appended destination; the caller
 * sorts and deduplicates before integration.
 */
void appendGwBoundaryCuts(const GwResolvedBoundary&, double, double, double, double, double,
                          std::vector<double>&);
/**
 * @brief Integrate requested edge water and species receipts without booking or donor limiting.
 * - head (in): Held interior hydraulic head m.
 * - bottom (in): Aquifer bottom elevation m.
 * - thickness (in): Nonnegative aquifer thickness m.
 * - conductivity (in): Finite nonnegative hydraulic conductivity m/s.
 * - species (in): Nonnegative result row count; every resolved term row must be within it.
 * @return Owned pulse of water m3 and native_concentration*m3 species receipts. Throws on
 * nonfinite integrals or allocation failure; does not mutate model/solver state.
 * @par Leading unnamed arguments Stable resolved boundary, finite elapsed start seconds, and
 * finite positive duration seconds. Geometry and signals must already be resolved; the consuming
 * solver applies budgets/receipts.
 */
GwBoundaryPulse integrateGwBoundary(const GwResolvedBoundary&, double, double, double head,
                                    double bottom, double thickness, double conductivity,
                                    int species);
}
#endif
