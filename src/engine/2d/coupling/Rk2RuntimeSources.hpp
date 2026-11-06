// SPDX-License-Identifier: Apache-2.0

/**
 * @file      Rk2RuntimeSources.hpp
 * @brief     Prepared source prescriptions and weather for managed RK2 sessions.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_RK2_RUNTIME_SOURCES_HPP
#define OPENSWMM_RK2_RUNTIME_SOURCES_HPP
#include "RuntimeCoupling.hpp"
#include <cstdint>
#include <map>
namespace openswmm {
struct HeatState;
namespace climate {
struct ClimateState;
}
namespace detail {
class CompletedSourceArchive;
}
}
namespace openswmm::twoD {
class SubsurfaceSolver;
struct BoundaryData;
struct MeshData;
struct SurfaceStateData;
/**
 * @brief Validate copied RK2 source prescriptions and water/heat/species receipt shapes.
 *
 * @param[in] sources Stable source rows; no live lookup caches or physical state are changed.
 * @param[in] cells Prepared nonnegative cell count.
 * @param[in] species Prepared nonnegative species count; each row concentration/rate and receipt
 * mass vector must contain exactly this many entries.
 * @param[in] temperature Optional signed temperature row, -1 when absent; nonzero heat requires
 * one.
 * @param[in] age Optional age row, -1 when absent; age/temperature independent mass rates must be
 * zero.
 * @param[in] coverage Optional mask of exactly cells entries selecting active groundwater
 * receivers. Its presence permits boundary-edge/head metadata; without it only nonboundary rows
 * are accepted.
 * @return Empty on valid IDs/cells/uniqueness/finite flow+heat/expiry/species and gross/net
 * receipts, otherwise diagnostic. Temperature concentrations may be signed; other concentrations
 * nonnegative. Expiry may be zero or positive infinity here; active completed-time expiry is
 * checked separately.
 * @note Does not validate namespace owners, derived lookup caches, boundary edge upper topology,
 * complete time horizons or physical donor availability. Local identity checking allocates and
 * may throw; input rows/receipts remain unchanged.
 */
std::string validateRk2Sources(const RuntimeSources&, int cells, int species, int temperature,
                               int age, const std::vector<uint8_t>* coverage = nullptr);
/**
 * @brief Qualify prepared surface source/weather/snow state for the managed RK2 numerical
 * profile.
 *
 * @param[in] surface Stable prepared cell/transport/weather/snow state with valid row identities.
 * @param[in] ground Optional stable groundwater owner; used for paired infiltration coverage and
 * snow incompatibility with groundwater heat/transport. Null leaves that coverage check
 * deferred.
 * @return Empty on qualified source rows, forcing channels/modes/extents/receipts and
 * accepted-rain pending arrays; otherwise first diagnostic. Does not mutate prescriptions,
 * caches or water state.
 * @note Active snow requires its water-only owned context/geometry/catch factors, no
 * heat/transport, no copied weather or pending copied rain, no porous ground-contact mask,
 * finite native weather and valid accepted cover/water/energy ledgers. General forcing channels
 * 1..4 permit value/scale weather modes but no scale on infiltration/concentration; paired
 * infiltration requires local receivers. Held weather has no applied receipts; infiltration uses
 * outward water budget pairing. Allocation for duplicate-key checks can throw.
 * Namespace/replay-time checks are separate.
 */
std::string rk2SurfaceRuntimeProfile(const SurfaceStateData&,
                                     const SubsurfaceSolver* ground = nullptr);
// A managed restore binds copied prescriptions and prepared native weather.
// The standalone numerical loader retains its strict caller-held identity.
struct Rk2CheckpointBinding {
    const climate::ClimateState* climate = nullptr;
    const HeatState* heat = nullptr;
    const RuntimeSources* surface = nullptr;
    const RuntimeSources* groundwater = nullptr;
    const RuntimeForcings* forcing = nullptr;
    const BoundaryData* boundary = nullptr;
    const std::map<int, std::vector<double>>* boundary_quality = nullptr;
};
/**
 * @brief Detect any hydraulic boundary use or retained override/receipt history.
 *
 * @param[in] boundary Optional stable boundary owner.
 * @return False for null or wall-only arrays with no history; true for any receipts, saved
 * runtime originals or non-WALL current type. This capability test does not validate arrays or
 * descriptors.
 */
bool rk2HasSurfaceBoundary(const BoundaryData*);
/**
 * @brief Validate prepared RK2 exterior boundary descriptors, composition and physical receipts.
 *
 * @param[in] mesh Stable valid mixed mesh; padded slot shape is four times cell count, with valid
 * cell vertex counts and neighbors for exterior-edge tests.
 * @param[in] surface Stable boundary and transport owner. Boundary arrays must have exact slot
 * shape, transport ledgers at least n_species entries and prepared valid row metadata.
 * @return Empty for supported resolved exterior boundaries and exact override-owner/composition
 * layout, finite scalar/species receipts and gross/net pairing; otherwise diagnostic. Without a
 * hydraulic boundary profile, runtime boundary quality must be empty.
 * @note Authored quality must belong to a nonwall fallback edge and valid species; concentration
 * may be signed only for temperature. Applied boundary species receipts must match gained-minus-
 * lost physical transport ledgers at 1e-10 scale. Type/series arrangements require resolved
 * indices, but actual table identity/records are checked by model binding elsewhere. This
 * validator is read-only, does not reconstruct caches, and does not reject finite signed bed
 * slopes.
 */
std::string validateRk2SurfaceBoundary(const MeshData&, const SurfaceStateData&);
/**
 * @brief Archive a matched RK2 boundary model identity or its accepted runtime receipts.
 *
 * @param[in,out] a Sequential writer/reader; outer framing and final validation belong to the
 * owner.
 * @param[in,out] b Prepared matching boundary arrays. Model mode archives fixed descriptors,
 * fallback/owner maps and retained receipt keys; runtime mode fixed cumulative flux and existing
 * receipt values, plus head/flow work values for dynamic signals/curves.
 * @param[in,out] quality Runtime boundary concentration map, archived/rebuilt only in model mode.
 * @param[in] model True selects copied/authored/state-dependent/time-dependent/rated model
 * identity; false selects runtime payload against that already prepared exact profile and
 * receipt-key layout.
 * @param[in] species Nonnegative receipt row count used when rebuilding model receipt placeholders.
 * @throws std::runtime_error On profile mismatch, duplicate/unsorted/out-of-range keys, archive
 * shape or decoding failure. Allocation failures propagate; inputs may be partially decoded on
 * failure.
 * @note Dynamic work head/flow values are zeroed in writer model identity when supplied by a
 * resolved series/curve. Physical values retain SI/native units. This helper neither validates
 * full boundary physics/species budgets nor serializes referenced model table records; callers
 * stage private copies.
 */
void rk2SurfaceBoundaryArchive(detail::CompletedSourceArchive& a, BoundaryData& b,
                               std::map<int, std::vector<double>>& quality, bool model,
                               int species);
/**
 * @brief Archive copied RK2 source/forcing prescriptions for a managed model binding.
 *
 * @param[in,out] a Sequential archive; read mode may progressively replace row vectors.
 * @param[in,out] surface Surface source model rows.
 * @param[in,out] ground Groundwater source model rows, including optional boundary-edge/head
 * metadata.
 * @param[in,out] forcing Copied forcing model rows, including
 * channel/mode/rate/expiry/owner/composition.
 * @param[in] species Prepared nonnegative boundary receipt species count.
 * @param[in,out] boundary Optional hydraulic boundary model owner; required for encoded profile v2.
 * @param[in,out] quality Optional boundary composition map; required with v2 boundary profile.
 * @throws std::runtime_error On unsupported v1/v2 profile, missing boundary owner or archive
 * failures.
 * @note This archives prescriptions/identity, not complete active runtime: decoding disables
 * every row and clears source/forcing index/cell caches for later reconstruction. Boundary-head
 * sources omit derived flow when their head is finite. Receipts, enabled flags and live external
 * provider programs are not encoded by this helper. Full restored profile/replay validation is
 * separate; use private candidates because failures can leave partial row/model changes.
 */
void rk2ManagedRuntimeModel(detail::CompletedSourceArchive& a, RuntimeSources& surface,
                            RuntimeSources& ground, RuntimeForcings& forcing, int species,
                            BoundaryData* boundary = nullptr,
                            std::map<int, std::vector<double>>* quality = nullptr);
/**
 * @brief Check provider namespaces, active expiry and last-receipt horizons at a managed
 * boundary.
 *
 * @param[in] surface Stable source rows already validated for finite shapes/receipts.
 * @param[in] ground Stable groundwater source rows under the same prepared model.
 * @param[in] forcing Stable copied forcing rows already validated by the RK2 profile checks.
 * @param[in] completed Finite nonnegative accepted elapsed time s.
 * @return Empty when owners contain no slash/NUL, enabled records expire strictly later, last
 * receipt intervals fit completed/gross water, and nonboundary source IDs match direct/provider
 * local namespaces; otherwise diagnostic. Boundary IDs receive topology-specific validation
 * elsewhere.
 * @note Read-only; does not expire rows, rebuild lookups, verify external program state or
 * repeat all finite/shape validation. Prefix/sub-string construction can allocate and throw.
 */
std::string validateRk2ReplayIds(const RuntimeSources&, const RuntimeSources&,
                                 const RuntimeForcings&, double completed);
}
#endif
