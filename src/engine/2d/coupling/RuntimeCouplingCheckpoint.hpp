// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "RuntimeCoupling.hpp"
#include "../data/BoundaryData.hpp"

namespace openswmm::twoD {
/// Internal copied-prescription component contract. The caller binds exact
/// model/cell and driver replay identity and a completed elapsed-second boundary.
/// This does not capture solver water, queues or external programs. Boundary
/// support is a separate internal profile with authored wall fallback only.
struct RuntimeWaterCheckpointBinding {
    std::string identity;
    std::vector<bool> groundwater_cells;
    double completed_time = 0.0;
    std::vector<bool> boundary_slots; // exterior edges, flat MeshData slots
    const BoundaryData* authored_boundary = nullptr; // immutable initial wall model
};

/// Same-platform water-only snapshot: both source domains and rain/ET/paired
/// infiltration. Cleared/expired records retain their receipts. Derived lookup
/// tables are rebuilt, never serialized. Failure preserves every output.
class RuntimeWaterCheckpoint {
public:
    static std::string validate(const RuntimeWaterCheckpointBinding&,
        const RuntimeSources& surface, const RuntimeSources& groundwater,
        const RuntimeForcings&, const BoundaryData* boundary = nullptr, bool completed_boundary = true);
    static std::string capture(const RuntimeWaterCheckpointBinding&,
        const RuntimeSources& surface, const RuntimeSources& groundwater,
        const RuntimeForcings&, std::string& bytes, const BoundaryData* boundary = nullptr);
    static std::string restore(const RuntimeWaterCheckpointBinding&,
        std::string bytes, RuntimeSources& surface, RuntimeSources& groundwater,
        RuntimeForcings&, BoundaryData* boundary = nullptr);
    static std::string boundaryModel(const BoundaryData&);
};
}
