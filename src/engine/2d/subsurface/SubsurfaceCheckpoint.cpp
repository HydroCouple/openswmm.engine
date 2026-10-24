// SPDX-License-Identifier: Apache-2.0

/**
 * @file      SubsurfaceCheckpoint.cpp
 * @brief     Checkpoint persistence for subsurface water and transport state.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include "SubsurfaceSolver.hpp"
#include "../data/MeshData.hpp"
#include "../data/SolverOptions2D.hpp"
#include "../solver/InertialEdges.hpp"
#include "../heat/ThermalCheckpoint.hpp"
#include "../../core/CompletedSourceArchive.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace openswmm::twoD {
namespace {
/**
 * @brief Archive one reviewed surface intake's donor identity and paired water/native payload.
 * @param[in,out] a Completed-source archive in read or write mode.
 * @param[in,out] r Intake receipt; read replaces request metadata, elapsed start/end s, volume
 * m3 and native inventory tuple, while write reads it without semantic mutation.
 * @note Serialization only; enclosing checkpoint validates ownership, row shape,
 * signs/finiteness, provenance and clock consistency. Archive failure can leave partial read
 * fields; caller stages restore.
 */
void intake(detail::CompletedSourceArchive& a, SurfaceIntakeReceipt& r) {
    auto& q = r.request;
    a(q.kind, q.source, q.unit, q.cell, q.area, q.pond, q.water_ceiling, q.candidate, r.start,
      r.end, r.volume, r.mass);
}
/**
 * @brief Archive one reviewed return's kind, original intake and available paired payload.
 * @param[in,out] a Completed-source read/write archive.
 * @param[in,out] r Return receipt with elapsed availability s, m3 water and native inventory
 * vector.
 * @note Calls intake() for provenance then serializes returned values. No physical booking or
 * semantic validation occurs here; failed reads can partially mutate r and enclosing restore
 * stages it.
 */
void returned(detail::CompletedSourceArchive& a, SurfaceReturnReceipt& r) {
    a(r.kind);
    intake(a, r.intake);
    a(r.available_at, r.volume, r.mass);
}
/**
 * @brief Append a resolved groundwater signal's scalar and owned time/value copies to model
 * identity.
 * @param[in,out] a Model-identity archive, expected in write mode.
 * @param[in,out] s Resolved source signal, read for constant and borrowed time/ordinate
 * sequences.
 * @note Copies sequence contents before archiving; elapsed knots are s and ordinate units depend
 * on the resolved water/quality/heat term. On a read archive only constant would be restored:
 * local time/ value copies are not installed into s, so this is not a general signal restore
 * API. Allocates and may throw; enclosing model checkpoint owns validation and ordered identity.
 */
void signal(detail::CompletedSourceArchive& a, GwSourceSignal& s) {
    auto times = s.timePoints(), values = s.ordinates();
    a(s.constant, times, values);
}
}
/**
 * @brief Archive groundwater model identity or mutable completed-owner state without a held AIR
 * override.
 * @param[in,out] a Caller-owned CompletedSourceArchive. Model mode writes deterministic
 * identity; state mode writes or restores into an already initialized matching model.
 * @param[in] model True selects identity-only layout, false the mutable physical/receipt state.
 * @note Delegates to the held-air overload with nullptr, using current native air temperature
 * for qualified AIR model identity. Does not make a staged copy itself; failed state reads can
 * partially mutate this solver. Caller must stage and validate the enclosing whole-engine
 * transaction.
 */
void SubsurfaceSolver::checkpoint(detail::CompletedSourceArchive& a, bool model) {
    checkpoint(a, model, nullptr);
}
/**
 * @brief Archive qualified groundwater identity or completed hydraulic/native/physical receipt
 * state.
 * @param[in,out] a Caller-owned completed-source archive. For model=true use write mode; model
 * identity signal serialization is not a general model reconstruction reader. For model=false
 * read requires identical initialized topology/material/species/forcing identity established
 * externally.
 * @param[in] model True writes physical model, configuration, forcing, thermal/reaction/unit and
 * ownership identity, excluding mutable ledgers; false writes/restores canonical state and
 * receipts.
 * @param[in] held_air Optional readable native engine air temperature degF used only for RK2
 * porous AIR-boundary model identity. Null samples context climate temperature; pointer is never
 * retained.
 * @throws std::runtime_error Unsupported restart profile, unqualified Richards ownership,
 * nonfinite/ negative invalid state, mismatched rows/layers/physical
 * reservations/provenance/clocks/tiers or failed physical thermal archive validation. Archive
 * shape/truncation/allocation errors also propagate.
 * @note State mode includes pending side/native/physical energy packets, staged source/LID
 * receipt queues, per-cell clocks, tier lists/diagnostics and cumulative ledgers; pending water
 * remains paired rather than prematurely settled. Reading resets process-local recharge
 * caches/counters, validates physical/SIGMA owner correspondence and producer reservations, and
 * allows NaN only in designated unpublished diagnostics. Mutable payloads are applied before
 * semantic checks; this routine offers no rollback. Borrowed context/mesh/edges/callbacks remain
 * external and must be live/stable; caller uses a private initialized copy and adopts only after
 * full archive finish/whole-engine validation. No file I/O, water firing, external 1D node
 * mutation or engine calendar advance occurs.
 */
void SubsurfaceSolver::checkpoint(detail::CompletedSourceArchive& a, bool model,
                                  const double* held_air) {
    if (!lid_receipts_enabled_ &&
        std::any_of(richards_bed_owned_.begin(), richards_bed_owned_.end(),
                    [](bool owned) { return owned; }))
        throw std::runtime_error(
            "Richards joint owner checkpoints require the R5 restart extension.");
    auto& s = state_;
    auto& t = tr_;
    auto& o = options_;
    const bool finite_pore_ledgers =
        heat_.active() && std::any_of(heat_.cells.begin(), heat_.cells.end(),
                                      [](const auto& cell) { return cell.porewater.active(); });
    const bool rk2_transport = opts_ && opts_->reconstruction_order == 2 && t.active();
    const char* transport_profile =
        rk2_transport ? "RK2 groundwater transport ledgers v1"
        : t.n_layers == 0 && boundaries_.empty() && reactors_.empty()
            ? (finite_pore_ledgers ? "bulk finite-porewater ledgers v1"
                                   : "bulk shared-source transport clocks v1")
            : "SIGMA chemistry and groundwater edges v2";
    if (model) {
        if (needsTransportRestart()) {
            std::string profile = transport_profile;
            a(profile, t.n_layers, t.c_diff, transport_units_, reaction_identity_);
            if (mesh_) {
                auto x = mesh_->vx, y = mesh_->vy, z = mesh_->vz;
                auto nv = mesh_->cell_nv;
                auto vertices = mesh_->cell_v;
                a(x, y, z, nv, vertices);
            }
            a.sequence(boundaries_, [&](auto& b) {
                a(b.cell, b.edge, b.head, b.length, b.distance);
                signal(a, b.signal);
                a.sequence(b.terms, [&](auto& term) {
                    a(term.row, term.flux, term.scale);
                    signal(a, term.signal);
                });
            });
        }
        if (lid_receipts_enabled_) {
            std::string profile = "completed LID bed receipts v2";
            a(profile, richards_bed_owned_, mobile_lid_bed_);
        }
        a(s.active, s.n_cells, s.m_layers);
        a.fixed(s.Ks, s.zs, s.theta_s, s.theta_r, s.alpha, s.psi_b, s.lambda, s.vg_n, s.vg_L,
                s.c_loss, s.soil_char, s.closure, s.area, s.z_bed);
        a(o.soil_char, o.closure, o.m_layers, o.capillary_diff, o.c_gw, o.c_col,
          o.force_closed_form, o.per_subcatch, o.dunne, o.gw_et, o.wilting_suction,
          o.wilting_suction_set, o.node_auto, o.link_seepage, o.authored);
        a(source_owned_, source_species_, cell_beds_, cell_sources_, cell_nface_,
          subcatch_outlet_node_, cell_subcatch_);
        a.sequence(node_beds_, [&](auto& b) {
            a(b.node, b.cell, b.Kc, b.dC, b.area, b.locate, b.exchange, b.automatic);
        });
        a.sequence(sources_, [&](auto& r) {
            a(r.name, r.scale, r.cells);
            signal(a, r.flow);
            a.sequence(r.terms, [&](auto& v) {
                a(v.row, v.massRate, v.massScale);
                signal(a, v.signal);
            });
        });
        a(t.n_species, t.n_cells, t.n_pollut, t.n_msx, t.age_row, t.temp_row, t.dispersion_on);
        a.fixed(t.row_names, t.alpha_L, t.alpha_T, t.D_m, t.rho_b, t.kd, t.decay);
        auto thermal = checkpoint::groundHeatModelIdentity(heat_);
        a(thermal);
        if (rk2_transport && heat_.active() && heat_.config->options.surface_thermal_bc == "AIR") {
            std::string profile = "RK2 held AIR soil boundary v1";
            double air = held_air ? *held_air : heat_.context->climate_state.temperature;
            a(profile, air);
        }
        if (rk2_transport && heat_.active() && (heat_.top_series >= 0 || heat_.deep_series >= 0)) {
            std::string profile = "RK2 thermal series identity v1";
            a(profile, heat_.context->options.start_date);
            for (int series : {heat_.top_series, heat_.deep_series}) {
                a(series);
                if (series >= 0) {
                    auto x = heat_.context->tables[series].x, y = heat_.context->tables[series].y;
                    a(x, y);
                }
            }
        }
        if (recharge_backend_.evaluate) {
            std::string operator_profile = "host OpenMP CLOSED_FORM recharge v1";
            a(operator_profile);
        }
        return;
    }
    if (a.reading()) {
        // Operator caches/counters are process-local diagnostics. Canonical
        // state is restored below; the next batch recomputes its pure inputs.
        recharge_inputs_.clear();
        recharge_values_.clear();
        std::fill(recharge_slots_.begin(), recharge_slots_.end(), -1);
        recharge_batches_ = recharge_cells_ = 0;
    }
    a.fixed(s.hg, s.hu, s.theta_sigma, s.q0_last, s.qnode_last, s.qlat_last, s.qdeep_last,
            s.qet_last, s.dunne_last, s.qplus_last, s.infil_capacity, s.infil_remaining,
            s.infil_refresh, s.infil_interval, s.wetting_front, s.reject_last, s.reject_cumulative,
            s.dunne_cumulative, s.et_pending, s.et_potential_cumulative, s.et_surface_cumulative,
            s.et_soil_cumulative, s.et_unused_cumulative, s.et_surface_last, s.et_potential_last,
            s.et_stress, s.et_refresh, s.qlink_last, s.dt_cell, s.tier, s.eacc_L, s.eacc_R,
            s.xacc_from_surface, s.xacc_to_surface, s.nacc, s.lacc);

    a(s.led_recharge, s.led_lateral, s.led_deep, s.led_node, s.led_reject, s.led_dunne,
      s.led_caprise, s.led_et, s.led_infil_in, s.led_link, s.led_subcatch_in, s.led_init_storage,
      s.led_final_storage, s.led_source_in, s.led_source_out);
    a.fixed(t.sat_mass, t.unsat_mass, t.sacc_L, t.sacc_R, t.xacc_from_surface, t.xacc_to_surface,
            t.nacc_mass, t.lacc_mass, t.node_out_mass, t.lost_deep, t.lost_node, t.lost_link,
            t.lost_dunne, t.lost_et, t.gained_infil, t.gained_node, t.gained_link, t.net_lateral,
            t.gained_source, t.lost_source, t.lost_reaction, t.init_mass, t.internal_recharge);

    a(t.dispersion_limiter_binds, source_firing_end_, gather_fraction_, cells_by_tier_,
      faces_by_tier_, face_tier_, tier_firings_, pending_surface_, pending_flag_, drain_cap_binds_,
      node_refunds_, accumulators_pending_);
    a.fixed(source_cell_end_, bed_exchange_cum_, bed_gathered_, node_drawn_gw_, node_exchange_vol_);
    a.sequence(
        source_receipts_, [&](auto& rows) { a.sequence(rows, [&](auto& r) { intake(a, r); }); },
        true);
    a.sequence(source_returns_, [&](auto& r) { returned(a, r); });
    if (needsTransportRestart()) {
        std::string profile = transport_profile;
        a(profile);
        if (a.reading() && profile != transport_profile)
            throw std::runtime_error("Unsupported groundwater transport restart profile.");
        a(s.led_boundary_in, s.led_boundary_out, source_completed_end_);
        a.fixed(t.layer_mass, t.contact_water, t.contact_node_out, t.contact_link_out,
                t.contact_node_mass, t.contact_link_mass, t.boundary_in, t.boundary_out,
                t.boundary_flux_in, t.boundary_flux_out, t.lout_mass, t.link_gain_shortfall,
                cell_completed_);
        a(t.generated_age, t.gained_contact);
        a.sequence(
            boundaries_,
            [&](auto& b) {
                a(b.requested_in, b.requested_out, b.applied_in, b.applied_out);
                a.fixed(b.mass_in, b.mass_out, b.flux_in, b.flux_out);
            },
            true);
        if (a.reading()) {
            if (t.layer_mass.size() != std::size_t(t.n_species) * t.n_layers * t.n_cells)
                throw std::runtime_error("SIGMA chemistry restart layer shape differs.");
            if (t.contact_water.size() != std::size_t(t.n_layers) * t.n_cells ||
                (t.n_layers > 0 && (t.contact_node_out.size() != node_beds_.size() ||
                                    t.contact_link_out.size() != std::size_t(t.n_cells))) ||
                t.contact_node_mass.size() != t.layer_mass.size() ||
                t.contact_link_mass.size() != t.layer_mass.size())
                throw std::runtime_error("SIGMA contact restart shape differs.");
            for (const auto* values : {&t.contact_water, &t.contact_node_out, &t.contact_link_out})
                for (double v : *values)
                    if (!std::isfinite(v) || v < 0)
                        throw std::runtime_error("Invalid pending SIGMA contact water.");
            for (int p = 0; p < t.n_species; ++p)
                for (int i = 0; i < t.n_cells; ++i) {
                    double sum = 0;
                    for (int j = 0; j < t.n_layers; ++j) {
                        const double mass = t.layer_mass[t.layerIdx(p, j, i)];
                        if (!std::isfinite(mass) || (!t.signedRow(p) && mass < 0))
                            throw std::runtime_error("Invalid layer chemistry restart inventory.");
                        for (const auto* pending : {&t.contact_node_mass, &t.contact_link_mass}) {
                            const double v = (*pending)[t.layerIdx(p, j, i)];
                            if (!std::isfinite(v) || (!t.signedRow(p) && v < 0) ||
                                (t.contact_water[std::size_t(j) * t.n_cells + i] == 0 && v != 0))
                                throw std::runtime_error("Invalid pending SIGMA contact mass.");
                        }
                        sum += mass;
                    }
                    if (t.n_layers > 0 && sum != t.unsat_mass[t.idx(p, i)])
                        throw std::runtime_error(
                            "SIGMA chemistry restart bulk view differs from its layer owner.");
                }
            for (const auto* values : {&t.boundary_in, &t.boundary_out, &t.boundary_flux_in,
                                       &t.boundary_flux_out, &t.generated_age, &t.gained_contact,
                                       &t.lout_mass, &t.link_gain_shortfall, &cell_completed_})
                for (double value : *values)
                    if (!std::isfinite(value))
                        throw std::runtime_error("Nonfinite transport restart receipt.");
            for (double time : cell_completed_)
                if (time < 0)
                    throw std::runtime_error("Negative completed groundwater clock.");
            if ((!t.generated_age.empty() && t.generated_age.size() != std::size_t(t.n_species)) ||
                t.generated_age.size() != t.gained_contact.size())
                throw std::runtime_error("Age restart ledger shape differs.");
            if ((finite_pore_ledgers || (t.n_layers > 0 && t.age_row >= 0 &&
                                         std::any_of(cell_completed_.begin(), cell_completed_.end(),
                                                     [](double time) { return time > 0; }))) &&
                t.generated_age.size() != std::size_t(t.n_species))
                throw std::runtime_error(
                    "Finite-porewater generation/contact ledgers are missing.");
            if (!rk2_transport && !finite_pore_ledgers && t.n_layers == 0 &&
                !t.generated_age.empty())
                throw std::runtime_error(
                    "Compatibility groundwater has unexpected finite-porewater ledgers.");
            for (std::size_t row = 0; row < t.generated_age.size(); ++row)
                if (t.generated_age[row] < 0 ||
                    (int(row) != t.age_row && t.generated_age[row] != 0))
                    throw std::runtime_error("Invalid age generation ledger.");
        }
    }
    if (lid_receipts_enabled_) {
        std::string lid_receipt_profile = "accepted LID heat receipt state v3";
        a(lid_receipt_profile);
        if (a.reading() && lid_receipt_profile != "accepted LID heat receipt state v3")
            throw std::runtime_error("Unsupported completed LID heat receipt checkpoint profile.");
        a.fixed(cell_completed_, richards_down_cum_, richards_up_cum_, lid_booked_end_,
                lid_bed_factor_, node_pending_down_, node_pending_up_);
        a.sequence(
            lid_bed_diagnostics_,
            [&](auto& d) {
                a(d.start, d.end, d.groundwater_time, d.lid_head, d.groundwater_head,
                  d.requested_down, d.requested_up, d.reserved_down, d.reserved_up, d.accepted_down,
                  d.accepted_up);
            },
            true);
        a.sequence(
            lid_receipts_,
            [&](auto& rows) {
                a.sequence(rows, [&](auto& r) {
                    a(r.start, r.end, r.down, r.up, r.down_j, r.up_j, r.heat_applied);
                });
            },
            true);
    }
    if (heat_.active()) {
        std::string profile = "accepted-boundary porous thermal state v3";
        a(profile);
        checkpoint::groundHeat(a, heat_, false);
        a.fixed(heat_link_out_volume_, heat_preset_out_j_, heat_preset_out_water_,
                heat_node_out_volume_, heat_node_gathered_out_, porewater_preset_out_,
                heat_pending_left_, heat_pending_right_, heat_dt_);
        if (a.reading())
            for (const auto* row :
                 {&heat_link_out_volume_, &heat_preset_out_j_, &heat_preset_out_water_,
                  &heat_node_out_volume_, &heat_node_gathered_out_, &porewater_preset_out_,
                  &heat_pending_left_, &heat_pending_right_, &heat_dt_})
                for (double v : *row)
                    if (!std::isfinite(v))
                        throw std::runtime_error("Nonfinite pending thermal checkpoint receipt.");
        if (a.reading()) {
            const auto nl = std::size_t(heat_.config->options.thermal_layers),
                       nc = std::size_t(state_.n_cells);
            if (!nl || heat_preset_out_j_.size() != nc * nl ||
                heat_preset_out_water_.size() != nc * nl || heat_.cells.size() != nc)
                throw std::runtime_error(
                    "Pending thermal export checkpoint shape differs from its physical layers.");
            for (std::size_t i = 0; i < nc; ++i) {
                const auto& cell = heat_.cells[i];
                if (cell.column.energy.empty()) {
                    for (std::size_t j = 0; j < nl; ++j)
                        if (heat_preset_out_j_[i * nl + j] != 0 ||
                            heat_preset_out_water_[i * nl + j] != 0)
                            throw std::runtime_error(
                                "Unowned groundwater cell has a pending thermal export.");
                    if (!cell.reserved_out_water.empty())
                        throw std::runtime_error(
                            "Unowned groundwater cell has a thermal capacity reservation.");
                    continue;
                }
                if (cell.reserved_out_water.size() != nl)
                    throw std::runtime_error(
                        "Thermal heat-capacity reservation checkpoint shape differs from its physical layers.");
                for (std::size_t j = 0; j < nl; ++j) {
                    const auto k = i * nl + j;
                    if (heat_preset_out_water_[k] != cell.reserved_out_water[j] ||
                        !std::isfinite(heat_.activeCapacity(int(i), j)) ||
                        !(heat_.activeCapacity(int(i), j) > 0))
                        throw std::runtime_error(
                            "Pending thermal export does not match its reserved layer heat capacity.");
                }
            }
        }
        if (a.reading() && tr_.n_layers == 0 && !porewater_preset_out_.empty()) {
            const auto nl = std::size_t(heat_.config->options.thermal_layers),
                       nc = std::size_t(state_.n_cells);
            std::size_t ns = 0;
            for (const auto& cell : heat_.cells)
                ns = std::max(ns, std::size_t(cell.porewater.n_species));
            if (!nl || !ns || porewater_preset_out_.size() != ns * nc * nl)
                throw std::runtime_error(
                    "Pending porewater export checkpoint shape differs from its physical layers.");
            for (std::size_t i = 0; i < nc; ++i)
                for (std::size_t r = 0; r < ns; ++r)
                    for (std::size_t j = 0; j < nl; ++j) {
                        const auto packet = (r * nc + i) * nl + j;
                        const auto& pore = heat_.cells[i].porewater;
                        const double reserved = pore.active() ? pore.reserved_out[r * nl + j] : 0;
                        if (porewater_preset_out_[packet] != reserved)
                            throw std::runtime_error(
                                "Pending porewater export does not match its reserved physical-layer donor.");
                    }
        }
    }
    if (a.reading() && tr_.n_layers > 0 && heat_.active())
        for (int cell = 0; cell < s.n_cells; ++cell)
            if (covers(cell)) {
                const auto& pore = heat_.cells[cell].porewater;
                const auto& m = pore.sigma;
                const auto ne = m.overlaps.size();
                if (m.layers != t.n_layers || m.species != t.n_species)
                    throw std::runtime_error("SIGMA porous checkpoint owner differs.");
                for (int row = 0; row < t.n_species; ++row)
                    if (row != t.temp_row) {
                        const auto nf = heat_.cells[cell].column.theta.size();
                        for (std::size_t physical = 0; physical < nf; ++physical) {
                            double value = 0, reservation = 0;
                            for (std::size_t e = 0; e < ne; ++e)
                                if (m.overlaps[e].physical == int(physical)) {
                                    value += m.mass[std::size_t(row) * ne + e];
                                    reservation += m.reserved[std::size_t(row) * ne + e];
                                }
                            const auto k = std::size_t(row) * nf + physical;
                            if (std::abs(value - pore.mass[k]) >
                                    1e-12 *
                                        std::max({1., std::abs(value), std::abs(pore.mass[k])}) ||
                                std::abs(reservation - pore.reserved_out[k]) >
                                    1e-12 * std::max(1., reservation))
                                throw std::runtime_error(
                                    "SIGMA porous checkpoint physical view differs from its overlap owner.");
                        }
                        for (int reactor = 0; reactor <= t.n_layers; ++reactor) {
                            double total = 0, capacity = 0;
                            for (std::size_t e = 0; e < ne; ++e)
                                if (m.overlaps[e].reactor == reactor) {
                                    total += m.mass[std::size_t(row) * ne + e];
                                    capacity += m.capacity[std::size_t(row) * ne + e];
                                }
                            const double native =
                                reactor == t.n_layers
                                    ? t.sat_mass[t.idx(row, cell)]
                                    : t.layer_mass[t.layerIdx(row, reactor, cell)];
                            const double expected =
                                native + m.pending[std::size_t(row) * (t.n_layers + 1) + reactor];
                            if (capacity > 0 &&
                                std::abs(total - expected) >
                                    1e-12 * std::max({1., std::abs(total), std::abs(expected)}))
                                throw std::runtime_error(
                                    "SIGMA porous checkpoint duplicates or loses canonical mass.");
                        }
                        double held = 0;
                        for (std::size_t e = 0; e < ne; ++e)
                            held += m.reserved[std::size_t(row) * ne + e];
                        if (std::abs(held - pendingSaturatedMassExport(cell, row)) >
                            1e-12 * std::max(1., held))
                            throw std::runtime_error(
                                "SIGMA porous checkpoint reservation differs from its producer receipts.");
                    }
            }
    if (a.reading()) {
        richards_clock_valid_ = lid_receipts_enabled_;
        const auto fail = []() {
            throw std::runtime_error("Invalid completed-source aquifer state.");
        };
        const auto nonnegative = [&](double v) {
            if (!std::isfinite(v) || v < 0)
                fail();
        };
        if (lid_receipts_enabled_) {
            for (double t : cell_completed_)
                nonnegative(t);
            for (std::size_t b = 0; b < lid_receipts_.size(); ++b) {
                nonnegative(richards_down_cum_[b]);
                nonnegative(richards_up_cum_[b]);
                nonnegative(lid_booked_end_[b]);
                nonnegative(node_pending_down_[b]);
                nonnegative(node_pending_up_[b]);
                const auto& d = lid_bed_diagnostics_[b];
                for (double v :
                     {d.start, d.end, d.groundwater_time, d.requested_down, d.requested_up,
                      d.reserved_down, d.reserved_up, d.accepted_down, d.accepted_up})
                    nonnegative(v);
                if (!std::isfinite(d.lid_head) || !std::isfinite(d.groundwater_head) ||
                    d.end < d.start || d.groundwater_time > d.end + 1e-12 * std::max(1., d.end) ||
                    d.accepted_down > d.reserved_down || d.accepted_up > d.reserved_up)
                    fail();
                if (std::abs(s.nacc[b] - (node_pending_up_[b] - node_pending_down_[b])) >
                    1e-10 * std::max({1., node_pending_up_[b], node_pending_down_[b]}))
                    fail();
                if (!std::isfinite(lid_bed_factor_[b]) || lid_bed_factor_[b] < 0 ||
                    lid_bed_factor_[b] > 1)
                    fail();
                if (!richards_bed_owned_[b] &&
                    (!lid_receipts_[b].empty() || lid_booked_end_[b] != 0))
                    fail();
                if (!richards_bed_owned_[b] && !mobile_lid_bed_[b] &&
                    (richards_down_cum_[b] != 0 || richards_up_cum_[b] != 0))
                    fail();
                double previous = 0, down = 0, up = 0;
                for (const auto& r : lid_receipts_[b]) {
                    nonnegative(r.start);
                    nonnegative(r.end);
                    nonnegative(r.down);
                    nonnegative(r.up);
                    if (!std::isfinite(r.down_j) || !std::isfinite(r.up_j) ||
                        (r.down == 0 && r.down_j != 0) || (r.up == 0 && r.up_j != 0))
                        fail();
                    if (!r.heat_applied || r.start < previous || r.end <= r.start ||
                        r.end > lid_booked_end_[b] ||
                        r.end <= cell_completed_[node_beds_[b].cell] ||
                        (r.down == 0 && r.up == 0 && !heat_.active()))
                        fail();
                    previous = r.end;
                    down += r.down;
                    up += r.up;
                }
                if (down > richards_down_cum_[b] || up > richards_up_cum_[b] ||
                    std::abs(bed_exchange_cum_[b] - (richards_up_cum_[b] - richards_down_cum_[b])) >
                        1e-10 * std::max({1., richards_up_cum_[b], richards_down_cum_[b]}))
                    if (richards_bed_owned_[b] || mobile_lid_bed_[b])
                        fail();
            }
        }
        const auto paired = [&](double v, const auto& mass) {
            nonnegative(v);
            if (int(mass.size()) != t.n_species)
                fail();
            for (int p = 0; p < t.n_species; ++p) {
                const double m = mass[static_cast<std::size_t>(p)];
                if (!std::isfinite(m) || (p != t.temp_row && m < 0) || (v == 0 && m != 0))
                    fail();
            }
        };
        for (const auto* values : {&s.hg,
                                   &s.hu,
                                   &s.theta_sigma,
                                   &s.q0_last,
                                   &s.qnode_last,
                                   &s.qlat_last,
                                   &s.qdeep_last,
                                   &s.qet_last,
                                   &s.dunne_last,
                                   &s.qplus_last,
                                   &s.infil_interval,
                                   &s.wetting_front,
                                   &s.reject_last,
                                   &s.reject_cumulative,
                                   &s.dunne_cumulative,
                                   &s.et_pending,
                                   &s.et_potential_cumulative,
                                   &s.et_surface_cumulative,
                                   &s.et_soil_cumulative,
                                   &s.et_unused_cumulative,
                                   &s.qlink_last,
                                   &s.eacc_L,
                                   &s.eacc_R,
                                   &s.xacc_from_surface,
                                   &s.xacc_to_surface,
                                   &s.nacc,
                                   &s.lacc})
            for (double v : *values)
                if (!std::isfinite(v))
                    throw std::runtime_error("Nonfinite aquifer checkpoint state.");
        // NaN denotes an unpublished infiltration/ET diagnostic, including on untouched cells.
        for (const auto* values :
             {&s.infil_capacity, &s.infil_remaining, &s.infil_refresh, &s.et_surface_last,
              &s.et_potential_last, &s.et_refresh, &s.et_stress})
            for (double v : *values)
                if (std::isinf(v))
                    fail();
        for (const auto* values :
             {&t.sat_mass,          &t.unsat_mass,      &t.sacc_L,           &t.sacc_R,
              &t.xacc_from_surface, &t.xacc_to_surface, &t.nacc_mass,        &t.lacc_mass,
              &t.node_out_mass,     &t.lost_deep,       &t.lost_node,        &t.lost_link,
              &t.lost_dunne,        &t.lost_et,         &t.gained_infil,     &t.gained_node,
              &t.gained_link,       &t.net_lateral,     &t.gained_source,    &t.lost_source,
              &t.lost_reaction,     &t.init_mass,       &t.internal_recharge})
            for (double v : *values)
                if (!std::isfinite(v))
                    throw std::runtime_error("Nonfinite t checkpoint state.");
        const auto receipt = [&](const auto& r) {
            const auto& q = r.request;
            if (q.cell < 0 || q.cell >= s.n_cells || !source_owned_[q.cell] ||
                !std::isfinite(r.start) || !std::isfinite(r.end) || r.start < 0 ||
                r.end <= r.start || int(q.kind) < 0 || int(q.kind) > 2)
                fail();
            for (double v : {q.area, q.pond, q.water_ceiling, q.candidate})
                nonnegative(v);
            if (q.kind == SurfaceDonorKind::MESH
                    ? (q.source != "cell:" + std::to_string(q.cell) || q.unit != -1)
                    : q.source.empty())
                fail();
            paired(r.volume, r.mass);
        };
        for (int i = 0; i < s.n_cells; ++i) {
            nonnegative(s.hg[i]);
            nonnegative(s.hu[i]);
            if (s.hg[i] > s.zs[i])
                fail();
            nonnegative(source_cell_end_[i]);
            nonnegative(s.et_pending[i]);
            nonnegative(s.xacc_from_surface[i]);
            nonnegative(s.xacc_to_surface[i]);
            double volume = 0;
            std::vector<double> mass(t.n_species, 0);
            for (const auto& r : source_receipts_[i]) {
                receipt(r);
                if (r.request.cell != i)
                    fail();
                volume += r.volume;
                for (int p = 0; p < t.n_species; ++p)
                    mass[p] += r.mass[p];
            }
            if (source_owned_[i]) {
                if (std::abs(volume - s.xacc_from_surface[i]) > 1e-10 * std::max(1., volume) ||
                    s.xacc_to_surface[i] != 0)
                    fail();
                for (int p = 0; p < t.n_species; ++p)
                    if (std::abs(mass[p] - t.xacc_from_surface[t.idx(p, i)]) >
                            1e-10 * std::max(1., mass[p]) ||
                        t.xacc_to_surface[t.idx(p, i)] != 0)
                        fail();
            }
        }
        const auto validateSpeciesMass = [&](const std::vector<double>& mass) {
            // A water-only aquifer has no transport grid. Its correctly shaped
            // species stores are empty, so do not require t.n_cells > 0.
            if (t.n_species == 0) {
                if (!mass.empty())
                    fail();
                return;
            }
            if (t.n_species < 0 || t.n_cells <= 0 ||
                mass.size() != std::size_t(t.n_species) * std::size_t(t.n_cells))
                fail();
            for (std::size_t k = 0; k < mass.size(); ++k) {
                const int species = int(k / std::size_t(t.n_cells));
                const double value = mass[k];
                if (!std::isfinite(value) || (species != t.temp_row && value < 0))
                    fail();
            }
        };
        validateSpeciesMass(t.sat_mass);
        validateSpeciesMass(t.unsat_mass);
        for (const auto& r : source_returns_) {
            const int i = r.intake.request.cell;
            if (i < 0 || i >= s.n_cells || !source_owned_[i])
                fail();
            paired(r.volume, r.mass);
            nonnegative(r.available_at);
            if (int(r.kind) < 0 || int(r.kind) > 2 || r.available_at > source_cell_end_[i])
                fail();
            if (r.kind == SurfaceReturnKind::NUMERICAL_REFUND) {
                receipt(r.intake);
                if (r.available_at < r.intake.end || r.volume > r.intake.volume * (1 + 1e-10))
                    fail();
                for (int p = 0; p < t.n_species; ++p)
                    if (std::abs(r.mass[p] - (r.intake.volume > 0
                                                  ? r.intake.mass[p] * r.volume / r.intake.volume
                                                  : 0)) > 1e-10 * std::max(1., r.mass[p]))
                        fail();
            }
        }
        if (gather_fraction_ != 1 || cells_by_tier_.size() != faces_by_tier_.size() ||
            cells_by_tier_.size() != tier_firings_.size() || face_tier_.size() != s.eacc_L.size() ||
            pending_flag_.size() != std::size_t(s.n_cells))
            fail();
        if (cells_by_tier_.empty())
            fail();
        std::vector<int> cell_seen(s.n_cells, 0), face_seen(s.eacc_L.size(), 0);
        for (std::size_t k = 0; k < cells_by_tier_.size(); ++k) {
            if (tier_firings_[k] < 0)
                fail();
            for (int c : cells_by_tier_[k])
                if (c < 0 || c >= s.n_cells || ++cell_seen[c] != 1 || s.tier[c] != k)
                    fail();
            for (int f : faces_by_tier_[k])
                if (f < 0 || f >= int(s.eacc_L.size()) || ++face_seen[f] != 1 || face_tier_[f] != k)
                    fail();
        }
        for (int c = 0; c < s.n_cells; ++c)
            if (cell_seen[c] != (covered_[c] ? 1 : 0))
                fail();
        for (std::size_t f = 0; f < face_seen.size(); ++f) {
            const bool owned = covers(edges_->cL[f]) && covers(edges_->cR[f]);
            if (face_seen[f] != (owned ? 1 : 0))
                fail();
        }
        for (int c : pending_surface_)
            if (c < 0 || c >= s.n_cells)
                fail();
    }
}

std::string SubsurfaceSolver::transportRestartState() const {
    if (!needsTransportRestart())
        return {};
    auto copy = *this;
    detail::CompletedSourceArchive model, state, envelope;
    copy.checkpoint(model, true);
    copy.checkpoint(state, false);
    auto identity = model.bytes(), payload = state.bytes();
    std::string profile = "OpenSWMM groundwater transport v1";
    envelope(profile, identity, payload);
    return envelope.bytes();
}
std::string SubsurfaceSolver::restoreTransportRestart(const std::string& bytes, bool apply) {
    try {
        if (bytes.empty()) {
            if (needsTransportRestart())
                return "Hotstart lacks groundwater boundary, layer, ledger or shared-source clock state.";
            return {};
        }
        if (!needsTransportRestart())
            return "Hotstart groundwater transport profile differs from this model.";
        detail::CompletedSourceArchive envelope(bytes);
        std::string profile, identity, payload;
        envelope(profile, identity, payload);
        envelope.finish();
        if (profile != "OpenSWMM groundwater transport v1")
            return "Unknown groundwater transport hotstart profile.";
        auto trial = *this;
        detail::CompletedSourceArchive model;
        trial.checkpoint(model, true);
        if (identity != model.bytes())
            return "Groundwater hotstart mesh, species, units, forcing or layer model differs.";
        detail::CompletedSourceArchive state(payload);
        trial.checkpoint(state, false);
        state.finish();
        if (apply) {
            *this = std::move(trial);
            if (!node_concentrations_.empty())
                node_row_conc_ = node_concentrations_.data();
        }
        return {};
    } catch (const std::exception& e) {
        return e.what();
    }
}
}
