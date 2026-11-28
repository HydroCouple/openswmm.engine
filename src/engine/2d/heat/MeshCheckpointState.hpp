// SPDX-License-Identifier: Apache-2.0

/**
 * @file      MeshCheckpointState.hpp
 * @brief     Mesh state stored by thermal checkpoints.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#pragma once
namespace openswmm::detail {
class CompletedSourceArchive;
}
namespace openswmm::twoD {
struct SurfaceStateData;
struct SurfaceTransportState;
struct MeshData;
struct SolverOptions2D;
namespace checkpoint {
/**
 * @brief Archive and validate accepted mesh hydraulic, species and optional snow/thermal
 * runtime.
 * @par Arguments in order Sequential internal reader/writer archive, then mutable prepared
 * SurfaceStateData. Read mode requires matching model/profile and preallocated fixed-array
 * extents. Fields retain SI/native solver units; geometry, borrowed owners and provider
 * configuration are handled by the caller.
 * @throws std::runtime_error On unsupported participant profiles, archive shape or invalid
 * finite/water/species/age/bed runtime values. Allocation errors propagate.
 * @note Reading can change earlier fields before failure; restore into a private candidate for
 * transactional publication. Saves validate the same canonical state. No model reconstruction,
 * whole-engine framing or ownership transfer occurs here. The trailing `bool` (default true)
 * says whether the payload's format carries the mesh run statistics (meshStatistics()) after
 * the other mesh fields: writers pass true; a reader passes what the checkpoint's version or
 * profile marker says, so older checkpoints still load.
 */
void meshState(detail::CompletedSourceArchive&, SurfaceStateData&, bool statistics = true);
/**
 * @brief Archive the whole-run statistics block (SurfaceStateData::stat_max_head ...
 * stat_elapsed) that meshState() appends.
 * @par Arguments in order Sequential archive, the SurfaceStateData, and whether the payload
 * carries the block.
 * @note Absent on read (a checkpoint older than the block): the statistics restart from their
 * no-sample state (SurfaceStateData::reset_run_statistics()). Absent on write: nothing written.
 * @throws std::runtime_error On an unknown block profile, array extents that differ from the
 * prepared state, or infinite/negative-clock values.
 */
void meshStatistics(detail::CompletedSourceArchive&, SurfaceStateData&, bool present);
void meshModel(detail::CompletedSourceArchive&, const MeshData&, const SolverOptions2D&,
               const SurfaceTransportState&);
}
}
