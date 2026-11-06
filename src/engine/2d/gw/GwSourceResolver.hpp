// SPDX-License-Identifier: Apache-2.0

/**
 * @file      GwSourceResolver.hpp
 * @brief     Resolution of groundwater source prescriptions and ownership.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_GW_SOURCE_RESOLVER_HPP
#define OPENSWMM_GW_SOURCE_RESOLVER_HPP
#include "GwSourceForcing.hpp"
#include <unordered_map>
namespace openswmm {
struct SimulationContext;
}
namespace openswmm::twoD {
struct MeshData;
struct GwTransportData;
struct SubsurfaceState;
struct SubsurfaceTransportState;
/**
 * @brief Resolve a constant or immutable named series in elapsed simulation seconds.
 * - label (in): Nonnull diagnostic prefix, normally the calling input section name.
 * @return Owned signal sharing validated copied named ordinates, or a constant. Throws on
 * invalid constant/series, unknown name, I/O or allocation failure.
 * @par Unnamed arguments in order Simulation context (mutable table cache), finite constant in
 * caller units, optional series name, and a mutable per-context named-signal cache. Nonempty
 * name wins over the constant. File-backed tables may load cache pages; exact-name cache hits
 * require the same unchanged context/start date.
 */
GwSourceSignal resolveGwSignal(SimulationContext&, double, const std::string&,
                               std::unordered_map<std::string, GwSourceSignal>&, const char* label);
/**
 * @brief Resolve authored source scopes, normalized cell areas and transport prescriptions.
 * @return Owned source vector; throws on invalid scale, unmatched cells, invalid species/signals
 * or unsupported terms. A failed call returns no partial vector and leaves authored rows
 * unchanged.
 * @par Arguments in order Simulation context with mutable file-table cache; stable mesh with
 * tags; authored groundwater transport/source configuration; prepared hydraulic state with
 * positive active-cell area/depth; and prepared transport row identity. Signals are
 * copied/shared, flow is total m3/s, cell shares sum to one per source, and MASS is restricted
 * to pollutants with known native amount units.
 */
std::vector<GwResolvedSource> resolveGwSources(SimulationContext&, const MeshData&,
                                               const GwTransportData&, const SubsurfaceState&,
                                               const SubsurfaceTransportState&);
}
#endif
