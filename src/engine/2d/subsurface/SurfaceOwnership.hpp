// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin

/**
 * @file      SurfaceOwnership.hpp
 * @brief     Surface ownership assignments, review states, and validation.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_SURFACE_OWNERSHIP_HPP
#define OPENSWMM_SURFACE_OWNERSHIP_HPP
#include <functional>
#include <string>
#include <vector>
namespace openswmm {
struct SimulationContext;
}
namespace openswmm::twoD {
struct MeshData;
struct SolverOptions2D;
struct SubsurfaceConfig;
struct SurfaceOwnerRecord {
    std::string subcatch;
    /**
     * @brief Compare ownership records by their authored subcatchment identifier bytes.
     * @param[in] other Stable record to compare.
     * @return True when subcatch strings are exactly equal, including case; no name resolution,
     * whitespace normalization or ownership review is performed. Read-only, no retained
     * references.
     */
    bool operator==(const SurfaceOwnerRecord& other) const { return subcatch == other.subcatch; }
};
struct SurfaceOwnerObject {
    int subcatch = -1;
    std::string name, tag, reason;
    bool reviewed = false, lumped = false;
    double declared_area = 0, polygon_area = 0, lid_area = 0, pervious_area = 0,
           impervious_area = 0, native_lid_area = 0, inside_area = 0, outside_area = 0;
    int status = 0; // 0 outside, 1 unreviewed, 2 reviewed, 3 invalid/unmapped
};
struct SurfaceOwnerShare {
    int subcatch = -1, cell = -1;
    double weather_area = 0, pervious_area = 0, impervious_area = 0, lid_area = 0,
           native_lid_area = 0;
};
struct SurfaceOwnershipPreview {
    std::vector<SurfaceOwnerObject> objects;
    std::vector<SurfaceOwnerShare> shares;
    std::vector<double> mesh_weather_area;
    std::vector<std::string> errors;
    std::string token;
    bool cancelled = false;
    /**
     * @brief Test whether preview validation collected any error diagnostics.
     * @return errors.empty(). Cancellation and each object's status are separate fields and are
     * not checked, so callers must inspect cancelled before treating the preview as
     * complete/usable. No model activation, mutation or allocation occurs.
     */
    bool valid() const { return errors.empty(); }
};
// Geometry is read-only. The same resolver serves authoring and initialization.
/**
 * @brief Preview reviewed source footprints, receiver shares and weather-area ownership without
 * adoption.
 * - Argument ctx: Stable complete subcatchment names/area/impervious/lumped/LID/polygon,
 * unit/options and CRS metadata; all parallel vectors must cover declared counts.
 * - Argument mesh: Stable cell/vertex topology and geometry in declared authored or already SI
 * frame.
 * - Argument opts: Mesh scaling/process flags used to choose geometry conversion/qualification.
 * - Argument cfg: Stable authored aquifer/process/ownership settings used for profile and
 * fingerprint.
 * - Argument proposed: Candidate reviewed subcatchment records, matched by exact model names; no
 * live config mutation occurs.
 * - Argument progress (in): Optional synchronous callback(done,total),
 * total=n_cells+n_subcatches; false requests cancellation (opposite mapMeshPolyline callback
 * convention). May receive repeated total.
 * @return Owned preview with per-source status/reason, SI m2 shares/weather areas, diagnostics,
 * cancellation flag and FNV64 model token on normal completion. Cancellation may return partial
 * objects/shares and no token; errors retain diagnostics/derived preview, never install live
 * ownership.
 * @note Validates convex cell geometry, triangulates source polygons, checks declared versus
 * polygon area without renormalizing, subtracts LID/impervious fractions and exact cell
 * intersections, follows connected competing footprints and refuses unreviewed
 * competitors/overlapping polygons. Explicit review requires enabled MODE MESH/routing
 * ownership. Lumped aquifer precedence remains visible. Clamps only tiny weather-area residue
 * after reporting overbooking; no physics/native/source adapter activation or clock advance.
 * Allocations/callback exceptions propagate; stable exclusive lifecycle views required. Token is
 * a change detector, not authentication or full runtime qualification.
 */
SurfaceOwnershipPreview resolveSurfaceOwnership(const SimulationContext&, const MeshData&,
                                                const SolverOptions2D&, const SubsurfaceConfig&,
                                                const std::vector<SurfaceOwnerRecord>&,
                                                const std::function<bool(int, int)>& progress = {});
/**
 * @brief Retain one reviewed uniform subcatchment surface-ownership authoring record.
 * - Argument t: Exactly Subcatchment SUBCATCH UNIFORM, keywords byte case-insensitive.
 * - Argument rows: Appends exact subcatchment name only when no case-insensitive duplicate
 * exists.
 * @return Empty on success; unsupported representation/distribution/arity or duplicate
 * diagnostic.
 * @note Does not resolve name, geometry, physical ownership or runtime adapter qualification.
 * Allocation may throw; no water/native/clock update, exclusive authoring access.
 */
std::string parseSurfaceOwnerLine(const std::vector<std::string>&,
                                  std::vector<SurfaceOwnerRecord>&);
}
#endif
