// SPDX-License-Identifier: Apache-2.0
// Named authored properties follow their typed owner through rename and removal.
#pragma once
#include "SimulationContext.hpp"
#include "../transport/components/HeatFluxModules/HeatOverrides.hpp"
#include <algorithm>
namespace openswmm {
inline void refreshNamedHeatOverrides(SimulationContext& ctx) {
    auto& o = ctx.heat_config.overrides;
    o.rad_link.clear(); o.rad_node.clear(); o.sed_link.clear(); o.rad_cell.clear(); o.sed_cell.clear();
    if (!o.rows.empty()) transport::heat::resolveHeatOverrides(ctx);
}
inline void moveNamedOverrides(SimulationContext& ctx, const char* type, int index,
                               const std::string& from, const std::string& to) {
    // Work with typed variants, never their formatted API strings.
    for (const auto& def : ctx.user_flags.all_defs()) {
        auto value = ctx.user_flags.try_get_value(type, from, def.name);
        if (!value) continue;
        ctx.user_flags.unset(type, from, def.name);
        if (!to.empty()) ctx.user_flags.set(type, to, def.name, std::move(*value));
    }
    if (std::string(type) == "SUBCATCHMENT") {
        auto& rows = ctx.reactions.surface.rows.loadings;
        for (auto& row : rows)
            if (CiEqual{}(row.subcatch, from)) row.subcatch = to;
        rows.erase(std::remove_if(rows.begin(), rows.end(), [](const auto& row) {
            return row.subcatch.empty();
        }), rows.end());
    }
    const bool node = std::string(type) == "NODE", link = std::string(type) == "LINK";
    if (!node && !link) return;
    auto& iq = ctx.initial_quality;
    for (int row = iq.count() - 1; row >= 0; --row) {
        if ((iq.is_link[row] != 0) != link) continue;
        if (iq.elem_idx[row] == index) {
            if (to.empty()) iq.erase(row);
            else iq.elem_name[row] = to;
        } else if (to.empty() && iq.elem_idx[row] > index) --iq.elem_idx[row];
    }
    const auto scope = link ? HeatScope::LINK : HeatScope::NODE;
    auto& rows = ctx.heat_config.overrides.rows;
    for (auto& row : rows)
        if (row.scope == scope && CiEqual{}(row.name, from)) row.name = to;
    rows.erase(std::remove_if(rows.begin(), rows.end(), [&](const auto& row) {
        return row.scope == scope && row.name.empty();
    }), rows.end());
}
}
