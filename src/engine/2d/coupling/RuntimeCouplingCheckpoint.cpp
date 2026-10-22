// SPDX-License-Identifier: Apache-2.0

/**
 * @file      RuntimeCouplingCheckpoint.cpp
 * @brief     Checkpoint persistence for runtime coupling components.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include "RuntimeCouplingCheckpoint.hpp"
#include "../../core/CompletedSourceArchive.hpp"
#include "../data/MeshData.hpp"
#include "../../data/TableData.hpp"
#include <map>
#include <stdexcept>
#include <type_traits>
#include <tuple>

namespace openswmm::twoD {
namespace {
using Archive = detail::CompletedSourceArchive;
constexpr uint64_t maxBytes = 64 * 1024 * 1024;
static_assert(std::is_nothrow_move_assignable_v<RuntimeSources>);
static_assert(std::is_nothrow_move_assignable_v<RuntimeForcings>);
static_assert(std::is_nothrow_move_assignable_v<BoundaryData>);
struct State {
    RuntimeSources surface, groundwater;
    RuntimeForcings forcing;
    BoundaryData boundary;
};
/**
 * @brief Refuse an invalid copied-water checkpoint condition.
 * @param[in] ok True returns normally; false throws.
 * @param[in] message Nonnull diagnostic text.
 * @throws std::runtime_error On false; no caller state rollback occurs in this helper.
 */
void require(bool ok, const char* message) {
    if (!ok)
        throw std::runtime_error(message);
}
/**
 * @brief Compare finite water receipt values with relative/absolute tolerance.
 * @param[in] a First value in the same physical units as b.
 * @param[in] b Second value.
 * @return Both values finite and abs(a-b) <= 1e-10*max(1,abs(a),abs(b)). Read-only, no
 * allocation.
 */
bool close(double a, double b) {
    return std::isfinite(a) && std::isfinite(b) &&
           std::abs(a - b) <= 1e-10 * std::max({1.0, std::abs(a), std::abs(b)});
}
/**
 * @brief Compute reflected IEEE CRC-32 for copied-prescription payload integrity.
 * @param[in] bytes Binary payload, including embedded NULs.
 * @return CRC-32 with 0xedb88320 polynomial, all-one start and final inversion. Zero for empty
 * payload; no allocation/mutation and no cryptographic authenticity guarantee.
 */
uint32_t checksum(const std::string& bytes) {
    uint32_t crc = ~uint32_t(0);
    for (unsigned char c : bytes) {
        crc ^= c;
        for (int k = 0; k < 8; ++k)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0u);
    }
    return ~crc;
}
/**
 * @brief Test a provider owner component for disallowed separators/termination bytes.
 * @param[in] owner Owner bytes; empty is permitted for unowned/direct prescriptions.
 * @return True when no forward slash or embedded NUL occurs. Does not validate Unicode, other
 * characters or length; no mutation/allocation occurs.
 */
bool ownerOk(const std::string& owner) {
    return owner.find('/') == std::string::npos && owner.find('\0') == std::string::npos;
}
/**
 * @brief Test a nonempty local source ID outside reserved provider/boundary namespaces.
 * @param[in] id Candidate ID bytes.
 * @return Nonempty, without embedded NUL and not beginning provider/ or boundary/. Other slashes
 * and case are preserved; no model lookup, mutation or allocation occurs.
 */
bool localIdOk(const std::string& id) {
    return !id.empty() && id.find('\0') == std::string::npos && id.rfind("provider/", 0) != 0 &&
           id.rfind("boundary/", 0) != 0;
}
/**
 * @brief Validate a direct source ID or exact provider-owner namespace prefix.
 * @param[in] row Stable source record with id/owner strings.
 * @return True when ownerOk() holds and an empty owner uses localIdOk(id), or a named owner uses
 * `provider/<owner>/<valid local ID>`. No source cell/flow/expiry/receipt checks occur.
 * @note Prefix/sub-string construction can allocate; input strings are not changed.
 */
bool sourceIdOk(const RuntimeSource& row) {
    if (!ownerOk(row.owner))
        return false;
    if (row.owner.empty())
        return localIdOk(row.id);
    const auto prefix = "provider/" + row.owner + "/";
    return row.id.rfind(prefix, 0) == 0 && localIdOk(row.id.substr(prefix.size()));
}
/**
 * @brief Validate bounded immutable model/receiver/time binding metadata.
 * @param[in] b Identity must be nonempty and shorter than 64 MiB; groundwater_cells must contain
 * 1..1000000 eligibility entries; completed_time must be finite nonnegative elapsed seconds.
 * @throws std::runtime_error On those failures. Boundary pointers/slot shapes are checked
 * separately; no live model/source state is mutated.
 */
void bindingOk(const RuntimeWaterCheckpointBinding& b) {
    require(!b.identity.empty() && b.identity.size() < maxBytes && !b.groundwater_cells.empty() &&
                b.groundwater_cells.size() <= 1000000 && std::isfinite(b.completed_time) &&
                b.completed_time >= 0,
            "Invalid runtime water checkpoint binding.");
}
/**
 * @brief Validate accumulated water-only receipt signs, net budget and last interval evidence.
 * @param[in] r Stable receipt: requested/applied/water_in/water_out in m3, last_flow m3/s,
 * last_dt s. Heat/species requested/applied fields must be exactly zero/empty for this profile.
 * @param[in] time Finite nonnegative completed elapsed seconds limiting last_dt.
 * @param[in] outward_positive True expects applied=water_out-water_in; false the opposite.
 * @throws std::runtime_error On nonfinite values, negative gross water/duration, net-budget
 * mismatch, last integrated flow exceeding cumulative gross water, nonzero flow at zero dt, or
 * nonzero accumulated receipts at initial time zero. Uses close() tolerance; does not mutate the
 * receipt.
 */
void receiptOk(const RuntimeReceipt& r, double time, bool outward_positive = false) {
    require(r.heat_requested == 0 && r.heat_applied == 0 && r.requested_mass.empty() &&
                r.applied_mass.empty(),
            "Runtime water checkpoint heat and species receipts are not qualified.");
    for (double v : {r.requested, r.applied, r.water_in, r.water_out, r.last_flow, r.last_dt})
        require(std::isfinite(v), "Nonfinite runtime water checkpoint receipt.");
    require(r.water_in >= 0 && r.water_out >= 0 && r.last_dt >= 0 && r.last_dt <= time &&
                close(r.applied,
                      outward_positive ? r.water_out - r.water_in : r.water_in - r.water_out),
            "Invalid runtime water checkpoint receipt budget.");
    const double last = std::abs(r.last_flow) * r.last_dt;
    require(std::isfinite(last) && std::isfinite(r.water_in + r.water_out) &&
                last <=
                    r.water_in + r.water_out + 1e-10 * std::max(1.0, r.water_in + r.water_out) &&
                (r.last_dt != 0 || r.last_flow == 0),
            "Invalid runtime water checkpoint last receipt.");
    if (time == 0)
        require(r.requested == 0 && r.applied == 0 && r.water_in == 0 && r.water_out == 0,
                "Initial runtime water checkpoint has accumulated receipts.");
}
/**
 * @brief Archive one authored boundary fallback descriptor's scalar and signal-reference fields.
 * @param[in,out] a Sequential reader/writer; archive failures may follow earlier decoded fields.
 * @param[in,out] v Descriptor to encode/overwrite: type, head m, flow m3/(s m), slope and
 * series/ curve indices/names. Semantic/model validation is separate; no rollback or table
 * resolution here.
 */
void original(Archive& a, BoundaryData::RuntimeOriginal& v) {
    a(v.type, v.head, v.flow, v.slope, v.stage_ts, v.flow_ts, v.rating, v.stage_name, v.flow_name,
      v.rating_name);
}
/**
 * @brief Copy one flat boundary slot's current hydraulic descriptor using checked vector access.
 * @param[in] b Stable boundary storage with all descriptor arrays spanning k.
 * @param[in] k Nonnegative padded MeshData slot index.
 * @return Owned scalar/name descriptor. String copying may allocate; no array pointers retained.
 * @throws std::out_of_range On any missing slot/array entry. No physical/model validation
 * occurs.
 */
BoundaryData::RuntimeOriginal descriptor(const BoundaryData& b, int k) {
    return {b.edge_bc_type.at(k),
            b.edge_bc_head.at(k),
            b.edge_bc_flow.at(k),
            b.edge_bed_slope.at(k),
            b.edge_bc_tseries.at(k),
            b.edge_bc_flow_tseries.at(k),
            b.edge_bc_rating_curve.at(k),
            b.edge_bc_tseries_name.at(k),
            b.edge_bc_flow_tseries_name.at(k),
            b.edge_bc_rating_curve_name.at(k)};
}
/**
 * @brief Compare exact boundary type, scalars and signal-reference fields.
 * @param[in] a First stable fallback descriptor.
 * @param[in] b Second stable fallback descriptor.
 * @return Tuple equality, including exact double/string comparison. NaN scalars compare unequal;
 * no tolerance, reference resolution, mutation or allocation occurs.
 */
bool same(const BoundaryData::RuntimeOriginal& a, const BoundaryData::RuntimeOriginal& b) {
    return std::tie(a.type, a.head, a.flow, a.slope, a.stage_ts, a.flow_ts, a.rating, a.stage_name,
                    a.flow_name, a.rating_name) ==
           std::tie(b.type, b.head, b.flow, b.slope, b.stage_ts, b.flow_ts, b.rating, b.stage_name,
                    b.flow_name, b.rating_name);
}
// Resolved values are work state; the authored signal and its table remain
// the fallback model. Clearing re-evaluates it before the next native flux.
/**
 * @brief Resolve/validate an authored in-memory boundary signal reference.
 * @param[in] index -1 means absent and requires empty name; -2 resolves name; other nonnegative
 * indices must agree with an optional supplied name.
 * @param[in] name Exact authored signal name, optionally empty for a resolved nonnegative index.
 * @param[in] tables Borrowed live table owner, required for present references. Referenced table
 * must be single-column in memory, with no file handle, nonempty finite increasing x and finite
 * y.
 * @param[in] timeseries True requires TIMESERIES type; false requires a non-timeseries curve
 * type.
 * @return Resolved valid table index, or -1 for a valid absent reference.
 * @throws std::runtime_error On missing/mismatching/wrong-kind/file-backed/invalid table. Does
 * not load external signals, change table cursors or retain table pointers.
 */
int reference(int index, const std::string& name, const TableData* tables, bool timeseries) {
    if (index == -1) {
        require(name.empty(), "Unexpected authored boundary signal name.");
        return -1;
    }
    require(tables && index >= -2, "Boundary signal model is missing.");
    const int resolved = index == -2 ? tables->find_by_kind(name, timeseries) : index;
    require(resolved >= 0 && resolved < int(tables->tables.size()) &&
                (name.empty() ? index >= 0 : tables->find_by_kind(name, timeseries) == resolved),
            "Boundary signal name or table index differs.");
    const auto& t = tables->tables[resolved];
    require((t.type == TableType::TIMESERIES) == timeseries && !t.is_file_based && !t.file_handle &&
                t.num_cols == 1 && !t.x.empty() && t.y.size() == t.x.size(),
            "Boundary signal requires a single-column in-memory table of its authored kind.");
    for (std::size_t i = 0; i < t.x.size(); ++i)
        require(std::isfinite(t.x[i]) && std::isfinite(t.y[i]) && (i == 0 || t.x[i] > t.x[i - 1]),
                "Invalid authored boundary signal records.");
    return resolved;
}
/**
 * @brief Check that current/saved boundary work state preserves its authored fallback model.
 * @param[in] value Stable current or saved-original descriptor.
 * @param[in] base Immutable authored fallback descriptor.
 * @param[in] tables Borrowed in-memory referenced table owner, required for present signals.
 * @return True when finite current head/flow and all model fields match, allowing resolved
 * signal indices and recomputed work head/flow for authored series/rating curves. False for
 * mismatch.
 * @throws std::runtime_error On invalid authored signal reference via reference(). Temporary
 * string copies can allocate; no live descriptor or table is changed.
 */
bool fallback(const BoundaryData::RuntimeOriginal& value, const BoundaryData::RuntimeOriginal& base,
              const TableData* tables) {
    auto expected = base, current = value;
    const auto signal = [&](int& a, int& b, const std::string& name, bool ts) {
        const int resolved = reference(b, name, tables, ts);
        if (a != b && a != resolved)
            return false;
        a = b;
        return true;
    };
    if (!signal(current.stage_ts, expected.stage_ts, base.stage_name, true) ||
        !signal(current.flow_ts, expected.flow_ts, base.flow_name, true) ||
        !signal(current.rating, expected.rating, base.rating_name, false))
        return false;
    if (base.stage_ts != -1)
        current.head = base.head;
    if (base.flow_ts != -1 || base.rating != -1)
        current.flow = base.flow;
    return std::isfinite(value.head) && std::isfinite(value.flow) && same(current, expected);
}
/**
 * @brief Archive fixed-extent hydraulic boundary descriptors without receipts or model tables.
 * @param[in,out] a Sequential reader/writer; read requires matching preallocated array extents.
 * @param[in,out] b Descriptor arrays to encode/progressively overwrite, with head m and
 * per-metre flow m3/(s m). No fallback/shape/finite validation is performed here beyond archive
 * extents; caller decodes into private state and validates afterward.
 */
void boundaryFields(Archive& a, BoundaryData& b) {
    a.fixed(b.edge_bc_type, b.edge_bed_slope, b.edge_bc_head, b.edge_bc_tseries,
            b.edge_bc_tseries_name, b.edge_bc_flow, b.edge_bc_flow_tseries,
            b.edge_bc_flow_tseries_name, b.edge_bc_rating_curve, b.edge_bc_rating_curve_name);
}
/**
 * @brief Validate qualified exterior overrides, authored fallback and boundary receipt pairing.
 * @param[in] v Stable candidate boundary state.
 * @param[in] b Binding with immutable authored boundary/tables, exact four-slot-per-cell
 * exterior mask and completed elapsed seconds. Empty boundary_slots disables this boundary
 * profile check.
 * @param[in] completed True also validates accumulated receipt budgets and cumulative edge flux
 * against -applied. False defers those boundary-only completed-ledger checks during a stage.
 * @throws std::runtime_error On descriptor/flux/owner shape mismatch, invalid authored kind or
 * nonnegative-slope constraint, bad table references, interior override/receipt, lost fallback
 * or unsupported runtime override fields. Overrides are constant stage/flow with cleared signal
 * refs.
 * @note Current/retained fluxes must be finite. Tables must be in memory; model data is
 * read-only. This qualified checkpoint profile may be stricter than general runtime boundary
 * authoring.
 */
void boundaryOk(const BoundaryData& v, const RuntimeWaterCheckpointBinding& b, bool completed) {
    if (b.boundary_slots.empty())
        return;
    require(b.authored_boundary &&
                b.boundary_slots.size() == b.groundwater_cells.size() * kMaxCellVerts,
            "Invalid boundary checkpoint model binding.");
    const auto& authored = *b.authored_boundary;
    const auto shape = [&](const BoundaryData& data) {
        const auto n = b.boundary_slots.size();
        return data.edge_bc_type.size() == n && data.edge_bed_slope.size() == n &&
               data.edge_bc_head.size() == n && data.edge_bc_tseries.size() == n &&
               data.edge_bc_tseries_name.size() == n && data.edge_bc_flow.size() == n &&
               data.edge_bc_flow_tseries.size() == n &&
               data.edge_bc_flow_tseries_name.size() == n &&
               data.edge_bc_rating_curve.size() == n && data.edge_bc_rating_curve_name.size() == n;
    };
    require(shape(authored) && shape(v), "Boundary checkpoint descriptor shape differs.");
    require(v.edge_bc_cum_flux.size() == b.boundary_slots.size() &&
                v.runtime_original.size() == v.runtime_owner.size(),
            "Boundary checkpoint flux shape or ownership differs.");
    for (std::size_t k = 0; k < b.boundary_slots.size(); ++k) {
        const auto base = descriptor(authored, int(k)), current = descriptor(v, int(k));
        require(base.type >= int8_t(BoundaryType::WALL) &&
                    base.type <= int8_t(BoundaryType::RATING_CURVE) && std::isfinite(base.head) &&
                    std::isfinite(base.flow) && std::isfinite(base.slope) && base.slope >= 0 &&
                    (b.boundary_slots[k] || base.type == int8_t(BoundaryType::WALL)) &&
                    (base.type == int8_t(BoundaryType::SPECIFIED_STAGE) ||
                     (base.stage_ts == -1 && base.stage_name.empty())) &&
                    (base.type == int8_t(BoundaryType::SPECIFIED_FLOW) ||
                     (base.flow_ts == -1 && base.flow_name.empty())) &&
                    (base.type == int8_t(BoundaryType::RATING_CURVE)
                         ? base.rating != -1
                         : (base.rating == -1 && base.rating_name.empty())),
                "Invalid authored boundary fallback descriptor.");
        reference(base.stage_ts, base.stage_name, b.boundary_tables, true);
        reference(base.flow_ts, base.flow_name, b.boundary_tables, true);
        reference(base.rating, base.rating_name, b.boundary_tables, false);
        const auto old = v.runtime_original.find(int(k));
        if (old == v.runtime_original.end())
            require(fallback(current, base, b.boundary_tables) && !v.runtime_owner.count(int(k)),
                    "Boundary checkpoint changed its authored fallback without an override.");
        else
            require(b.boundary_slots[k] && fallback(old->second, base, b.boundary_tables) &&
                        v.runtime_owner.count(int(k)) && ownerOk(v.runtime_owner.at(int(k))) &&
                        v.receipts.count(int(k)) &&
                        (current.type == int8_t(BoundaryType::SPECIFIED_STAGE) ||
                         current.type == int8_t(BoundaryType::SPECIFIED_FLOW)) &&
                        std::isfinite(current.head) && std::isfinite(current.flow) &&
                        current.slope == base.slope && current.stage_ts == -1 &&
                        current.flow_ts == -1 && current.rating == -1 &&
                        current.stage_name.empty() && current.flow_name.empty() &&
                        current.rating_name.empty(),
                    "Invalid boundary checkpoint override or saved fallback.");
        require(std::isfinite(v.edge_bc_cum_flux[k]),
                "Nonfinite boundary checkpoint cumulative flux.");
        const auto receipt = v.receipts.find(int(k));
        require(!completed || (receipt != v.receipts.end()
                                   ? close(v.edge_bc_cum_flux[k], -receipt->second.applied)
                                   : v.edge_bc_cum_flux[k] == 0),
                "Boundary checkpoint edge flux and water receipt differ.");
    }
    for (const auto& [slot, old] : v.runtime_original)
        require(slot >= 0 && slot < int(b.boundary_slots.size()) && b.boundary_slots[slot],
                "Boundary checkpoint override is not an exterior edge.");
    for (const auto& [slot, r] : v.receipts) {
        require(slot >= 0 && slot < int(b.boundary_slots.size()) && b.boundary_slots[slot],
                "Boundary checkpoint receipt is not an exterior edge.");
        if (completed)
            receiptOk(r, b.completed_time);
    }
}
/**
 * @brief Validate a copied row's finite-positive or infinite validity horizon.
 * @param[in] until Expiry elapsed s, finite positive or positive infinity.
 * @param[in] enabled Whether the row is still active.
 * @param[in] time Completed elapsed s.
 * @throws std::runtime_error On invalid expiry or enabled row at/past expiry. Disabled rows may
 * retain old expiry and receipts; no automatic expire/delete operation occurs.
 */
void expiryOk(double until, bool enabled, double time) {
    require((std::isfinite(until) && until > 0) || until == std::numeric_limits<double>::infinity(),
            "Invalid runtime water checkpoint expiry.");
    require(!enabled || until > time,
            "Runtime water checkpoint has an active record at or past its expiry.");
}
/**
 * @brief Validate copied source records and rebuild their canonical derived indexes privately.
 * @param[in,out] store Private source store; replaced by its rebuilt candidate only after
 * success.
 * @param[in] b Validated receiver/time binding.
 * @param[in] check True compares existing source index/cell lookup to rebuilt rows; false
 * reconstructs without trusting old caches, as during restore.
 * @param[in] groundwater True permits qualified `boundary/<cell>/<edge>` records on enabled
 * exterior slots; false accepts direct/provider source IDs only.
 * @throws std::runtime_error On identity/cell/finite-flow, non-water fields,
 * boundary/expiry/receipt, duplicate-row or lookup mismatch. Allocation failures propagate
 * before store replacement.
 * @note Boundary records require infinite validity and finite-or-NaN boundary head. Nonboundary
 * records require edge=-1 and NaN head. Flow is signed m3/s; no physical water is booked.
 */
void sourceStore(RuntimeSources& store, const RuntimeWaterCheckpointBinding& b, bool check,
                 bool groundwater) {
    RuntimeSources next;
    next.rows = store.rows;
    const int nc = static_cast<int>(b.groundwater_cells.size());
    for (std::size_t k = 0; k < next.rows.size(); ++k) {
        const auto& r = next.rows[k];
        const bool boundary = groundwater && !b.boundary_slots.empty() && r.boundary_edge >= 0;
        require((boundary ? (ownerOk(r.owner) && r.id == "boundary/" + std::to_string(r.cell) +
                                                             "/" + std::to_string(r.boundary_edge))
                          : sourceIdOk(r)) &&
                    r.cell >= 0 && r.cell < nc && std::isfinite(r.flow),
                "Invalid runtime water checkpoint source identity, cell or flow.");
        require(r.heat == 0 && r.concentrations.empty() && r.rates.empty(),
                "Runtime water checkpoint heat and species sources are not qualified.");
        if (boundary)
            require(r.boundary_edge < kMaxCellVerts &&
                        b.boundary_slots.at(MeshData::slot(r.cell, r.boundary_edge)) &&
                        (std::isnan(r.boundary_head) || std::isfinite(r.boundary_head)) &&
                        r.until == std::numeric_limits<double>::infinity(),
                    "Invalid groundwater boundary checkpoint edge, stage or validity.");
        else
            require(r.boundary_edge == -1 && std::isnan(r.boundary_head),
                    "Runtime water checkpoint boundary profile is not qualified.");
        expiryOk(r.until, r.enabled, b.completed_time);
        receiptOk(r.receipt, b.completed_time);
        require(next.index[r.id].emplace(r.cell, k).second,
                "Duplicate runtime water checkpoint source identity.");
    }
    next.rebuild(nc);
    if (check)
        require(store.index == next.index &&
                    (store.cells == next.cells || (store.rows.empty() && store.cells.empty())),
                "Runtime water checkpoint source lookup differs from its records.");
    store = std::move(next);
}
/**
 * @brief Validate copied water-only sources/forcings/boundaries and reconstruct their lookup
 * caches.
 * @param[in,out] s Private candidate; sub-stores may be rebuilt before a later check fails.
 * @param[in] b Exact model/driver/receiver/time binding.
 * @param[in] check True verifies existing cache consistency; false rebuilds from restored
 * records.
 * @param[in] completed Controls boundary edge completed-flux/receipt checks only; source/forcing
 * receipts are always validated.
 * @throws std::runtime_error On binding/source/boundary, inactive GW receiver, nonfinite
 * aggregate receipt or forcing identity/mode/channel/rate/expiry/ownership/receipt/cache
 * mismatch.
 * @note Forcing channels 1 rain/2 ET/3 paired infiltration only; rates nonnegative m/s and no
 * species composition. Infiltration must be local active GW, non-scale mode, outward-positive
 * paired budget. Rain/ET have no applied-volume receipts in this profile. Uniform/local enabled
 * rows may overlap only with the same owner. Effective cache checks tolerate disabled expiry
 * entries. No physics is changed; allocations can throw and caller must discard a failed private
 * candidate.
 */
void validateState(State& s, const RuntimeWaterCheckpointBinding& b, bool check,
                   bool completed = true) {
    bindingOk(b);
    sourceStore(s.surface, b, check, false);
    sourceStore(s.groundwater, b, check, true);
    boundaryOk(s.boundary, b, completed);
    if (!b.boundary_slots.empty()) {
        double incoming = 0, outgoing = 0;
        for (const auto& r : s.groundwater.rows) {
            incoming += r.receipt.water_in;
            outgoing += r.receipt.water_out;
        }
        require(std::isfinite(incoming) && std::isfinite(outgoing),
                "Nonfinite aggregate groundwater provider receipts.");
    }
    for (const auto& r : s.groundwater.rows)
        require(b.groundwater_cells[r.cell],
                "Runtime water checkpoint source has no groundwater receiver.");
    RuntimeForcings next;
    next.rows = s.forcing.rows;
    const int nc = static_cast<int>(b.groundwater_cells.size());
    std::map<int, std::string> uniform;
    for (std::size_t k = 0; k < next.rows.size(); ++k) {
        const auto& r = next.rows[k];
        require(r.channel >= 1 && r.channel <= 3 && r.cell >= -1 && r.cell < nc && r.mode >= 0 &&
                    r.mode <= 2 && (r.mode != 0 || !r.enabled) && std::isfinite(r.rate) &&
                    r.rate >= 0 && ownerOk(r.owner),
                "Invalid runtime water checkpoint forcing.");
        require(r.concentrations.empty(),
                "Runtime water checkpoint rain quality is not qualified.");
        if (r.channel == 3)
            require(r.mode != 2 && r.cell >= 0 && b.groundwater_cells[r.cell],
                    "Runtime water checkpoint infiltration needs a local groundwater receiver.");
        expiryOk(r.until, r.enabled, b.completed_time);
        receiptOk(r.receipt, b.completed_time, r.channel == 3);
        if (r.channel == 3)
            require(r.receipt.requested >= 0 && r.receipt.applied >= 0 &&
                        r.receipt.applied <=
                            r.receipt.requested + 1e-10 * std::max(1.0, r.receipt.requested) &&
                        r.receipt.water_in == 0 && r.receipt.last_flow >= 0,
                    "Invalid runtime water checkpoint paired infiltration receipt.");
        else
            require(r.receipt.requested == 0 && r.receipt.applied == 0 && r.receipt.water_in == 0 &&
                        r.receipt.water_out == 0 && r.receipt.last_flow == 0 &&
                        r.receipt.last_dt == 0,
                    "Runtime water checkpoint rain and ET do not have applied-volume receipts.");
        require(next.index[r.channel].emplace(r.cell, k).second,
                "Duplicate runtime water checkpoint forcing identity.");
        if (r.enabled && r.cell == -1)
            uniform.emplace(r.channel, r.owner);
    }
    for (const auto& r : next.rows)
        if (r.enabled && r.cell >= 0) {
            const auto u = uniform.find(r.channel);
            require(u == uniform.end() || u->second == r.owner,
                    "Runtime water checkpoint forcing ownership overlaps.");
        }
    next.rebuild(nc);
    if (check) {
        const auto& old = s.forcing;
        require(old.index == next.index && (old.cells.size() == std::size_t(nc) ||
                                            (old.rows.empty() && old.cells.empty())),
                "Runtime water checkpoint forcing lookup differs from its records.");
        for (int k : old.uniform)
            require(k >= -1 && k < int(old.rows.size()),
                    "Invalid runtime water checkpoint uniform lookup.");
        for (const auto& cell : old.cells)
            for (int k : cell)
                require(k >= -1 && k < int(old.rows.size()),
                        "Invalid runtime water checkpoint cell lookup.");
        // Expiry leaves disabled forcing indices in place; compare the effective
        // lookups instead of requiring identical inactive cache entries.
        for (int c = 0; c < nc; ++c)
            for (int ch = 1; ch <= 4; ++ch) {
                const auto* a = old.get(ch, c);
                const auto* n = next.get(ch, c);
                require((!a && !n) || (a && n && a - old.rows.data() == n - next.rows.data()),
                        "Runtime water checkpoint effective forcing lookup differs.");
            }
    }
    s.forcing = std::move(next);
}
/**
 * @brief Archive only the six qualified water receipt fields.
 * @param[in,out] a Sequential reader/writer.
 * @param[in,out] r Water requested/applied/gross m3 and last flow/duration m3/s/s to encode or
 * progressively overwrite. Heat/species fields are excluded and must already be zero/empty. No
 * semantic validation or rollback; archive/allocation errors propagate.
 */
void receipt(Archive& a, RuntimeReceipt& r) {
    a(r.requested, r.applied, r.water_in, r.water_out, r.last_flow, r.last_dt);
}
/**
 * @brief Archive copied source/forcing rows and optional fixed boundary overrides/receipts.
 * @param[in,out] a Sequential archive, with outer framing/CRC validation owned by caller.
 * @param[in,out] s Private state to encode/decode; row vectors resize, boundary descriptor/flux
 * vectors require prepared authored extents. Lookup caches are omitted and rebuilt afterward.
 * @param[in] boundaries True includes boundary edge/head source metadata, hydraulic descriptors,
 * cumulative outward edge flux, fallback owners and retained receipts; false uses water-only v1.
 * @throws std::runtime_error On duplicate decoded override/receipt keys or archive failures.
 * @note Decoding is progressive and may clear/rebuild maps before failure. No physical owners
 * are changed until the outer restore publishes complete validated state.
 */
void state(Archive& a, State& s, bool boundaries) {
    const auto source = [&](auto& r) {
        a(r.id, r.owner, r.cell, r.flow, r.until, r.enabled);
        if (boundaries)
            a(r.boundary_edge, r.boundary_head);
        receipt(a, r.receipt);
    };
    a.sequence(s.surface.rows, source);
    a.sequence(s.groundwater.rows, source);
    a.sequence(s.forcing.rows, [&](auto& r) {
        a(r.channel, r.cell, r.mode, r.rate, r.until, r.enabled, r.owner);
        receipt(a, r.receipt);
    });
    if (boundaries) {
        boundaryFields(a, s.boundary);
        a.fixed(s.boundary.edge_bc_cum_flux);
        struct Row {
            int slot = 0;
            BoundaryData::RuntimeOriginal old{};
            std::string owner;
        };
        std::vector<Row> rows;
        for (const auto& [slot, old] : s.boundary.runtime_original)
            rows.push_back({slot, old, s.boundary.runtime_owner.at(slot)});
        a.sequence(rows, [&](auto& r) {
            a(r.slot);
            original(a, r.old);
            a(r.owner);
        });
        std::vector<std::pair<int, RuntimeReceipt>> receipts(s.boundary.receipts.begin(),
                                                             s.boundary.receipts.end());
        a.sequence(receipts, [&](auto& r) {
            a(r.first);
            receipt(a, r.second);
        });
        if (a.reading()) {
            s.boundary.runtime_original.clear();
            s.boundary.runtime_owner.clear();
            s.boundary.receipts.clear();
            for (auto& r : rows) {
                require(s.boundary.runtime_original.emplace(r.slot, std::move(r.old)).second,
                        "Duplicate boundary override.");
                s.boundary.runtime_owner.emplace(r.slot, std::move(r.owner));
            }
            for (auto& r : receipts)
                require(s.boundary.receipts.emplace(r.first, std::move(r.second)).second,
                        "Duplicate boundary receipt.");
        }
    }
}
/**
 * @brief Copy live prescription stores into a private validation/capture candidate.
 * @param[in] b Binding selecting whether boundary profile exists.
 * @param[in] s Stable surface source rows/caches.
 * @param[in] g Stable groundwater source rows/caches; must be distinct from s at the public
 * entry.
 * @param[in] f Stable forcing rows/caches.
 * @param[in] boundary Must be null without boundary slots, or a live candidate with a nonnull
 * authored binding when boundary slots exist. Borrowed only during copying.
 * @return Owned State; strings/rows/maps copied independently while immutable shared model
 * objects retain their normal ownership. No input state change.
 * @throws std::runtime_error On boundary profile/pointer mismatch; allocation errors propagate.
 */
State privateState(const RuntimeWaterCheckpointBinding& b, const RuntimeSources& s,
                   const RuntimeSources& g, const RuntimeForcings& f,
                   const BoundaryData* boundary) {
    require(b.boundary_slots.empty() ? !boundary : boundary && b.authored_boundary,
            "Boundary checkpoint profile or target differs.");
    return {s, g, f, boundary ? *boundary : BoundaryData{}};
}
}
/**
 * @brief Encode exact authored fallback descriptors and referenced in-memory signal identity.
 * @param[in] b Stable authored boundary model, with consistent descriptor-array extents.
 * @param[in] tables Optional stable model table owner, required for any series/curve reference.
 * @return Owned binary identity: descriptor arrays plus unique referenced table
 * IDs/types/records, relative-time anchoring and column metadata. Table lookup cursors/runtime
 * receipts are excluded.
 * @throws std::runtime_error On invalid referenced signal or archive failure; checked slot
 * access can throw std::out_of_range and allocation errors propagate. No tables/model arrays are
 * changed.
 * @note Constant/wall-only model encoding retains its older descriptor-only identity. References
 * must be finite single-column in-memory data; external file-backed signals are not qualified.
 */
std::string RuntimeWaterCheckpoint::boundaryModel(const BoundaryData& b, const TableData* tables) {
    auto copy = b;
    Archive a;
    boundaryFields(a, copy);
    std::vector<int> used;
    for (int k = 0; k < b.size(); ++k)
        for (int i :
             {reference(b.edge_bc_tseries.at(k), b.edge_bc_tseries_name.at(k), tables, true),
              reference(b.edge_bc_flow_tseries.at(k), b.edge_bc_flow_tseries_name.at(k), tables,
                        true),
              reference(b.edge_bc_rating_curve.at(k), b.edge_bc_rating_curve_name.at(k), tables,
                        false)})
            if (i >= 0)
                used.push_back(i);
    std::sort(used.begin(), used.end());
    used.erase(std::unique(used.begin(), used.end()), used.end());
    // Preserve the existing wall/constant model bytes. Referenced tables bind
    // identity, type, records and relative-time anchoring, excluding cursors.
    if (!used.empty()) {
        std::string tag = "authored boundary signals v1";
        a(tag, used);
        for (int i : used) {
            auto t = tables->tables.at(i);
            a(t.id, t.type, t.x, t.y, t.n_relative, t.rel_anchor, t.num_cols, t.column_ids);
        }
    }
    return a.bytes();
}
/**
 * @brief Check copied water prescriptions against exact accepted model/receiver/time metadata.
 * @param[in] b Stable model/driver identity, active-GW/exterior masks and completed elapsed s.
 * @param[in] surface Stable surface source store, a distinct object from groundwater.
 * @param[in] groundwater Stable groundwater source store.
 * @param[in] forcing Stable rain/ET/paired-infiltration prescription store.
 * @param[in] boundary Optional stable current boundary, required exactly when b has boundary
 * slots.
 * @param[in] completed_boundary True checks completed boundary cumulative flux/receipts; false
 * defers only those boundary checks during in-progress stage accounting.
 * @return Empty for a valid privately copied/rebuilt candidate, otherwise first validation or
 * caught standard-exception diagnostic. Every input remains unchanged; no expiry/physics is
 * applied.
 * @note This internal same-platform profile excludes heat/species prescriptions and file-backed
 * boundary signals. Caller supplies synchronization and borrowed model/table lifetime.
 */
std::string RuntimeWaterCheckpoint::validate(const RuntimeWaterCheckpointBinding& b,
                                             const RuntimeSources& surface,
                                             const RuntimeSources& groundwater,
                                             const RuntimeForcings& forcing,
                                             const BoundaryData* boundary,
                                             bool completed_boundary) {
    try {
        require(&surface != &groundwater,
                "Runtime water checkpoint source domains must be distinct.");
        State s = privateState(b, surface, groundwater, forcing, boundary);
        validateState(s, b, true, completed_boundary);
        return {};
    } catch (const std::exception& e) {
        return e.what();
    }
}
/**
 * @brief Encode a fully validated completed copied-water prescription snapshot.
 * @param[in] b Exact immutable model/driver/receiver/time binding.
 * @param[in] surface Stable surface store, distinct from groundwater.
 * @param[in] groundwater Stable groundwater store, including qualified boundary records when
 * enabled.
 * @param[in] forcing Stable rain/ET/paired-infiltration store.
 * @param[out] bytes Swapped only after successful complete encoding and <=64 MiB size check;
 * unchanged on reported failure.
 * @param[in] boundary Live hydraulic boundary state, required exactly for nonempty boundary
 * slots.
 * @return Empty on capture, otherwise validation/archive/size or caught standard-exception
 * diagnostic.
 * @note Saves rows and receipts including disabled/expired history, not derived lookup maps.
 * Envelope binds version 1/2, native int/double widths/endian, identity, receiver mask, exact
 * completed time, optional authored boundary model and CRC-32. Inputs/physics remain unchanged;
 * external driver programs, heat/species and external signal files are outside this profile.
 */
std::string RuntimeWaterCheckpoint::capture(const RuntimeWaterCheckpointBinding& b,
                                            const RuntimeSources& surface,
                                            const RuntimeSources& groundwater,
                                            const RuntimeForcings& forcing, std::string& bytes,
                                            const BoundaryData* boundary) {
    try {
        require(&surface != &groundwater,
                "Runtime water checkpoint source domains must be distinct.");
        State s = privateState(b, surface, groundwater, forcing, boundary);
        validateState(s, b, true);
        const bool boundaries = !b.boundary_slots.empty();
        Archive payload;
        state(payload, s, boundaries);
        std::string magic = "OpenSWMM Runtime Water";
        uint32_t version = boundaries ? 2 : 1;
        uint32_t endian = 0x01020304, integer_size = sizeof(int), real_size = sizeof(double);
        auto identity = b.identity;
        auto cells = b.groundwater_cells;
        auto time = b.completed_time;
        auto data = payload.bytes();
        auto crc = checksum(data);
        Archive record;
        record(magic, version, endian, integer_size, real_size, identity, cells, time);
        if (boundaries) {
            auto slots = b.boundary_slots;
            auto model = boundaryModel(*b.authored_boundary, b.boundary_tables);
            record(slots, model);
        }
        record(data, crc);
        require(record.bytes().size() <= maxBytes, "Runtime water checkpoint exceeds 64 MiB.");
        auto prepared = record.bytes();
        bytes.swap(prepared);
        return {};
    } catch (const std::exception& e) {
        return e.what();
    }
}
/**
 * @brief Validate and atomically replace copied prescription stores against a matching prepared
 * model.
 * @param[in] b Immutable model/driver/receiver/time binding and optional authored
 * boundary/tables.
 * @param[in] bytes Owned <=64 MiB snapshot; exact profile/platform/model/masks/time/CRC and
 * framing must match. Malformed/empty input is rejected by archive decoding.
 * @param[out] surface Replaced only after all private decoding and validation succeeds; must be
 * a distinct source object from groundwater.
 * @param[out] groundwater Replaced on success with rebuilt source lookup/receipt state.
 * @param[out] forcing Replaced on success with reconstructed effective forcing lookups.
 * @param[out] boundary Mutable target required exactly when boundary slots exist; seeded
 * privately from authored model and replaced on success. Without that profile it must be null.
 * @return Empty after no-throw moves publish every target; otherwise diagnostic or caught
 * standard exception, with targets unchanged. No external program, physical water/solver state
 * or table cursor is restored. Disabled prescription histories/receipts are preserved.
 * @note Caller excludes all target/model mutation during validation/publication and keeps
 * immutable fallback model data separate from mutable runtime ownership. No thread
 * synchronization occurs.
 */
std::string RuntimeWaterCheckpoint::restore(const RuntimeWaterCheckpointBinding& b,
                                            std::string bytes, RuntimeSources& surface,
                                            RuntimeSources& groundwater, RuntimeForcings& forcing,
                                            BoundaryData* boundary) {
    try {
        require(&surface != &groundwater,
                "Runtime water checkpoint source domains must be distinct.");
        bindingOk(b);
        require(bytes.size() <= maxBytes, "Runtime water checkpoint exceeds 64 MiB.");
        Archive record(std::move(bytes));
        std::string magic, identity, data;
        uint32_t version = 0, endian = 0, integer_size = 0, real_size = 0, crc = 0;
        std::vector<bool> cells;
        double time = 0;
        record(magic, version, endian, integer_size, real_size, identity, cells, time);
        const bool boundaries = !b.boundary_slots.empty();
        require(boundaries ? boundary && b.authored_boundary : !boundary,
                "Boundary checkpoint profile or target differs.");
        if (boundaries) {
            std::vector<bool> slots;
            std::string model;
            record(slots, model);
            require(slots == b.boundary_slots &&
                        model == boundaryModel(*b.authored_boundary, b.boundary_tables),
                    "Boundary checkpoint edge or authored model differs.");
        }
        record(data, crc);
        record.finish();
        require(magic == "OpenSWMM Runtime Water" && version == (boundaries ? 2u : 1u) &&
                    endian == 0x01020304 && integer_size == sizeof(int) &&
                    real_size == sizeof(double),
                "Unsupported runtime water checkpoint version or platform.");
        require(
            identity == b.identity && cells == b.groundwater_cells && time == b.completed_time,
            "Runtime water checkpoint model, driver identity, receivers or completed time differs.");
        require(crc == checksum(data), "Runtime water checkpoint checksum differs.");
        State s;
        if (boundaries)
            s.boundary = *b.authored_boundary;
        Archive payload(std::move(data));
        state(payload, s, boundaries);
        payload.finish();
        validateState(s, b, false);
        surface = std::move(s.surface);
        groundwater = std::move(s.groundwater);
        forcing = std::move(s.forcing);
        if (boundaries)
            *boundary = std::move(s.boundary);
        return {};
    } catch (const std::exception& e) {
        return e.what();
    }
}
}
