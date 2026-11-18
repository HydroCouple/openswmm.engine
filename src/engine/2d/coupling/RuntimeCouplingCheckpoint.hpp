// SPDX-License-Identifier: Apache-2.0

/**
 * @file      RuntimeCouplingCheckpoint.hpp
 * @brief     Checkpoint persistence for runtime coupling components.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#pragma once
#include "RuntimeCoupling.hpp"
#include "../data/BoundaryData.hpp"

namespace openswmm {
struct TableData;
}
namespace openswmm::twoD {
/// Internal copied-prescription component contract. The caller binds exact
/// model/cell and driver replay identity and a completed elapsed-second boundary.
/// This does not capture solver water, queues or external programs. Boundary
/// support is a separate internal profile with authored in-memory fallback.
struct RuntimeWaterCheckpointBinding {
    std::string identity;
    std::vector<bool> groundwater_cells;
    double completed_time = 0.0;
    std::vector<bool> boundary_slots;                // exterior edges, flat MeshData slots
    const BoundaryData* authored_boundary = nullptr; // immutable initial authored descriptors
    const TableData* boundary_tables = nullptr;      // referenced in-memory model signals
};

/// Same-platform water-only snapshot: both source domains and rain/ET/paired
/// infiltration. Cleared/expired records retain their receipts. Derived lookup
/// tables are rebuilt, never serialized. Failure preserves every output.
class RuntimeWaterCheckpoint {
public:
    /**
     * @brief Check copied water prescriptions against exact accepted model/receiver/time
     * metadata.
     *
     * @par Arguments
     * - unnamed binding/model argument (b) (in): Stable model/driver identity,
     * active-GW/exterior masks and completed elapsed s.
     * - surface (in): Stable surface source store, a distinct object from groundwater.
     * - groundwater (in): Stable groundwater source store.
     * - forcing (in): Stable rain/ET/paired-infiltration prescription store.
     * - boundary (in): Optional stable current boundary, required exactly when b has boundary
     * slots.
     * - completed_boundary (in): True checks completed boundary cumulative flux/receipts; false
     * defers only those boundary checks during in-progress stage accounting.
     * @return Empty for a valid privately copied/rebuilt candidate, otherwise first validation
     * or caught standard-exception diagnostic. Every input remains unchanged; no expiry/physics
     * is applied.
     * @note This internal same-platform profile excludes heat/species prescriptions and
     * file-backed boundary signals. Caller supplies synchronization and borrowed model/table
     * lifetime.
     */
    static std::string validate(const RuntimeWaterCheckpointBinding&, const RuntimeSources& surface,
                                const RuntimeSources& groundwater, const RuntimeForcings&,
                                const BoundaryData* boundary = nullptr,
                                bool completed_boundary = true);
    /**
     * @brief Encode a fully validated completed copied-water prescription snapshot.
     *
     * @par Arguments
     * - unnamed binding/model argument (b) (in): Exact immutable model/driver/receiver/time
     * binding.
     * - surface (in): Stable surface store, distinct from groundwater.
     * - groundwater (in): Stable groundwater store, including qualified boundary records when
     * enabled.
     * - forcing (in): Stable rain/ET/paired-infiltration store.
     * - bytes (out): Swapped only after successful complete encoding and <=64 MiB size check;
     * unchanged on reported failure.
     * - boundary (in): Live hydraulic boundary state, required exactly for nonempty boundary
     * slots.
     * @return Empty on capture, otherwise validation/archive/size or caught standard-exception
     * diagnostic.
     * @note Saves rows and receipts including disabled/expired history, not derived lookup maps.
     * Envelope binds version 1/2, native int/double widths/endian, identity, receiver mask,
     * exact completed time, optional authored boundary model and CRC-32. Inputs/physics remain
     * unchanged; external driver programs, heat/species and external signal files are outside
     * this profile.
     */
    static std::string capture(const RuntimeWaterCheckpointBinding&, const RuntimeSources& surface,
                               const RuntimeSources& groundwater, const RuntimeForcings&,
                               std::string& bytes, const BoundaryData* boundary = nullptr);
    /**
     * @brief Validate and atomically replace copied prescription stores against a matching
     * prepared model.
     *
     * @par Arguments
     * - unnamed binding/model argument (b) (in): Immutable model/driver/receiver/time binding
     * and optional authored boundary/tables.
     * - bytes (in): Owned <=64 MiB snapshot; exact profile/platform/model/masks/time/CRC and
     * framing must match. Malformed/empty input is rejected by archive decoding.
     * - surface (out): Replaced only after all private decoding and validation succeeds; must be
     * a distinct source object from groundwater.
     * - groundwater (out): Replaced on success with rebuilt source lookup/receipt state.
     * - forcing (out): Replaced on success with reconstructed effective forcing lookups.
     * - boundary (out): Mutable target required exactly when boundary slots exist; seeded
     * privately from authored model and replaced on success. Without that profile it must be
     * null.
     * @return Empty after no-throw moves publish every target; otherwise diagnostic or caught
     * standard exception, with targets unchanged. No external program, physical water/solver
     * state or table cursor is restored. Disabled prescription histories/receipts are preserved.
     * @note Caller excludes all target/model mutation during validation/publication and keeps
     * immutable fallback model data separate from mutable runtime ownership. No thread
     * synchronization occurs.
     */
    static std::string restore(const RuntimeWaterCheckpointBinding&, std::string bytes,
                               RuntimeSources& surface, RuntimeSources& groundwater,
                               RuntimeForcings&, BoundaryData* boundary = nullptr);
    /**
     * @brief Encode exact authored fallback descriptors and referenced in-memory signal
     * identity.
     *
     * @par Arguments
     * - unnamed binding/model argument (b) (in): Stable authored boundary model, with consistent
     * descriptor-array extents.
     * - tables (in): Optional stable model table owner, required for any series/curve reference.
     * @return Owned binary identity: descriptor arrays plus unique referenced table
     * IDs/types/records, relative-time anchoring and column metadata. Table lookup
     * cursors/runtime receipts are excluded.
     * @throws std::runtime_error On invalid referenced signal or archive failure; checked slot
     * access can throw std::out_of_range and allocation errors propagate. No tables/model arrays
     * are changed.
     * @note Constant/wall-only model encoding retains its older descriptor-only identity.
     * References must be finite single-column in-memory data; external file-backed signals are
     * not qualified.
     */
    static std::string boundaryModel(const BoundaryData&, const TableData* tables = nullptr);
};
}
