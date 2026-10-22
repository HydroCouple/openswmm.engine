// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin

/**
 * @file      RoutingCheckpoint.cpp
 * @brief     Checkpoint persistence for hydraulic routing state.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include "Routing.hpp"
#include "../core/CompletedSourceArchive.hpp"
#include "../core/SimulationContext.hpp"
#include "../core/QualityCheckpoint.hpp"
#include "../core/DateTime.hpp"
#include "../hydrology/LidNode.hpp"
#include "Node.hpp"
#include "../core/UnitConversion.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace openswmm {
namespace {
using Archive = detail::CompletedSourceArchive;
constexpr uint64_t maxBytes = 64 * 1024 * 1024;
constexpr const char* magicName = "OpenSWMM accepted DW hydraulic boundary";
/**
 * @brief Throw the hydraulic-checkpoint diagnostic when an invariant fails.
 *
 * No state rollback or cleanup is performed by this guard.
 *
 * @param[in] ok Required condition; false throws runtime_error.
 * @param[in] message Non-NULL diagnostic copied on failure.
 */
void require(bool ok, const char* message) {
    if (!ok)
        throw std::runtime_error(message);
}
/**
 * @brief Compare two scalars with the hydraulic-checkpoint relative/absolute tolerance.
 *
 * @param[in] a First finite value in units matching b.
 * @param[in] b Second finite value.
 * @return abs(a-b)<=1e-11*max(1,abs(a),abs(b)); caller validates finiteness separately.
 */
bool close(double a, double b) {
    return std::abs(a - b) <= 1.e-11 * std::max({1., std::abs(a), std::abs(b)});
}
/**
 * @brief Require every numeric vector element to be finite after double conversion.
 *
 * Throws runtime_error for first nonfinite conversion; does not check units/ranges or mutate
 * vector.
 *
 * @param[in] row Borrowed numeric vector; values cast to double for isfinite.
 */
template <class T> void finite(const std::vector<T>& row) {
    for (auto v : row)
        require(std::isfinite(static_cast<double>(v)), "Nonfinite hydraulic checkpoint field.");
}
/**
 * @brief Require every numeric vector element to be finite and nonnegative.
 *
 * Checks finiteness first, then v>=0; throws runtime_error without mutation. Empty vectors pass.
 *
 * @param[in] row Borrowed numeric inventory/statistic vector in its retained units.
 */
template <class T> void nonnegative(const std::vector<T>& row) {
    finite(row);
    for (auto v : row)
        require(v >= 0, "Negative hydraulic checkpoint inventory/statistic.");
}
/**
 * @brief Compute reflected CRC-32 over hydraulic checkpoint bytes.
 *
 * @param[in] bytes Borrowed binary string, including embedded NUL.
 * @return CRC-32 using0xedb88320 and initial/final complement; integrity check, not authentication.
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
 * @brief Read one nonempty hydraulic checkpoint bounded to64 MiB.
 *
 * Throws for missing/empty/oversized/short/changing file and allocation/filesystem errors. No
 * UTF-8-specific Windows path adapter, locking or checksum verification occurs here.
 *
 * @param[in] path Borrowed platform narrow filesystem path; stream and file_size use the standard
 *     string-path conversion.
 * @return Owned binary bytes after exact-size read and EOF check.
 */
std::string readFile(const std::string& path) {
    std::error_code ec;
    auto size = std::filesystem::file_size(path, ec);
    require(!ec && size > 0 && size <= maxBytes,
            "Hydraulic checkpoint is missing or exceeds its size limit.");
    std::ifstream in(path, std::ios::binary);
    std::string bytes(size, '\0');
    in.read(bytes.data(), bytes.size());
    require(bool(in) && in.peek() == std::char_traits<char>::eof(),
            "Hydraulic checkpoint read failed.");
    return bytes;
}
/**
 * @brief Stage hydraulic checkpoint bytes then rename to the destination.
 *
 * Exclusively creates a sibling .hydraulic-part-N using standard narrow fopen, writes/closes, then
 * filesystem rename. Write/close/rename failure throws and attempts staging cleanup; replacement
 * behavior follows platform rename semantics. Existing-file magic is checked without a complete
 * CRC/schema validation. No fsync/crash-durability guarantee or interprocess lock; caller
 * serializes one destination.
 *
 * @param[in] path Borrowed platform narrow destination; an existing file must start with the
 *     recognized hydraulic checkpoint magic.
 * @param[in] bytes Borrowed serialized bytes, validated/bounded by caller before publication.
 */
void publish(const std::string& path, const std::string& bytes) {
    if (std::filesystem::exists(path)) {
        Archive old(readFile(path));
        std::string magic;
        old(magic);
        require(magic == magicName, "Hydraulic checkpoint destination is an unrelated file.");
    }
    static std::atomic<uint64_t> serial{0};
    auto part = path + ".hydraulic-part-" + std::to_string(++serial);
    FILE* file = std::fopen(part.c_str(), "wbx");
    require(file, "Cannot create hydraulic checkpoint publication file.");
    bool written = std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
    bool closed = std::fclose(file) == 0;
    if (!written || !closed) {
        std::filesystem::remove(part);
        throw std::runtime_error("Hydraulic checkpoint write failed.");
    }
    std::error_code ec;
    std::filesystem::rename(part, path, ec);
    if (ec) {
        std::filesystem::remove(part);
        throw std::runtime_error("Cannot publish hydraulic checkpoint: " + ec.message());
    }
}
/**
 * @brief Visit explicit Green-Ampt material and history fields in native archive representation.
 *
 * Includes saturation flag. No finite/material checks here; later RestartState::validate handles
 * saved consistency. Decode exceptions leave partial fields consumed/changed.
 *
 * @param[in,out] a Borrowed read/write archive; native same-platform encoding.
 * @param[in,out] s Borrowed source/destination, retaining native feet/time/fraction conventions.
 */
void greenAmpt(Archive& a, GreenAmptState& s) {
    a(s.S, s.Ks, s.IMDmax, s.IMD, s.F, s.Fu, s.Fumax, s.Lu, s.T, s.saturated);
}
/**
 * @brief Visit retained NODE LID water, Richards, transfer and optional thermal continuation.
 *
 * Archives solver report/transfers, surface infiltration, quality-labelled water exchange records,
 * mobile/held inventories and optional mesh completion. Some variable vectors resize, fixed/cell
 * vectors require matching dimensions. Does not serialize cell geometry/porosity, resolve ports or
 * validate balances; exceptions can leave partial trial state mutation.
 *
 * @param[in,out] a Borrowed same-platform archive; read cursor advances incrementally.
 * @param[in,out] s Borrowed LID state whose fixed Richards/port/cell vectors already match
 *     initialized model shape on decode.
 * @param[in] mesh Include completed mesh-bed receipt/clock fields when saved richards flag is true.
 * @param[in] thermal Include native thermal_energy entries in joules.
 * @param[in] thermal_contact_receipts Include per-cell ground-contact energy receipts in joules;
 *     caller selects producer-compatible version flags.
 */
void lidState(Archive& a, LidNodeState& s, bool mesh, bool thermal, bool thermal_contact_receipts) {
    a(s.richards);
    a.fixed(s.richards_water, s.richards_pressure);
    auto& r = s.richards_report;
    a(r.ok, r.error, r.accepted, r.rejected, r.rhs, r.newton, r.min_step, r.bottom_volume,
      r.evaporation, r.balance);
    a.sequence(r.transfers, [&](auto& t) { a(t.from, t.to, t.volume); });
    a(s.richards_bottom_loss);
    greenAmpt(a, s.surface_infil);
    a(s.infiltration_cell, s.infiltration_head, s.quality_mass, s.quality_old_water);
    a.sequence(s.quality_transfers, [&](auto& t) { a(t.from, t.to, t.volume); });
    a.sequence(s.quality_ports, [&](auto& t) { a(t.link, t.cell, t.volume); });
    a(s.quality_outlet_conc, s.quality_old_mobile, s.mobile_delta);
    a.fixed(s.port_delta);
    a.sequence(s.cells, [&](auto& c) { a(c.theta); }, true);
    if (thermal)
        a(s.thermal_energy);
    if (thermal_contact_receipts)
        a(s.thermal_ground_contact_j);
    a(s.held_volume, s.evap_volume, s.captured_flow, s.treated_volume);
    if (mesh && s.richards)
        a(s.richards_completed_time, s.richards_mesh_start, s.richards_mesh_pending_finish,
          s.richards_mesh_routed, r.bottom_outflow, r.bottom_inflow, r.inflow);
}
// Explicit fields only. Model arrays are compared, never restored. Old/new
// report values, pending exchange volumes, retained stores and statistics are
// state. Solver geometry carried across a step is NOT disposable scratch:
// area_mid supplies the next unsteady term; q1 preserves per-barrel rounding.
#define NODE_MODEL(X)                                                                              \
    X(type)                                                                                        \
    X(invert_elev)                                                                                 \
    X(full_depth)                                                                                  \
    X(init_depth)                                                                                  \
    X(sur_depth)                                                                                   \
    X(ponded_area)                                                                                 \
    X(is_virtual)                                                                                  \
    X(is_inlet)                                                                                    \
    X(rim_depth)                                                                                   \
    X(crown_elev)                                                                                  \
    X(degree)                                                                                      \
    X(comments)                                                                                    \
    X(tags)                                                                                        \
    X(rpt_flag)
#define LINK_MODEL(X)                                                                              \
    X(type)                                                                                        \
    X(node1)                                                                                       \
    X(node2)                                                                                       \
    X(offset1)                                                                                     \
    X(offset2)                                                                                     \
    X(q0)                                                                                          \
    X(q_limit)                                                                                     \
    X(xsect_shape)                                                                                 \
    X(xsect_y_full)                                                                                \
    X(xsect_a_full)                                                                                \
    X(xsect_w_max)                                                                                 \
    X(xsect_geom1)                                                                                 \
    X(xsect_geom2)                                                                                 \
    X(xsect_geom3)                                                                                 \
    X(xsect_geom4)                                                                                 \
    X(xsect_curve)                                                                                 \
    X(xsect_r_full)                                                                                \
    X(xsect_s_full)                                                                                \
    X(xsect_s_max)                                                                                 \
    X(xsect_y_bot)                                                                                 \
    X(xsect_a_bot)                                                                                 \
    X(xsect_s_bot)                                                                                 \
    X(xsect_r_bot)                                                                                 \
    X(xsect_yw_max)                                                                                \
    X(xsect_batch_shape)                                                                           \
    X(direction)                                                                                   \
    X(pump_curve_name)                                                                             \
    X(has_flap_gate)                                                                               \
    X(comments)                                                                                    \
    X(tags)                                                                                        \
    X(rpt_flag)
#define NODE_STATE(X)                                                                              \
    X(depth)                                                                                       \
    X(head)                                                                                        \
    X(volume)                                                                                      \
    X(lat_flow)                                                                                    \
    X(user_lat_flow)                                                                               \
    X(runoff_inflow)                                                                               \
    X(gw_inflow)                                                                                   \
    X(ext_inflow)                                                                                  \
    X(dwf_inflow)                                                                                  \
    X(rdii_inflow)                                                                                 \
    X(iface_inflow)                                                                                \
    X(coupling_inflow)                                                                             \
    X(coupling_volume)                                                                             \
    X(coupling_queue)                                                                              \
    X(coupling_qual_queue)                                                                         \
    X(coupling_qual_inflow)                                                                        \
    X(coupling_age_vol_queue)                                                                      \
    X(coupling_temp_vol_queue)                                                                     \
    X(coupling_age_vol_inflow)                                                                     \
    X(coupling_temp_vol_inflow)                                                                    \
    X(qual_mass_in)                                                                                \
    X(qual_vol_in)                                                                                 \
    X(iface_qual_mass)                                                                             \
    X(ext_qual_mass)                                                                               \
    X(dwf_qual_mass)                                                                               \
    X(lid_drain_qual_load)                                                                         \
    X(lid_drain_qual_vol)                                                                          \
    X(lid_drain_qual_load_old)                                                                     \
    X(lid_drain_qual_vol_old)                                                                      \
    X(lid_drain_inflow)                                                                            \
    X(conc)                                                                                        \
    X(conc_old)                                                                                    \
    X(hrt)                                                                                         \
    X(user_conc_mass_flux)                                                                         \
    X(inflow)                                                                                      \
    X(outflow)                                                                                     \
    X(overflow)                                                                                    \
    X(losses)                                                                                      \
    X(old_net_inflow)                                                                              \
    X(full_volume)                                                                                 \
    X(rpt_full_volume)                                                                             \
    X(old_depth)                                                                                   \
    X(old_volume)                                                                                  \
    X(old_lat_flow)                                                                                \
    X(old_inflow)                                                                                  \
    X(stat_vol_flooded)                                                                            \
    X(stat_time_flooded)                                                                           \
    X(stat_max_depth)                                                                              \
    X(stat_max_overflow)                                                                           \
    X(stat_max_overflow_date)                                                                      \
    X(stat_sum_depth)                                                                              \
    X(stat_sum_volume)                                                                             \
    X(stat_storage_max_volume)                                                                     \
    X(stat_storage_max_volume_date)                                                                \
    X(stat_max_depth_date)                                                                         \
    X(stat_max_rpt_depth)                                                                          \
    X(stat_max_inflow_date)                                                                        \
    X(stat_time_surcharged)                                                                        \
    X(stat_max_surcharge_height)                                                                   \
    X(stat_outfall_avg_flow)                                                                       \
    X(stat_max_lat_inflow)                                                                         \
    X(stat_max_total_inflow)                                                                       \
    X(stat_storage_max_outflow)                                                                    \
    X(stat_lat_inflow_vol)                                                                         \
    X(stat_total_inflow_vol)                                                                       \
    X(stat_total_outflow_vol)                                                                      \
    X(stat_outfall_max_flow)                                                                       \
    X(stat_outfall_periods)                                                                        \
    X(stat_non_converged_count)                                                                    \
    X(stat_time_courant_critical)                                                                  \
    X(stat_total_load)
#define LINK_STATE(X)                                                                              \
    X(setting)                                                                                     \
    X(target_setting)                                                                              \
    X(time_last_set)                                                                               \
    X(dqdh)                                                                                        \
    X(flow)                                                                                        \
    X(kw_inflow)                                                                                   \
    X(depth)                                                                                       \
    X(volume)                                                                                      \
    X(slot_volume)                                                                                 \
    X(stat_slot_vol_dt)                                                                            \
    X(stat_vol_dt)                                                                                 \
    X(stat_peak_slot_share)                                                                        \
    X(stat_time_slot_above)                                                                        \
    X(froude)                                                                                      \
    X(flow_class)                                                                                  \
    X(is_closed)                                                                                   \
    X(old_flow)                                                                                    \
    X(old_depth)                                                                                   \
    X(old_volume)                                                                                  \
    X(conc)                                                                                        \
    X(conc_old)                                                                                    \
    X(stat_vol_flow)                                                                               \
    X(stat_max_flow)                                                                               \
    X(stat_max_veloc)                                                                              \
    X(stat_max_filling)                                                                            \
    X(stat_time_surcharged)                                                                        \
    X(stat_flow_class)                                                                             \
    X(stat_norm_ltd)                                                                               \
    X(stat_inlet_ctrl)                                                                             \
    X(stat_max_flow_date)                                                                          \
    X(stat_time_full_upstream)                                                                     \
    X(stat_time_full_dnstream)                                                                     \
    X(stat_time_full_both)                                                                         \
    X(stat_time_capacity_limited)                                                                  \
    X(stat_total_load)                                                                             \
    X(stat_pump_cycles)                                                                            \
    X(stat_pump_on_time)                                                                           \
    X(stat_pump_volume)                                                                            \
    X(stat_pump_energy)                                                                            \
    X(stat_pump_sum_flow)                                                                          \
    X(stat_pump_periods)                                                                           \
    X(stat_pump_off_curve_low)                                                                     \
    X(stat_pump_off_curve_high)                                                                    \
    X(stat_pump_was_on)                                                                            \
    X(stat_flow_turns)                                                                             \
    X(stat_flow_turn_sign)                                                                         \
    X(stat_time_courant_critical)                                                                  \
    X(stat_lstep_min)                                                                              \
    X(stat_lstep_max)                                                                              \
    X(stat_lstep_dt_time)                                                                          \
    X(stat_lstep_time)                                                                             \
    X(stat_lstep_bin_time)                                                                         \
    X(stat_conv_steps)                                                                             \
    X(stat_conv_total)
#define CONDUIT_MODEL(X)                                                                           \
    X(link_idx)                                                                                    \
    X(roughness)                                                                                   \
    X(length)                                                                                      \
    X(true_length)                                                                                 \
    X(slope)                                                                                       \
    X(mod_length)                                                                                  \
    X(barrels)                                                                                     \
    X(beta)                                                                                        \
    X(rough_factor)                                                                                \
    X(q_full)                                                                                      \
    X(q_max)                                                                                       \
    X(loss_inlet)                                                                                  \
    X(loss_outlet)                                                                                 \
    X(loss_avg)                                                                                    \
    X(seep_rate)                                                                                   \
    X(culvert_code)                                                                                \
    X(gw_coupled)                                                                                  \
    X(gw_kc)                                                                                       \
    X(gw_dc)
#define CONDUIT_STATE(X)                                                                           \
    X(evap_loss_rate)                                                                              \
    X(seep_loss_rate)                                                                              \
    X(normal_flow_limited)                                                                         \
    X(inlet_control)                                                                               \
    X(full_state)                                                                                  \
    X(gw_head_rel)                                                                                 \
    X(gw_gain_max)
#define STORAGE_MODEL(X)                                                                           \
    X(node_idx)                                                                                    \
    X(curve)                                                                                       \
    X(curve_name)                                                                                  \
    X(shape)                                                                                       \
    X(a)                                                                                           \
    X(b)                                                                                           \
    X(c)                                                                                           \
    X(p1)                                                                                          \
    X(p2)                                                                                          \
    X(p3)                                                                                          \
    X(seep_rate)                                                                                   \
    X(evap_frac)                                                                                   \
    X(exfil_suction)                                                                               \
    X(exfil_ksat)                                                                                  \
    X(exfil_imd)
#define DW_STATE(X)                                                                                \
    X(losses_all_zero_)                                                                            \
    X(last_converged_)                                                                             \
    X(variable_step_)                                                                              \
    X(sim_time_)                                                                                   \
    X(evap_rate)                                                                                   \
    X(area1_)                                                                                      \
    X(area2_)                                                                                      \
    X(area_mid_)                                                                                   \
    X(hrad_mid_)                                                                                   \
    X(width_mid_)                                                                                  \
    X(depth1_)                                                                                     \
    X(depth2_)                                                                                     \
    X(depth_mid_)                                                                                  \
    X(velocity_)                                                                                   \
    X(froude_)                                                                                     \
    X(sigma_)                                                                                      \
    X(dqdh_)                                                                                       \
    X(new_flow_)                                                                                   \
    X(q1_)                                                                                         \
    X(area_old_)                                                                                   \
    X(bypassed_)                                                                                   \
    X(surf_area1_)                                                                                 \
    X(surf_area2_)                                                                                 \
    X(hrad1_)                                                                                      \
    X(width1_)                                                                                     \
    X(width2_)                                                                                     \
    X(tpa_latch_)                                                                                  \
    X(tpa_latch_changed_)
}

struct Router::RestartState {
    NodeData nodes;
    LinkData links;
    StorageData storages;
    OutfallData outfalls;
    ConduitData conduits;
    SimulationContext::OutfallExports exports;
    dynwave::DWSolver dw;
    double time, date, elapsed, old_elapsed, next_report, evap, infil_factor, recovery_factor;
    bool link_steps;
    double link_step_span;
    /**
     * @brief Copy a private hydraulic restart candidate from live routing/context state.
     *
     * Copies node/link/subtype inventories and DW solver storage. Embedded non-owning solver views
     * retain original ownership; do not execute this private solver copy. For qualified no-LID network
     * pollutant profile canonicalizes absent old drain-load history to a zero matrix. Live model is
     * unchanged; allocation can throw.
     *
     * @param[in] r Borrowed initialized Router supplying DW state and link-step policy.
     * @param[in] c Borrowed coherent native context, outfall exports and clocks matching Router.
     */
    RestartState(const Router& r, const SimulationContext& c)
        : nodes(c.nodes), links(c.links), storages(c.node_subtypes.storages),
          outfalls(c.node_subtypes.outfalls), conduits(c.link_subtypes.conduits),
          exports(c.outfall_exports), dw(r.dw_solver_), time(c.current_time), date(c.current_date),
          elapsed(c.elapsed_ms), old_elapsed(c.old_elapsed_ms), next_report(c.next_report_ms),
          evap(c.climate_state.evap_rate), infil_factor(c.climate_state.infil_factor),
          recovery_factor(c.climate_state.recovery_factor), link_steps(r.lstep_enabled_),
          link_step_span(r.lstep_rs_) {
        // This no-LID profile has no old drain load. Native routing creates
        // the zero history on its first step; canonicalize it on private
        // snapshots so a fresh target has the same fixed archive shape.
        if (detail::checkpointNetworkPollutants(c) && nodes.lid_drain_qual_load_old.empty())
            nodes.lid_drain_qual_load_old.assign(std::size_t(c.n_nodes()) * c.n_pollutants(), 0.);
    }
    /**
     * @brief Encode or decode selected hydraulic restart state with fixed initialized shapes.
     *
     * Visits exports, clocks, node/link/conduit arrays, LID state, retained DW geometry/iteration
     * history and xnode/DPS vectors. Does not restore model arrays, file/plugin handles or compiled
     * expressions. Fixed shapes refuse mismatch; consumption/allocation can throw with a partial
     * candidate. Validation/install are separate.
     *
     * @param[in,out] a Borrowed native read/write archive; decoded fields mutate this private
     *     candidate.
     * @param[in] thermal_lid_state Producer-compatible flag selecting LID thermal energy payload.
     * @param[in] thermal_lid_contact_receipts Producer-compatible flag selecting LID ground energy
     *     receipt payload.
     */
    void archive(Archive& a, bool thermal_lid_state, bool thermal_lid_contact_receipts) {
        a.fixed(exports.water_m3, exports.reverse_m3, exports.heat_j, exports.pollutant_amounts);
        a(time, date, elapsed, old_elapsed, next_report, evap, infil_factor, recovery_factor,
          link_steps, link_step_span);
#define FIELD(v) a.fixed(nodes.v);
        NODE_STATE(FIELD)
#undef FIELD
#define FIELD(v) a.fixed(links.v);
        LINK_STATE(FIELD)
#undef FIELD
#define FIELD(v) a.fixed(conduits.v);
        CONDUIT_STATE(FIELD)
#undef FIELD
        a.fixed(storages.evap_loss, storages.exfil_loss, storages.exfil_rate, outfalls.head_2d,
                outfalls.ramp_2d);
        std::size_t lid_row = 0;
        a.sequence(
            storages.lid_state,
            [&](auto& s) {
                lidState(a, s, storages.lid[lid_row++].bottom == LidNodeBottom::MeshAquifer,
                         thermal_lid_state, thermal_lid_contact_receipts);
            },
            true);
        a(dw.losses_all_zero_, dw.last_converged_, dw.variable_step_, dw.sim_time_, dw.evap_rate);
        // Scalar fields above; remaining arrays below have initialized model shapes.
        a.fixed(dw.area1_, dw.area2_, dw.area_mid_, dw.hrad_mid_, dw.width_mid_, dw.depth1_,
                dw.depth2_, dw.depth_mid_, dw.velocity_, dw.froude_, dw.sigma_, dw.dqdh_,
                dw.new_flow_, dw.q1_, dw.area_old_, dw.bypassed_, dw.surf_area1_, dw.surf_area2_,
                dw.hrad1_, dw.width1_, dw.width2_, dw.tpa_latch_, dw.tpa_latch_changed_);
        auto& x = dw.xnode_;
        a.fixed(x.new_surf_area, x.old_surf_area, x.sumdqdh, x.dYdT, x.converged, x.is_surcharged);
        auto& d = dw.dps_;
        a.fixed(d.As, d.hs, d.hs_prev_iter, d.P, d.P_hat, d.P_hat_0, d.T_s_target, d.t_s,
                d.surcharged);
    }
    /**
     * @brief Validate decoded hydraulic state against initialized model and accepted-boundary
     * invariants.
     *
     * Throws on invalid clocks/date/duration, nonfinite/negative inventories, flags/flow classes,
     * head-depth inconsistency, qualified pollutant layout, LID formulation/material/pore limits,
     * transfer/port/retention balance and Richards diagnostics. Allows specified pressure/ODE roundoff
     * budgets and optional NaN outfall stage conventions. Recomputes LID-dependent full volumes on a
     * private context. Does not install or mutate target; temporary allocation can throw.
     *
     * @param[in] router Borrowed target Router supplying model DPS constants and owned-session clock
     *     policy.
     * @param[in] c Borrowed target model/material/layout with matching dimensions established by fixed
     *     archive decoding.
     */
    void validate(const Router& router, const SimulationContext& c) const {
        nonnegative(exports.water_m3);
        nonnegative(exports.reverse_m3);
        finite(exports.heat_j);
        nonnegative(exports.pollutant_amounts);
        for (double v :
             {time, date, elapsed, old_elapsed, next_report, evap, infil_factor, recovery_factor,
              link_step_span, dw.variable_step_, dw.sim_time_, dw.evap_rate})
            require(std::isfinite(v), "Nonfinite hydraulic checkpoint clock/forcing.");
        require(time >= 0 && close(elapsed, 1000 * time) && old_elapsed >= 0 &&
                    old_elapsed <= elapsed && next_report >= 0 &&
                    date == datetime::addSeconds(c.options.start_date, time) && evap >= 0 &&
                    infil_factor >= 0 && recovery_factor >= 0 && dw.variable_step_ >= 0 &&
                    dw.sim_time_ >= 0 && dw.evap_rate >= 0 && link_step_span >= 0,
                "Invalid hydraulic checkpoint clocks/forcing.");
        require((link_step_span == 0 || link_step_span == c.options.routing_step) &&
                    (!link_steps || link_step_span == c.options.routing_step),
                "Invalid hydraulic checkpoint link-step sampling span.");
        require(c.options.total_duration_ms < 0 || elapsed <= c.options.total_duration_ms,
                "Hydraulic checkpoint exceeds the model duration.");
        require(dw.surcharge_method != dynwave::SurchargeMethod::DYNAMIC_SLOT ||
                    (router.checkpoint_owned_session_
                         ? dw.sim_time_ <= time + 1.e-11 * std::max(1., time)
                         : close(dw.sim_time_, time)),
                "Hydraulic/DPS completed clocks differ.");
        if (detail::checkpointNetworkPollutants(c)) {
            const auto np = std::size_t(c.n_pollutants()), nn = std::size_t(c.n_nodes()),
                       nl = std::size_t(c.n_links());
            require(nodes.conc_n_pollutants == int(np) && links.conc_n_pollutants == int(np) &&
                        nodes.stat_n_pollutants == int(np) && links.stat_n_pollutants == int(np) &&
                        nodes.conc.size() == nn * np && nodes.conc_old.size() == nn * np &&
                        links.conc.size() == nl * np && links.conc_old.size() == nl * np &&
                        nodes.stat_total_load.size() == nn * np &&
                        links.stat_total_load.size() == nl * np,
                    "Network pollutant checkpoint layout differs.");
            require(nodes.lid_drain_qual_load_old.size() == nn * np &&
                        std::all_of(nodes.lid_drain_qual_load_old.begin(),
                                    nodes.lid_drain_qual_load_old.end(),
                                    [](double v) { return v == 0; }) &&
                        std::all_of(nodes.lid_drain_qual_load.begin(),
                                    nodes.lid_drain_qual_load.end(),
                                    [](double v) { return v == 0; }),
                    "Unexpected LID pollutant history without a LID owner.");
            for (const auto* row : {&nodes.conc, &nodes.conc_old, &links.conc, &links.conc_old,
                                    &nodes.stat_total_load, &links.stat_total_load})
                nonnegative(*row);
        }
#define FINITE(v) finite(nodes.v);
        NODE_STATE(FINITE)
#undef FINITE
#define FINITE(v) finite(links.v);
        LINK_STATE(FINITE)
#undef FINITE
#define FINITE(v) finite(conduits.v);
        CONDUIT_STATE(FINITE)
#undef FINITE
        for (const auto* row : {&nodes.depth,
                                &nodes.volume,
                                &nodes.full_volume,
                                &nodes.rpt_full_volume,
                                &nodes.old_depth,
                                &nodes.old_volume,
                                &links.depth,
                                &links.volume,
                                &links.old_depth,
                                &links.old_volume,
                                &storages.evap_loss,
                                &storages.exfil_loss,
                                &dw.area1_,
                                &dw.area2_,
                                &dw.area_mid_,
                                &dw.hrad_mid_,
                                &dw.width_mid_,
                                &dw.area_old_,
                                &dw.xnode_.new_surf_area,
                                &dw.xnode_.old_surf_area,
                                &dw.xnode_.dYdT,
                                &dw.dps_.As,
                                &dw.dps_.hs,
                                &dw.dps_.hs_prev_iter,
                                &dw.dps_.P,
                                &dw.dps_.P_hat,
                                &dw.dps_.P_hat_0,
                                &dw.dps_.T_s_target,
                                &dw.dps_.t_s})
            nonnegative(*row);
        for (const auto* row :
             {&dw.depth1_, &dw.depth2_, &dw.depth_mid_, &dw.velocity_, &dw.froude_, &dw.sigma_,
              &dw.dqdh_, &dw.new_flow_, &dw.q1_, &dw.surf_area1_, &dw.surf_area2_, &dw.hrad1_,
              &dw.width1_, &dw.width2_, &dw.xnode_.sumdqdh, &storages.exfil_rate,
              &outfalls.ramp_2d})
            finite(*row);
        for (const auto* row : {&dw.bypassed_, &dw.xnode_.converged, &dw.xnode_.is_surcharged,
                                &dw.dps_.surcharged, &dw.tpa_latch_, &dw.tpa_latch_changed_,
                                &conduits.normal_flow_limited, &conduits.inlet_control})
            for (auto v : *row)
                require(v <= 1, "Invalid hydraulic checkpoint flag.");
        for (double t : dw.dps_.t_s)
            require(t <= dw.sim_time_, "Future hydraulic surcharge timestamp.");
        for (double p : dw.dps_.P)
            require(p >= 1, "Invalid hydraulic DPS pressure number.");
        require(dw.dps_.P_hat_0 == router.dw_solver_.dps_.P_hat_0 &&
                    dw.dps_.T_s_target == router.dw_solver_.dps_.T_s_target,
                "Hydraulic checkpoint changed DPS model constants.");
        for (double v : outfalls.head_2d)
            require(!std::isinf(v), "Invalid hydraulic outfall head.");
        for (auto v : conduits.full_state)
            require(v >= 0 && v <= 7, "Invalid hydraulic full-state mask.");
        for (auto v : links.flow_class)
            require(int(v) >= 0 && int(v) <= int(FlowClass::DN_CRITICAL),
                    "Invalid hydraulic flow class.");
        for (auto v : links.is_closed)
            require(v <= 1, "Invalid hydraulic closed flag.");
        for (std::size_t i = 0; i < nodes.depth.size(); ++i)
            require(close(nodes.head[i], nodes.invert_elev[i] + nodes.depth[i]),
                    "Hydraulic node head/depth differ.");
        for (std::size_t r = 0; r < storages.lid_state.size(); ++r) {
            const auto& s = storages.lid_state[r];
            const auto& initial = c.node_subtypes.storages.lid_state[r];
            require(s.richards == initial.richards &&
                        s.infiltration_cell == initial.infiltration_cell,
                    "Hydraulic checkpoint LID formulation differs.");
            require(s.quality_mass.empty() && s.quality_outlet_conc.empty() &&
                        (((time == 0 || router.checkpoint_owned_session_) &&
                          s.quality_old_water.empty()) ||
                         s.quality_old_water.size() == s.cells.size()),
                    "Unqualified hydraulic checkpoint LID quality shape.");
            require(s.thermal_energy.empty() || s.thermal_energy.size() == s.cells.size(),
                    "Hydraulic checkpoint LID thermal-energy shape differs.");
            require(s.thermal_ground_contact_j.empty() ||
                        s.thermal_ground_contact_j.size() == s.cells.size(),
                    "Hydraulic checkpoint LID ground-contact receipt shape differs.");
            require(
                s.thermal_ground_contact_j.empty() || s.thermal_energy.size() == s.cells.size(),
                "Hydraulic checkpoint LID ground-contact receipt has no matching thermal state.");
            finite(s.richards_water);
            finite(s.richards_pressure);
            finite(s.port_delta);
            finite(s.quality_old_water);
            finite(s.thermal_energy);
            finite(s.thermal_ground_contact_j);
            double held = 0;
            for (std::size_t i = 0; i < s.cells.size(); ++i) {
                const auto& cell = s.cells[i];
                const double poreRoundoff = 32 * std::numeric_limits<double>::epsilon();
                require(std::isfinite(cell.theta) && cell.theta >= -poreRoundoff &&
                            cell.theta <= cell.porosity + poreRoundoff,
                        ("Hydraulic checkpoint LID moisture is outside pores: " +
                         std::to_string(cell.theta) + "/" + std::to_string(cell.porosity))
                            .c_str());
                if (!s.quality_old_water.empty())
                    require(s.quality_old_water[i] >= -poreRoundoff * cell.geometric_volume,
                            "Negative hydraulic checkpoint LID previous water inventory.");
                if (s.richards) {
                    require(s.richards_water[i] >= 0,
                            "Negative hydraulic checkpoint Richards storage.");
                    if (i) {
                        auto stack = lidnode::layers(c, storages.lid[r].control);
                        const auto& p = stack[cell.layer - 1].retention;
                        double head = richards::pressure(
                            p, cell.porosity, s.richards_water[i] / cell.geometric_volume);
                        require(close(s.richards_pressure[i] * .3048, head) &&
                                    close(cell.theta, richards::theta(p, cell.porosity, head)),
                                "Hydraulic checkpoint Richards water/pressure/moisture differ.");
                    } else
                        require(s.richards_water[i] == 0 && cell.theta == 0,
                                "Hydraulic LID surface contains retained Richards water.");
                    held += s.richards_water[i];
                } else
                    held += cell.theta * cell.geometric_volume;
            }
            const auto& g = s.surface_infil;
            for (double v :
                 {s.richards_bottom_loss, s.infiltration_head, s.quality_old_mobile, s.mobile_delta,
                  s.held_volume, s.evap_volume, s.captured_flow, s.treated_volume, g.S, g.Ks,
                  g.IMDmax, g.IMD, g.F, g.Fu, g.Fumax, g.Lu, g.T})
                require(std::isfinite(v), "Nonfinite hydraulic checkpoint LID history.");
            require(close(held, s.held_volume) && s.held_volume >= 0 && s.evap_volume >= 0 &&
                        s.richards_bottom_loss >= 0 && s.treated_volume >= 0 &&
                        s.captured_flow == 0 && s.quality_old_mobile >= 0 &&
                        s.infiltration_head >= 0 && g.S >= 0 && g.Ks >= 0 && g.IMDmax >= 0 &&
                        g.IMDmax <= 1 && g.IMD >= 0 && g.IMD <= g.IMDmax && g.F >= 0 && g.Fu >= 0 &&
                        g.Fumax >= 0 && g.Lu >= 0 && g.Fu <= g.Fumax + 1.e-12,
                    "Invalid/unclosed hydraulic checkpoint LID inventory/history.");
            require(g.S == initial.surface_infil.S && g.Ks == initial.surface_infil.Ks &&
                        g.IMDmax == initial.surface_infil.IMDmax &&
                        g.Lu == initial.surface_infil.Lu && g.Fumax == initial.surface_infil.Fumax,
                    "Hydraulic checkpoint changed LID infiltration parameters.");
            auto validCell = [&](int index) { return index >= -5 && index < int(s.cells.size()); };
            for (const auto& t : s.quality_transfers)
                require(validCell(t.from) && validCell(t.to) && std::isfinite(t.volume) &&
                            t.volume >= 0,
                        "Invalid hydraulic checkpoint LID water transfer.");
            std::vector<double> portBalance(s.cells.size(), 0);
            for (const auto& t : s.quality_ports) {
                require(t.link >= 0 && t.link < c.n_links() && t.cell >= 0 &&
                            t.cell < int(s.cells.size()) && std::isfinite(t.volume),
                        "Invalid hydraulic checkpoint LID port transfer.");
                const int n = storages.node_idx[r];
                require((links.node1[t.link] == n || links.node2[t.link] == n) &&
                            lidnode::portCell(s, lidnode::portOffset(c, t.link, n)) == t.cell,
                        "Hydraulic checkpoint LID port is attached to another node/cell.");
                portBalance[t.cell] += t.volume;
            }
            std::vector<double> retained = s.quality_old_water;
            for (const auto& t : s.quality_transfers) {
                if (!retained.empty() && t.from >= 0)
                    retained[t.from] -= t.volume;
                if (!retained.empty() && t.to >= 0)
                    retained[t.to] += t.volume;
            }
            for (std::size_t i = 0; i < s.cells.size(); ++i) {
                // Classical node continuity may additionally withdraw retained
                // water to fill new conduit storage (DynamicWave::setNodeDepth).
                // This is carried in port_delta, but is not a layer-port flow.
                // Mesh columns consume and clear their trial deltas at the
                // joint acceptance boundary. Their immutable port receipts
                // still describe the accepted interval and close its ledger.
                const bool mesh =
                    s.richards && storages.lid[r].bottom == LidNodeBottom::MeshAquifer;
                const double delta = mesh ? portBalance[i] : s.port_delta[i];
                require(mesh         ? s.port_delta[i] == 0
                        : s.richards ? close(portBalance[i], delta)
                                     : (delta <= portBalance[i] || close(portBalance[i], delta)),
                        "Hydraulic checkpoint LID port/continuity ledger differs.");
                if (!retained.empty()) {
                    double final = s.richards ? s.richards_water[i]
                                              : s.cells[i].theta * s.cells[i].geometric_volume;
                    // BDF1 accepts two half-step Newton residuals per step;
                    // each is bounded by this fraction of the cell volume.
                    // Preserve that solved state, without enforcing an exact
                    // flux-integral equality the ODE never promises.
                    double budget =
                        1.e-11 * std::max({1., std::abs(retained[i] + delta), std::abs(final)});
                    if (s.richards) {
                        auto stack = lidnode::layers(c, storages.lid[r].control);
                        budget += 2. * s.richards_report.accepted *
                                  std::min(1.e-11, stack.front().flow.atol * 1.e-3) *
                                  s.cells[i].geometric_volume;
                    }
                    require(
                        std::abs(retained[i] + delta - final) <= budget,
                        ("Hydraulic checkpoint LID retained-store transfer balance differs: cell " +
                         std::to_string(i) + " initial+transfers=" +
                         std::to_string(retained[i] + delta) + " final=" + std::to_string(final))
                            .c_str());
                }
            }
            const auto& report = s.richards_report;
            if (s.richards && storages.lid[r].bottom == LidNodeBottom::MeshAquifer) {
                require(
                    s.richards_completed_time == time && !s.richards_mesh_pending_finish &&
                        !s.richards_mesh_routed && std::isfinite(s.richards_mesh_start) &&
                        s.richards_mesh_start >= 0 && s.richards_mesh_start <= time &&
                        std::isfinite(report.bottom_outflow) && report.bottom_outflow >= 0 &&
                        std::isfinite(report.bottom_inflow) && report.bottom_inflow >= 0 &&
                        std::isfinite(report.inflow) && report.inflow >= 0 &&
                        close(report.bottom_volume, report.bottom_outflow - report.bottom_inflow),
                    "Invalid hydraulic checkpoint completed LID bed receipt.");
            }
            require(report.ok && report.error.empty() && report.accepted >= 0 &&
                        report.accepted <= 200000 && report.rejected >= 0 && report.rhs >= 0 &&
                        report.newton >= 0 && std::isfinite(report.min_step) &&
                        report.min_step >= 0 && std::isfinite(report.bottom_volume) &&
                        std::isfinite(report.evaporation) && report.evaporation >= 0 &&
                        std::isfinite(report.balance),
                    "Invalid hydraulic checkpoint Richards diagnostic.");
            if (s.richards && time > 0 &&
                (!router.checkpoint_owned_session_ || !s.quality_old_water.empty()))
                require(report.accepted > 0 && report.min_step > 0 &&
                            (report.min_step <= (router.checkpoint_owned_session_
                                                     ? time
                                                     : time - old_elapsed / 1000) ||
                             close(report.min_step, time - old_elapsed / 1000)) &&
                            report.rhs > 0 && report.newton > 0,
                        "Hydraulic checkpoint lacks an accepted Richards interval.");
            for (const auto& t : report.transfers)
                require(validCell(t.from) && validCell(t.to) && std::isfinite(t.volume) &&
                            t.volume >= 0,
                        "Invalid hydraulic checkpoint Richards transfer.");
        }
        // Recompute LID-dependent mobile capacity on a private context so a
        // saved retained store cannot silently disagree with its node geometry.
        SimulationContext geometry;
        geometry.nodes = nodes;
        geometry.node_subtypes.storages = storages;
        geometry.node_subtypes.subtype_row = c.node_subtypes.subtype_row;
        const int us = ucf::getUnitSystem(int(c.options.flow_units));
        for (std::size_t r = 0; r < storages.lid_state.size(); ++r)
            if (!storages.lid_state[r].cells.empty()) {
                int n = storages.node_idx[r];
                geometry.nodes.full_volume[n] = 0;
                double full = node::getVolume(geometry.nodes, n, nodes.full_depth[n], nullptr, us,
                                              &geometry.node_subtypes);
                require(close(nodes.full_volume[n], full) && close(nodes.rpt_full_volume[n], full),
                        "Hydraulic checkpoint LID/mobile capacity differs.");
            }
    }
    /**
     * @brief Move a fully validated restart candidate into its matching live Router/context.
     *
     * Consumes candidate vectors with intended nonthrowing moves; borrowed views into replaced
     * live/candidate arrays invalidate. Sets accepted checkpoint time/readiness and clears
     * prepared/step-finished flags. Requires prior shape/model/semantic validation and exclusive
     * access; performs no file publication or rollback.
     *
     * @param[in,out] r Borrowed matching Router; selected DW/xnode/DPS arrays, clocks and checkpoint
     *     readiness are replaced.
     * @param[in,out] c Borrowed matching context; native state/exports/forcing clocks are
     *     moved/assigned, preserving model configuration.
     */
    void install(Router& r, SimulationContext& c) noexcept {
        c.outfall_exports = std::move(exports);
#define MOVE(v)                                                                                    \
    static_assert(std::is_nothrow_move_assignable_v<decltype(nodes.v)>);                           \
    c.nodes.v = std::move(nodes.v);
        NODE_STATE(MOVE)
#undef MOVE
#define MOVE(v)                                                                                    \
    static_assert(std::is_nothrow_move_assignable_v<decltype(links.v)>);                           \
    c.links.v = std::move(links.v);
        LINK_STATE(MOVE)
#undef MOVE
#define MOVE(v) c.link_subtypes.conduits.v = std::move(conduits.v);
        CONDUIT_STATE(MOVE)
#undef MOVE
        auto& s = c.node_subtypes.storages;
        s.evap_loss = std::move(storages.evap_loss);
        s.exfil_loss = std::move(storages.exfil_loss);
        s.exfil_rate = std::move(storages.exfil_rate);
        s.lid_state = std::move(storages.lid_state);
        c.node_subtypes.outfalls.head_2d = std::move(outfalls.head_2d);
        c.node_subtypes.outfalls.ramp_2d = std::move(outfalls.ramp_2d);
#define MOVE(v) r.dw_solver_.v = std::move(dw.v);
        DW_STATE(MOVE)
#undef MOVE
        r.dw_solver_.xnode_ = std::move(dw.xnode_);
        r.dw_solver_.dps_ = std::move(dw.dps_);
        c.current_time = time;
        c.current_date = date;
        c.elapsed_ms = elapsed;
        c.old_elapsed_ms = old_elapsed;
        c.next_report_ms = next_report;
        c.climate_state.evap_rate = evap;
        c.climate_state.infil_factor = infil_factor;
        c.climate_state.recovery_factor = recovery_factor;
        r.lstep_enabled_ = link_steps;
        r.lstep_rs_ = link_step_span;
        r.checkpoint_time_ = r.checkpoint_step_end_ = time;
        r.checkpoint_ready_ = true;
        r.checkpoint_prepared_ = r.checkpoint_step_finished_ = false;
    }
};

void Router::checkCheckpointProfile(const SimulationContext& c) const {
    require(model_ == RouteModel::DYNWAVE && c.options.routing_model == RoutingModel::DYNWAVE &&
                c.options.surcharge_method >= 0 && c.options.surcharge_method <= 3 &&
                dw_solver_.groups_ == &groups_ && dw_solver_.n_nodes_ == c.n_nodes() &&
                dw_solver_.n_links_ == c.n_links() && c.nodes.count() == c.n_nodes() &&
                c.links.count() == c.n_links(),
            "Hydraulic checkpoint requires an initialized DW routing component.");
    const bool no_hydraulic_quality_state =
        c.n_nodes() == 0 && c.n_links() == 0 && c.n_subcatches() == 0;
    require(
        (no_hydraulic_quality_state ||
         (checkpoint_owned_session_ && detail::checkpointNetworkPollutants(c)) ||
         (c.n_pollutants() == 0 && !c.options.water_age && c.nodes.conc_n_pollutants == 0 &&
          c.links.conc_n_pollutants == 0 && c.nodes.stat_n_pollutants == 0 &&
          c.links.stat_n_pollutants == 0)) &&
            (!c.options.heat_transport || (c.n_nodes() == 0 && c.n_links() == 0) ||
             checkpoint_owned_session_) &&
            (c.n_nodes() == 0 || checkpoint_owned_session_ ||
             (!c.nodes.coupling_tuple_age && !c.nodes.coupling_tuple_temp)) &&
            (!structures_ || checkpoint_owned_session_),
        "Hydraulic checkpoint quality, age, heat and structure/control solvers are not qualified.");
    require(c.nodes.aquifer2d_bed.empty() ||
                c.nodes.aquifer2d_bed.size() == std::size_t(c.n_nodes()),
            "Hydraulic checkpoint node-bed flag shape differs.");
    for (int n = 0; n < c.n_nodes(); ++n) {
        require(
            (c.nodes.type[n] == NodeType::JUNCTION || c.nodes.type[n] == NodeType::STORAGE ||
             c.nodes.type[n] == NodeType::OUTFALL) &&
                !c.nodes.is_virtual[n] && !c.nodes.is_inlet[n] &&
                (c.nodes.aquifer2d_bed.empty() || !c.nodes.aquifer2d_bed[n] ||
                 checkpoint_aquifer_session_),
            "Hydraulic checkpoint dividers, virtual/inlet junctions and aquifer-owned node beds are not qualified.");
        if (c.nodes.type[n] == NodeType::STORAGE) {
            int r = c.node_subtypes.storage_row(n);
            require(r >= 0, "Hydraulic checkpoint storage join is missing.");
            require(c.node_subtypes.storages.shape[r] != StorageShape::TABULAR,
                    "Hydraulic checkpoint tabulated storage is not qualified.");
        }
        if (c.nodes.type[n] == NodeType::OUTFALL) {
            int r = c.node_subtypes.outfall_row(n);
            require(r >= 0, "Hydraulic checkpoint outfall join is missing.");
            auto kind = c.node_subtypes.outfalls.bc_type[r];
            require(kind == OutfallType::FREE || kind == OutfallType::NORMAL ||
                        kind == OutfallType::FIXED,
                    "Hydraulic checkpoint time-dependent outfall providers are not qualified.");
        }
    }
    const auto& sub = c.link_subtypes;
    require(checkpoint_owned_session_
                ? structures_ && sub.conduits.count() + sub.pumps.count() + sub.orifices.count() +
                                         sub.weirs.count() + sub.outlets.count() ==
                                     c.n_links()
                : sub.conduits.count() == c.n_links(),
            "Hydraulic checkpoint link subtype layout differs.");
    for (int j = 0; j < c.n_links(); ++j) {
        int row = -1;
        switch (c.links.type[j]) {
        case LinkType::CONDUIT:
            row = sub.conduit_row(j);
            break;
        case LinkType::PUMP:
            if (checkpoint_owned_session_)
                row = sub.pump_row(j);
            break;
        case LinkType::ORIFICE:
            if (checkpoint_owned_session_)
                row = sub.orifice_row(j);
            break;
        case LinkType::WEIR:
            if (checkpoint_owned_session_)
                row = sub.weir_row(j);
            break;
        case LinkType::OUTLET:
            if (checkpoint_owned_session_)
                row = sub.outlet_row(j);
            break;
        }
        require(row >= 0 && c.links.node1[j] >= 0 && c.links.node1[j] < c.n_nodes() &&
                    c.links.node2[j] >= 0 && c.links.node2[j] < c.n_nodes(),
                "Hydraulic checkpoint link topology differs.");
        auto shape = c.links.xsect_shape[j];
        require(shape != XsectShape::IRREGULAR && shape != XsectShape::CUSTOM &&
                    shape != XsectShape::STREET_XSECT,
                "Hydraulic checkpoint external geometry tables are not qualified.");
    }
    require(c.node_subtypes.storages.lid_state.size() == c.node_subtypes.storages.node_idx.size(),
            "Hydraulic checkpoint LID storage shape differs.");
    require(c.node_subtypes.storages.lid.size() == c.node_subtypes.storages.lid_state.size(),
            "Hydraulic checkpoint LID configuration shape differs.");
    for (std::size_t r = 0; r < c.node_subtypes.storages.lid_state.size(); ++r) {
        const auto& state = c.node_subtypes.storages.lid_state[r];
        require(state.port_delta.size() == state.cells.size() &&
                    (!state.richards || (state.richards_water.size() == state.cells.size() &&
                                         state.richards_pressure.size() == state.cells.size())),
                "Hydraulic checkpoint LID cell shape differs.");
        require(state.thermal_energy.empty() || state.thermal_energy.size() == state.cells.size(),
                "Hydraulic checkpoint LID thermal-energy shape differs.");
        require(state.thermal_ground_contact_j.empty() ||
                    state.thermal_ground_contact_j.size() == state.cells.size(),
                "Hydraulic checkpoint LID ground-contact receipt shape differs.");
        require(state.thermal_ground_contact_j.empty() ||
                    state.thermal_energy.size() == state.cells.size(),
                "Hydraulic checkpoint LID ground-contact receipt has no matching thermal state.");
        finite(state.thermal_energy);
        finite(state.thermal_ground_contact_j);
        const int control = c.node_subtypes.storages.lid[r].control;
        if (control >= 0) {
            auto stack = lidnode::layers(c, control);
            require(!stack.empty() && lidnode::validateStack(stack).empty() &&
                        !state.cells.empty() && state.richards == stack.front().flow.enabled,
                    "Hydraulic checkpoint requires initialized matching LID cells/materials.");
        } else
            require(state.cells.empty(), "Hydraulic checkpoint has unassigned LID cells.");
    }
}
std::string Router::checkpointModel(const SimulationContext& c) const {
    Archive a;
    auto o = c.options;
    a(o.start_date, o.end_date, o.total_duration_ms, o.report_start, o.routing_step,
      o.min_routing_step, o.report_step, o.variable_step, o.lengthening_step, o.flow_units,
      o.routing_model, o.surcharge_method, o.node_continuity, o.anderson_accel, o.head_tol,
      o.max_trials, o.min_surf_area, o.normal_flow_ltd, o.force_main_eqn, o.inertial_damping,
      o.allow_ponding, o.dps_target_celerity, o.dps_alpha, o.dps_decay_time, o.unsteady_friction,
      o.uf_k3, o.tpa_celerity, o.virtual_junction_momentum, o.ignore_quality, o.water_age,
      o.heat_transport);
    auto nodes = c.nodes;
    auto links = c.links;
    auto st = c.node_subtypes.storages;
    auto out = c.node_subtypes.outfalls;
    auto cd = c.link_subtypes.conduits;
    auto ns = c.node_subtypes.subtype_row, ls = c.link_subtypes.subtype_row;
    auto coupled = c.coupled_node;
#define FIELD(v) a(nodes.v);
    NODE_MODEL(FIELD)
#undef FIELD
#define FIELD(v) a(links.v);
    LINK_MODEL(FIELD)
#undef FIELD
#define FIELD(v) a(st.v);
    STORAGE_MODEL(FIELD)
#undef FIELD
#define FIELD(v) a(cd.v);
    CONDUIT_MODEL(FIELD)
#undef FIELD
    detail::networkPollutantModel(a, c);
    a(ns, ls, coupled, nodes.aquifer2d_bed, out.node_idx, out.bc_type, out.param, out.param_name,
      out.has_flap_gate, out.route_to, out.link_idx, out.link_offset);
    for (int n = 0; n < c.n_nodes(); ++n) {
        auto name = c.node_names.name_of(n);
        a(name);
    }
    for (int j = 0; j < c.n_links(); ++j) {
        auto name = c.link_names.name_of(j);
        a(name);
    }
    for (std::size_t r = 0; r < st.lid.size(); ++r) {
        a(st.lid[r].control, st.lid[r].initial_saturation);
        // Keep CURRENT fingerprints byte-compatible with qualified R4 models.
        if (st.lid[r].bottom != LidNodeBottom::Current) {
            std::string bottom_owner = "LID bottom owner";
            a(bottom_owner, st.lid[r].bottom);
        }
        auto layers = lidnode::layers(c, st.lid[r].control);
        a.sequence(layers, [&](auto& layer) {
            auto& m = layer.retention;
            auto& f = layer.flow;
            a(layer.kind, layer.params, m.theta_r, m.alpha, m.n, m.l, m.specific_storage, f.enabled,
              f.cells_per_layer, f.atol, f.rtol, f.max_step);
            a.sequence(layer.treatment,
                       [&](auto& t) { a(t.pollutant, t.removal, t.decay, t.expression); });
        });
        a.sequence(st.lid_state[r].cells, [&](auto& cell) {
            a(cell.bottom, cell.top, cell.geometric_volume, cell.area, cell.porosity,
              cell.field_capacity, cell.wilting_point, cell.conductivity, cell.conductivity_slope,
              cell.suction, cell.kind, cell.layer);
        });
    }
    auto outlets = c.lid_node_outlets;
    a.sequence(outlets, [&](auto& p) { a(p.link, p.layer, p.top); });
    auto d = dw_solver_;
    a(d.num_threads_, d.head_tol, d.max_trials, d.omega, d.surcharge_method, d.node_continuity,
      d.anderson_accel, d.unsteady_friction, d.uf_k3, d.tpa_celerity, d.dps_config_.c_pT,
      d.dps_config_.alpha, d.dps_config_.r);
    if (checkpoint_owned_session_) {
        std::string owner = "engine session owns structures and controls";
        a(owner);
    }
    require(a.bytes().size() <= maxBytes, "Hydraulic checkpoint model exceeds its size limit.");
    return a.bytes();
}
void Router::enableCheckpointing(SimulationContext& c) {
    require(!checkpoint_context_ && c.current_time == 0,
            "Enable hydraulic checkpointing at the initial routing boundary.");
    checkCheckpointProfile(c);
    auto model = checkpointModel(c);
    RestartState s(*this, c);
    s.validate(*this, c);
    checkpoint_model_ = std::move(model);
    checkpoint_context_ = &c;
    checkpoint_time_ = checkpoint_step_end_ = 0;
    checkpoint_ready_ = true;
    checkpoint_prepared_ = checkpoint_step_finished_ = false;
}
void Router::beginCheckpointStep(SimulationContext& c, double dt) {
    require(&c == checkpoint_context_ && checkpoint_ready_ && c.current_time == checkpoint_time_ &&
                std::isfinite(dt) && dt > 0 && std::isfinite(c.current_time + dt) &&
                c.current_time + dt > c.current_time &&
                (c.options.total_duration_ms < 0 ||
                 1000 * (c.current_time + dt) <= c.options.total_duration_ms ||
                 (checkpoint_owned_session_ &&
                  close(1000 * (c.current_time + dt), c.options.total_duration_ms))),
            "Tracked hydraulic preparation requires an accepted chronological boundary.");
    checkCheckpointProfile(c);
    require(checkpoint_model_ == checkpointModel(c),
            "Hydraulic checkpoint model changed after attachment.");
    checkpoint_step_end_ = c.current_time + dt;
    checkpoint_ready_ = false;
    checkpoint_prepared_ = true;
    checkpoint_step_finished_ = false;
}
void Router::checkCheckpointStep(SimulationContext& c, double dt, double evap_rate, bool callback,
                                 bool owned_callback) {
    require(
        &c == checkpoint_context_ && checkpoint_prepared_ && !checkpoint_ready_ &&
            c.current_time == checkpoint_time_ && std::isfinite(dt) && dt > 0 &&
            c.current_time + dt == checkpoint_step_end_ && std::isfinite(evap_rate) &&
            evap_rate >= 0 && (!callback || (checkpoint_owned_session_ && owned_callback)),
        "Tracked hydraulic step requires prior preparation, its scheduled duration and no external callbacks.");
    checkCheckpointProfile(c);
    require(checkpoint_model_ == checkpointModel(c),
            "Hydraulic checkpoint model changed after attachment.");
    checkpoint_prepared_ = false; // any subsequent exception leaves an unaccepted boundary
}
void Router::acceptCheckpointBoundary(SimulationContext& c) {
    require(
        &c == checkpoint_context_ && !checkpoint_ready_ && checkpoint_step_finished_ &&
            c.current_time == checkpoint_step_end_,
        "Hydraulic checkpoint acceptance requires postprocessing and the completed routing clock.");
    checkCheckpointProfile(c);
    require(checkpoint_model_ == checkpointModel(c),
            "Hydraulic checkpoint model changed after attachment.");
    RestartState s(*this, c);
    s.validate(*this, c);
    checkpoint_time_ = c.current_time;
    checkpoint_ready_ = true;
    checkpoint_step_finished_ = false;
}
std::string Router::checkpointBytes(const SimulationContext& c, std::string& result) const {
    try {
        require(&c == checkpoint_context_ && checkpoint_ready_ &&
                    c.current_time == checkpoint_time_,
                "Hydraulic checkpoint requires a completed accepted routing boundary.");
        checkCheckpointProfile(c);
        require(checkpoint_model_ == checkpointModel(c),
                "Hydraulic checkpoint model changed after attachment.");
        RestartState s(*this, c);
        s.validate(*this, c);
        Archive payload;
        s.archive(payload, true, true);
        auto bytes = payload.bytes(), model = checkpoint_model_;
        auto crc = checksum(bytes);
        std::string magic = magicName;
        uint32_t version = 5;
        Archive out;
        out(magic, version, model, crc, bytes);
        require(out.bytes().size() <= maxBytes, "Hydraulic checkpoint exceeds its size limit.");
        result = out.bytes();
        return {};
    } catch (const std::exception& e) {
        return e.what();
    }
}
std::string Router::saveCheckpoint(const SimulationContext& c, const std::string& path) const {
    try {
        std::string bytes;
        const auto error = checkpointBytes(c, bytes);
        if (!error.empty())
            return error;
        publish(path, bytes);
        return {};
    } catch (const std::exception& e) {
        return e.what();
    }
}
std::shared_ptr<Router::RestartState>
Router::prepareCheckpointBytes(const SimulationContext& c, std::string bytes,
                               const std::array<double, 6>* boundary, const double* delivery,
                               const OutfallData* feedback) const {
    require(&c == checkpoint_context_,
            "Hydraulic checkpoint restore requires an enabled routing component.");
    checkCheckpointProfile(c);
    require(checkpoint_model_ == checkpointModel(c), "Hydraulic checkpoint target model changed.");
    require(!bytes.empty() && bytes.size() <= maxBytes,
            "Hydraulic checkpoint exceeds its size limit.");
    Archive in(std::move(bytes));
    std::string magic, model, payload;
    uint32_t version = 0, crc = 0;
    in(magic, version, model, crc, payload);
    in.finish();
    require(magic == magicName && (version == 3 || version == 4 || version == 5) &&
                model == checkpoint_model_ && crc == checksum(payload),
            "Hydraulic checkpoint format/model/checksum differs.");
    require(version == 3 || version == 4 || version == 5,
            "Hydraulic checkpoint version is unsupported.");
    if (version == 3)
        for (const auto& lid : c.node_subtypes.storages.lid_state)
            require(lid.thermal_energy.empty(),
                    "Legacy hydraulic checkpoint cannot restore LID thermal energy.");
    if (version < 5)
        for (const auto& lid : c.node_subtypes.storages.lid_state)
            require(lid.thermal_ground_contact_j.empty(),
                    "Legacy hydraulic checkpoint cannot restore LID ground-contact receipts.");
    auto s = std::make_shared<RestartState>(*this, c);
    Archive state(std::move(payload));
    s->archive(state, version >= 4, version >= 5);
    state.finish();
    s->validate(*this, c);
    if (boundary)
        require(*boundary == std::array<double, 6>{s->time, s->date, s->elapsed, s->old_elapsed,
                                                   s->next_report, s->evap},
                "Joint hydraulic/surface checkpoint clocks or shared forcing differ.");
    if (delivery)
        validateJointDelivery(s->nodes, c.coupled_node, *delivery);
    if (feedback) {
        const auto& heads = feedback->head_2d;
        require(heads.size() == s->outfalls.head_2d.size() &&
                    feedback->ramp_2d == s->outfalls.ramp_2d,
                "Joint outfall feedback shape or wetness differs.");
        for (std::size_t row = 0; row < heads.size(); ++row)
            require(heads[row] == s->outfalls.head_2d[row] ||
                        (std::isnan(heads[row]) && std::isnan(s->outfalls.head_2d[row])),
                    "Joint hydraulic/surface tailwater feedback differs.");
    }
    return s;
}
void Router::validateSessionStorage(const std::shared_ptr<RestartState>& s,
                                    const SimulationContext& c, double expected, bool routed) {
    // Same reported mobile + retained storage convention as SWMMEngine.
    double volume = 0;
    for (std::size_t n = 0; n < s->nodes.depth.size(); ++n) {
        const auto& nodes = s->nodes;
        if (nodes.type[n] == NodeType::STORAGE) {
            volume += nodes.volume[n];
            const int row = c.node_subtypes.storage_row(static_cast<int>(n));
            const auto& lid = s->storages.lid_state[static_cast<std::size_t>(row)];
            require(!routed || lid.quality_old_water.size() == lid.cells.size(),
                    "Engine routed LID previous-water shape differs.");
            volume += lid.held_volume;
        } else if (nodes.full_depth[n] > 0) {
            const bool can_pond =
                c.options.allow_ponding || (n < c.coupled_node.size() && c.coupled_node[n] != 0);
            volume += (can_pond && nodes.ponded_area[n] > 0 && nodes.depth[n] > nodes.full_depth[n])
                          ? nodes.volume[n]
                          : nodes.rpt_full_volume[n] * (nodes.depth[n] / nodes.full_depth[n]);
        }
    }
    for (double v : s->links.volume)
        volume += v;
    require(std::isfinite(expected) && close(volume, expected),
            "Engine continuity storage differs from hydraulic inventories.");
}
void Router::validateJointDelivery(const NodeData& nodes, const std::vector<uint8_t>& coupled,
                                   double delivery) {
    require(std::isfinite(delivery) && delivery >= 0 &&
                coupled.size() == std::size_t(nodes.count()) &&
                nodes.coupling_queue.size() == coupled.size() &&
                nodes.coupling_volume.size() == coupled.size() &&
                nodes.coupling_inflow.size() == coupled.size(),
            "Joint checkpoint delivery layout differs.");
    for (std::size_t n = 0; n < coupled.size(); ++n)
        require(
            coupled[n] <= 1 && std::isfinite(nodes.coupling_queue[n]) &&
                nodes.coupling_volume[n] == 0 &&
                (coupled[n] || (nodes.coupling_queue[n] == 0 && nodes.coupling_inflow[n] == 0)) &&
                (delivery > 0 || nodes.coupling_queue[n] == 0),
            "Joint checkpoint has unpaired or undeliverable node exchange.");
}
void Router::installCheckpoint(SimulationContext& c, std::shared_ptr<RestartState> s) noexcept {
    s->install(*this, c);
}
std::string Router::loadCheckpoint(SimulationContext& c, const std::string& path) {
    try {
        auto s = prepareCheckpointBytes(c, readFile(path));
        installCheckpoint(c, std::move(s));
        return {};
    } catch (const std::exception& e) {
        return e.what();
    }
}
#undef NODE_MODEL
#undef LINK_MODEL
#undef NODE_STATE
#undef LINK_STATE
#undef CONDUIT_MODEL
#undef CONDUIT_STATE
#undef STORAGE_MODEL
#undef DW_STATE
} // namespace openswmm
