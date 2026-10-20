// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "../transport/components/ReactionModule/ReactionsWriter.hpp"
#include "CompletedSourceArchive.hpp"
#include "SimulationContext.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace openswmm::detail {
inline bool checkpointReactionComponents(const SimulationContext &c) {
    return std::all_of(c.process_component_specs.begin(), c.process_component_specs.end(),
                       [&](const auto &p) {
                           return p.id == "org.hydrocouple.openswmm.reactions" && c.reactions.configured &&
                                  c.reactions.compiled && p.args.empty();
                       });
}
inline void reactionModel(CompletedSourceArchive &a, const SimulationContext &c) {
    if (!c.reactions.configured)
        return; // preserve existing water-only fingerprints
    auto text = transport::serializeReactionSystem(c);
    a(text);
    auto f = c.options.fv;
    auto ard = c.ard_config;
    a(f.cell_length, f.min_cells, f.scalar_scheme, f.limiter, ard.configured, ard.dispersion_mode,
      ard.dispersion_value, ard.conduit_disp_link, ard.conduit_disp_value, ard.bc_node, ard.bc_msx,
      ard.bc_value, ard.bc_ts, ard.src_link, ard.src_msx, ard.src_value, ard.src_ts, ard.target_dx);
    auto s = c.reactions.surface;
    a(s.n_landuses, s.n_species, s.n_subcatch, s.resolved, s.bu_type, s.bu_c1, s.bu_c2, s.bu_c3,
      s.bu_normalizer, s.bu_max_days, s.wo_type, s.wo_coeff, s.wo_expon, s.wo_sweep_effic, s.wo_bmp_effic,
      s.init_loading, s.mcf, s.c_rain, s.snow_only, s.lid_rmvl);
    auto land = c.landuses;
    auto sub = c.subcatches;
    a(sub.coverage, sub.curb_length, land.sweep_interval, land.sweep_removal, land.last_swept);
}
inline void reactionState(CompletedSourceArchive &a, ReactionData &r) {
    uint32_t version = 3;
    a(version);
    if (version != 3)
        throw std::runtime_error("Unsupported reaction checkpoint version.");
    a(r.booster_mass);
    a(r.warned_react_failure, r.warned_negative_clamp, r.msx_node_conc, r.msx_link_conc, r.msx_ext_mass_in,
      r.msx_coupling_queue);
    a(r.lid.node_mass, r.lid.node_outlet, r.lid.node_mobile_old, r.lid.group_offsets, r.lid.mass,
      r.lid.old_water, r.lid.surface_conc, r.lid.drain_conc, r.lid.surface_export, r.lid.surface_carry,
      r.lid.subcatchments);
    auto &l = r.msx_ledger;
    a(l.armed, l.init, l.inflow, l.outflow, l.flooding, l.seepage, l.reacted, l.clamped, l.final_dry);
    auto &s = r.surface;
    a(s.buildup, s.washoff_conc, s.washoff_conc_old, s.ponded_qual, s.runon_qual_rate, s.lid_drain_load,
      s.lid_drain_load_old, s.lid_drain_runon_load, s.led_init_buildup, s.led_buildup, s.led_runoff_load,
      s.led_sweeping, s.led_bmp_removal, s.led_wet_deposition, s.led_infil_loss, s.led_reacted,
      s.led_reaction_clamped, s.led_ponded_lost, s.led_subcatch_load);
}
inline void validateReactionState(const ReactionData &r, const SimulationContext &c) {
    const auto ns = std::size_t(r.n_species()), nn = std::size_t(c.n_nodes()), nl = std::size_t(c.n_links());
    auto row = [](const auto &v, std::size_t size, bool optional = false) {
        if (v.size() != size && !(optional && v.empty()))
            throw std::runtime_error("Reaction checkpoint inventory shape differs.");
        for (double value : v)
            if (!std::isfinite(value))
                throw std::runtime_error("Nonfinite reaction checkpoint inventory.");
    };
    row(r.msx_node_conc, nn * ns);
    row(r.msx_link_conc, nl * ns);
    row(r.msx_ext_mass_in, nn * ns, true);
    row(r.msx_coupling_queue, nn * ns, true);
    if (r.lid_controls.empty()) {
        if (!r.lid.node_mass.empty() || !r.lid.node_outlet.empty() || !r.lid.group_offsets.empty() ||
            !r.lid.subcatchments.empty())
            throw std::runtime_error("Unexpected LID reaction checkpoint inventory.");
        for (const auto *v : {&r.lid.mass, &r.lid.old_water, &r.lid.node_mobile_old, &r.lid.surface_conc,
                              &r.lid.drain_conc, &r.lid.surface_export, &r.lid.surface_carry})
            row(*v, 0);
    }
    if (!r.lid_controls.empty()) {
        const auto &st = c.node_subtypes.storages;
        if (r.lid.node_mass.size() != std::size_t(st.count()) ||
            r.lid.node_outlet.size() != std::size_t(st.count()))
            throw std::runtime_error("LID reaction checkpoint node count differs.");
        row(r.lid.node_mobile_old, st.count());
        if (r.lid.group_offsets != c.reactions.lid.group_offsets ||
            r.lid.subcatchments != c.reactions.lid.subcatchments)
            throw std::runtime_error("LID reaction checkpoint ownership differs.");
        const auto units = r.lid.group_offsets.empty() ? 0 : std::size_t(r.lid.group_offsets.back());
        row(r.lid.mass, units * 4 * ns);
        row(r.lid.old_water, units * 4);
        for (const auto *v : {&r.lid.mass, &r.lid.old_water, &r.lid.node_mobile_old, &r.lid.surface_export,
                              &r.lid.surface_carry})
            for (double x : *v)
                if (x < 0)
                    throw std::runtime_error("Negative LID checkpoint mass or water.");
        row(r.lid.surface_conc, units * ns);
        row(r.lid.drain_conc, units * ns);
        row(r.lid.surface_export, std::size_t(c.n_subcatches()) * ns);
        row(r.lid.surface_carry, std::size_t(c.n_subcatches()) * ns);
        for (int i = 0; i < st.count(); ++i) {
            row(r.lid.node_mass[i], r.lid_enabled(st.lid[i].control) ? st.lid_state[i].cells.size() * ns : 0);
            for (double x : r.lid.node_mass[i])
                if (x < 0)
                    throw std::runtime_error("Negative LID retained checkpoint mass.");
            const auto &out = r.lid.node_outlet[i];
            if (!out.empty() && out.size() != nl * ns)
                throw std::runtime_error("LID reaction checkpoint outlet shape differs.");
            for (double v : out)
                if (std::isinf(v))
                    throw std::runtime_error("Infinite LID outlet concentration.");
        }
    }
    row(r.booster_mass, ns, true);
    for (double x : r.booster_mass)
        if (x < 0)
            throw std::runtime_error("Negative booster injection in checkpoint.");
    const auto &l = r.msx_ledger;
    for (const auto *v :
         {&l.init, &l.inflow, &l.outflow, &l.flooding, &l.seepage, &l.reacted, &l.clamped, &l.final_dry})
        row(*v, l.armed ? ns : 0);
    const auto &s = r.surface;
    const auto sc = std::size_t(c.n_subcatches()) * ns;
    row(s.buildup, s.active() ? sc * std::size_t(s.n_landuses) : 0);
    for (const auto *v : {&s.washoff_conc, &s.washoff_conc_old, &s.ponded_qual, &s.runon_qual_rate,
                          &s.lid_drain_runon_load, &s.led_subcatch_load})
        row(*v, s.active() ? sc : 0);
    for (const auto *v : {&s.lid_drain_load, &s.lid_drain_load_old})
        row(*v, nn * ns, true);
    for (const auto *v : {&s.led_init_buildup, &s.led_buildup, &s.led_runoff_load, &s.led_sweeping,
                          &s.led_bmp_removal, &s.led_wet_deposition, &s.led_infil_loss, &s.led_reacted,
                          &s.led_reaction_clamped, &s.led_ponded_lost})
        row(*v, s.active() ? ns : 0);
}
} // namespace openswmm::detail
