// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin
#pragma once
#include "core/CompletedSourceArchive.hpp"
#include "core/SimulationContext.hpp"
#include "quality/PassiveStorageExchange.hpp"
#include <cstring>
#include <functional>
#include <stdexcept>

namespace live_recovery_test {
using Archive = openswmm::detail::CompletedSourceArchive;
inline std::uint32_t crc(const std::string& bytes) {
    std::uint32_t value = ~std::uint32_t(0);
    for (unsigned char c : bytes) {
        value ^= c;
        for (int bit = 0; bit < 8; ++bit) value = (value >> 1) ^ ((value & 1) ? 0xedb88320u : 0u);
    }
    return ~value;
}
inline void editBlob(std::string& parent, const std::string& magic,
                     const std::function<void(std::string&)>& edit) {
    const auto found = parent.find(magic);
    if (found == std::string::npos || found < 16) throw std::runtime_error("Missing nested test archive.");
    const auto start = found - 16;
    std::uint64_t size; std::memcpy(&size, parent.data() + start, sizeof(size));
    if (size > parent.size() - start - 8) throw std::runtime_error("Bad nested test archive extent.");
    auto child = parent.substr(start + 8, size);
    edit(child);
    Archive replacement; replacement(child);
    parent.replace(start, 8 + size, replacement.bytes());
}
inline void editOwner(std::string& bytes, const std::function<void(std::string&)>& edit) {
    Archive in(bytes);
    std::string magic, payload; std::uint32_t version, endian, checksum;
    in(magic, version, endian, checksum, payload); in.finish();
    edit(payload); checksum = crc(payload);
    Archive out; out(magic, version, endian, checksum, payload); bytes = out.bytes();
}
inline std::string editSurface(std::string bytes, const std::function<void(std::string&)>& edit,
                              const char* surface_magic = "OpenSWMM paired passive surface boundary") {
    Archive in(bytes);
    std::string magic, model, payload; std::uint32_t version, checksum;
    in(magic, version, model, checksum, payload); in.finish();
    editBlob(payload, surface_magic, [&](std::string& child) {
        Archive f(child);
        std::string name, identity, body; std::uint32_t v, endian, width, sum;
        f(name, v, endian, width, identity, body, sum); f.finish();
        edit(body); sum = crc(body);
        Archive out; out(name, v, endian, width, identity, body, sum); child = out.bytes();
    });
    checksum = crc(payload);
    Archive out; out(magic, version, model, checksum, payload); return out.bytes();
}
inline std::string encodeProof(openswmm::quality::PassiveStorageExchange::LiveProof p) {
    Archive a; a(p.si_to_native);
    std::uint64_t n = p.points.size(); a(n);
    for (auto point : p.points) a(point.point, point.cell, point.node, point.exchange_si);
    n = p.legs.size(); a(n);
    for (auto l : p.legs)
        a(l.point, l.cell, l.node, l.drain, l.applied, l.water_si, l.mass_si, l.water_native,
          l.mass_native, l.before, l.after, l.sample, l.batch_start_ms, l.batch_end_ms, l.phase,
          l.volume_before, l.volume_after, l.offset_s, l.duration_s, l.withdrawal_si);
    return a.bytes();
}
inline std::string corruptProof(const std::string& bytes,
        openswmm::quality::PassiveStorageExchange::LiveProof valid,
        const std::function<void(openswmm::quality::PassiveStorageExchange::LiveProof&)>& edit) {
    auto bad = valid; edit(bad);
    return editSurface(bytes, [&](std::string& surface) {
        editBlob(surface, "OpenSWMM live passive owner", [&](std::string& owner) {
            editOwner(owner, [&](std::string& live) {
                editBlob(live, "OpenSWMM passive storage receipt owner", [&](std::string& book) {
                    editOwner(book, [&](std::string& payload) {
                        const auto original = encodeProof(valid), changed = encodeProof(bad);
                        const auto where = payload.find(original);
                        if (where == std::string::npos) throw std::runtime_error("Missing test proof.");
                        payload.replace(where, original.size(), changed);
                    });
                });
            });
        });
    });
}
// Visit the documented no-snow/no-heat mesh archive fields. These helpers alter
// complete checksum-valid physical proposals, rather than failing only CRC checks.
inline void meshVector(std::string& payload, const std::string& target,
                       const std::function<void(char*, std::size_t)>& edit,
                       const std::string& marker = "native passive mesh state v1") {
    auto cursor = payload.find(marker);
    if (cursor == std::string::npos) throw std::runtime_error("Missing test mesh profile.");
    cursor += marker.size();
    auto visit = [&](const char* name, std::size_t width) {
        std::uint64_t n; std::memcpy(&n, payload.data() + cursor, 8); cursor += 8;
        if (n > (payload.size() - cursor) / width) throw std::runtime_error("Bad test mesh shape.");
        if (target == name) edit(payload.data() + cursor, n);
        cursor += n * width;
    };
    for (auto name : {"depth", "head", "volume", "grad_hx", "grad_hy", "grad_hx_lim", "grad_hy_lim",
            "vert_head", "vert_depth_signed", "face_vx", "face_vy", "cell_continuity_err", "edge_flux",
            "rainfall", "evap_rate", "infil_rate", "infil_applied", "coupling_flux", "coupling_applied", "net_source"})
        visit(name, sizeof(double));
    visit("rainfall_forced", 1); visit("rainfall_persist", 1); visit("rainfall_force_val", sizeof(double));
    visit("evap_forced", 1); visit("evap_persist", 1); visit("evap_force_val", sizeof(double));
    visit("coupling_forced", 1); visit("coupling_persist", 1); visit("coupling_force_val", sizeof(double));
    for (auto name : {"coupling_native", "old_depth", "old_volume", "stat_max_depth", "stat_max_velocity",
                      "stat_max_cont_err", "stat_cum_volume"}) visit(name, sizeof(double));
    cursor += 2 * sizeof(bool) + sizeof(double); // forcing flags, evaporation receipt
    visit("cell_mass", sizeof(double));
}
inline void addFirstDouble(char* values, std::size_t n, double amount) {
    if (n == 0) throw std::runtime_error("Empty test physical vector.");
    double x; std::memcpy(&x, values, sizeof(x)); x += amount; std::memcpy(values, &x, sizeof(x));
}
inline std::string corruptNodeConcentration(std::string bytes) {
    return editSurface(std::move(bytes), [&](std::string& surface) {
        editBlob(surface, "OpenSWMM accepted DW hydraulic boundary", [&](std::string& child) {
            Archive f(child);
            std::string name, identity, body; std::uint32_t version, checksum;
            f(name, version, identity, checksum, body); f.finish();
            Archive in(body), prefix;
            std::vector<double> water, reverse, heat, pollutant;
            double time, date, elapsed, old_elapsed, report, evap, infil, recovery, link_span; bool links;
            in(water, reverse, heat, pollutant, time, date, elapsed, old_elapsed, report, evap, infil, recovery, links, link_span);
            prefix(water, reverse, heat, pollutant, time, date, elapsed, old_elapsed, report, evap, infil, recovery, links, link_span);
            auto cursor = prefix.bytes().size();
            for (int vector = 0; vector <= 31; ++vector) {
                std::uint64_t n; std::memcpy(&n, body.data() + cursor, 8); cursor += 8;
                if (n > (body.size() - cursor) / sizeof(double)) throw std::runtime_error("Bad test hydraulic extent.");
                if (vector == 31) {
                    addFirstDouble(body.data() + cursor, n, 1);
                    if (n == 2) addFirstDouble(body.data() + cursor + sizeof(double), 1, -1);
                }
                cursor += n * sizeof(double);
            }
            checksum = crc(body); Archive out; out(name, version, identity, checksum, body); child = out.bytes();
        });
    });
}
}

namespace live_recovery_test {
inline std::string encodeWaterBalance(openswmm::SimulationContext::MassBalance2D m) {
    Archive a;
    a(m.init_storage, m.final_storage, m.external_in, m.external_out, m.rainfall_in,
      m.coupling_1d_to_2d_in, m.coupling_2d_to_1d_out, m.outfall_in, m.outfall_out, m.boundary_in,
      m.boundary_out, m.evap_out, m.infil_out, m.infil_to_aquifer, m.infil_aquifer_pending,
      m.aquifer_in, m.active, m.solver_nsteps, m.solver_nrhs, m.solver_avg_h, m.solver_last_h,
      m.solver_active_min, m.solver_active_mean, m.solver_active_max, m.solver_n_tiers);
    for (auto v : m.solver_tier_cells) a(v);
    return a.bytes();
}
}
