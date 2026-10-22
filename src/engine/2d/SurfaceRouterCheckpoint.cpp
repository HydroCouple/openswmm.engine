// SPDX-License-Identifier: Apache-2.0

/**
 * @file      SurfaceRouterCheckpoint.cpp
 * @brief     Checkpoint persistence for the 2D surface router.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include "SurfaceRouter2D.hpp"
#include "solver/ExplicitInertialSolver.hpp"
#include "coupling/Rk2RuntimeSources.hpp"
#include "coupling/RuntimeCouplingCheckpoint.hpp"
#include "subsurface/CommonSourceStage.hpp"
#include "../core/CompletedSourceArchive.hpp"
#include "../core/SimulationContext.hpp"
#include "../core/QualityCheckpoint.hpp"
#include "../core/DateTime.hpp"
#include "../hydraulics/Routing.hpp"
#include "../hydrology/LidNode.hpp"
#include <cstdlib>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace openswmm::twoD {
namespace {
using Archive = detail::CompletedSourceArchive;
constexpr uint64_t maxBytes = 64 * 1024 * 1024;
constexpr const char* magicName = "OpenSWMM source router boundary";
constexpr const char* jointMagic = "OpenSWMM joint DW surface/source boundary";
constexpr const char* rk2Magic = "OpenSWMM managed standalone RK2 router boundary";
/**
 * @brief Compare two finite checkpoint scalar values with a relative floor tolerance.
 * @param[in] a First scalar in the same physical units as b.
 * @param[in] b Second scalar.
 * @return abs(a-b)<=1e-12*max(1,abs(a),abs(b)); nonfinite inputs are not admitted by this
 * helper.
 * @note No mutation/allocation. This numerical ledger test is not an identity comparison.
 */
bool close(double a, double b) {
    return std::abs(a - b) <= 1e-12 * std::max({1.0, std::abs(a), std::abs(b)});
}
/**
 * @brief Compute reflected CRC-32 of an encoded checkpoint payload.
 * @param[in] bytes Raw payload bytes, including embedded nulls.
 * @return CRC with polynomial 0xedb88320 and complemented initial/final accumulator.
 * @note Allocation-free and read-only; detects corruption, not authenticity or model
 * compatibility.
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
 * @brief Read one complete nonempty router checkpoint under the 64 MiB internal size bound.
 * @param[in] path Readable binary file path whose contents remain stable during this call.
 * @return Owned exact file bytes.
 * @throws std::runtime_error On missing/empty/oversized file, failed read or trailing growth.
 * @note Allocates the file buffer; no decode/profile/CRC check, owner mutation or clock advance.
 */
std::string readFile(const std::string& path) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size == 0 || size > maxBytes)
        throw std::runtime_error("Router checkpoint is missing or exceeds its size limit.");
    std::ifstream in(path, std::ios::binary);
    std::string bytes(size, '\0');
    in.read(bytes.data(), bytes.size());
    if (!in || in.peek() != std::char_traits<char>::eof())
        throw std::runtime_error("Router checkpoint read failed.");
    return bytes;
}
/**
 * @brief Publish encoded bytes using an exclusive temporary file and final filesystem rename.
 * @param[in] path Destination with existing writable parent; caller serializes writers.
 * @param[in] bytes Complete already validated encoded checkpoint envelope.
 * @param[in] expectedMagic Nonnull expected destination record magic; an existing file must
 * decode this prefix before replacement is allowed.
 * @throws std::runtime_error On unrelated/invalid destination, create/write/close or rename
 * failure; filesystem/archive/allocation operations may also throw.
 * @note Uses process-unique .router-part-N suffix and exclusive creation. Removes temporary file
 * on reported write/rename failure where removal succeeds; exceptional filesystem failure can
 * retain it. No fsync/durable-storage guarantee or complete validation of existing payload. No
 * model mutation.
 */
void publish(const std::string& path, const std::string& bytes, const char* expectedMagic) {
    if (std::filesystem::exists(path)) {
        Archive old(readFile(path));
        std::string magic;
        old(magic);
        if (magic != expectedMagic)
            throw std::runtime_error("Router checkpoint destination is an unrelated file.");
    }
    static std::atomic<uint64_t> serial{0};
    const auto part = path + ".router-part-" + std::to_string(++serial);
    FILE* file = std::fopen(part.c_str(), "wbx");
    if (!file)
        throw std::runtime_error("Cannot create router checkpoint publication file.");
    const bool written = std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
    const bool closed = std::fclose(file) == 0;
    if (!written || !closed) {
        std::filesystem::remove(part);
        throw std::runtime_error("Router checkpoint write failed.");
    }
    std::error_code ec;
    std::filesystem::rename(part, path, ec);
    if (ec) {
        std::filesystem::remove(part);
        throw std::runtime_error("Cannot publish router checkpoint: " + ec.message());
    }
}
/**
 * @brief Compute immutable authored boundary identity with evaluated series/curve ordinates
 * removed.
 * @param[in] boundary Owned prepared boundary copy; zeroes dynamic head/flow values on this
 * copy.
 * @param[in] tables Stable prepared referenced table store for immutable identity serialization.
 * @return Binary boundary model identity from RuntimeWaterCheckpoint.
 * @note Requires consistent descriptor extents; no live owner/table mutation or clock advance.
 * Allocation/archive may throw. Runtime overrides must be cleared by caller when fallback is
 * desired.
 */
std::string rk2AuthoredBoundaryModel(BoundaryData boundary, const TableData& tables) {
    for (int slot = 0; slot < boundary.size(); ++slot) {
        if (boundary.edge_bc_tseries[slot] >= 0)
            boundary.edge_bc_head[slot] = 0;
        if (boundary.edge_bc_flow_tseries[slot] >= 0 || boundary.edge_bc_rating_curve[slot] >= 0)
            boundary.edge_bc_flow[slot] = 0;
    }
    return RuntimeWaterCheckpoint::boundaryModel(boundary, &tables);
}
/**
 * @brief Archive the explicit 2D water balance and solver report counters.
 * @param[in,out] a Sequential archive; reads progressively replace fields.
 * @param[in,out] m Candidate balance, with water m3 and solver time s in prepared internal
 * convention.
 * @note Includes tier-cell counters, not arbitrary whole-engine balances. No admissibility
 * validation, physics/calendar update or transactional read; caller stages and validates before
 * install.
 */
void massBalance(Archive& a, SimulationContext::MassBalance2D& m) {
    a(m.init_storage, m.final_storage, m.external_in, m.external_out, m.rainfall_in,
      m.coupling_1d_to_2d_in, m.coupling_2d_to_1d_out, m.outfall_in, m.outfall_out, m.boundary_in,
      m.boundary_out, m.evap_out, m.infil_out, m.infil_to_aquifer, m.infil_aquifer_pending,
      m.aquifer_in, m.active, m.solver_nsteps, m.solver_nrhs, m.solver_avg_h, m.solver_last_h,
      m.solver_active_min, m.solver_active_mean, m.solver_active_max, m.solver_n_tiers);
    for (auto& v : m.solver_tier_cells)
        a(v);
}
// Explicit router prognostic fields; scratch, model geometry and provider
// resources are excluded. The joint profile pairs live 1D stores separately.
#define ROUTER_FIELDS(X)                                                                           \
    X(infil_elapsed_)                                                                              \
    X(infil_cum_applied_)                                                                          \
    X(coupling_cum_applied_)                                                                       \
    X(rain_cum_)                                                                                   \
    X(co_refresh_elapsed_)                                                                         \
    X(co_render_elapsed_)                                                                          \
    X(render_dirty_)                                                                               \
    X(co_forcing_elapsed_)                                                                         \
    X(co_forcing_first_)                                                                           \
    X(sim_time_)                                                                                   \
    X(pending_dt_)                                                                                 \
    X(report_old_depth_)                                                                           \
    X(report_old_head_)                                                                            \
    X(report_window_old_ms_)                                                                       \
    X(report_window_new_ms_)                                                                       \
    X(report_old_valid_)                                                                           \
    X(last_t_)                                                                                     \
    X(boundary_names_resolved_)                                                                    \
    X(bc_nonwall_dirty_)                                                                           \
    X(rain_gage_last_)                                                                             \
    X(gage_rate_)                                                                                  \
    X(gage_cum_)                                                                                   \
    X(gage_window_depth_)                                                                          \
    X(gage_window_rate_)                                                                           \
    X(gage_since_)                                                                                 \
    X(gage_report_depth_)                                                                          \
    X(report_rain_cum_)                                                                            \
    X(report_rain_ms_)
}

struct SurfaceRouter2D::RestartState {
#define DECLARE(v) decltype(SurfaceRouter2D::v) v;
    ROUTER_FIELDS(DECLARE)
#undef DECLARE
    double current_time, current_date, elapsed_ms, old_elapsed_ms, next_report_ms, evap_rate;
    std::vector<double> rainfall;
    SimulationContext::MassBalance2D mb;
    double delivery = 0;
    bool snow_profile = false;
    double snow_delivery_adjustment = 0;
    OutfallExchange exchange;
    std::vector<double> window_outfall_accum_, limits;
    bool outfall_state_changed_ = false;
    bool runtime_water = false, runtime_used = false, boundary_water = false;
    std::vector<int> boundary_slots;
    double boundary_in = 0, boundary_out = 0;
    OutfallData feedback;
    bool rk2 = false;
    int rk2_species = 0;
    BoundaryData rk2_boundary;
    std::map<int, std::vector<double>> rk2_boundary_quality;
    RuntimeSources rk2_surface, rk2_ground;
    RuntimeForcings rk2_forcing;
    /**
     * @brief Copy explicit router/context checkpoint fields into a private restart candidate.
     * @param[in] r Stable prepared router with matching optional runtime/joint/RK2 owner
     * profile.
     * @param[in] c Exact attached context supplying clocks, weather, water balance and native
     * feedback.
     * @note Copies gage/report/source fields, accepted snow adjustment, joint outfall
     * reservations and runtime model descriptors. Allocations may throw; live owner state is not
     * changed.
     */
    RestartState(const SurfaceRouter2D& r, const SimulationContext& c) {
#define COPY(v) v = r.v;
        ROUTER_FIELDS(COPY)
#undef COPY
        snow_profile = r.state_.snow.active();
        for (const auto& snow : r.state_.snow.ledgers())
            snow_delivery_adjustment += snow.liquid - snow.raw_precipitation;
        exchange = r.outfall_exchange_;
        window_outfall_accum_ = r.window_outfall_accum_;
        outfall_state_changed_ = r.outfall_state_changed_;
        limits = c.surface_outfall_link_limit;
        feedback.head_2d = c.node_subtypes.outfalls.head_2d;
        feedback.ramp_2d = c.node_subtypes.outfalls.ramp_2d;
        current_time = c.current_time;
        current_date = c.current_date;
        elapsed_ms = c.elapsed_ms;
        old_elapsed_ms = c.old_elapsed_ms;
        next_report_ms = c.next_report_ms;
        evap_rate = c.climate_state.evap_rate;
        rainfall = c.gages.rainfall;
        mb = c.mass_balance_2d;
        delivery = c.coupling_delivery_remaining;
        runtime_water = r.runtimeWaterCheckpointing();
        runtime_used = c.runtime_coupling_used;
        rk2 = r.standaloneRk2Checkpointing();
        rk2_species = r.state_.transport.n_species;
        if (rk2) {
            rk2_surface = r.state_.runtime_sources;
            rk2_ground = r.subsurface_.runtimeSources();
            rk2_forcing = r.state_.runtime_forcings;
            rk2_boundary = r.boundary_;
            rk2_boundary_quality = r.state_.transport.runtime_bc_conc;
        }
        boundary_water = r.runtimeBoundaryCheckpointing() || rk2;
        boundary_slots = r.bc_nonwall_slots_;
        boundary_in = r.prev_boundary_in_;
        boundary_out = r.prev_boundary_out_;
    }
    /**
     * @brief Serialize or progressively decode explicit router candidate clocks, weather and
     * receipts.
     * @param[in,out] a Sequential archive; reads alter this private candidate.
     * @note Profile flags established from the prepared model gate snow/runtime/boundary/RK2
     * records. No full qualification or canonical install occurs; archive/allocation can throw
     * after partial decode.
     */
    void archive(Archive& a) {
#define FIELD(v) a(v);
        ROUTER_FIELDS(FIELD)
#undef FIELD
        a(current_time, current_date, elapsed_ms, old_elapsed_ms, next_report_ms, evap_rate,
          rainfall);
        massBalance(a, mb);
        if (snow_profile)
            a(snow_delivery_adjustment);
        if (runtime_water)
            a(runtime_used);
        if (boundary_water)
            a(boundary_slots, boundary_in, boundary_out);
        if (rk2)
            rk2ManagedRuntimeModel(a, rk2_surface, rk2_ground, rk2_forcing, rk2_species,
                                   &rk2_boundary, &rk2_boundary_quality);
    }
    /**
     * @brief Archive joint outfall batch, reservation, donor limit and native tailwater feedback
     * state.
     * @param[in,out] a Sequential archive, reading progressively mutates the private candidate.
     * @note Requires prepared matching joint model extents. Validation/install are separate; no
     * physical transfer or clock advance, and archive/allocation failures are not transactional
     * on this candidate.
     */
    void archiveJoint(Archive& a) {
        a(window_outfall_accum_, outfall_state_changed_);
        exchange.checkpoint(a);
        a.fixed(limits, feedback.head_2d, feedback.ramp_2d);
    }
    /**
     * @brief Validate staged router extents, clocks, report/weather integrals and qualified
     * water receipts.
     * @param[in] r Stable prepared live router model supplying mesh/cadence/owner identity.
     * @param[in] c Exact attached prepared context supplying gage/native/outfall model and
     * initial storage.
     * @throws std::runtime_error On shape, nonfinite/negative water, completed/report clock,
     * unsupported exchange, boundary slots, joint delivery/outfall feedback or pre-finalization
     * statistics mismatch.
     * @note Read-only candidate/model check; delegates outfall qualification and uses
     * tolerance-based SI water balance comparisons. No physical update, canonical mutation or
     * repair; checked access may throw.
     */
    void validate(const SurfaceRouter2D& r, const SimulationContext& c) const {
        const auto n = static_cast<std::size_t>(r.mesh_.n_cells()),
                   ng = static_cast<std::size_t>(c.n_gages());
        for (const auto* row :
             {&coupling_cum_applied_, &rain_cum_, &report_old_depth_, &report_old_head_})
            if (row->size() != n)
                throw std::runtime_error("Router checkpoint cell shape differs.");
        const bool tracks_infiltration = r.infil_.active() || r.common_stage_;
        if ((tracks_infiltration && infil_cum_applied_.size() != n) ||
            (!tracks_infiltration && !infil_cum_applied_.empty()))
            throw std::runtime_error(
                "Router checkpoint infiltration shape differs from its resolved model.");
        if (report_rain_cum_.size() != (report_rain_ms_ >= 0 ? n : 0) || rainfall.size() != ng)
            throw std::runtime_error("Router checkpoint report/weather shape differs.");
        const auto booked = gage_rate_.size();
        if (booked != 0 && booked != ng)
            throw std::runtime_error("Router checkpoint gage shape differs.");
        for (const auto* row : {&gage_cum_, &gage_window_depth_, &gage_report_depth_})
            if (row->size() != booked)
                throw std::runtime_error("Router checkpoint gage integral shape differs.");
        for (const auto* row : {&gage_window_rate_, &rain_gage_last_})
            if (row->size() != 0 && row->size() != ng)
                throw std::runtime_error("Router checkpoint gage rate shape differs.");
        for (const auto* row :
             {&infil_cum_applied_, &coupling_cum_applied_, &rain_cum_, &report_old_depth_,
              &report_old_head_, &rain_gage_last_, &gage_rate_, &gage_cum_, &gage_window_depth_,
              &gage_window_rate_, &gage_report_depth_, &report_rain_cum_, &rainfall})
            for (double v : *row)
                if (!std::isfinite(v))
                    throw std::runtime_error("Nonfinite router checkpoint field.");
        for (const auto* row : {&infil_cum_applied_, &rain_cum_, &report_old_depth_, &gage_cum_,
                                &gage_window_depth_, &gage_report_depth_, &report_rain_cum_})
            for (double v : *row)
                if (v < 0)
                    throw std::runtime_error("Negative router checkpoint water/integral.");
        for (double v : coupling_cum_applied_)
            if (!r.joint_hydraulic_ && v != 0)
                throw std::runtime_error("Unqualified router checkpoint coupling.");
        for (double v : {infil_elapsed_, co_refresh_elapsed_, co_render_elapsed_,
                         co_forcing_elapsed_, sim_time_, pending_dt_, report_window_old_ms_,
                         report_window_new_ms_, last_t_, gage_since_, current_time, current_date,
                         elapsed_ms, old_elapsed_ms, next_report_ms, evap_rate, report_rain_ms_})
            if (!std::isfinite(v))
                throw std::runtime_error("Nonfinite router checkpoint clock.");
        if (sim_time_ < 0 || pending_dt_ < 0 || infil_elapsed_ < 0 || co_refresh_elapsed_ < 0 ||
            co_render_elapsed_ < 0 || co_forcing_elapsed_ < 0 || co_refresh_elapsed_ >= 30 ||
            co_forcing_elapsed_ >= 30 ||
            co_render_elapsed_ >= (c.options.report_step > 0 ? c.options.report_step : 30) ||
            (!render_dirty_ && co_render_elapsed_ != 0) ||
            infil_elapsed_ >= r.infil_.stepSeconds() ||
            pending_dt_ >= r.couplingSyncSpan(c, r.options_) || last_t_ < 0 ||
            last_t_ > current_time || gage_since_ < 0 ||
            !close(current_time, sim_time_ + pending_dt_) ||
            !close(elapsed_ms, 1000 * current_time) || old_elapsed_ms < 0 ||
            old_elapsed_ms > elapsed_ms || next_report_ms <= 0 ||
            current_date != datetime::addSeconds(c.options.start_date, current_time) ||
            report_window_old_ms_ < 0 || report_window_old_ms_ > report_window_new_ms_ ||
            !close(report_window_new_ms_, 1000 * sim_time_) || report_rain_ms_ < -1 ||
            report_rain_ms_ > report_window_new_ms_ ||
            (!report_old_valid_ && report_window_old_ms_ != 0) ||
            (sim_time_ > 0 && co_forcing_first_))
            throw std::runtime_error("Invalid router checkpoint clocks/report window.");
        double rain = 0, infil = 0;
        for (std::size_t i = 0; i < n; ++i) {
            rain += rain_cum_[i];
            if (!infil_cum_applied_.empty())
                infil += infil_cum_applied_[i] * r.mesh_.tri_area[i];
        }
        // Raw precipitation is reported separately from snow liquid delivery.
        if (!std::isfinite(snow_delivery_adjustment))
            throw std::runtime_error("Invalid snow checkpoint delivery.");
        rain += snow_delivery_adjustment;
        for (double v : {mb.init_storage, mb.rainfall_in, mb.evap_out, mb.infil_out, mb.aquifer_in,
                         mb.external_in, mb.external_out, mb.boundary_in, mb.boundary_out})
            if (!std::isfinite(v) || v < 0)
                throw std::runtime_error("Invalid router checkpoint mass balance.");
        // Incremental outfall accounting can leave an ulp below zero when all
        // cells are empty. Loaded cell water stays nonnegative and the CPU
        // transaction separately matches its sum to this diagnostic total.
        if (!std::isfinite(mb.final_storage) || mb.final_storage < -1e-12)
            throw std::runtime_error("Invalid router checkpoint final storage.");
        if (!mb.active || !close(rain, mb.rainfall_in) || !close(infil, mb.infil_out) ||
            mb.init_storage != c.mass_balance_2d.init_storage ||
            (!r.runtimeWaterCheckpointing() && (mb.external_in != 0 || mb.external_out != 0)) ||
            (!r.joint_hydraulic_ &&
             (mb.coupling_1d_to_2d_in != 0 || mb.coupling_2d_to_1d_out != 0)) ||
            (!r.joint_hydraulic_ && (mb.outfall_in != 0 || mb.outfall_out != 0)) ||
            (!boundary_water && (mb.boundary_in != 0 || mb.boundary_out != 0)) ||
            mb.infil_to_aquifer != 0 || mb.infil_aquifer_pending != 0 || mb.solver_nsteps != -1 ||
            mb.solver_nrhs != 0 || mb.solver_avg_h != 0 || mb.solver_last_h != 0 ||
            mb.solver_active_min != -1 || mb.solver_active_mean != -1 ||
            mb.solver_active_max != -1 || mb.solver_n_tiers != 0)
            throw std::runtime_error(
                "Router checkpoint ledgers differ or include unqualified exchanges.");
        if (boundary_water) {
            if (!close(boundary_in, mb.boundary_in) || !close(boundary_out, mb.boundary_out) ||
                !std::is_sorted(boundary_slots.begin(), boundary_slots.end()) ||
                std::adjacent_find(boundary_slots.begin(), boundary_slots.end()) !=
                    boundary_slots.end())
                throw std::runtime_error("Router boundary checkpoint counters or index differ.");
            for (int slot : boundary_slots)
                if (slot < 0 || slot >= r.boundary_.size() ||
                    MeshData::slot_local(slot) >=
                        r.mesh_.cell_vertex_count(MeshData::slot_cell(slot)) ||
                    r.mesh_.cell_neighbour(MeshData::slot_cell(slot), MeshData::slot_local(slot)) >=
                        0)
                    throw std::runtime_error(
                        "Router boundary checkpoint index is not an exterior edge.");
        }
        if (r.joint_hydraulic_) {
            if (window_outfall_accum_.size() != r.coupling_points_.size() ||
                (pending_dt_ == 0 && outfall_state_changed_) ||
                feedback.head_2d.size() != c.node_subtypes.outfalls.head_2d.size() ||
                feedback.ramp_2d.size() != feedback.head_2d.size())
                throw std::runtime_error("Joint outfall checkpoint batch/feedback shape differs.");
            if (!exchange.points_.empty() && ((current_time == 0) != (exchange.prepared_dt_ == 0) ||
                                              exchange.prepared_dt_ > c.options.routing_step))
                throw std::runtime_error("Invalid joint outfall reservation duration.");
            exchange.validateCheckpoint(r.options_, limits, window_outfall_accum_,
                                        outfall_state_changed_, {mb.outfall_in, mb.outfall_out});
            for (const auto& p : exchange.points_) {
                const int row = c.node_subtypes.outfall_row(p.node);
                if (row < 0 || std::size_t(row) >= feedback.head_2d.size() ||
                    std::isinf(feedback.head_2d[row]) ||
                    (exchange.prepared_dt_ > 0 && !std::isfinite(feedback.head_2d[row])) ||
                    !std::isfinite(feedback.ramp_2d[row]) || feedback.ramp_2d[row] < 0 ||
                    feedback.ramp_2d[row] > 1)
                    throw std::runtime_error("Invalid joint outfall tailwater feedback.");
            }
            double net = 0;
            for (double v : coupling_cum_applied_)
                net += v;
            if (!std::isfinite(net) || !std::isfinite(delivery) || delivery < 0 ||
                delivery > r.couplingSyncSpan(c, r.options_) + c.options.routing_step ||
                !std::isfinite(mb.coupling_1d_to_2d_in) || mb.coupling_1d_to_2d_in < 0 ||
                !std::isfinite(mb.coupling_2d_to_1d_out) || mb.coupling_2d_to_1d_out < 0 ||
                !std::isfinite(mb.outfall_in) || mb.outfall_in < 0 ||
                !std::isfinite(mb.outfall_out) || mb.outfall_out < 0 ||
                std::abs(net - (mb.coupling_1d_to_2d_in - mb.coupling_2d_to_1d_out + mb.outfall_in -
                                mb.outfall_out)) >
                    1e-12 * std::max({1.0, std::abs(net), mb.coupling_1d_to_2d_in,
                                      mb.coupling_2d_to_1d_out, mb.outfall_in, mb.outfall_out}))
                throw std::runtime_error(
                    "Joint checkpoint delivery or surface exchange ledger differs.");
        }
        for (long v : mb.solver_tier_cells)
            if (v != 0)
                throw std::runtime_error("Router checkpoint has finalized statistics.");
    }
    /**
     * @brief Adopt already validated router/context fields by nonthrowing moves.
     * @param[in,out] r Exact matching prepared router; receives prognostic/report fields and
     * ready flag.
     * @param[in,out] c Exact attached context; receives saved clocks/weather/balance and
     * joint/runtime feedback where enabled. Underlying CPU/source/hydraulic owners must have
     * been staged/installed.
     * @note Consumes this candidate, invalidates prior vector views and performs no validation,
     * physics replay or allocation. Requires exclusive ownership and prior complete
     * cross-component qualification.
     */
    void install(SurfaceRouter2D& r, SimulationContext& c) noexcept {
#define MOVE(v)                                                                                    \
    static_assert(std::is_nothrow_move_assignable_v<decltype(v)>);                                 \
    r.v = std::move(v);
        ROUTER_FIELDS(MOVE)
#undef MOVE
        c.current_time = current_time;
        c.current_date = current_date;
        c.elapsed_ms = elapsed_ms;
        c.old_elapsed_ms = old_elapsed_ms;
        c.next_report_ms = next_report_ms;
        c.climate_state.evap_rate = evap_rate;
        c.gages.rainfall = std::move(rainfall);
        c.mass_balance_2d = mb;
        if (runtime_water)
            c.runtime_coupling_used = runtime_used;
        if (boundary_water) {
            r.bc_nonwall_slots_ = std::move(boundary_slots);
            r.prev_boundary_in_ = boundary_in;
            r.prev_boundary_out_ = boundary_out;
        }
        if (r.joint_hydraulic_) {
            static_assert(std::is_nothrow_move_assignable_v<OutfallExchange>);
            c.coupling_delivery_remaining = delivery;
            c.surface_outfall_link_limit = std::move(limits);
            r.outfall_exchange_ = std::move(exchange);
            r.window_outfall_accum_ = std::move(window_outfall_accum_);
            r.outfall_state_changed_ = outfall_state_changed_;
        }
        r.source_boundary_ready_ = true;
    }
};
#undef ROUTER_FIELDS

/**
 * @brief Encode immutable router report/weather, coupling policy and authored boundary identity.
 * @param[in] ctx Stable prepared options, gage locations/names and boundary tables.
 * @return Binary model identity, extended by joint coupling structures and RK2 authored boundary
 * fallback when those profiles are used. No mutable state checkpoint by itself.
 * @note Serializes copies; source runtime overrides are cleared only on a private boundary copy.
 * Allocation/archive can throw. Requires matching stable initialized views; no physical/calendar
 * change.
 */
std::string SurfaceRouter2D::sourceRouterModel(const SimulationContext& ctx) const {
    Archive a;
    auto o = ctx.options;
    auto x = ctx.spatial.gage_x, y = ctx.spatial.gage_y;
    auto step = infil_.stepSeconds();
    auto sync = couplingSyncSpan(ctx, options_);
    a(o.start_date, o.total_duration_ms, o.flow_units, o.routing_step, o.wet_step, o.report_step,
      o.ignore_quality, o.ignore_snow_melt, o.ignore_2d, o.water_age, o.heat_transport, x, y, step,
      sync);
    if (options_.theta_reference_step > 0.0) {
        std::string profile = "physical-time momentum smoothing v1";
        double step = options_.theta_reference_step;
        a(profile, step);
    }
    for (int g = 0; g < ctx.n_gages(); ++g) {
        auto name = ctx.gage_names.name_of(g);
        a(name);
    }
    // Pond / equalisation options join only when set (legacy identity unchanged).
    if (options_.coupling_pond_mode != CouplingPondMode2D::FOOTPRINT ||
        options_.coupling_rim_band >= 0 || options_.coupling_pond_relax != 0 ||
        options_.coupling_equalize >= 0) {
        auto pm = options_.coupling_pond_mode;
        auto band = options_.coupling_rim_band;
        auto relax = options_.coupling_pond_relax;
        auto eq = options_.coupling_equalize;
        a(pm, band, relax, eq);
    }
    if (joint_hydraulic_) {
        auto points = coupling_points_;
        a.sequence(points, [&](auto& p) {
            a(p.cell_idx, p.vertex_idx, p.node_idx, p.cd, p.area, p.is_outfall, p.has_flap_gate,
              p.area_authored);
            // WEIR_ORIFICE structure joins only when used: legacy identities are unchanged.
            if (p.law)
                a(p.law, p.geo.z_crest, p.geo.l_weir, p.geo.a_orif, p.coef.c_w, p.coef.c_sw,
                  p.coef.c_oi, p.coef.c_oo, p.coef.h_eps);
            if (p.spread > 0 || p.tw_relax > 0)
                a(p.spread, p.tw_relax);
        });
    }
    if (options_.reconstruction_order == 2) {
        auto boundary = boundary_;
        std::vector<int> overridden;
        for (const auto& entry : boundary.runtime_original)
            overridden.push_back(entry.first);
        for (int slot : overridden)
            boundary.clearRuntime(slot);
        std::string profile = "managed RK2 authored surface boundary model v1",
                    identity = rk2AuthoredBoundaryModel(boundary, ctx.tables);
        a(profile, identity);
    }
    return a.bytes();
}
/**
 * @brief Test whether the router has attached standalone second-order checkpoint ownership.
 * @return True for nonnull source context, reconstruction order 2 and no common stage.
 * @note Allocation-free flag/identity query; does not qualify current physics or receipt
 * boundary.
 */
bool SurfaceRouter2D::standaloneRk2Checkpointing() const noexcept {
    return source_context_ && options_.reconstruction_order == 2 && !common_stage_;
}
/**
 * @brief Qualify the completed managed standalone coupled RK2 router boundary.
 * @param[in] ctx Exact attached live context with matching elapsed source/router clock and
 * ledgers.
 * @return Empty on success, otherwise CPU/global RK2, shared-owner, model, forcing, replay-ID or
 * accepted water receipt refusal.
 * @note Requires no network/catchment/landuse/contact owners and the prepared joint hydraulic
 * session identity. Delegates full CPU profile and replay checks; does not settle, flush or
 * repair state. Read-only but allocation/checked lookups can throw. Exclusive stable owner views
 * required.
 */
std::string SurfaceRouter2D::standaloneRk2Boundary(const SimulationContext& ctx) const {
    const auto* cpu = dynamic_cast<const ExplicitInertialSolver*>(solver_.get());
    if (!active_ || !cpu || options_.momentum != Momentum2D::FULL_SWE ||
        options_.reconstruction_order != 2 || &ctx != source_context_ || common_stage_ ||
        !joint_hydraulic_ || channel_ground_.active() || ctx.n_subcatches() != 0 ||
        !aquiferConfig().surface_owners.empty() || !coupling_points_.empty() ||
        !pond_track_ptr_.empty() || !subcatch_recharge_.empty() || !cell_subcatch_.empty() ||
        surface_quality_.active() || !aquifer_link_shares_.empty() || pending_dt_ < 0 ||
        !close(ctx.current_time, sim_time_ + pending_dt_) ||
        !close(cpu->completedSourceTime(), sim_time_))
        return "Managed RK2 requires its completed CPU aquifer/router boundary without shared network or catchment owners.";
    if (auto error = cpu->rk2CheckpointProfile(); !error.empty())
        return error;
    if (auto error = validateRk2ReplayIds(state_.runtime_sources, subsurface_.runtimeSources(),
                                          state_.runtime_forcings, sim_time_);
        !error.empty())
        return error;
    for (const auto* values : {&state_.rainfall, &state_.evap_rate})
        for (double value : *values)
            if (!std::isfinite(value) || value < 0)
                return "Managed RK2 native forcing is inadmissible.";
    double incoming = 0, outgoing = 0;
    for (const auto& row : state_.runtime_sources.rows) {
        incoming += row.receipt.water_in;
        outgoing += row.receipt.water_out;
    }
    if (!close(incoming, ctx.mass_balance_2d.external_in) ||
        !close(outgoing, ctx.mass_balance_2d.external_out) ||
        !close(ctx.mass_balance_2d.final_storage, totalVolume()))
        return "Managed RK2 accepted surface receipts differ from the router.";
    if (source_router_model_ != sourceRouterModel(ctx))
        return "Managed RK2 router project changed after attachment.";
    return {};
}
/**
 * @brief Attach fresh managed standalone RK2 checkpoint context and immutable router identity.
 * @param[in,out] ctx Exact prepared initial context, current_time zero and matching hydraulic
 * session.
 * @note Requires no previous source/common attachment, zero numerical/pending time and
 * pre-attached matching joint hydraulic owner. Constructs model, then validates completed
 * profile; on validation refusal clears the new context/model and throws std::invalid_argument.
 * Allocation/delegated exceptions can propagate. Sets ready flag on success; no physics/calendar
 * advance or ownership transfer. Borrowed context/hydraulic/model owners must remain alive and
 * stable through use.
 */
void SurfaceRouter2D::attachStandaloneRk2Checkpoint(SimulationContext& ctx) {
    if (source_context_ || common_stage_ || !joint_hydraulic_ ||
        joint_hydraulic_->checkpoint_context_ != &ctx || sim_time_ != 0 || pending_dt_ != 0 ||
        ctx.current_time != 0)
        throw std::invalid_argument(
            "Attach the standalone RK2 session at its fresh initial boundary.");
    auto model = sourceRouterModel(ctx);
    source_context_ = &ctx;
    source_router_model_ = std::move(model);
    if (auto error = standaloneRk2Boundary(ctx); !error.empty()) {
        source_context_ = nullptr;
        source_router_model_.clear();
        throw std::invalid_argument(error);
    }
    source_boundary_ready_ = true;
}
/**
 * @brief Validate a completed attached common-source or managed RK2 router checkpoint boundary.
 * @param[in] ctx Exact attached context with consistent source/router/native receipts and
 * immutable model.
 * @return Empty on qualification, otherwise unsupported live owner/profile,
 * boundary/quality/receipt, clock/model or provider-use diagnostic.
 * @note Standalone RK2 delegates. First-order requires all-cell matching common stage and source
 * ready flag; rejects uncarried tracking ponds, conduit/catchment/landuse/runtime providers and
 * unsettled outfall reservations. Checks native node-cache ownership, source capacities, CPU
 * profile, gross boundary/external water, completed clocks and surface/source water totals. No
 * repair/flush, serialization or physics advance; allocation/checked lookup may throw under
 * inconsistent layout.
 */
std::string SurfaceRouter2D::sourceBoundary(const SimulationContext& ctx) const {
    if (standaloneRk2Checkpointing())
        return standaloneRk2Boundary(ctx);
    if (!active_ || !common_stage_ || !source_boundary_ready_ || &ctx != source_context_ ||
        !common_stage_->matches(&mesh_, &state_, &subsurface_) ||
        int(common_stage_->cells().size()) != mesh_.n_cells())
        return "Router checkpoint requires an accepted attached all-cell source boundary.";
    // TRACKING relaxes each coupled pond area in time; the checkpoint does not
    // carry that state yet, so refuse rather than restore a different pond.
    if (!pond_track_ptr_.empty())
        return "Router checkpoint does not carry COUPLING_POND_MODE TRACKING pond state.";
    if ((!joint_hydraulic_ &&
         (ctx.n_nodes() != 0 || ctx.n_links() != 0 || !coupling_points_.empty())) ||
        !aquifer_link_shares_.empty() || !subcatch_recharge_.empty() || !cell_subcatch_.empty() ||
        surface_quality_.active() || (runtimeActive() && !runtimeWaterCheckpointing()) ||
        !ramp_prev_head_.empty() ||
        (!joint_hydraulic_ && (outfall_state_changed_ || !window_outfall_accum_.empty())))
        return "Router checkpoint live 1D, land-use quality and external providers are not qualified.";
    if (joint_hydraulic_) {
        const bool standalone_local_bed_quality =
            common_stage_ && common_stage_->standaloneLocalBed();
        const bool mesh_only_ground_quality =
            common_stage_ && common_stage_->meshOnlyGroundQuality();
        const bool network_quality =
            detail::checkpointNetworkPollutants(ctx) && common_stage_->meshOnly();
        if (network_quality &&
            (state_.node_row_conc != &node_row_conc_ ||
             node_row_conc_.size() != std::size_t(ctx.n_nodes()) * ctx.n_pollutants()))
            return "Joint checkpoint requires the owned native pollutant row publisher.";
        if (state_.nodes_1d != ((node_coupling_points_.empty() && subsurface_.nodeBeds().empty())
                                    ? nullptr
                                    : &ctx.nodes) ||
            state_.node_coupling !=
                (node_coupling_points_.empty() ? nullptr : &node_coupling_points_) ||
            ((coupling_points_.empty() && subsurface_.nodeBeds().empty() &&
              !channel_ground_.active()) &&
             (ctx.n_nodes() != 0 || ctx.n_links() != 0)) ||
            (ctx.n_pollutants() != 0 && !standalone_local_bed_quality &&
             !mesh_only_ground_quality && !network_quality))
            return "Joint router checkpoint coupling/receiver layout differs.";
        if (outfall_exchange_.prepared_)
            return "Joint checkpoint has an unconsumed outfall reservation.";
    }
    const auto& infiltration_rows = infil_.resolvedRows();
    if (!infiltration_rows.empty() && infiltration_rows.size() != mesh_.tri_area.size())
        return "Router checkpoint requires complete aquifer-owned infiltration rows.";
    for (int i = 0; i < mesh_.n_cells(); ++i)
        if (state_.coupling_flux[i] != 0 || state_.coupling_forced[i] != 0 ||
            (!infiltration_rows.empty() && infiltration_rows[i].has_method))
            return "Router checkpoint requires aquifer-owned cells without external coupling donors.";
    const auto* cpu = dynamic_cast<ExplicitInertialSolver*>(solver_.get());
    if (!cpu)
        return "Router checkpoint requires the first-order CPU marcher.";
    if (joint_hydraulic_ && joint_hydraulic_->checkpoint_owned_session_ && cpu->source_history_)
        return "Engine checkpoint does not recover a source history recorder.";
    const auto error = cpu->checkpointBoundary();
    if (!error.empty())
        return error;
    double incoming = 0, outgoing = 0;
    for (const auto& row : state_.runtime_sources.rows) {
        incoming += row.receipt.water_in;
        outgoing += row.receipt.water_out;
    }
    if (!close(incoming, ctx.mass_balance_2d.external_in) ||
        !close(outgoing, ctx.mass_balance_2d.external_out))
        return "Router/provider completed water receipts differ.";
    double boundary_in = 0, boundary_out = 0;
    std::vector<int> slots;
    for (const auto& [slot, receipt] : boundary_.receipts) {
        boundary_in += receipt.water_in;
        boundary_out += receipt.water_out;
    }
    for (int k = 0; k < boundary_.size(); ++k)
        if (boundary_.edge_bc_type[k] != int8_t(BoundaryType::WALL))
            slots.push_back(k);
    if (!close(boundary_in, ctx.mass_balance_2d.boundary_in) ||
        !close(boundary_out, ctx.mass_balance_2d.boundary_out) ||
        (!bc_nonwall_dirty_ && slots != bc_nonwall_slots_))
        return "Router completed boundary receipts or index differ.";
    if (cpu->completedSourceTime() != sim_time_)
        return "Router/source completed clocks differ.";
    const auto current_model = sourceRouterModel(ctx) + channel_ground_.modelIdentity();
    if (source_router_model_ != current_model)
        return "Router checkpoint model changed after attachment.";
    if (!close(ctx.mass_balance_2d.rainfall_in, common_stage_->meshRain()) ||
        !close(ctx.mass_balance_2d.evap_out, common_stage_->meshEvaporation()) ||
        !close(ctx.mass_balance_2d.aquifer_in, common_stage_->meshReturn()) ||
        !close(ctx.mass_balance_2d.final_storage, totalVolume()))
        return "Router/source completed ledgers differ.";
    return {};
}
/**
 * @brief Attach a borrowed all-cell source stage to a fresh first-order CPU router.
 * @param[in,out] ctx Exact matching initial context without live network coupling.
 * @param[in,out] stage Prepared matching mesh/surface/subsurface owner spanning all cells;
 * borrowed for later routing/checkpoint and must outlive attachment with stable identities.
 * @note Delegates attachment validation/setup with no hydraulic joint owner. Does not activate
 * authored recharge ownership or advance physics. Throws std::invalid_argument on profile/phase
 * refusal and may allocate after partial preparation; exclusive fresh lifecycle access required.
 */
void SurfaceRouter2D::setCommonSourceStage(SimulationContext& ctx, CommonSourceStage& stage) {
    attachCommonSourceStage(ctx, stage, nullptr);
}
/**
 * @brief Attach matching first-order source and tracked native hydraulic owners for joint
 * checkpoints.
 * @param[in,out] ctx Exact prepared initial context shared by both routers and LID setup.
 * @param[in,out] stage Borrowed matching all-cell common-source stage, stable/alive through
 * routing.
 * @param[in,out] hydraulic Borrowed initialized tracked hydraulic owner sharing ctx; must remain
 * alive.
 * @note Delegates joint ownership, quality/thermal, LID, boundary and coupling qualification.
 * Disallows unsupported external head ramps/conduit owners. No physics advance; refusal throws,
 * allocation or delegated setup may leave partial preparation. Call serially after native
 * routers/LID initialize.
 */
void SurfaceRouter2D::setJointSourceStage(SimulationContext& ctx, CommonSourceStage& stage,
                                          Router& hydraulic) {
    attachCommonSourceStage(ctx, stage, &hydraulic);
}
/**
 * @brief Require the tracked native hydraulic solve to have accepted this surface interval.
 * @param[in] ctx Exact matching hydraulic context with current_time at interval start s.
 * @param[in] dt Positive accepted duration s, no greater than configured routing step.
 * @param[in] start Accepted interval start elapsed s.
 * @throws std::invalid_argument On joint context/finished-step/start/end/duration mismatch.
 * @note No check for no joint owner or standalone RK2 route; no mutation/allocation/clock
 * change.
 */
void SurfaceRouter2D::checkJointRoutingStep(const SimulationContext& ctx, double dt,
                                            double start) const {
    if (joint_hydraulic_ && !standaloneRk2Checkpointing() &&
        (joint_hydraulic_->checkpoint_context_ != &ctx ||
         !joint_hydraulic_->checkpoint_step_finished_ ||
         joint_hydraulic_->checkpoint_time_ != start ||
         joint_hydraulic_->checkpoint_step_end_ != start + dt || ctx.current_time != start ||
         dt > ctx.options.routing_step))
        throw std::invalid_argument(
            "Joint surface advance requires its completed hydraulic step and routing duration.");
}
/**
 * @brief Qualify and install borrowed source/router checkpoint ownership at fresh initial time.
 * @param[in,out] ctx Exact prepared context shared by mesh, source and optional hydraulic owner.
 * @param[in,out] stage Matching borrowed all-cell source stage and stable physical/native
 * owners.
 * @param[in,out] hydraulic Optional borrowed tracked hydraulic owner; null requires no live
 * network.
 * @note Requires active CPU, no prior common attachment/runtime providers/pending time, zero
 * native clock, no external infiltration/coupling donors, and supported all-cell
 * boundary/quality/LID/thermal profile. Serializes joint identity, seeds accepted infiltration
 * ledger, binds snow contacts then CPU stage, joint flag, context/model and ready boundary. No
 * ownership transfer or physics advance. Throws std::invalid_argument on refusal;
 * allocations/setup can throw after partial preparatory ledger/view changes. Every borrowed
 * owner must stay alive/identity-stable for routing/restoration.
 */
void SurfaceRouter2D::attachCommonSourceStage(SimulationContext& ctx, CommonSourceStage& stage,
                                              Router* hydraulic) {
    auto* cpu = dynamic_cast<ExplicitInertialSolver*>(solver_.get());
    if (common_stage_ || !active_ || !cpu || sim_time_ != 0 || pending_dt_ != 0 ||
        ctx.current_time != 0 || !stage.matches(&mesh_, &state_, &subsurface_) ||
        int(stage.cells().size()) != mesh_.n_cells() ||
        (!hydraulic && (ctx.n_nodes() != 0 || ctx.n_links() != 0 || !coupling_points_.empty())) ||
        !subcatch_recharge_.empty() ||
        (!infil_.resolvedRows().empty() && infil_.resolvedRows().size() != mesh_.tri_area.size()) ||
        !cell_subcatch_.empty() || surface_quality_.active() || runtimeActive())
        throw std::invalid_argument(
            "Router source attachment requires its initial CPU/all-cell profile and matching exchange scope.");
    if (hydraulic) {
        for (const auto& bed : subsurface_.nodeBeds())
            if (bed.exchange && bed.cell >= 0 &&
                (!lidnode::active(ctx, bed.node) ||
                 lidnode::bottomMode(ctx, bed.node) != LidNodeBottom::MeshAquifer))
                throw std::invalid_argument(
                    "Joint source recovery requires explicit LID ownership for active node beds.");
        std::string bytes;
        const auto error = hydraulic->checkpointBytes(ctx, bytes);
        const char* ramp = std::getenv("OPENSWMM_2D_HEAD_RAMP");
        if (!error.empty())
            throw std::invalid_argument(error);
        const auto& transport = state_.transport;
        const bool temperature_only = transport.active() && transport.n_species == 1 &&
                                      transport.n_pollut == 0 && transport.n_msx == 0 &&
                                      transport.age_row < 0 && transport.temp_row == 0 &&
                                      transport.rowIndex("__TEMPERATURE__") == 0;
        const bool standalone_local_bed_quality = stage.standaloneLocalBed() &&
                                                  !subsurface_.active() && state_.heat.enabled &&
                                                  state_.heat.bed && !state_.heat.ground_column;
        const bool mesh_only_ground_quality = stage.meshOnlyGroundQuality() && stage.meshOnly();
        const bool network_quality =
            detail::checkpointNetworkPollutants(ctx) && stage.meshOnly() &&
            transport.n_pollut == ctx.n_pollutants() && transport.n_species == ctx.n_pollutants() &&
            transport.n_msx == 0 && transport.age_row < 0 && transport.temp_row < 0;
        if (network_quality &&
            (state_.node_row_conc != &node_row_conc_ ||
             node_row_conc_.size() != std::size_t(ctx.n_nodes()) * ctx.n_pollutants()))
            throw std::invalid_argument(
                "Joint attachment requires the owned native pollutant row publisher.");
        if ((ramp && ramp[0] == '1') ||
            ((coupling_points_.empty() && subsurface_.nodeBeds().empty() &&
              !channel_ground_.active()) &&
             (ctx.n_nodes() != 0 || ctx.n_links() != 0)) ||
            (ctx.n_pollutants() != 0 && !standalone_local_bed_quality &&
             !mesh_only_ground_quality && !network_quality) ||
            (transport.active() && !temperature_only && !standalone_local_bed_quality &&
             !mesh_only_ground_quality && !network_quality) ||
            !aquifer_link_shares_.empty())
            throw std::invalid_argument(
                "Joint source attachment requires a supported exchange profile without head ramps or node/link beds.");
    }
    const auto& infiltration_rows = infil_.resolvedRows();
    for (int i = 0; i < mesh_.n_cells(); ++i) {
        if (state_.coupling_flux[i] != 0 || state_.coupling_forced[i] != 0 ||
            (!infiltration_rows.empty() && infiltration_rows[i].has_method))
            throw std::invalid_argument(
                "Router source attachment requires aquifer-owned cells without external coupling donors.");
        for (int k = 0; k < mesh_.cell_vertex_count(i); ++k)
            if (!stage.runtimeBoundariesEnabled() && mesh_.cell_neighbour(i, k) < 0 &&
                boundary_.edge_bc_type.at(MeshData::slot(i, k)) != int8_t(BoundaryType::WALL))
                throw std::invalid_argument("Router source attachment requires wall boundaries.");
    }
    // Empty node row publishers do not represent a live node provider.
    if (node_row_conc_.empty())
        state_.node_row_conc = nullptr;
    // The common source stage can withdraw water through the mesh/aquifer
    // contact even when no standalone 2D infiltration method was authored.
    // Keep the same per-cell accepted-depth ledger so the surface continuity
    // check and its checkpoint can still close.
    if (infil_cum_applied_.empty())
        infil_cum_applied_.assign(mesh_.n_cells(), 0.0);
    // Construct the joint identity before installing any attachment pointers.
    auto points = coupling_points_;
    Archive identity;
    if (hydraulic)
        identity.sequence(points, [&](auto& p) {
            identity(p.cell_idx, p.vertex_idx, p.node_idx, p.cd, p.area, p.is_outfall,
                     p.has_flap_gate, p.area_authored);
            if (p.law)
                identity(p.law, p.geo.z_crest, p.geo.l_weir, p.geo.a_orif, p.coef.c_w, p.coef.c_sw,
                         p.coef.c_oi, p.coef.c_oo, p.coef.h_eps);
        });
    auto model = sourceRouterModel(ctx) + identity.bytes();
    model += channel_ground_.modelIdentity();
    configureSnowGroundContacts();
    cpu->setCommonSourceStage(stage);
    cpu->joint_checkpoint_ = hydraulic != nullptr;
    joint_hydraulic_ = hydraulic;
    common_stage_ = &stage;
    source_context_ = &ctx;
    source_router_model_ = std::move(model);
    source_boundary_ready_ = true;
}
/**
 * @brief Attach a fresh borrowed accepted-source history recorder through the CPU source
 * marcher.
 * @param[in,out] history Prepared compatible recorder; borrowed and must outlive attachment/use.
 * @note Requires common stage and disallows directly attaching to an engine-owned joint
 * checkpoint session. Delegates recorder freshness/identity/boundary validation and throws on
 * refusal. No physics or calendar advance; exclusive source/history/lifecycle access, no
 * duplicate recorder ownership.
 */
void SurfaceRouter2D::setSourceResultHistory(SourceResultHistory& history) {
    if (joint_hydraulic_ && joint_hydraulic_->checkpoint_owned_session_)
        throw std::invalid_argument("Attach history to the whole engine checkpoint session.");
    if (!common_stage_)
        throw std::invalid_argument("Router source results require an attached stage.");
    static_cast<ExplicitInertialSolver*>(solver_.get())->setSourceResultHistory(history);
}
/**
 * @brief Resume a saved accepted-source recorder prefix at a completed router boundary.
 * @param[in,out] history Compatible borrowed saved recorder prefix with exact checkpoint match;
 * must remain stable/alive through recording.
 * @note Requires attached common stage and ready boundary, then delegates exact prefix/phase
 * checks to CPU marcher. Throws on refusal; does not replay physical intervals or advance engine
 * calendar. Exclusive router/source/recorder access required.
 */
void SurfaceRouter2D::resumeSourceResultHistory(SourceResultHistory& history) {
    if (!common_stage_ || !source_boundary_ready_)
        throw std::invalid_argument("Router source history requires an accepted boundary.");
    static_cast<ExplicitInertialSolver*>(solver_.get())->resumeSourceResultHistory(history);
}
/**
 * @brief Serialize and publish a qualified source/router or joint component boundary.
 * @param[in] ctx Exact attached context, held stable/exclusive through
 * qualification/serialization.
 * @param[in] path Writable destination with existing parent; existing record must match profile
 * magic.
 * @return Empty on success, otherwise owner, qualification, archive, publication or caught
 * standard exception diagnostic. Engine-owned managed RK2/joint sessions require engine
 * checkpoint entry.
 * @note Does not flush pending physics or advance clocks. Component scope includes qualified
 * router, CPU/source, optional channel/native hydraulic state; whole-engine/output state is
 * separate. Publisher uses exclusive temporary+rename and may leave temporary files on
 * exceptional filesystem failure.
 */
std::string SurfaceRouter2D::saveCheckpoint(const SimulationContext& ctx,
                                            const std::string& path) const {
    try {
        if (standaloneRk2Checkpointing() ||
            (joint_hydraulic_ && joint_hydraulic_->checkpoint_owned_session_))
            return "Engine-owned surface restart requires the engine session checkpoint.";
        std::string bytes;
        const auto error = checkpointBytes(ctx, bytes);
        if (!error.empty())
            return error;
        publish(path, bytes, joint_hydraulic_ ? jointMagic : magicName);
        return {};
    } catch (const std::exception& e) {
        return e.what();
    }
}
/**
 * @brief Encode qualified router, CPU/source and optional joint/channel state under model
 * identity.
 * @param[in] ctx Exact attached stable context whose completed ledgers and ownership qualify.
 * @param[out] result Replaced only after complete successful bounded envelope encoding;
 * unchanged on qualification/caught standard exception diagnostics.
 * @return Empty on success, otherwise source/receipt/native hydraulic/profile/archive/size
 * diagnostic.
 * @note Snapshots explicit router fields, water/report clocks, gage integrals,
 * source/runtime/boundary receipts, native joint delivery/outfall feedback and channel
 * thermal/native receipts where enabled. Includes profile-dependent version, endian/long-width,
 * model identity and payload CRC. Does not settle/flush/advance physics, mutate canonical owners
 * or restore arbitrary whole-engine/output state. Requires exclusive consistent views and live
 * borrowed owners throughout copies/validation.
 */
std::string SurfaceRouter2D::checkpointBytes(const SimulationContext& ctx,
                                             std::string& result) const {
    try {
        const auto error = sourceBoundary(ctx);
        if (!error.empty())
            return error;
        RestartState snapshot(*this, ctx);
        snapshot.validate(*this, ctx);
        if (joint_hydraulic_ && state_.coupling_applied.size() != mesh_.tri_area.size())
            return "Joint surface coupling receipt shape differs.";
        if (joint_hydraulic_)
            for (std::size_t c = 0; c < state_.coupling_applied.size(); ++c)
                if (state_.coupling_applied[c] !=
                    (snapshot.exchange.pending_.empty() ? 0 : snapshot.exchange.pending_[c]))
                    return "Joint outfall receipts differ from undrained surface coupling.";
        std::string cpu;
        const auto* numerical = static_cast<ExplicitInertialSolver*>(solver_.get());
        const auto ce = standaloneRk2Checkpointing() ? numerical->rk2CheckpointBytes(cpu)
                                                     : numerical->checkpointBytes(cpu);
        if (!ce.empty())
            return ce;
        Archive state;
        snapshot.archive(state);
        state(cpu);
        if (channel_ground_.active()) {
            std::string profile = "routed channel thermal and species receipts v2";
            state(profile);
            auto channel = channel_ground_;
            channel.checkpoint(state);
        }
        if (joint_hydraulic_) {
            Router::validateJointDelivery(ctx.nodes, ctx.coupled_node, snapshot.delivery);
            std::string hydraulic;
            const auto he = joint_hydraulic_->checkpointBytes(ctx, hydraulic);
            if (!he.empty())
                return he;
            state(hydraulic, snapshot.delivery);
            snapshot.archiveJoint(state);
        }
        auto payload = state.bytes(), model = source_router_model_;
        auto magic = std::string(standaloneRk2Checkpointing() ? rk2Magic
                                 : joint_hydraulic_           ? jointMagic
                                                              : magicName);
        uint32_t version = joint_hydraulic_ ? 2 : 1, endian = 0x01020304, longBytes = sizeof(long),
                 crc = checksum(payload);
        if (runtimeWaterCheckpointing())
            version += 2;
        if (runtimeBoundaryCheckpointing() || standaloneRk2Checkpointing())
            version += 2;
        if (outfallTailwaterCheckpointing())
            version += 8; // coupled outfalls carry their tailwater record
        Archive file;
        file(magic, version, endian, longBytes, model, payload, crc);
        if (file.bytes().size() > maxBytes)
            return "Router checkpoint exceeds its size limit.";
        result = file.bytes();
        return {};
    } catch (const std::exception& e) {
        return e.what();
    }
}
/**
 * @brief Read and stage a router component checkpoint into fresh matching attached owners.
 * @param[in,out] ctx Exact fresh attached context; successful install updates checkpointed
 * clocks, gage rainfall/evaporation, mass balance and qualified native/source owners.
 * @param[in] path Readable nonempty bounded checkpoint file.
 * @return Empty on success, otherwise freshness/profile/model/file/decode diagnostic.
 * Engine-owned managed RK2 or joint sessions must use the engine checkpoint route.
 * @note Delegates private staged load; no general whole-engine/output hotstart. Requires
 * exclusive live prepared owners and no evolution since attachment; does not physically replay
 * saved intervals.
 */
std::string SurfaceRouter2D::loadCheckpoint(SimulationContext& ctx, const std::string& path) {
    try {
        if (standaloneRk2Checkpointing() ||
            (joint_hydraulic_ && joint_hydraulic_->checkpoint_owned_session_))
            return "Engine-owned surface restart requires the engine session checkpoint.";
        return loadCheckpointBytes(ctx, readFile(path));
    } catch (const std::exception& e) {
        return e.what();
    }
}
/**
 * @brief Decode, cross-check and inspect/install a router checkpoint using private candidates.
 * @param[in,out] ctx Exact fresh attached context with zero router/native completed and pending
 * time. Successful install replaces only qualified checkpoint scope and restores its
 * clocks/weather receipts.
 * @param[in] bytes Owned nonempty bounded envelope, consumed locally.
 * @param[in] engine_boundary Optional exact six-value clock/weather guard: elapsed s, absolute
 * DateTime days, elapsed ms, old elapsed ms, next report ms and native evaporation ft/s.
 * @param[in] engine_rainfall Optional exact native display rainfall vector guard, one per gage.
 * @param[in] engine_storage Optional native hydraulic session storage guard, used after positive
 * saved time when a joint hydraulic owner exists.
 * @param[in] engine_routed Routing-history mode passed with engine_storage to hydraulic
 * validation.
 * @param[in] validate_source Optional pointer to nonempty staged source-driver callback;
 * forbidden for managed RK2. Temporary callback views expire after invocation.
 * @param[in] before_install Optional pointer to nonempty final pre-install hook after
 * validation; its external effects are not rolled back.
 * @param[out] inspect_source_end Optional completed numerical/source time s; selects
 * validation-only return, leaves canonical owners uninstalled and skips before_install.
 * @param[in] prepared_climate Optional borrowed prepared weather identity override for managed
 * RK2; null uses live context weather. Stable only for this call, not adopted as a borrowed
 * pointer.
 * @param[in] prepared_heat Optional borrowed prepared native heat identity override for managed
 * RK2; null uses live heat identity. Stable for this call.
 * @return Empty on success/inspection, otherwise
 * source/freshness/envelope/platform/checksum/model, clock/weather/physical receipt or caught
 * standard exception diagnostic.
 * @note Stages router/channel/native hydraulic snapshots, validates all payload/ledger/clock and
 * outfall/boundary shared receipts, then asks CPU to stage/install its matching owners before
 * final nonthrowing hydraulic/channel/router adoption. Managed RK2 also verifies copied
 * prescriptions and authored boundary fallback against prepared model; refuses existing copied
 * runtime owners. Before installation canonical state is preserved apart from caller callback
 * side effects. No saved physical interval is replayed; unrelated whole-engine/output state is
 * excluded. Exclusive stable prepared identities and live borrowed owners are mandatory.
 */
std::string SurfaceRouter2D::loadCheckpointBytes(
    SimulationContext& ctx, std::string bytes, const std::array<double, 6>* engine_boundary,
    const std::vector<double>* engine_rainfall, const double* engine_storage, bool engine_routed,
    const std::function<void(const runoff::SourceWaterDriver&)>* validate_source,
    const std::function<void()>* before_install, double* inspect_source_end,
    const climate::ClimateState* prepared_climate, const HeatState* prepared_heat) {
    try {
        const auto error = sourceBoundary(ctx);
        if (!error.empty())
            return error;
        if (sim_time_ != 0 || pending_dt_ != 0 || ctx.current_time != 0)
            return "Router checkpoint restore requires a fresh attached router.";
        if (bytes.empty() || bytes.size() > maxBytes)
            return "Router checkpoint exceeds its size limit.";
        Archive file(std::move(bytes));
        std::string magic, model, payload;
        uint32_t version = 0, endian = 0, longBytes = 0, crc = 0;
        file(magic, version, endian, longBytes, model, payload, crc);
        file.finish();
        if (magic != (standaloneRk2Checkpointing() ? rk2Magic
                      : joint_hydraulic_           ? jointMagic
                                                   : magicName) ||
            version !=
                (joint_hydraulic_ ? 2u : 1u) + (runtimeWaterCheckpointing() ? 2u : 0u) +
                    ((runtimeBoundaryCheckpointing() || standaloneRk2Checkpointing()) ? 2u : 0u) +
                    (outfallTailwaterCheckpointing() ? 8u : 0u) ||
            endian != 0x01020304 || longBytes != sizeof(long))
            return "Unsupported router checkpoint version/platform.";
        if (model != source_router_model_)
            return "Router checkpoint report/weather model differs.";
        if (crc != checksum(payload))
            return "Router checkpoint checksum differs.";
        RestartState snapshot(*this, ctx);
        auto channel = channel_ground_;
        Archive state(std::move(payload));
        snapshot.archive(state);
        std::string cpu, hydraulic;
        state(cpu);
        if (channel.active()) {
            std::string profile;
            state(profile);
            if (profile != "routed channel thermal and species receipts v2")
                return "Unsupported routed channel thermal/species checkpoint profile.";
            channel.checkpoint(state);
        }
        if (joint_hydraulic_) {
            state(hydraulic, snapshot.delivery);
            snapshot.archiveJoint(state);
        }
        state.finish();
        snapshot.validate(*this, ctx);
        const std::array<double, 6> clock{snapshot.current_time,   snapshot.current_date,
                                          snapshot.elapsed_ms,     snapshot.old_elapsed_ms,
                                          snapshot.next_report_ms, snapshot.evap_rate};
        if (engine_boundary && clock != *engine_boundary)
            return "Engine/surface checkpoint clocks differ.";
        if (engine_rainfall && snapshot.rainfall != *engine_rainfall)
            return "Engine/surface checkpoint rainfall differs.";
        const std::array<double, 10> expected{
            snapshot.sim_time_,       snapshot.mb.rainfall_in,       snapshot.mb.evap_out,
            snapshot.mb.aquifer_in,   snapshot.mb.final_storage,     snapshot.mb.external_in,
            snapshot.mb.external_out, double(snapshot.runtime_used), snapshot.mb.boundary_in,
            snapshot.mb.boundary_out};
        // Stage hydraulic state and cross-check the duplicated shared clock
        // before the CPU/source transaction can install anything.
        std::shared_ptr<Router::RestartState> prepared;
        if (joint_hydraulic_) {
            prepared = joint_hydraulic_->prepareCheckpointBytes(
                ctx, std::move(hydraulic), &clock, &snapshot.delivery, &snapshot.feedback);
            if (engine_storage && clock[0] > 0)
                Router::validateSessionStorage(prepared, ctx, *engine_storage, engine_routed);
        }
        if (standaloneRk2Checkpointing()) {
            if (validate_source)
                return "Managed RK2 cannot install a borrowed source driver.";
            if (!state_.runtime_sources.rows.empty() ||
                !subsurface_.runtimeSources().rows.empty() ||
                !state_.runtime_forcings.rows.empty() || ctx.runtime_coupling_used)
                return "Managed RK2 restore requires fresh copied prescription owners.";
            Rk2CheckpointBinding binding;
            binding.climate = prepared_climate ? prepared_climate : &ctx.climate_state;
            binding.heat = prepared_heat ? prepared_heat : &ctx.heat_state;
            binding.surface = &snapshot.rk2_surface;
            binding.groundwater = &snapshot.rk2_ground;
            binding.forcing = &snapshot.rk2_forcing;
            binding.boundary = &snapshot.rk2_boundary;
            binding.boundary_quality = &snapshot.rk2_boundary_quality;
            auto fallback = snapshot.rk2_boundary;
            std::vector<int> overridden;
            for (const auto& entry : fallback.runtime_original)
                overridden.push_back(entry.first);
            for (int slot : overridden)
                fallback.clearRuntime(slot);
            if (rk2AuthoredBoundaryModel(fallback, ctx.tables) !=
                rk2AuthoredBoundaryModel(boundary_, ctx.tables))
                return "Managed RK2 surface boundary authored fallback differs.";
            std::function<void(const ExplicitInertialSolver&, const SurfaceStateData&,
                               const SubsurfaceSolver&)>
                validate = [&](const auto& numerical, const auto& surface, const auto& ground) {
                    if (!close(numerical.completedSourceTime(), snapshot.sim_time_) ||
                        !close(numerical.t_last_sync_, snapshot.sim_time_))
                        throw std::runtime_error(
                            "Managed RK2 numerical/router completed clocks differ.");
                    double volume = 0, incoming = 0, outgoing = 0;
                    for (double water : surface.volume)
                        volume += water;
                    for (const auto& row : surface.runtime_sources.rows) {
                        incoming += row.receipt.water_in;
                        outgoing += row.receipt.water_out;
                    }
                    if (!close(volume, snapshot.mb.final_storage) ||
                        !close(incoming, snapshot.mb.external_in) ||
                        !close(outgoing, snapshot.mb.external_out))
                        throw std::runtime_error(
                            "Managed RK2 physical/router accepted receipts differ.");
                    double boundary_in = 0, boundary_out = 0;
                    for (const auto& [slot, r] : surface.boundary->receipts) {
                        boundary_in += r.water_in;
                        boundary_out += r.water_out;
                        if (!close(-r.applied, surface.boundary->edge_bc_cum_flux.at(slot)))
                            throw std::runtime_error(
                                "Managed RK2 surface boundary cumulative flux differs from its receipt.");
                    }
                    if (!close(boundary_in, snapshot.mb.boundary_in) ||
                        !close(boundary_out, snapshot.mb.boundary_out) ||
                        (!snapshot.bc_nonwall_dirty_ &&
                         numerical.bc_slot_ != snapshot.boundary_slots))
                        throw std::runtime_error(
                            "Managed RK2 surface boundary physical/router receipts differ.");
                    for (const auto* values : {&surface.rainfall, &surface.evap_rate})
                        for (double value : *values)
                            if (!std::isfinite(value) || value < 0)
                                throw std::runtime_error("Invalid managed RK2 native forcing.");
                    if (auto error =
                            validateRk2ReplayIds(surface.runtime_sources, ground.runtimeSources(),
                                                 surface.runtime_forcings, snapshot.sim_time_);
                        !error.empty())
                        throw std::runtime_error(error);
                };
            const auto ce =
                static_cast<ExplicitInertialSolver*>(solver_.get())
                    ->loadRk2CheckpointBytes(std::move(cpu), &binding, &validate, before_install,
                                             inspect_source_end != nullptr);
            if (!ce.empty())
                return ce;
            if (inspect_source_end) {
                *inspect_source_end = snapshot.sim_time_;
                return {};
            }
            if (joint_hydraulic_)
                joint_hydraulic_->installCheckpoint(ctx, std::move(prepared));
            snapshot.install(*this, ctx);
            return {};
        }
        const auto ce =
            static_cast<ExplicitInertialSolver*>(solver_.get())
                ->loadCheckpointBytes(
                    std::move(cpu), &expected,
                    snapshot.exchange.pending_.empty() ? nullptr : &snapshot.exchange.pending_,
                    validate_source, before_install,
                    snapshot.boundary_water && !snapshot.bc_nonwall_dirty_
                        ? &snapshot.boundary_slots
                        : nullptr,
                    inspect_source_end);
        if (!ce.empty())
            return ce;
        if (inspect_source_end)
            return {};
        // Parsing/model/ledger/clock checks have all completed. CPU installation
        // above and these remaining explicit moves cannot throw.
        if (joint_hydraulic_)
            joint_hydraulic_->installCheckpoint(ctx, std::move(prepared));
        channel_ground_ = std::move(channel);
        snapshot.install(*this, ctx);
        return {};
    } catch (const std::exception& e) {
        return e.what();
    }
}
}
