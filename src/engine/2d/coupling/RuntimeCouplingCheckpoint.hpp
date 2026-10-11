// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "RuntimeCoupling.hpp"

namespace openswmm::twoD {
/// Internal copied-prescription component contract. The caller binds exact
/// model/cell and driver replay identity and a completed elapsed-second boundary.
/// This does not capture solver water, queues, boundaries, or external programs.
struct RuntimeWaterCheckpointBinding {
    std::string identity;
    std::vector<bool> groundwater_cells;
    double completed_time = 0.0;
};

/// Same-platform water-only snapshot: both source domains and rain/ET/paired
/// infiltration. Cleared/expired records retain their receipts. Derived lookup
/// tables are rebuilt, never serialized. Failure preserves every output.
class RuntimeWaterCheckpoint {
public:
    static std::string validate(const RuntimeWaterCheckpointBinding&,
        const RuntimeSources& surface, const RuntimeSources& groundwater,
        const RuntimeForcings&);
    static std::string capture(const RuntimeWaterCheckpointBinding&,
        const RuntimeSources& surface, const RuntimeSources& groundwater,
        const RuntimeForcings&, std::string& bytes);
    static std::string restore(const RuntimeWaterCheckpointBinding&,
        std::string bytes, RuntimeSources& surface, RuntimeSources& groundwater,
        RuntimeForcings&);
};
}
