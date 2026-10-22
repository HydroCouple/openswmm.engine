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
 * whole-engine framing or ownership transfer occurs here.
 */
void meshState(detail::CompletedSourceArchive&, SurfaceStateData&);
}
}
