// SPDX-License-Identifier: Apache-2.0

/**
 * @file      ExplicitInertialCheckpoint.cpp
 * @brief     Checkpoint persistence for explicit inertial surface routing.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include "ExplicitInertialSolver.hpp"
#include "InertialKernels.hpp"
#include "../subsurface/CommonSourceStage.hpp"
#include "../subsurface/SubsurfaceSolver.hpp"
#include "../heat/MeshCheckpointState.hpp"
#include "../heat/ThermalCheckpoint.hpp"
#include "../data/MeshData.hpp"
#include "../data/SurfaceStateData.hpp"
#include "../data/BoundaryData.hpp"
#include "../coupling/NodeCoupling.hpp"
#include "../coupling/Rk2RuntimeSources.hpp"
#include "../../data/NodeData.hpp"
#include "../../core/CompletedSourceArchive.hpp"
#include "../../core/SimulationContext.hpp"
#include "../../core/QualityCheckpoint.hpp"
#include "../../hydrology/surface/SnowCheckpoint.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <set>

namespace openswmm::twoD {
namespace {
using Archive = detail::CompletedSourceArchive;
constexpr uint64_t maxBytes = 64 * 1024 * 1024;
constexpr const char* magicName = "OpenSWMM CPU surface/source boundary";
/**
 * @brief Compute reflected IEEE CRC-32 over a CPU checkpoint payload.
 * @param[in] bytes Stable binary bytes, including embedded NULs.
 * @return CRC-32 with polynomial 0xedb88320, all-one initialization/final inversion; zero for
 * empty payload. No allocation or mutation; this detects corruption rather than authenticating
 * files.
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
 * @brief Read a complete bounded CPU checkpoint file into owned memory.
 * @param[in] path Stable existing binary file path, nonempty and no larger than 64 MiB.
 * @return Owned bytes, including embedded NULs.
 * @throws std::runtime_error On size/stat/open/short read or trailing file growth beyond
 * measured length. Allocation/filesystem errors propagate; no file or live solver state changes.
 */
std::string readFile(const std::string& path) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size == 0 || size > maxBytes)
        throw std::runtime_error("CPU checkpoint is missing or exceeds its size limit.");
    std::ifstream in(path, std::ios::binary);
    std::string bytes(size, '\0');
    in.read(bytes.data(), bytes.size());
    if (!in || in.peek() != std::char_traits<char>::eof())
        throw std::runtime_error("CPU checkpoint read failed.");
    return bytes;
}
/**
 * @brief Publish a CPU checkpoint through an exclusively created sibling and rename.
 * @param[in] path Destination with existing parent. An existing file must pass bounded read and
 * its leading archive magic must identify the CPU surface/source checkpoint format.
 * @param[in] bytes Complete encoded binary checkpoint supplied by the caller.
 * @throws std::runtime_error On unrelated existing target, exclusive-create/write/close/rename
 * failure; filesystem cleanup/allocation exceptions can propagate.
 * @note A creation collision is not deleted. Later failures attempt removal of this call's
 * sibling; existing target changes only at rename. No fsync, whole-engine transaction or
 * concurrent-writer coordination is supplied. This helper checks existing magic, not its entire
 * checkpoint validity.
 */
void publish(const std::string& path, const std::string& bytes) {
    // Refuse to replace an input, result or component-checkpoint file.
    if (std::filesystem::exists(path)) {
        Archive old(readFile(path));
        std::string magic;
        old(magic);
        if (magic != magicName)
            throw std::runtime_error("CPU checkpoint destination is an unrelated file.");
    }
    static std::atomic<uint64_t> serial{0};
    const auto part = path + ".cpu-source-part-" + std::to_string(++serial);
    FILE* file = std::fopen(part.c_str(), "wbx");
    if (!file)
        throw std::runtime_error("Cannot create CPU checkpoint publication file.");
    const bool written = std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
    const bool closed = std::fclose(file) == 0;
    if (!written || !closed) {
        std::filesystem::remove(part);
        throw std::runtime_error("CPU checkpoint write failed.");
    }
    std::error_code error;
    std::filesystem::rename(part, path, error);
    if (error) {
        std::filesystem::remove(part);
        throw std::runtime_error("Cannot publish CPU checkpoint: " + error.message());
    }
}
}

// Copied prescriptions belong to the numerical transaction. External
// provider programs and callback state are not represented.
/**
 * @brief Validate copied RK2 source prescriptions and water/heat/species receipt shapes.
 * @param[in] sources Stable source rows; no live lookup caches or physical state are changed.
 * @param[in] cells Prepared nonnegative cell count.
 * @param[in] species Prepared nonnegative species count; each row concentration/rate and receipt
 * mass vector must contain exactly this many entries.
 * @param[in] temperature Optional signed temperature row, -1 when absent; nonzero heat requires
 * one.
 * @param[in] age Optional age row, -1 when absent; age/temperature independent mass rates must
 * be zero.
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
std::string validateRk2Sources(const RuntimeSources& sources, int cells, int species,
                               int temperature, int age, const std::vector<uint8_t>* coverage) {
    std::set<std::pair<std::string, int>> keys;
    for (const auto& row : sources.rows) {
        const auto ns = std::size_t(species);
        if (row.id.empty() || row.cell < 0 || std::size_t(row.cell) >= std::size_t(cells) ||
            !keys.emplace(row.id, row.cell).second ||
            (coverage && (coverage->size() != std::size_t(cells) || !(*coverage)[row.cell])) ||
            row.boundary_edge < -1 ||
            (!coverage && (row.boundary_edge != -1 || !std::isnan(row.boundary_head))) ||
            (row.boundary_edge < 0 && !std::isnan(row.boundary_head)) ||
            (!std::isnan(row.boundary_head) && !std::isfinite(row.boundary_head)) ||
            !std::isfinite(row.flow) || !std::isfinite(row.heat) || std::isnan(row.until) ||
            row.until < 0 || row.concentrations.size() != ns || row.rates.size() != ns ||
            row.receipt.requested_mass.size() != ns || row.receipt.applied_mass.size() != ns ||
            (row.heat != 0 && temperature < 0))
            return "Invalid RK2 runtime prescription or species shape.";
        for (int r = 0; r < species; ++r)
            if (!std::isfinite(row.concentrations[r]) ||
                (r != temperature && row.concentrations[r] < 0) || !std::isfinite(row.rates[r]) ||
                ((r == temperature || r == age) && row.rates[r] != 0) ||
                !std::isfinite(row.receipt.requested_mass[r]) ||
                !std::isfinite(row.receipt.applied_mass[r]))
                return "Invalid RK2 runtime species/receipt.";
        const auto& b = row.receipt;
        for (double value : {b.requested, b.applied, b.water_in, b.water_out, b.heat_requested,
                             b.heat_applied, b.last_flow, b.last_dt})
            if (!std::isfinite(value))
                return "Nonfinite RK2 runtime receipt.";
        if (b.water_in < 0 || b.water_out < 0 || b.last_dt < 0)
            return "Negative RK2 runtime gross water or interval.";
        if (std::abs(b.applied - (b.water_in - b.water_out)) >
            1e-12 * std::max({1., b.water_in, b.water_out, std::abs(b.applied)}))
            return "RK2 runtime net and gross water receipts differ.";
    }
    return {};
}
/**
 * @brief Qualify prepared surface source/weather/snow state for the managed RK2 numerical
 * profile.
 * @param[in] surface Stable prepared cell/transport/weather/snow state with valid row
 * identities.
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
std::string rk2SurfaceRuntimeProfile(const SurfaceStateData& surface,
                                     const SubsurfaceSolver* ground) {
    const auto& tr = surface.transport;
    if (surface.snow.active()) {
        if (!surface.snow_context || surface.snow_context->options.heat_transport || tr.n_species ||
            surface.heat.enabled || !surface.runtime_forcings.rows.empty() ||
            !surface.rk2_rain_pending.empty() ||
            surface.snow.geometry().size() != surface.volume.size() ||
            surface.snow_catch.size() != surface.volume.size() ||
            (ground && (ground->transport().n_species || ground->heat().active())) ||
            std::any_of(surface.snow.groundContactMask().begin(),
                        surface.snow.groundContactMask().end(),
                        [](auto value) { return value != 0; }))
            return "RK2 snow requires an owned water-only source without coupled heat, transport or copied weather.";
        for (double value : surface.snow_catch)
            if (!std::isfinite(value) || value < 0)
                return "Invalid RK2 snow catch factor.";
        for (int cell = 0; cell < int(surface.volume.size()); ++cell)
            if (surface.snow.owns(cell) && surface.snow.exposedFractions()[cell] !=
                                               1 - surface.snow.outputs()[cell].cover_frac)
                return "RK2 snow exposure differs from accepted cover.";
        const auto& weather = surface.snow_context->climate_state;
        for (double value : {weather.temperature, weather.humidity, weather.wind_speed,
                             weather.gamma, weather.snow_divt})
            if (!std::isfinite(value))
                return "Nonfinite RK2 snow weather.";
        for (const auto& book : surface.snow.ledgers()) {
            for (double value : {book.initial, book.precipitation, book.raw_precipitation,
                                 book.liquid, book.stored})
                if (!std::isfinite(value) || value < 0)
                    return "Invalid RK2 snow water ledger.";
            for (double value :
                 {book.net_energy, book.geometry_energy, book.initial_energy, book.stored_energy,
                  book.prescribed_ground_energy, book.ground_contact_energy, book.sublimation})
                if (!std::isfinite(value))
                    return "Nonfinite RK2 snow energy ledger.";
        }
    }
    if (auto error = validateRk2Sources(surface.runtime_sources, int(surface.volume.size()),
                                        tr.n_species, tr.temp_row, tr.age_row);
        !error.empty())
        return error;
    std::set<std::pair<int, int>> forcing_keys;
    for (const auto& row : surface.runtime_forcings.rows) {
        const auto ns = std::size_t(tr.n_species);
        const auto& b = row.receipt;
        if (row.channel < 1 || row.channel > 4 || row.cell < -1 ||
            (row.cell >= 0 && std::size_t(row.cell) >= surface.volume.size()) || row.mode < 0 ||
            row.mode > 2 || (row.enabled && row.mode == 0) || (row.channel >= 3 && row.mode == 2) ||
            !std::isfinite(row.rate) || row.rate < 0 || std::isnan(row.until) || row.until <= 0 ||
            !forcing_keys.emplace(row.channel, row.cell).second || b.requested_mass.size() != ns ||
            b.applied_mass.size() != ns ||
            (!row.concentrations.empty() && row.concentrations.size() != ns) ||
            ((row.channel == 2 || row.channel == 3 || row.mode == 0) &&
             !row.concentrations.empty()) ||
            (row.channel == 4 && row.enabled && row.concentrations.size() != ns) ||
            (row.channel == 3 && (row.cell < 0 || (ground && !ground->covers(row.cell)))))
            return "Invalid RK2 runtime forcing prescription or paired groundwater owner.";
        for (int r = 0; r < tr.n_species; ++r) {
            if ((!row.concentrations.empty() &&
                 (!std::isfinite(row.concentrations[r]) ||
                  (r != tr.temp_row && row.concentrations[r] < 0))) ||
                !std::isfinite(b.requested_mass[r]) || !std::isfinite(b.applied_mass[r]))
                return "Invalid RK2 forcing quality/receipt.";
            if (row.channel != 3 && (b.requested_mass[r] != 0 || b.applied_mass[r] != 0))
                return "Unexpected receipt for a held RK2 weather prescription.";
        }
        for (double value : {b.requested, b.applied, b.water_in, b.water_out, b.heat_requested,
                             b.heat_applied, b.last_flow, b.last_dt}) {
            if (!std::isfinite(value))
                return "Nonfinite RK2 forcing receipt.";
            if (row.channel != 3 && value != 0)
                return "Unexpected receipt for a held RK2 weather prescription.";
        }
        if (row.channel == 3 &&
            (b.requested < 0 || b.applied < 0 || b.water_in != 0 || b.water_out < 0 ||
             b.last_dt < 0 || b.applied - b.requested > 1e-12 * std::max(1., b.requested) ||
             std::abs(b.applied - b.water_out) > 1e-12 * std::max({1., b.applied, b.water_out})))
            return "RK2 paired infiltration gross water receipts differ.";
    }
    if (!surface.rk2_rain_pending.empty()) {
        if (surface.runtime_forcings.rows.empty() ||
            surface.rk2_rain_pending.size() != surface.volume.size())
            return "RK2 accepted rain receipt shape differs.";
        for (double water : surface.rk2_rain_pending)
            if (!std::isfinite(water) || water < 0)
                return "Invalid RK2 accepted rainfall receipt.";
    }
    return {};
}
namespace {
/**
 * @brief Archive held RK2 surface/groundwater source identity or mutable accepted receipts.
 * @param[in,out] a Sequential archive; reading replaces selected fields and derived lookup
 * caches.
 * @param[in,out] sources Prepared row store, with fixed layout on mutable-state decode.
 * @param[in] cells Nonnegative prepared cell count for rebuilding row-to-cell caches.
 * @param[in] model True for immutable ID/owner/cell/flow/heat/expiry/composition/rate
 * prescription; false for enabled flags and cumulative/last-step accepted receipts.
 * @param[in] groundwater True to distinguish ground profiles and include boundary-edge/head
 * identity when any row is attached to a boundary. Finite-head evaluated Darcy flow is mutable
 * state.
 * @note Empty stores emit/read nothing. Reads verify profile, rebuild ID/cell index and active
 * cache. No complete row admissibility/duplicate validation or clock advancement occurs here.
 * Archive, allocation or rebuild failures can leave a partially decoded store; use a private
 * candidate. SI water m3/m3/s, heat J, and native species inventories retain their prepared
 * conventions.
 */
void rk2SurfaceRuntimeArchive(Archive& a, RuntimeSources& sources, int cells, bool model,
                              bool groundwater = false) {
    if (sources.rows.empty())
        return;
    const bool boundary =
        groundwater && std::any_of(sources.rows.begin(), sources.rows.end(),
                                   [](const auto& row) { return row.boundary_edge >= 0; });
    const std::string expected = boundary      ? "RK2 copied groundwater boundaries v1"
                                 : groundwater ? "RK2 copied groundwater runtime sources v1"
                                               : "RK2 copied surface runtime sources v1";
    std::string profile = expected;
    a(profile);
    if (profile != expected)
        throw std::runtime_error("Unsupported RK2 runtime source record.");
    a.sequence(
        sources.rows,
        [&](auto& row) {
            if (model) {
                a(row.id, row.owner, row.cell);
                if (boundary)
                    a(row.boundary_edge, row.boundary_head);
                // A HEAD row's evaluated Darcy flow is mutable diagnostic state.
                if (!boundary || !std::isfinite(row.boundary_head))
                    a(row.flow);
                a(row.heat, row.until, row.concentrations, row.rates);
            } else {
                if (boundary && std::isfinite(row.boundary_head))
                    a(row.flow);
                a(row.enabled);
                auto& b = row.receipt;
                a(b.requested, b.applied, b.water_in, b.water_out, b.heat_requested, b.heat_applied,
                  b.last_flow, b.last_dt, b.requested_mass, b.applied_mass);
            }
        },
        true);
    if (a.reading()) {
        sources.index.clear();
        for (std::size_t k = 0; k < sources.rows.size(); ++k) {
            const auto& row = sources.rows[k];
            sources.index[row.id][row.cell] = k;
        }
        sources.rebuild(cells);
    }
}
/**
 * @brief Archive held forcing prescriptions or enabled flags, accepted receipts and pending
 * rain.
 * @param[in,out] a Sequential archive; read failures may leave partial candidate changes.
 * @param[in,out] surface Prepared surface owner including forcing store and pending rain m3
 * vector.
 * @param[in] model True for channel/cell/mode/rate/expiry/owner/composition identity; false for
 * mutable enabled flags, cumulative/last-step receipts and accepted per-cell pending rainfall.
 * @note Empty forcing emits/reads nothing. Validates record profile, then rebuilds channel/cell
 * index and active cache on reads. Physical units remain those of each forcing channel and
 * native row. Does not independently validate full forcing/layout/receipt admissibility or
 * advance time; caller must validate staged candidates before adoption.
 * Archive/allocation/rebuild can throw.
 */
void rk2ForcingArchive(Archive& a, SurfaceStateData& surface, bool model) {
    auto& forcing = surface.runtime_forcings;
    if (forcing.rows.empty())
        return;
    std::string profile = "RK2 held forcing and accepted rain v2";
    a(profile);
    if (profile != "RK2 held forcing and accepted rain v2")
        throw std::runtime_error("Unsupported RK2 forcing record.");
    a.sequence(
        forcing.rows,
        [&](auto& row) {
            if (model)
                a(row.channel, row.cell, row.mode, row.rate, row.until, row.owner,
                  row.concentrations);
            else {
                a(row.enabled);
                auto& b = row.receipt;
                a(b.requested, b.applied, b.water_in, b.water_out, b.heat_requested, b.heat_applied,
                  b.last_flow, b.last_dt, b.requested_mass, b.applied_mass);
            }
        },
        true);
    if (!model)
        a(surface.rk2_rain_pending);
    if (a.reading()) {
        forcing.index.clear();
        for (std::size_t k = 0; k < forcing.rows.size(); ++k) {
            const auto& row = forcing.rows[k];
            forcing.index[row.channel][row.cell] = k;
        }
        forcing.rebuild(int(surface.volume.size()));
    }
}

}
/**
 * @brief Validate an accepted attached first-order CPU/source checkpoint boundary.
 * @return Empty on qualification, otherwise configuration, clock, live-provider, coupling,
 * boundary layout/receipt or source-stage refusal text.
 * @note Requires initialized owners. Checks common-stage identity/completed clocks and allowed
 * joint native quality/thermal profiles, non-outfall 1D exchanges, source pins and reconstructed
 * exterior boundary work lists. Does not serialize, mutate, settle packets or validate arbitrary
 * whole-engine hotstart state. Allocations/checked vector accesses may throw. Exclusive stable
 * router/source/model views must be retained during subsequent serialization.
 */
std::string ExplicitInertialSolver::checkpointBoundary() const {
    if (!initialized_ || !common_stage_ || (!gw_ && !common_stage_->standaloneLocalBed()) ||
        second_order_ || !checkpoint_ready_ || !common_stage_->matches(mesh_, state_, gw_))
        return "CPU checkpoint requires an accepted attached first-order source marcher.";
    if (t_last_sync_ != common_stage_->completedEnd() || source_phase_end_ != t_last_sync_)
        return "CPU/source completed clocks differ.";
    const auto& transport = state_->transport;
    const bool temperature_only =
        joint_checkpoint_ && transport.active() && transport.n_species == 1 &&
        transport.n_pollut == 0 && transport.n_msx == 0 && transport.age_row < 0 &&
        transport.temp_row == 0 && transport.rowIndex("__TEMPERATURE__") == 0;
    const bool standalone_local_bed_quality = common_stage_->standaloneLocalBed() &&
                                              state_->heat.enabled && state_->heat.bed &&
                                              !state_->heat.ground_column;
    const bool mesh_only_ground_quality =
        joint_checkpoint_ && common_stage_->meshOnlyGroundQuality();
    const bool network_quality =
        joint_checkpoint_ && common_stage_->meshOnly() &&
        detail::checkpointNetworkPollutants(common_stage_->sources().context()) &&
        transport.n_pollut == int(common_stage_->sources().pollutantNames().size()) &&
        transport.n_species == transport.n_pollut && transport.n_msx == 0 &&
        transport.age_row < 0 && transport.temp_row < 0;
    const bool supported_transport = temperature_only || standalone_local_bed_quality ||
                                     mesh_only_ground_quality || network_quality;
    if (network_quality && state_->node_row_conc) {
        if (!state_->nodes_1d || state_->node_row_conc->size() !=
                                     std::size_t(state_->nodes_1d->count()) * transport.n_species)
            return "Joint native pollutant row cache shape differs.";
        for (double value : *state_->node_row_conc)
            if (!std::isfinite(value) || value < 0)
                return "Invalid joint native pollutant row cache.";
        // publishNodeRows rebuilds this view from the actual native owner
        // before every hydraulic/source interval; it carries no private state.
    }
    if (!joint_checkpoint_ && (state_->nodes_1d || state_->node_coupling))
        return "CPU checkpoint requires a joint hydraulic owner for live 1D coupling.";
    if (state_->node_row_conc && !supported_transport)
        return "CPU checkpoint live node-row providers are not qualified.";
    if (!exch_head_slope_.empty())
        return "CPU checkpoint live head-slope providers are not qualified.";
    if (!common_stage_->runtimeBoundariesEnabled() && !bc_cell_.empty())
        return "CPU checkpoint live external boundary providers are not qualified.";
    if (joint_checkpoint_) {
        if (bool(state_->nodes_1d) != bool(state_->node_coupling) ||
            (transport.active() && !supported_transport) ||
            node_drawn_.size() != (state_->nodes_1d ? std::size_t(state_->nodes_1d->count()) : 0) ||
            exch_.size() != (state_->node_coupling ? state_->node_coupling->size() : 0))
            return "Joint CPU checkpoint coupling layout differs.";
        if (state_->node_coupling)
            for (const auto& cp : *state_->node_coupling)
                if (cp.is_outfall || cp.cell_idx < 0 || cp.cell_idx >= mesh_->n_cells() ||
                    cp.node_idx < 0 || cp.node_idx >= state_->nodes_1d->count() ||
                    !std::isfinite(cp.cd) || cp.cd < 0 || !std::isfinite(cp.area) || cp.area <= 0)
                    return "Joint CPU checkpoint requires valid non-outfall exchange points.";
    }
    if (state_->boundary && !common_stage_->runtimeBoundariesEnabled()) {
        if (!state_->boundary->runtime_original.empty())
            return "CPU checkpoint boundary runtime providers are not qualified.";
        for (int c = 0; c < mesh_->n_cells(); ++c)
            for (int k = 0; k < mesh_->cell_vertex_count(c); ++k)
                if (mesh_->cell_neighbour(c, k) < 0 &&
                    state_->boundary->edge_bc_type.at(MeshData::slot(c, k)) !=
                        int8_t(BoundaryType::WALL))
                    return "CPU checkpoint requires wall boundaries.";
    }
    if (common_stage_->runtimeBoundariesEnabled()) {
        if (!state_->boundary)
            return "CPU boundary checkpoint has no boundary model.";
        std::vector<int> cells, slots, walls, ptr{0};
        for (int c = 0; c < mesh_->n_cells(); ++c) {
            for (int e = 0; e < mesh_->cell_vertex_count(c); ++e)
                if (mesh_->cell_neighbour(c, e) < 0) {
                    const int k = MeshData::slot(c, e);
                    if (state_->boundary->edge_bc_type.at(k) == int8_t(BoundaryType::WALL))
                        walls.push_back(k);
                    else {
                        cells.push_back(c);
                        slots.push_back(k);
                    }
                }
            ptr.push_back(int(walls.size()));
        }
        const bool unused_walls =
            wall_slot_.empty() && wall_ptr_.empty() && state_->boundary->runtime_original.empty() &&
            std::all_of(state_->boundary->receipts.begin(), state_->boundary->receipts.end(),
                        [&](const auto& row) {
                            return common_stage_->authored_boundary_.edge_bc_type.at(row.first) !=
                                   int8_t(BoundaryType::WALL);
                        });
        if (cells != bc_cell_ || slots != bc_slot_ ||
            (!unused_walls && (walls != wall_slot_ || ptr != wall_ptr_)) ||
            bc_q_.size() != slots.size() || bc_accum_.size() != slots.size())
            return "CPU checkpoint boundary layout differs from its exterior-edge model.";
        for (const auto* row : {&bc_q_, &bc_accum_})
            for (double v : *row)
                if (!std::isfinite(v))
                    return "CPU checkpoint boundary momentum or accumulator is nonfinite.";
    }
    return common_stage_->checkpointBoundary();
}

/**
 * @brief Archive and validate fixed CPU momentum, frozen schedule, settled packets and
 * statistics.
 * - Argument a (in,out): Sequential archive; read mode mutates this candidate progressively.
 * @note Preserves configured pin and telemetry identity; serializes edge momentum,
 * transfer/native packets, cell/face masks/tiers/work lists, source clocks, optional
 * reaction/boundary/joint receipts and march telemetry. Reconstructible scratch is omitted.
 * Rejects nonfinite values, unsettled packets, inconsistent pin/tier/work lists, invalid
 * clocks/counters and telemetry extents/ranges. Prepared mesh/options/owners and matching vector
 * extents must be live. Does not advance physics, rebuild momentum or install another owner.
 * Throws on archive/shape/state/allocation failure; caller must decode into a private copy
 * because failure can leave this candidate partly modified.
 */
void ExplicitInertialSolver::checkpointState(Archive& a) {
    // Scratch reconstructed before use is excluded; frozen scheduling and
    // prognostic momentum must survive without a rebuild or momentum reset.
    const auto pins = pin_t0_;
    const bool telemetry_enabled = !telemetry_path_.empty();
    bool saved_telemetry = telemetry_enabled;
    a.fixed(q_, qcx_, qcy_, facc_L_, facc_R_, macc_x_L_, macc_x_R_, macc_y_L_, macc_y_R_, sacc_L_,
            sacc_R_, dw_slope_, cell_active_, tier_, face_tier_, pin_t0_, frontier_,
            source_ledgers_, cells_by_tier_, edges_by_tier_);
    a(active_cells_, active_faces_, dt0_, t_last_sync_, source_phase_end_, cycles_since_rebuild_,
      accumulators_pending_, front_breach_, h_on_front_, exch_tau_, spill_deficit_, substeps_run_,
      face_passes_, last_steps_, last_dt_, active_frac_min_, active_frac_max_, active_frac_sum_,
      active_samples_, tier_occupancy_, saved_telemetry, telemetry_);
    if (!rk_reacted_.empty())
        a.fixed(rk_reacted_, rk_reacted_reported_);
    if ((common_stage_ && common_stage_->runtimeBoundariesEnabled()) ||
        (second_order_ && gw_ && rk2HasSurfaceBoundary(state_->boundary)))
        a(bc_cell_, bc_slot_, bc_q_, bc_accum_, wall_ptr_, wall_slot_);
    if (joint_checkpoint_) {
        a.fixed(exch_, node_drawn_);
        for (double v : exch_)
            if (!std::isfinite(v))
                throw std::runtime_error("Nonfinite joint CPU exchange.");
        for (double v : node_drawn_)
            if (!std::isfinite(v) || v < 0)
                throw std::runtime_error("Invalid joint CPU donor withdrawal.");
    }
    if (pin_t0_ != pins || saved_telemetry != telemetry_enabled)
        throw std::runtime_error("CPU checkpoint pin/telemetry configuration differs.");
    for (const auto* row :
         {&q_, &qcx_, &qcy_, &facc_L_, &facc_R_, &macc_x_L_, &macc_x_R_, &macc_y_L_, &macc_y_R_,
          &sacc_L_, &sacc_R_, &dw_slope_, &source_ledgers_})
        for (double v : *row)
            if (!std::isfinite(v))
                throw std::runtime_error("Nonfinite CPU checkpoint state.");
    for (const auto* row : {&facc_L_, &facc_R_, &macc_x_L_, &macc_x_R_, &macc_y_L_, &macc_y_R_,
                            &sacc_L_, &sacc_R_, &source_ledgers_})
        for (double v : *row)
            if (v != 0)
                throw std::runtime_error("CPU checkpoint has unsettled transfers.");
    for (double v : {dt0_, t_last_sync_, source_phase_end_, h_on_front_, exch_tau_, spill_deficit_,
                     last_dt_, active_frac_min_, active_frac_max_, active_frac_sum_})
        if (!std::isfinite(v))
            throw std::runtime_error("Nonfinite CPU checkpoint clock/statistic.");
    if (t_last_sync_ < 0 || source_phase_end_ != t_last_sync_ || dt0_ < 0 ||
        (t_last_sync_ > 0 && dt0_ <= 0) || h_on_front_ < 0 || exch_tau_ < 0 || spill_deficit_ < 0 ||
        (!joint_checkpoint_ && spill_deficit_ != 0) || last_dt_ < 0 || accumulators_pending_ ||
        cycles_since_rebuild_ < 0 || cycles_since_rebuild_ > (t_last_sync_ == 0 ? 1000 : 4) ||
        substeps_run_ < 0 || face_passes_ < 0 || last_steps_ < 0 || last_steps_ > substeps_run_ ||
        active_samples_ < 0)
        throw std::runtime_error("Invalid CPU checkpoint boundary/schedule.");
    const int n = mesh_->n_cells(), K = int(cells_by_tier_.size());
    std::vector<int> cells, faces;
    std::vector<std::vector<int>> cell_tiers(K), face_tiers(K);
    for (int i = 0; i < n; ++i) {
        if (cell_active_[i] > 1 || tier_[i] >= K || pin_t0_[i] > 1 ||
            (!frontier_.empty() && frontier_[i] > 1))
            throw std::runtime_error("Invalid CPU checkpoint cell mask/tier.");
        if (cell_active_[i]) {
            cells.push_back(i);
            cell_tiers[tier_[i]].push_back(i);
        }
        if (active_samples_ > 0 && pin_t0_[i] && (!cell_active_[i] || tier_[i] != 0))
            throw std::runtime_error("CPU checkpoint lost a pinned source cell.");
    }
    for (int e = 0; e < edges_.ne; ++e) {
        if (face_tier_[e] >= K)
            throw std::runtime_error("Invalid CPU checkpoint face tier.");
        if (cell_active_[edges_.cL[e]] && cell_active_[edges_.cR[e]]) {
            const auto tier = std::min(tier_[edges_.cL[e]], tier_[edges_.cR[e]]);
            if (face_tier_[e] != tier)
                throw std::runtime_error("CPU checkpoint face/cell tiers differ.");
            faces.push_back(e);
            face_tiers[tier].push_back(e);
        } else if (active_samples_ > 0 && q_[e] != 0)
            throw std::runtime_error("Inactive CPU checkpoint face carries momentum.");
    }
    if (cells != active_cells_ || faces != active_faces_ || cell_tiers != cells_by_tier_ ||
        face_tiers != edges_by_tier_)
        throw std::runtime_error("CPU checkpoint work lists differ from their masks/tiers.");
    if (active_samples_ == 0 && (active_frac_min_ != 1e30 || active_frac_max_ != -1e30 ||
                                 active_frac_sum_ != 0 || t_last_sync_ != 0))
        throw std::runtime_error("Invalid initial CPU checkpoint statistics.");
    if (active_samples_ > 0 &&
        (active_frac_min_ < 0 || active_frac_max_ > 1 || active_frac_min_ > active_frac_max_ ||
         active_frac_sum_ < 0 || active_frac_sum_ > double(active_samples_)))
        throw std::runtime_error("Invalid CPU checkpoint active statistics.");
    for (long v : tier_occupancy_)
        if (v < 0)
            throw std::runtime_error("Invalid CPU checkpoint tier statistics.");
    if ((saved_telemetry && telemetry_.size() != static_cast<std::size_t>(active_samples_)) ||
        (!saved_telemetry && !telemetry_.empty()))
        throw std::runtime_error("CPU checkpoint telemetry sample count differs.");
    double last = -1;
    for (const auto& [t, count] : telemetry_) {
        if (!std::isfinite(t) || t < last || t < 0 || t > t_last_sync_ || count < 0 || count > n)
            throw std::runtime_error("Invalid CPU checkpoint telemetry.");
        last = t;
    }
}

/**
 * @brief Publish a qualified component checkpoint through the temporary-file publisher.
 * @param[in] path Destination filesystem path, with a writable existing parent and exclusive
 * writer.
 * @return Empty on success; joint-router ownership refusal, serialization or caught standard
 * file exception text otherwise. A failed publication may leave a temporary file; see publish().
 * @note Does not settle or advance the marcher. Captures the selected qualified first-order or
 * RK2 component boundary, not a complete engine/output hotstart. No source/model ownership
 * transfers.
 */
std::string ExplicitInertialSolver::saveCheckpoint(const std::string& path) const {
    try {
        if (joint_checkpoint_)
            return "Live coupling requires the joint router checkpoint.";
        std::string bytes;
        const auto error = checkpointBytes(bytes);
        if (!error.empty())
            return error;
        publish(path, bytes);
        return {};
    } catch (const std::exception& e) {
        return e.what();
    }
}

/**
 * @brief Encode an accepted CPU/source or standalone coupled RK2 component checkpoint.
 * @param[out] bytes Replaced only after successful complete envelope encoding and size
 * validation; retained on qualification/standard-exception failure.
 * @return Empty on success; profile, component, size or caught standard exception diagnostic.
 * @note Uses private marcher copies and includes source component, settled CPU schedule,
 * version, endian/long-width markers and payload checksum. RK2 without common stage delegates
 * its specialized version-4 format. No physics, clock, recorder or borrowed-owner mutation;
 * caller serializes access.
 */
std::string ExplicitInertialSolver::checkpointBytes(std::string& bytes) const {
    if (second_order_ && !common_stage_)
        return rk2CheckpointBytes(bytes);
    try {
        const auto error = checkpointBoundary();
        if (!error.empty())
            return error;
        std::string component;
        const auto state_error = common_stage_->checkpointBytes(component);
        if (!state_error.empty())
            return state_error;
        auto trial = *this;
        Archive state;
        state(component);
        trial.checkpointState(state);
        std::string magic = magicName, payload = state.bytes();
        uint32_t version = common_stage_->runtimeBoundariesEnabled() ? 2 : 1, endian = 0x01020304,
                 longBytes = sizeof(long), crc = checksum(payload);
        Archive file;
        file(magic, version, endian, longBytes, payload, crc);
        if (file.bytes().size() > maxBytes)
            return "CPU checkpoint exceeds internal qualification size limit.";
        bytes = file.bytes();
        return {};
    } catch (const std::exception& e) {
        return e.what();
    }
}

/**
 * @brief Read and restore a qualified component checkpoint into fresh prepared matching owners.
 * @param[in] path Readable checkpoint file path within the internal size bound.
 * @return Empty on success; joint-router ownership refusal, read/decode/qualification or caught
 * standard exception diagnostic otherwise.
 * @note Delegates to loadCheckpointBytes; no live history recorder, evolved numerical owner or
 * mismatched model is admitted. Component restore preserves borrowed owner identities and does
 * not restore arbitrary whole-engine/output state. Requires exclusive matching owners.
 */
std::string ExplicitInertialSolver::loadCheckpoint(const std::string& path) {
    try {
        if (joint_checkpoint_)
            return "Live coupling requires the joint router checkpoint.";
        return loadCheckpointBytes(readFile(path));
    } catch (const std::exception& e) {
        return e.what();
    }
}

/**
 * @brief Stage, inspect or install a first-order CPU/source checkpoint with optional router
 * guards.
 * @param[in] bytes Owned encoded envelope; consumed by local decoding, must be nonempty and
 * bounded.
 * @param[in] router_boundary Optional ten-value router guard: completed time s; rain,
 * evaporation, return and current surface water m3; runtime source inflow/outflow m3;
 * provider-use flag; boundary inflow/outflow m3. Values must match staged receipts within
 * documented numerical comparisons.
 * @param[in] pending_outfalls Optional per-cell undrained outfall m3 used with router guard;
 * null requires staged coupling receipts zero. Staged infiltration must already be drained.
 * @param[in] validate_source Optional pointer to a nonempty callback that checks the staged
 * source owner by const reference before install/inspection; borrowed views are valid only
 * during the call.
 * @param[in] before_install Optional pointer to a nonempty callback invoked after all
 * validation, before nonthrowing moves; external callback effects are not rolled back.
 * @param[in] boundary_slots Optional exact ordered exterior boundary slot guard.
 * @param[out] inspect_source_end Optional completed source time s output; successful validation
 * writes it and returns without installing any owners or invoking before_install.
 * @return Empty on success/inspection; unsupported profile/platform, checksum/size/model/layout,
 * clock/receipt or caught standard exception diagnostic otherwise.
 * @note Requires initialized fresh matching first-order owners without history and exclusive
 * access. Decodes source, groundwater, surface, boundary and marcher privately,
 * validates/rebuilds derived contact ledgers, then rebinds original borrowed identities and
 * installs by nonthrowing moves. Failure before installation preserves canonical owners, apart
 * from caller callback side effects. Standalone RK2 dispatch permits none of these optional
 * router/source guards and delegates its specialized staged restore. Does not restore unrelated
 * engine/calendar/output state.
 */
std::string ExplicitInertialSolver::loadCheckpointBytes(
    std::string bytes, const std::array<double, 10>* router_boundary,
    const std::vector<double>* pending_outfalls,
    const std::function<void(const runoff::SourceWaterDriver&)>* validate_source,
    const std::function<void()>* before_install, const std::vector<int>* boundary_slots,
    double* inspect_source_end) {
    if (second_order_ && !common_stage_) {
        if (router_boundary || pending_outfalls || validate_source || before_install ||
            boundary_slots || inspect_source_end)
            return "Standalone RK2 checkpoint cannot restore borrowed router/source owners.";
        return loadRk2CheckpointBytes(std::move(bytes));
    }
    try {
        const auto error = checkpointBoundary();
        if (!error.empty())
            return error;
        if (t_last_sync_ != 0 || common_stage_->intervals() != 0 || source_history_ ||
            engine_source_history_)
            return "CPU checkpoint restore requires a fresh marcher without a history recorder.";
        if (bytes.empty() || bytes.size() > maxBytes)
            return "CPU checkpoint exceeds its size limit.";
        Archive file(std::move(bytes));
        std::string magic, payload;
        uint32_t version = 0, endian = 0, longBytes = 0, crc = 0;
        file(magic, version, endian, longBytes, payload, crc);
        file.finish();
        if (magic != magicName ||
            version != (common_stage_->runtimeBoundariesEnabled() ? 2u : 1u) ||
            endian != 0x01020304 || longBytes != sizeof(long))
            return "Unsupported CPU checkpoint version/platform.";
        if (crc != checksum(payload))
            return "CPU checkpoint checksum differs.";
        auto* live_gw = gw_ ? gw_ : common_stage_->gw_;
        if (!live_gw)
            return "CPU checkpoint has no matching groundwater state owner.";
        auto source = common_stage_->sources_->waterTrial();
        auto gw = *live_gw;
        auto surface = *state_;
        auto stage = *common_stage_;
        auto trial = *this;
        BoundaryData boundary;
        if (common_stage_->runtimeBoundariesEnabled()) {
            boundary = *state_->boundary;
            surface.boundary = &boundary;
        }
        stage.sources_ = &source;
        stage.gw_ = &gw;
        stage.surface_ = &surface;
        trial.gw_ = gw_ ? &gw : nullptr;
        trial.state_ = &surface;
        trial.common_stage_ = &stage;
        Archive state(std::move(payload));
        std::string component;
        state(component);
        trial.checkpointState(state);
        state.finish();
        const auto component_error = stage.loadCheckpointBytes(std::move(component));
        if (!component_error.empty())
            return component_error;
        // `gained_contact` is a derived aggregate of the persisted per-layer
        // contact ledger. Rebuild it on the staged aquifer before installation
        // so restored continuity diagnostics match the saved porewater state.
        if (!gw.transport().gained_contact.empty())
            if (const auto contact_error = gw.refreshPorewaterContactLedger();
                !contact_error.empty())
                return contact_error;
        const auto invalid = trial.checkpointBoundary();
        if (!invalid.empty())
            return invalid;
        if (boundary_slots && trial.bc_slot_ != *boundary_slots)
            return "Router/CPU checkpoint boundary indices differ.";
        if (router_boundary) {
            if (trial.t_last_sync_ != (*router_boundary)[0])
                return "Router/CPU checkpoint clocks differ.";
            double volume = 0;
            for (double v : surface.volume)
                volume += v;
            const std::array<double, 4> ledger{stage.meshRain(), stage.meshEvaporation(),
                                               stage.meshReturn(), volume};
            for (int k = 0; k < 4; ++k)
                if (std::abs(ledger[k] - (*router_boundary)[k + 1]) >
                    1e-12 * std::max(1.0, std::abs(ledger[k])))
                    return "Router/source checkpoint ledgers differ.";
            double incoming = 0, outgoing = 0;
            for (const auto& row : surface.runtime_sources.rows) {
                incoming += row.receipt.water_in;
                outgoing += row.receipt.water_out;
            }
            if (!std::isfinite(incoming) || !std::isfinite(outgoing) ||
                std::abs(incoming - (*router_boundary)[5]) > 1e-12 * std::max(1., incoming) ||
                std::abs(outgoing - (*router_boundary)[6]) > 1e-12 * std::max(1., outgoing))
                return "Router/provider checkpoint water receipts differ.";
            double boundary_in = 0, boundary_out = 0;
            if (surface.boundary)
                for (const auto& [slot, r] : surface.boundary->receipts) {
                    boundary_in += r.water_in;
                    boundary_out += r.water_out;
                }
            if (!std::isfinite(boundary_in) || !std::isfinite(boundary_out) ||
                std::abs(boundary_in - (*router_boundary)[8]) > 1e-12 * std::max(1., boundary_in) ||
                std::abs(boundary_out - (*router_boundary)[9]) > 1e-12 * std::max(1., boundary_out))
                return "Router/boundary checkpoint water receipts differ.";
            if ((*router_boundary)[7] == 0 &&
                (!surface.runtime_sources.rows.empty() || !gw.runtimeSources().rows.empty() ||
                 !surface.runtime_forcings.rows.empty() ||
                 (surface.boundary &&
                  (!surface.boundary->runtime_original.empty() ||
                   std::any_of(surface.boundary->receipts.begin(), surface.boundary->receipts.end(),
                               [&](const auto& row) {
                                   return stage.authored_boundary_.edge_bc_type.at(row.first) ==
                                          int8_t(BoundaryType::WALL);
                               })))))
                return "Router checkpoint lost its provider-use history.";
            for (double v : surface.infil_applied)
                if (v != 0)
                    return "Router checkpoint has undrained infiltration.";
            if (pending_outfalls && pending_outfalls->size() != surface.coupling_applied.size())
                return "Router outfall receipt shape differs.";
            for (std::size_t c = 0; c < surface.coupling_applied.size(); ++c)
                if (surface.coupling_applied[c] != (pending_outfalls ? (*pending_outfalls)[c] : 0))
                    return "Router checkpoint undrained coupling differs from its outfall receipts.";
        }
        if (validate_source)
            (*validate_source)(source);
        if (inspect_source_end) {
            *inspect_source_end = stage.completedEnd();
            return {};
        }
        if (before_install)
            (*before_install)();
        // All parsing, model, shape, schedule and component checks preceded
        // installation. Moves cannot throw; restored pointers retain ownership.
        static_assert(std::is_nothrow_move_assignable_v<ExplicitInertialSolver>);
        static_assert(std::is_nothrow_move_assignable_v<CommonSourceStage>);
        static_assert(std::is_nothrow_move_assignable_v<runoff::SourceWaterDriver>);
        static_assert(std::is_nothrow_move_assignable_v<SubsurfaceSolver>);
        static_assert(std::is_nothrow_move_assignable_v<SurfaceStateData>);
        if (common_stage_->runtimeBoundariesEnabled()) {
            *const_cast<BoundaryData*>(state_->boundary) = std::move(boundary);
            surface.boundary = state_->boundary;
        }
        stage.sources_ = common_stage_->sources_;
        stage.gw_ = live_gw;
        stage.surface_ = state_;
        trial.gw_ = gw_;
        trial.state_ = state_;
        trial.common_stage_ = common_stage_;
        *stage.sources_ = std::move(source);
        *live_gw = std::move(gw);
        *state_ = std::move(surface);
        if (state_->heat.enabled)
            state_->heat.aquifer_ground = state_->heat.ground_column ? &live_gw->heat() : nullptr;
        *common_stage_ = std::move(stage);
        *this = std::move(trial);
        return {};
    } catch (const std::exception& e) {
        return e.what();
    }
}
namespace {
/**
 * @brief Test authored fallback boundary slots for a resolved stage or flow series.
 * @param[in] boundary Stable consistently sized descriptor arrays and optional runtime_original
 * map.
 * @return True when any stored fallback (or current descriptor without an override) has a
 * nonnegative matching table index. No table lookup, value validation or state mutation occurs.
 * Checked vector access can throw on inconsistent arrays; names alone do not establish a signal.
 */
bool rk2HasSurfaceSeries(const BoundaryData& boundary) {
    for (std::size_t slot = 0; slot < boundary.edge_bc_type.size(); ++slot) {
        const auto original = boundary.runtime_original.find(int(slot));
        const int stage = original == boundary.runtime_original.end()
                              ? boundary.edge_bc_tseries.at(slot)
                              : original->second.stage_ts;
        const int flow = original == boundary.runtime_original.end()
                             ? boundary.edge_bc_flow_tseries.at(slot)
                             : original->second.flow_ts;
        if (stage >= 0 || flow >= 0)
            return true;
    }
    return false;
}
/**
 * @brief Test authored fallback boundary slots for a resolved rating curve.
 * @param[in] boundary Stable consistently sized descriptor arrays and optional runtime_original
 * map.
 * @return True when any stored fallback (or current descriptor without an override) has a
 * nonnegative matching table index. No table lookup, value validation or state mutation occurs.
 * Checked vector access can throw on inconsistent arrays; names alone do not establish a signal.
 */
bool rk2HasSurfaceCurve(const BoundaryData& boundary) {
    for (std::size_t slot = 0; slot < boundary.edge_bc_type.size(); ++slot) {
        const auto original = boundary.runtime_original.find(int(slot));
        const int curve = original == boundary.runtime_original.end()
                              ? boundary.edge_bc_rating_curve.at(slot)
                              : original->second.rating;
        if (curve >= 0)
            return true;
    }
    return false;
}
/**
 * @brief Validate the supported type-specific boundary signal-reference arrangement.
 * @param[in] type Boundary kind byte.
 * @param[in] stage Resolved stage-series index or -1.
 * @param[in] flow Resolved flow-series index or -1.
 * @param[in] rating Resolved rating-curve index or -1.
 * @param[in] stage_name Optional stage name; must be empty when its index is absent.
 * @param[in] flow_name Optional flow name; must be empty when its index is absent.
 * @param[in] rating_name Optional curve name, empty outside rating boundaries.
 * @return True for wall/normal without signals, stage with no flow signal, flow with no stage
 * signal, or rating with a nonnegative curve index and no series. Deferred -2 references are
 * refused. Does not validate table existence/records, hydraulic values, edge topology or input
 * mutation.
 */
bool rk2SurfaceDescriptor(int8_t type, int stage, int flow, int rating,
                          const std::string& stage_name, const std::string& flow_name,
                          const std::string& rating_name) {
    if (type == int8_t(BoundaryType::RATING_CURVE))
        return rating >= 0 && stage == -1 && flow == -1 && stage_name.empty() && flow_name.empty();
    if (rating != -1 || !rating_name.empty() || stage < -1 || flow < -1 ||
        (stage == -1 && !stage_name.empty()) || (flow == -1 && !flow_name.empty()))
        return false;
    if (type == int8_t(BoundaryType::WALL) || type == int8_t(BoundaryType::NORMAL_FLOW))
        return stage == -1 && flow == -1;
    if (type == int8_t(BoundaryType::SPECIFIED_STAGE))
        return flow == -1;
    return type == int8_t(BoundaryType::SPECIFIED_FLOW) && stage == -1;
}
/**
 * @brief Test whether authored fallback descriptors contain NORMAL_FLOW or RATING_CURVE.
 * @param[in] boundary Stable descriptor arrays and optional override fallback map.
 * @return True when any fallback kind (or current kind without saved original) matches. No
 * topology, value or table validation occurs and no model state is changed.
 */
bool rk2HasStateBoundary(const BoundaryData& boundary) {
    for (std::size_t slot = 0; slot < boundary.edge_bc_type.size(); ++slot) {
        const auto original = boundary.runtime_original.find(int(slot));
        const auto type = original == boundary.runtime_original.end() ? boundary.edge_bc_type[slot]
                                                                      : original->second.type;
        if (type == int8_t(BoundaryType::NORMAL_FLOW) || type == int8_t(BoundaryType::RATING_CURVE))
            return true;
    }
    return false;
}
/**
 * @brief Test whether authored fallback descriptors contain any non-WALL kind.
 * @param[in] boundary Stable descriptor arrays and optional override fallback map.
 * @return True when any fallback kind (or current kind without saved original) matches. No
 * topology, value or table validation occurs and no model state is changed.
 */
bool rk2HasAuthoredSurfaceBoundary(const BoundaryData& boundary) {
    for (std::size_t slot = 0; slot < boundary.edge_bc_type.size(); ++slot) {
        const auto original = boundary.runtime_original.find(int(slot));
        const auto type = original == boundary.runtime_original.end() ? boundary.edge_bc_type[slot]
                                                                      : original->second.type;
        if (type != int8_t(BoundaryType::WALL))
            return true;
    }
    return false;
}
}
/**
 * @brief Detect any hydraulic boundary use or retained override/receipt history.
 * @param[in] boundary Optional stable boundary owner.
 * @return False for null or wall-only arrays with no history; true for any receipts, saved
 * runtime originals or non-WALL current type. This capability test does not validate arrays or
 * descriptors.
 */
bool rk2HasSurfaceBoundary(const BoundaryData* boundary) {
    return boundary &&
           (!boundary->receipts.empty() || !boundary->runtime_original.empty() ||
            std::any_of(boundary->edge_bc_type.begin(), boundary->edge_bc_type.end(),
                        [](int8_t type) { return type != int8_t(BoundaryType::WALL); }));
}
/**
 * @brief Validate prepared RK2 exterior boundary descriptors, composition and physical receipts.
 * @param[in] mesh Stable valid mixed mesh; padded slot shape is four times cell count, with
 * valid cell vertex counts and neighbors for exterior-edge tests.
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
std::string validateRk2SurfaceBoundary(const MeshData& mesh, const SurfaceStateData& surface) {
    if (!rk2HasSurfaceBoundary(surface.boundary))
        return surface.transport.runtime_bc_conc.empty()
                   ? std::string{}
                   : "RK2 boundary quality has no hydraulic edge.";
    const auto& b = *surface.boundary;
    const auto& tr = surface.transport;
    const auto n = std::size_t(mesh.n_cells() * kMaxCellVerts), ns = std::size_t(tr.n_species);
    if (b.edge_bc_type.size() != n || b.edge_bc_head.size() != n || b.edge_bc_flow.size() != n ||
        b.edge_bed_slope.size() != n || b.edge_bc_tseries.size() != n ||
        b.edge_bc_flow_tseries.size() != n || b.edge_bc_rating_curve.size() != n ||
        b.edge_bc_tseries_name.size() != n || b.edge_bc_flow_tseries_name.size() != n ||
        b.edge_bc_rating_curve_name.size() != n || b.edge_bc_cum_flux.size() != n)
        return "RK2 surface boundary shape differs.";
    const auto exterior = [&](int slot) {
        return slot >= 0 && std::size_t(slot) < n &&
               MeshData::slot_local(slot) < mesh.cell_vertex_count(MeshData::slot_cell(slot)) &&
               mesh.cell_neighbour(MeshData::slot_cell(slot), MeshData::slot_local(slot)) < 0;
    };
    for (std::size_t slot = 0; slot < n; ++slot) {
        const auto type = BoundaryType(b.edge_bc_type[slot]);
        if ((type != BoundaryType::WALL && type != BoundaryType::NORMAL_FLOW &&
             type != BoundaryType::SPECIFIED_STAGE && type != BoundaryType::SPECIFIED_FLOW &&
             type != BoundaryType::RATING_CURVE) ||
            (type != BoundaryType::WALL &&
             (!exterior(int(slot)) || !b.receipts.count(int(slot)))) ||
            !std::isfinite(b.edge_bc_head[slot]) || !std::isfinite(b.edge_bc_flow[slot]) ||
            !std::isfinite(b.edge_bed_slope[slot]) || !std::isfinite(b.edge_bc_cum_flux[slot]) ||
            !rk2SurfaceDescriptor(b.edge_bc_type[slot], b.edge_bc_tseries[slot],
                                  b.edge_bc_flow_tseries[slot], b.edge_bc_rating_curve[slot],
                                  b.edge_bc_tseries_name[slot], b.edge_bc_flow_tseries_name[slot],
                                  b.edge_bc_rating_curve_name[slot]))
            return "RK2 surface boundaries require supported exterior prescriptions and resolved signals.";
    }
    if (b.runtime_original.size() != b.runtime_owner.size() ||
        b.runtime_original.size() != tr.runtime_bc_conc.size())
        return "RK2 surface boundary owner/quality layout differs.";
    for (const auto& [slot, old] : b.runtime_original) {
        const auto owner = b.runtime_owner.find(slot);
        const auto quality = tr.runtime_bc_conc.find(slot);
        if (!exterior(slot) ||
            (old.type != int8_t(BoundaryType::WALL) &&
             old.type != int8_t(BoundaryType::NORMAL_FLOW) &&
             old.type != int8_t(BoundaryType::SPECIFIED_STAGE) &&
             old.type != int8_t(BoundaryType::SPECIFIED_FLOW) &&
             old.type != int8_t(BoundaryType::RATING_CURVE)) ||
            !std::isfinite(old.head) || !std::isfinite(old.flow) || !std::isfinite(old.slope) ||
            !rk2SurfaceDescriptor(old.type, old.stage_ts, old.flow_ts, old.rating, old.stage_name,
                                  old.flow_name, old.rating_name) ||
            owner == b.runtime_owner.end() || owner->second.find('/') != std::string::npos ||
            owner->second.find('\0') != std::string::npos || quality == tr.runtime_bc_conc.end() ||
            quality->second.size() != ns)
            return "Invalid RK2 surface boundary fallback, owner or quality.";
        for (int row = 0; row < tr.n_species; ++row)
            if (!std::isfinite(quality->second[row]) ||
                (row != tr.temp_row && quality->second[row] < 0))
                return "Invalid RK2 surface boundary composition.";
    }
    for (const auto& row : tr.bc_quality_rows) {
        if (!exterior(row.slot) || row.species < 0 || row.species >= tr.n_species ||
            !std::isfinite(row.conc) || (row.species != tr.temp_row && row.conc < 0))
            return "Invalid RK2 authored surface boundary quality.";
        const auto original = b.runtime_original.find(row.slot);
        const auto type =
            original == b.runtime_original.end() ? b.edge_bc_type[row.slot] : original->second.type;
        if (type == int8_t(BoundaryType::WALL))
            return "RK2 authored boundary quality has no hydraulic fallback.";
    }
    for (const auto& [slot, r] : b.receipts) {
        if (!exterior(slot) || r.requested_mass.size() != ns || r.applied_mass.size() != ns ||
            r.water_in < 0 || r.water_out < 0 || r.last_dt < 0 ||
            std::abs(r.applied - (r.water_in - r.water_out)) >
                1e-10 * std::max({1., r.water_in, r.water_out}))
            return "Invalid RK2 surface boundary water receipt.";
        for (double v : {r.requested, r.applied, r.water_in, r.water_out, r.heat_requested,
                         r.heat_applied, r.last_flow, r.last_dt})
            if (!std::isfinite(v))
                return "Nonfinite RK2 surface boundary receipt.";
        for (std::size_t row = 0; row < ns; ++row)
            if (!std::isfinite(r.requested_mass[row]) || !std::isfinite(r.applied_mass[row]))
                return "Nonfinite RK2 surface boundary species receipt.";
    }
    for (int row = 0; row < tr.n_species; ++row) {
        double applied = 0;
        for (const auto& entry : b.receipts)
            applied += entry.second.applied_mass[row];
        const double net = tr.gained_boundary[row] - tr.lost_boundary[row];
        if (!std::isfinite(applied) || !std::isfinite(net) ||
            std::abs(applied - net) > 1e-10 * std::max({1., std::abs(applied), std::abs(net)}))
            return "RK2 surface boundary physical species ledger differs.";
    }
    return {};
}
/**
 * @brief Archive a matched RK2 boundary model identity or its accepted runtime receipts.
 * @param[in,out] a Sequential writer/reader; outer framing and final validation belong to the
 * owner.
 * @param[in,out] b Prepared matching boundary arrays. Model mode archives fixed descriptors,
 * fallback/owner maps and retained receipt keys; runtime mode fixed cumulative flux and existing
 * receipt values, plus head/flow work values for dynamic signals/curves.
 * @param[in,out] quality Runtime boundary concentration map, archived/rebuilt only in model
 * mode.
 * @param[in] model True selects copied/authored/state-dependent/time-dependent/rated model
 * identity; false selects runtime payload against that already prepared exact profile and
 * receipt-key layout.
 * @param[in] species Nonnegative receipt row count used when rebuilding model receipt
 * placeholders.
 * @throws std::runtime_error On profile mismatch, duplicate/unsorted/out-of-range keys, archive
 * shape or decoding failure. Allocation failures propagate; inputs may be partially decoded on
 * failure.
 * @note Dynamic work head/flow values are zeroed in writer model identity when supplied by a
 * resolved series/curve. Physical values retain SI/native units. This helper neither validates
 * full boundary physics/species budgets nor serializes referenced model table records; callers
 * stage private copies.
 */
void rk2SurfaceBoundaryArchive(Archive& a, BoundaryData& b,
                               std::map<int, std::vector<double>>& quality, bool model,
                               int species) {
    const bool signals = rk2HasSurfaceSeries(b), curves = rk2HasSurfaceCurve(b),
               dynamic = signals || curves;
    const std::string expected =
        curves                             ? "RK2 rated surface boundaries v1"
        : signals                          ? "RK2 time-dependent surface boundaries v1"
        : rk2HasStateBoundary(b)           ? "RK2 state-dependent surface boundaries v1"
        : rk2HasAuthoredSurfaceBoundary(b) ? "RK2 authored surface boundaries v1"
                                           : "RK2 copied surface boundaries v1";
    std::string profile = expected;
    a(profile);
    if (profile != expected)
        throw std::runtime_error("Unsupported RK2 surface boundary record.");
    const auto keys = [&](auto& map, auto fields) {
        std::vector<int> slots;
        if (!a.reading())
            for (const auto& entry : map)
                slots.push_back(entry.first);
        a(slots);
        if (a.reading()) {
            if (!std::is_sorted(slots.begin(), slots.end()) ||
                std::adjacent_find(slots.begin(), slots.end()) != slots.end())
                throw std::runtime_error("Duplicate RK2 surface boundary record.");
            map.clear();
            for (int slot : slots) {
                if (slot < 0 || slot >= b.size())
                    throw std::runtime_error("Invalid RK2 surface boundary slot.");
                map.try_emplace(slot);
            }
        }
        for (int slot : slots)
            fields(map.at(slot));
    };
    if (model) {
        auto head = b.edge_bc_head, flow = b.edge_bc_flow;
        if (dynamic && !a.reading())
            for (std::size_t slot = 0; slot < head.size(); ++slot) {
                if (b.edge_bc_tseries[slot] >= 0)
                    head[slot] = 0;
                if (b.edge_bc_flow_tseries[slot] >= 0 || b.edge_bc_rating_curve[slot] >= 0)
                    flow[slot] = 0;
            }
        a.fixed(b.edge_bc_type, head, flow, b.edge_bed_slope, b.edge_bc_tseries,
                b.edge_bc_flow_tseries, b.edge_bc_rating_curve, b.edge_bc_tseries_name,
                b.edge_bc_flow_tseries_name, b.edge_bc_rating_curve_name);
        if (a.reading()) {
            b.edge_bc_head = std::move(head);
            b.edge_bc_flow = std::move(flow);
        }
        keys(b.runtime_original, [&](auto& v) {
            auto head = v.head, flow = v.flow;
            if (dynamic && !a.reading()) {
                if (v.stage_ts >= 0)
                    head = 0;
                if (v.flow_ts >= 0 || v.rating >= 0)
                    flow = 0;
            }
            a(v.type, head, flow, v.slope, v.stage_ts, v.flow_ts, v.rating, v.stage_name,
              v.flow_name, v.rating_name);
            if (a.reading()) {
                v.head = head;
                v.flow = flow;
            }
        });
        keys(b.runtime_owner, [&](auto& owner) { a(owner); });
        keys(quality, [&](auto& values) { a(values); });
        // History keeps a cleared edge's canonical receipt and pin footprint.
        std::vector<int> history;
        if (!a.reading())
            for (const auto& entry : b.receipts)
                history.push_back(entry.first);
        a(history);
        if (a.reading()) {
            b.receipts.clear();
            for (int slot : history) {
                if (slot < 0 || slot >= b.size() || b.receipts.count(slot))
                    throw std::runtime_error("Invalid RK2 surface boundary history.");
                b.ensureReceipt(slot, species);
            }
        }
    } else {
        a.fixed(b.edge_bc_cum_flux);
        if (dynamic)
            a.fixed(b.edge_bc_head, b.edge_bc_flow);
        for (auto& [slot, r] : b.receipts)
            a(r.requested, r.applied, r.water_in, r.water_out, r.heat_requested, r.heat_applied,
              r.last_flow, r.last_dt, r.requested_mass, r.applied_mass);
    }
}
/**
 * @brief Validate the complete accepted standalone coupled RK2 checkpoint profile.
 * @return Empty on qualification, otherwise layout, owner, forcing, immutable boundary signal,
 * receipt, native inventory, thermal, snow, reaction or completed-clock refusal.
 * @note Requires initialized global FULL_SWE second-order surface+groundwater, one tier,
 * supported source ownership, no common/joint/channel owner, source recorder/stream or live node
 * exchanges. Checks completed numerical/source/GW/snow clocks, boundary work/quality and
 * physical heat receipts, matching native species and admissible inventories; delegates
 * ground/thermal/runtime profile checks. Read-only but allocation and checked lookups may throw.
 * Does not settle or repair invalid state.
 */
std::string ExplicitInertialSolver::rk2CheckpointProfile() const {
    if (!initialized_ || !second_order_ || !gw_ || common_stage_ || joint_checkpoint_ ||
        channel_ground_ || source_history_ || engine_source_history_ || engine_source_stream_ ||
        !checkpoint_ready_ || mode_ != Momentum2D::FULL_SWE || opts_->lts_tiers != 1 ||
        (state_->node_coupling && !state_->node_coupling->empty()))
        return "RK2 checkpoint requires the standalone global transport/age coupled profile.";
    if (state_->snow.active() && state_->snow.completedEnd() != source_phase_end_)
        return "RK2 snow and numerical source clocks differ.";
    if (auto error = validateRk2SurfaceBoundary(*mesh_, *state_); !error.empty())
        return error;
    if (state_->boundary) {
        const auto& b = *state_->boundary;
        std::size_t matched = 0;
        for (int slot = 0; slot < b.size(); ++slot) {
            const auto original = b.runtime_original.find(slot);
            const auto type =
                original == b.runtime_original.end() ? b.edge_bc_type[slot] : original->second.type;
            const bool curve = type == int8_t(BoundaryType::RATING_CURVE);
            const int table =
                curve ? (original == b.runtime_original.end() ? b.edge_bc_rating_curve[slot]
                                                              : original->second.rating)
                : type == int8_t(BoundaryType::SPECIFIED_STAGE)
                    ? (original == b.runtime_original.end() ? b.edge_bc_tseries[slot]
                                                            : original->second.stage_ts)
                    : (original == b.runtime_original.end() ? b.edge_bc_flow_tseries[slot]
                                                            : original->second.flow_ts);
            if (table < 0)
                continue;
            const bool stage = type == int8_t(BoundaryType::SPECIFIED_STAGE);
            const auto found = std::find_if(
                rk_boundary_signals_.begin(), rk_boundary_signals_.end(), [&](const auto& row) {
                    return row.slot == slot && row.table == table && row.stage == stage &&
                           bool(row.curve) == curve;
                });
            if (found == rk_boundary_signals_.end())
                return "RK2 surface boundary immutable signal binding differs.";
            ++matched;
            if (original == b.runtime_original.end()) {
                double expected = found->scale * found->signal.at(t_last_sync_);
                if (found->curve) {
                    const int cell = MeshData::slot_cell(slot);
                    double eta = 0, depth = 0;
                    inertial::cellEtaDepth(*mesh_, *opts_, cell, state_->volume.at(cell), eta,
                                           depth);
                    expected = found->scale * table_lookupEx(*found->curve, eta / found->x_scale);
                }
                const double actual = (stage ? b.edge_bc_head : b.edge_bc_flow)[slot];
                if (!std::isfinite(expected) ||
                    std::abs(expected - actual) >
                        1e-12 * std::max({1., std::abs(expected), std::abs(actual)}))
                    return "RK2 evaluated surface boundary differs from its completed clock.";
            }
        }
        if (matched != rk_boundary_signals_.size())
            return "RK2 surface boundary immutable signal layout differs.";
    }
    if (rk2HasSurfaceBoundary(state_->boundary)) {
        std::vector<int> cells, slots, walls, ptr{0};
        for (int cell = 0; cell < mesh_->n_cells(); ++cell) {
            for (int edge = 0; edge < mesh_->cell_vertex_count(cell); ++edge)
                if (mesh_->cell_neighbour(cell, edge) < 0) {
                    const int slot = MeshData::slot(cell, edge);
                    if (state_->boundary->edge_bc_type[slot] == int8_t(BoundaryType::WALL))
                        walls.push_back(slot);
                    else {
                        cells.push_back(cell);
                        slots.push_back(slot);
                    }
                }
            ptr.push_back(int(walls.size()));
        }
        if (cells != bc_cell_ || slots != bc_slot_ || walls != wall_slot_ || ptr != wall_ptr_ ||
            bc_q_.size() != slots.size() || bc_accum_.size() != slots.size())
            return "RK2 surface boundary work layout differs.";
        const auto ns = std::size_t(state_->transport.n_species);
        if (state_->transport.bc_conc.size() != slots.size() * ns)
            return "RK2 surface boundary quality lookup shape differs.";
        for (std::size_t k = 0; k < slots.size(); ++k) {
            std::vector<double> expected(ns, 0);
            for (const auto& row : state_->transport.bc_quality_rows)
                if (row.slot == slots[k])
                    expected[row.species] = row.conc;
            const auto copied = state_->transport.runtime_bc_conc.find(slots[k]);
            if (copied != state_->transport.runtime_bc_conc.end())
                expected = copied->second;
            for (std::size_t row = 0; row < ns; ++row)
                if (state_->transport.bc_conc[k * ns + row] != expected[row])
                    return "RK2 surface boundary quality lookup differs.";
        }
        for (const auto* values : {&bc_q_, &bc_accum_})
            for (double v : *values)
                if (!std::isfinite(v))
                    return "Nonfinite RK2 surface boundary work state.";
        for (const auto& [slot, r] : state_->boundary->receipts) {
            if (r.last_dt > t_last_sync_ || (r.last_dt == 0 && r.last_flow != 0) ||
                std::abs(r.last_flow) * r.last_dt >
                    r.water_in + r.water_out + 1e-10 * std::max(1., r.water_in + r.water_out))
                return "RK2 surface boundary last receipt differs from its completed clock.";
            const int temp = state_->transport.temp_row;
            const double requested =
                             temp < 0 ? 0 : r.requested_mass[temp] * opts_->water_heat_capacity,
                         applied = temp < 0 ? 0 : r.applied_mass[temp] * opts_->water_heat_capacity;
            if (std::abs(requested - r.heat_requested) >
                    1e-10 * std::max(1., std::abs(requested)) ||
                std::abs(applied - r.heat_applied) > 1e-10 * std::max(1., std::abs(applied)))
                return "RK2 surface boundary physical heat receipt differs.";
        }
    }
    if (auto error = rk2ThermalProfileError(); !error.empty())
        return error;
    if (auto error = rk2SurfaceRuntimeProfile(*state_, gw_); !error.empty())
        return error;
    const auto& tr = state_->transport;
    const auto& ground = gw_->transport();
    if (tr.active() && (tr.row_names != ground.row_names || tr.n_species != ground.n_species ||
                        tr.age_row != ground.age_row || tr.temp_row != ground.temp_row))
        return "Coupled RK2 requires matching surface/groundwater species and age rows.";
    if (ground.temp_row >= 0 && (tr.temp_row < 0 || (opts_->coupled_heat && !state_->heat.enabled)))
        return "RK2 water temperature requires matching surface rows and a qualified thermal owner.";
    if (tr.n_cells < 0 || tr.n_species < 0 ||
        tr.cell_mass.size() != std::size_t(tr.n_cells) * std::size_t(tr.n_species))
        return "RK2 surface transport shape differs.";
    for (std::size_t k = 0; k < tr.cell_mass.size(); ++k)
        if (!std::isfinite(tr.cell_mass[k]) ||
            (int(k / std::size_t(tr.n_cells)) != tr.temp_row && tr.cell_mass[k] < 0))
            return "RK2 surface transport inventory is inadmissible.";
    if (!rk_reacted_.empty()) {
        if (rk_reacted_.size() != std::size_t(tr.n_species) ||
            rk_reacted_reported_.size() != std::size_t(tr.n_pollut))
            return "RK2 surface reaction ledger shape differs.";
        for (int row = 0; row < tr.n_species; ++row)
            if (!std::isfinite(rk_reacted_[row]) ||
                (row >= tr.n_pollut + tr.n_msx && rk_reacted_[row] != 0))
                return "RK2 surface reaction ledger is inadmissible.";
        for (int row = 0; row < tr.n_pollut; ++row)
            if (!std::isfinite(rk_reacted_reported_[row]) || rk_reacted_reported_[row] < 0 ||
                rk_reacted_reported_[row] > rk_reacted_[row])
                return "RK2 surface reaction report receipt is inadmissible.";
    }
    for (const auto* row : {&state_->coupling_flux, &state_->coupling_applied})
        for (double v : *row)
            if (v != 0)
                return "RK2 checkpoint cannot recover external coupling receipts.";
    if (source_phase_end_ != t_last_sync_ || accumulators_pending_)
        return "RK2 checkpoint requires a completed numerical boundary.";
    for (int i = 0; i < gw_->state_.n_cells; ++i)
        if (gw_->covers(i) && gw_->cell_completed_[i] != t_last_sync_)
            return "RK2 surface and groundwater clocks differ.";
    return gw_->rk2ProfileError();
}
/**
 * @brief Encode the current immutable qualified RK2 model and held forcing identity.
 * @return Binary identity string from the binding-aware overload with no external overrides.
 * @note Requires live matching prepared owners. Allocates/serializes and may throw; no physical
 * or clock mutation occurs. The result is compared byte-for-byte during restore.
 */
std::string ExplicitInertialSolver::rk2CheckpointModel() const {
    return rk2CheckpointModel(nullptr);
}
/**
 * @brief Serialize RK2 mesh/options/native/thermal/snow and immutable forcing prescriptions.
 * @param[in] binding Optional borrowed managed candidates for
 * source/forcing/boundary/weather/heat identity. Supplied fields must match qualified prepared
 * layouts and remain stable during the call; null fields use live owners. No ownership transfers
 * or long-lived views are retained.
 * @return Binary model identity, including stable policy/version tags and held rain/evaporation.
 * @note Archives private copies of GW, mesh, options and runtime records; includes immutable
 * boundary series/curves, reaction definitions, physical heat/ground and snow forcing
 * configuration. Not a mutable-state checkpoint by itself. Requires prior profile validation,
 * exclusive stable borrowed context/table identities; allocation/archive/copy can throw without
 * physics advance.
 */
std::string ExplicitInertialSolver::rk2CheckpointModel(const Rk2CheckpointBinding* binding) const {
    Archive a;
    auto m = *mesh_;
    auto o = *opts_;
    auto gw = *gw_;
    gw.checkpoint(a, true, binding && binding->climate ? &binding->climate->temperature : nullptr);
    a(m.vx, m.vy, m.vz, m.vtag, m.cell_nv, m.cell_v, m.cell_nbr, m.tri_area, m.tri_cx, m.tri_cy,
      m.tri_cz, m.edge_length, m.edge_nx, m.edge_ny, m.edge_mx, m.edge_my, m.edge_mz, m.edge_dist_c,
      m.quad_vfr_z, m.quad_vfr_a, m.edge_conveyance, m.mannings_n, m.tri_init_depth, m.tri_init_u,
      m.tri_init_v, m.tri_tag, m.vert_stencil_ptr, m.vert_stencil_idx, m.vert_stencil_wt,
      m.vert_coupled_node, m.tri_coupled_node, m.vert_coupling_cd, m.vert_coupling_area,
      m.vert_coupling_area_set, m.tri_coupling_cd, m.tri_coupling_area, m.vert_coupled_node_name,
      m.tri_coupled_node_name);
    a(o.momentum, o.front_rebuild, o.reconstruction_order, o.max_timestep, o.dry_depth,
      o.limiter_epsilon, o.flux_dh_eps, o.coupling_sync, o.coupling_cd, o.report_2d,
      o.output_compression, o.report_2d_vars, o.report_2d_step, o.infiltration, o.infil_step,
      o.evaporation, o.transport_pollutants, o.transport_msx, o.transport_age,
      o.transport_temperature, o.groundwater, o.rainfall_mode, o.cell_closure,
      o.face_reconstruction, o.vfr_min_wet_frac, o.theta, o.cfl_number, o.h_move, o.lts_tiers,
      o.froude_max, o.advection, o.exchange_beta, o.dispersion, o.exchange_relax,
      o.coupling_area_auto, o.coupling_in_flooding, o.backend, o.len_1d_to_2d, o.len_2d_to_1d,
      o.vol_1d_to_2d, o.flow_1d_to_2d, o.flow_2d_to_1d, o.num_threads, o.requested_threads,
      o.mesh_units_si, o.mesh_scaled_to_si, o.mesh_to_si_factor, o.pending_rows_drained,
      o.bc_flow_to_si_applied);
    a(o.infil_default_method, o.infil_destination, o.gw_et);
    if (o.auto_backend_cpu) {
        std::string policy = "RK2 session CPU default v1";
        a(policy, o.auto_backend_cpu);
    }
    // Exchange law options join the identity only when set, so a legacy
    // model's checkpoint identity is unchanged.
    if (o.coupling_law != CouplingLaw2D::ORIFICE || o.coupling_head != CouplingHead2D::CELL ||
        o.coupling_coeffs != 0)
        a(o.coupling_law, o.coupling_head, o.coupling_coeffs);
    if (o.coupling_pond_mode != CouplingPondMode2D::FOOTPRINT || o.coupling_pond_relax != 0 ||
        o.coupling_rim_band >= 0 || o.coupling_equalize >= 0)
        a(o.coupling_pond_mode, o.coupling_pond_relax, o.coupling_rim_band, o.coupling_equalize);

    if (state_->transport.active()) {
        std::string profile = "shared RK2 passive transport and age v1";
        auto tr = state_->transport;
        a(profile, tr.n_species, tr.n_pollut, tr.n_msx, tr.age_row, tr.temp_row, tr.row_names,
          tr.rain_conc, tr.bc_conc);
        if (tr.temp_row >= 0)
            a(o.coupled_heat, o.water_heat_capacity);
    }
    if (!rk_reacted_.empty()) {
        std::string profile = "shared RK2 surface reactions v1", identity = rk_reaction_identity_;
        auto decay = rk_decay_;
        auto rows = rk_reaction_rows_;
        a(profile, identity, decay, rows);
    }
    if (state_->heat.enabled) {
        std::string profile = opts_->cell_closure == CellClosure2D::VFR
                                  ? "RK2 finite surface/soil geometric contact v2"
                                  : "RK2 finite surface/soil thermal owner v1";
        auto heat = checkpoint::meshHeatModelIdentity(state_->heat);
        a(profile, heat);
        std::string evaporation = "RK2 accepted mesh evaporation receipt v1";
        a(evaporation);
        if (surfaceLatentCooling()) {
            std::string latent = "RK2 accepted surface latent cooling v1";
            a(latent);
        }
        if (state_->heat.surface || state_->heat.radiative) {
            std::string atmosphere = "RK2 nonlinear atmospheric midpoint v1";
            a(atmosphere);
            auto& ctx = *state_->heat.context;
            const auto& climate =
                binding && binding->climate ? *binding->climate : ctx.climate_state;
            const auto& heat = binding && binding->heat ? *binding->heat : ctx.heat_state;
            auto temperature = climate.temperature, humidity = climate.humidity,
                 wind = climate.wind_speed;
            auto shortwave = heat.shortwave_now, cloud = heat.cloud_now,
                 longwave = climate.radiation.longwave;
            auto shared = climate.radiation.shared_heat_forcing;
            a(temperature, humidity, wind, shortwave, cloud, shared, longwave,
              ctx.options.water_density, ctx.options.wind_func_coeff_a,
              ctx.options.wind_func_coeff_b, ctx.options.pressure_ratio);
        }
    }
    if (state_->snow.active()) {
        const auto& ctx = *state_->snow_context;
        const auto& climate = binding && binding->climate ? *binding->climate : ctx.climate_state;
        std::string policy = "RK2 symmetric owned snow water source v1",
                    identity = state_->snow.configurationIdentity();
        auto forcing = surface::snowForcingIdentity(ctx);
        auto packs = ctx.snowpacks.pervious;
        auto anchor = ctx.options.start_date;
        auto duration = ctx.options.totalDurationMs();
        auto units = ctx.options.flow_units;
        auto temperature = climate.temperature, humidity = climate.humidity,
             wind = climate.wind_speed, gamma = climate.gamma, division = climate.snow_divt;
        a(policy, identity, forcing, packs, anchor, duration, units, temperature, humidity, wind,
          gamma, division);
    }
    if (o.evaporation == 0 && std::any_of(state_->evap_rate.begin(), state_->evap_rate.end(),
                                          [](double value) { return value != 0; })) {
        std::string evaporation = "RK2 inactive native evaporation gate v1";
        a(evaporation);
    }
    auto runtime = binding && binding->surface ? *binding->surface : state_->runtime_sources;
    rk2SurfaceRuntimeArchive(a, runtime, mesh_->n_cells(), true);
    auto ground_runtime =
        binding && binding->groundwater ? *binding->groundwater : gw_->runtimeSources();
    rk2SurfaceRuntimeArchive(a, ground_runtime, mesh_->n_cells(), true, true);
    auto forcing = *state_;
    if (binding && binding->forcing)
        forcing.runtime_forcings = *binding->forcing;
    rk2ForcingArchive(a, forcing, true);
    if (rk2HasSurfaceBoundary(state_->boundary)) {
        auto boundary = *state_->boundary;
        auto quality = state_->transport.runtime_bc_conc;
        rk2SurfaceBoundaryArchive(a, boundary, quality, true, state_->transport.n_species);
    }
    if (state_->boundary && rk2HasAuthoredSurfaceBoundary(*state_->boundary)) {
        // Bind authored fallback quality even when a copied full tuple hides it.
        std::string policy = "RK2 authored surface boundary quality v1";
        a(policy);
        auto rows = state_->transport.bc_quality_rows;
        a.sequence(rows, [&](auto& row) { a(row.slot, row.species, row.conc); });
    }
    if (state_->boundary && rk2HasStateBoundary(*state_->boundary)) {
        std::string policy = "RK2 held state-dependent surface derivatives v1";
        a(policy);
    }
    if (!rk_boundary_signals_.empty()) {
        const bool curves = std::any_of(rk_boundary_signals_.begin(), rk_boundary_signals_.end(),
                                        [](const auto& row) { return bool(row.curve); });
        std::string policy = curves ? "RK2 immutable surface boundary signals v2"
                                    : "RK2 immutable surface boundary signals v1";
        auto anchor = rk_boundary_anchor_;
        a(policy, anchor);
        auto signals = rk_boundary_signals_;
        a.sequence(signals, [&](auto& row) {
            bool curve = bool(row.curve);
            if (curves)
                a(curve);
            a(row.slot, row.table, row.stage, row.scale, row.name);
            if (curve) {
                auto table = *row.curve;
                auto scale = row.x_scale;
                a(scale, table.type, table.x, table.y);
            } else {
                auto times = row.signal.timePoints(), values = row.signal.ordinates();
                a(times, values);
            }
        });
    }
    auto rain = state_->rainfall, evap = state_->evap_rate;
    a(rain, evap);
    return a.bytes();
}
/**
 * @brief Encode the qualified coupled RK2 boundary with immutable identity and mutable owner
 * state.
 * @param[out] bytes Replaced only after successful complete version-4 envelope and size
 * validation.
 * @return Empty on success, otherwise profile/size or caught standard exception diagnostic.
 * @note Copies marcher/surface/GW, archives physical surface/native/heat/snow state, GW
 * completed clocks, settled schedule, runtime/forcing/boundary receipts and checksum/platform
 * markers. Borrowed boundary is copied. No owner/clock/physics mutation; caller holds exclusive
 * consistent views.
 */
std::string ExplicitInertialSolver::rk2CheckpointBytes(std::string& bytes) const {
    try {
        if (auto error = rk2CheckpointProfile(); !error.empty())
            return error;
        auto trial = *this;
        auto surface = *state_;
        auto gw = *gw_;
        Archive state;
        auto model = rk2CheckpointModel();
        state(model);
        checkpoint::meshState(state, surface);
        gw.checkpoint(state, false);
        state(gw.cell_completed_);
        trial.checkpointState(state);
        rk2SurfaceRuntimeArchive(state, surface.runtime_sources, mesh_->n_cells(), false);
        rk2ForcingArchive(state, surface, false);
        rk2SurfaceRuntimeArchive(state, gw.runtimeSources(), mesh_->n_cells(), false, true);
        if (rk2HasSurfaceBoundary(surface.boundary)) {
            auto boundary = *surface.boundary;
            rk2SurfaceBoundaryArchive(state, boundary, surface.transport.runtime_bc_conc, false,
                                      surface.transport.n_species);
        }
        std::string magic = magicName, payload = state.bytes();
        uint32_t version = 4, endian = 0x01020304, longBytes = sizeof(long),
                 crc = checksum(payload);
        Archive file;
        file(magic, version, endian, longBytes, payload, crc);
        if (file.bytes().size() > maxBytes)
            return "RK2 checkpoint exceeds its size limit.";
        bytes = file.bytes();
        return {};
    } catch (const std::exception& e) {
        return e.what();
    }
}
/**
 * @brief Restore a standalone RK2 component checkpoint without managed identity overrides.
 * @param[in] bytes Owned bounded nonempty encoded version-4 envelope.
 * @return Empty on successful staged install, otherwise specialized restore diagnostic.
 * @note Delegates with null binding/validation/install hooks and inspect=false; requires fresh
 * qualified exclusive matching owners and preserves original borrowed owner identities.
 */
std::string ExplicitInertialSolver::loadRk2CheckpointBytes(std::string bytes) {
    return loadRk2CheckpointBytes(std::move(bytes), nullptr, nullptr, nullptr, false);
}
/**
 * @brief Decode and validate a coupled RK2 checkpoint privately before inspection or
 * installation.
 * @param[in] bytes Owned bounded nonempty version-4 envelope, consumed by local archive
 * decoding.
 * @param[in] binding Optional borrowed managed source/forcing/boundary/weather candidates whose
 * immutable identity must match encoded prescriptions; null fields fall back to prepared live
 * owners.
 * @param[in] validate Optional pointer to a nonempty callback receiving const staged marcher,
 * surface and groundwater references; views expire after the call and callback effects are
 * external.
 * @param[in] before_install Optional pointer to a nonempty callback invoked after all checks
 * when installing, before moving candidate owners; its external effects are not rolled back.
 * @param[in] inspect True to validate and invoke validate only, without installing or calling
 * before_install. False installs the validated boundary, surface, GW and marcher.
 * @return Empty on success/inspection, otherwise profile, freshness, envelope/platform/checksum,
 * model/forcing/layout/admissibility or caught standard exception diagnostic.
 * @note Requires initialized fresh global coupled profile, zero completed numerical
 * time/samples, exclusive owners and stable live mesh/options/context identities. Managed
 * candidates are checked before enabled flags rebuild indexed caches; all payload and
 * post-decode profiles are validated. Rebinds the original surface/GW/boundary and
 * physical-ground heat pointers on adoption. Refusal before install preserves canonical owners
 * except external callback effects. Does not restore unrelated engine/calendar/output state.
 */
std::string ExplicitInertialSolver::loadRk2CheckpointBytes(
    std::string bytes, const Rk2CheckpointBinding* binding,
    const std::function<void(const ExplicitInertialSolver&, const SurfaceStateData&,
                             const SubsurfaceSolver&)>* validate,
    const std::function<void()>* before_install, bool inspect) {
    try {
        if (auto error = rk2CheckpointProfile(); !error.empty())
            return error;
        if (t_last_sync_ != 0 || active_samples_ != 0)
            return "RK2 restore requires a fresh numerical owner.";
        if (bytes.empty() || bytes.size() > maxBytes)
            return "RK2 checkpoint exceeds its size limit.";
        Archive file(std::move(bytes));
        std::string magic, payload;
        uint32_t version = 0, endian = 0, longBytes = 0, crc = 0;
        file(magic, version, endian, longBytes, payload, crc);
        file.finish();
        if (magic != magicName || version != 4 || endian != 0x01020304 || longBytes != sizeof(long))
            return "Unsupported RK2 checkpoint version/platform.";
        if (crc != checksum(payload))
            return "RK2 checkpoint checksum differs.";
        Archive state(std::move(payload));
        std::string model;
        state(model);
        if (!binding && model != rk2CheckpointModel())
            return "RK2 checkpoint mesh, options, groundwater or held forcing differs.";
        auto trial = *this;
        auto surface = *state_;
        auto gw = *gw_;
        BoundaryData boundary;
        if (state_->boundary) {
            boundary = binding && binding->boundary ? *binding->boundary : *state_->boundary;
            surface.boundary = &boundary;
        }
        if (binding) {
            if (binding->surface)
                surface.runtime_sources = *binding->surface;
            if (binding->groundwater)
                gw.runtimeSources() = *binding->groundwater;
            if (binding->forcing)
                surface.runtime_forcings = *binding->forcing;
            if (binding->boundary_quality)
                surface.transport.runtime_bc_conc = *binding->boundary_quality;
            // Managed descriptors are untrusted input. Admit their immutable
            // layout before a mutable enabled flag can rebuild indexed tables.
            for (auto* store : {&surface.runtime_sources, &gw.runtimeSources()})
                for (auto& row : store->rows) {
                    const auto count = store == &surface.runtime_sources
                                           ? surface.transport.n_species
                                           : gw.transport().n_species;
                    row.receipt.requested_mass.assign(count, 0);
                    row.receipt.applied_mass.assign(count, 0);
                }
            for (auto& row : surface.runtime_forcings.rows) {
                row.receipt.requested_mass.assign(surface.transport.n_species, 0);
                row.receipt.applied_mass.assign(surface.transport.n_species, 0);
            }
            if (auto error = rk2SurfaceRuntimeProfile(surface, &gw); !error.empty())
                return error;
            if (auto error = gw.rk2ProfileError(); !error.empty())
                return error;
        }
        trial.state_ = &surface;
        trial.gw_ = &gw;
        if (surface.heat.enabled)
            surface.heat.aquifer_ground = &gw.heat();
        if (binding && rk2HasSurfaceBoundary(surface.boundary)) {
            if (auto error = validateRk2SurfaceBoundary(*mesh_, surface); !error.empty())
                return error;
            trial.refreshBoundaries();
            for (const auto& [slot, r] : boundary.receipts)
                trial.pin_t0_.at(MeshData::slot_cell(slot)) = 1;
        }
        checkpoint::meshState(state, surface);
        if (binding && model != trial.rk2CheckpointModel(binding))
            return "Managed RK2 checkpoint project, prescriptions or prepared weather differs.";
        gw.checkpoint(state, false);
        state.fixed(gw.cell_completed_);
        trial.checkpointState(state);
        rk2SurfaceRuntimeArchive(state, surface.runtime_sources, mesh_->n_cells(), false);
        rk2ForcingArchive(state, surface, false);
        rk2SurfaceRuntimeArchive(state, gw.runtimeSources(), mesh_->n_cells(), false, true);
        if (rk2HasSurfaceBoundary(surface.boundary))
            rk2SurfaceBoundaryArchive(state, boundary, surface.transport.runtime_bc_conc, false,
                                      surface.transport.n_species);
        state.finish();
        if (!trial.rk_boundary_signals_.empty() && model != trial.rk2CheckpointModel(binding))
            return "RK2 surface boundary payload changes an immutable prescription.";
        if (auto error = trial.rk2CheckpointProfile(); !error.empty())
            return error;
        if (validate)
            (*validate)(trial, surface, gw);
        if (inspect)
            return {};
        if (before_install)
            (*before_install)();
        trial.state_ = state_;
        trial.gw_ = gw_;
        if (surface.boundary) {
            static_assert(std::is_nothrow_move_assignable_v<BoundaryData>);
            *const_cast<BoundaryData*>(state_->boundary) = std::move(boundary);
            surface.boundary = state_->boundary;
        }
        *state_ = std::move(surface);
        *gw_ = std::move(gw);
        if (state_->heat.enabled)
            state_->heat.aquifer_ground = &gw_->heat();
        *this = std::move(trial);
        return {};
    } catch (const std::exception& e) {
        return e.what();
    }
}

/**
 * @brief Archive copied RK2 source/forcing prescriptions for a managed model binding.
 * @param[in,out] a Sequential archive; read mode may progressively replace row vectors.
 * @param[in,out] surface Surface source model rows.
 * @param[in,out] ground Groundwater source model rows, including optional boundary-edge/head
 * metadata.
 * @param[in,out] forcing Copied forcing model rows, including
 * channel/mode/rate/expiry/owner/composition.
 * @param[in] species Prepared nonnegative boundary receipt species count.
 * @param[in,out] boundary Optional hydraulic boundary model owner; required for encoded profile
 * v2.
 * @param[in,out] quality Optional boundary composition map; required with v2 boundary profile.
 * @throws std::runtime_error On unsupported v1/v2 profile, missing boundary owner or archive
 * failures.
 * @note This archives prescriptions/identity, not complete active runtime: decoding disables
 * every row and clears source/forcing index/cell caches for later reconstruction. Boundary-head
 * sources omit derived flow when their head is finite. Receipts, enabled flags and live external
 * provider programs are not encoded by this helper. Full restored profile/replay validation is
 * separate; use private candidates because failures can leave partial row/model changes.
 */
void rk2ManagedRuntimeModel(Archive& a, RuntimeSources& surface, RuntimeSources& ground,
                            RuntimeForcings& forcing, int species, BoundaryData* boundary,
                            std::map<int, std::vector<double>>* quality) {
    std::string profile = rk2HasSurfaceBoundary(boundary) ? "managed RK2 copied prescriptions v2"
                                                          : "managed RK2 copied prescriptions v1";
    a(profile);
    if (profile != "managed RK2 copied prescriptions v1" &&
        profile != "managed RK2 copied prescriptions v2")
        throw std::runtime_error("Unsupported managed RK2 prescription record.");
    const auto sources = [&](RuntimeSources& store) {
        a.sequence(store.rows, [&](auto& row) {
            a(row.id, row.owner, row.cell, row.boundary_edge, row.boundary_head);
            if (!std::isfinite(row.boundary_head))
                a(row.flow);
            a(row.heat, row.until, row.concentrations, row.rates);
            if (a.reading())
                row.enabled = false;
        });
        if (a.reading()) {
            store.index.clear();
            store.cells.clear();
        }
    };
    sources(surface);
    sources(ground);
    a.sequence(forcing.rows, [&](auto& row) {
        a(row.channel, row.cell, row.mode, row.rate, row.until, row.owner, row.concentrations);
        if (a.reading())
            row.enabled = false;
    });
    if (a.reading()) {
        forcing.index.clear();
        forcing.cells.clear();
    }
    if (profile == "managed RK2 copied prescriptions v2") {
        if (!boundary || !quality)
            throw std::runtime_error("Managed RK2 boundary owner is missing.");
        rk2SurfaceBoundaryArchive(a, *boundary, *quality, true, species);
    }
}
/**
 * @brief Check provider namespaces, active expiry and last-receipt horizons at a managed
 * boundary.
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
std::string validateRk2ReplayIds(const RuntimeSources& surface, const RuntimeSources& ground,
                                 const RuntimeForcings& forcing, double completed) {
    const auto owner = [](const std::string& value) {
        return value.find('/') == std::string::npos && value.find('\0') == std::string::npos;
    };
    const auto receipt = [&](const RuntimeReceipt& r) {
        return r.last_dt <= completed && (r.last_dt != 0 || r.last_flow == 0) &&
               std::abs(r.last_flow) * r.last_dt <=
                   r.water_in + r.water_out + 1e-12 * std::max(1., r.water_in + r.water_out);
    };
    for (const auto* store : {&surface, &ground})
        for (const auto& row : store->rows) {
            if (!owner(row.owner) || row.id.find('\0') != std::string::npos ||
                !receipt(row.receipt) || (row.enabled && row.until <= completed))
                return "Invalid managed RK2 source owner, expiry or completed receipt.";
            if (row.boundary_edge < 0) {
                const auto prefix =
                    row.owner.empty() ? std::string{} : "provider/" + row.owner + "/";
                if (row.id.rfind(prefix, 0) != 0)
                    return "Managed RK2 provider source identity differs.";
                const auto local = row.id.substr(prefix.size());
                if (local.empty() || local.rfind("provider/", 0) == 0 ||
                    local.rfind("boundary/", 0) == 0)
                    return "Invalid managed RK2 local source identity.";
            }
        }
    for (const auto& row : forcing.rows)
        if (!owner(row.owner) || !receipt(row.receipt) || (row.enabled && row.until <= completed))
            return "Invalid managed RK2 forcing owner, expiry or completed receipt.";
    return {};
}

}
