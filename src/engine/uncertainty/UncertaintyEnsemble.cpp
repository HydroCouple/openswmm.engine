/**
 * @file UncertaintyEnsemble.cpp
 * @brief UncertaintyEnsemble — implementation.
 *
 * @ingroup engine_uncertainty
 */

#include "UncertaintyEnsemble.hpp"
#include "LhsShuffle.hpp"
#include <stdexcept>

namespace openswmm::uncertainty {

// ============================================================================
// registerParam
// ============================================================================

RegisteredParam& UncertaintyEnsemble::registerParam(
    const std::string& name, LayerTarget layer,
    ParamEntry entry, DistType dist, double pert)
{
    // Update in place if this (name, layer) is already registered — keeps its
    // seed_offset stable so re-configuration doesn't reshuffle other columns.
    for (auto& p : params) {
        if (p.name == name && p.layer == layer) {
            p.entry = entry;
            p.dist  = dist;
            p.pert  = pert;
            return p;
        }
    }

    RegisteredParam p;
    p.name  = name;
    p.layer = layer;
    p.entry = entry;
    p.dist  = dist;
    p.pert  = pert;
    // Next free seed_offset: one past the current maximum (the legacy layout
    // reserves offsets 0..3, so user params start at 4).
    uint64_t next = 0;
    for (const auto& q : params)
        next = std::max(next, q.seed_offset + 1);
    p.seed_offset = next;
    params.push_back(std::move(p));
    return params.back();
}

// ============================================================================
// registerDefaults
// ============================================================================

void UncertaintyEnsemble::registerDefaults() {
    // Fixed seed offsets 1/2/3 and the ascending reference column reproduce
    // the pre-registry (PR-5) columns bit-exactly. See PARAMETER_REGISTRY.md §4.
    auto& mann = registerParam("MANNINGS_N", LayerTarget::TWO_D,
                               ParamEntry::RATE_MULT, DistType::UNIFORM,
                               mannings_pert_2d);
    mann.reference_column = true;
    mann.seed_offset      = 0;

    auto& rain = registerParam("RAINFALL", LayerTarget::TWO_D,
                               ParamEntry::FORCING_MULT, DistType::UNIFORM,
                               rainfall_pert_2d);
    rain.seed_offset = 1;

    auto& soil = registerParam("SOIL", LayerTarget::RUNOFF,
                               ParamEntry::RATE_MULT, DistType::UNIFORM,
                               soil_pert);
    soil.seed_offset = 2;

    auto& cd = registerParam("CD", LayerTarget::TWO_D,
                             ParamEntry::COUPLING_MULT, DistType::UNIFORM,
                             cd_pert);
    cd.seed_offset = 3;
}

// ============================================================================
// column
// ============================================================================

const std::vector<double>* UncertaintyEnsemble::column(
    const std::string& name, LayerTarget layer) const noexcept
{
    for (const auto& p : params) {
        if (p.name == name &&
            (layer == LayerTarget::NONE || p.layer == layer)) {
            return p.column.empty() ? nullptr : &p.column;
        }
    }
    return nullptr;
}

// ============================================================================
// generate
// ============================================================================

void UncertaintyEnsemble::generate() {
    if (n_members < 2)
        throw std::invalid_argument(
            "UncertaintyEnsemble::generate: n_members must be >= 2");

    if (params.empty())
        registerDefaults();

    const std::size_t M  = static_cast<std::size_t>(n_members);
    const double      Md = static_cast<double>(n_members);

    for (auto& p : params) {
        p.column.resize(M);
        if (p.reference_column) {
            // Ascending strata, no shuffle — the reference column all other
            // rank correlations are judged against (Manning prototype).
            for (int i = 0; i < n_members; ++i) {
                const double t = (static_cast<double>(i) + 0.5) / Md;
                p.column[static_cast<std::size_t>(i)] =
                    invCdfMultiplier(p.dist, p.pert, t);
            }
        } else {
            const auto t = shuffledStrata(n_members, seed + p.seed_offset);
            for (int i = 0; i < n_members; ++i) {
                auto ui = static_cast<std::size_t>(i);
                p.column[ui] = invCdfMultiplier(p.dist, p.pert, t[ui]);
            }
        }
    }

    // Legacy back-compat vectors — copies of the registered columns.
    auto copy_or_ones = [&](const char* name, LayerTarget layer,
                            std::vector<double>& dst) {
        if (const auto* col = column(name, layer)) dst = *col;
        else dst.assign(M, 1.0);
    };
    copy_or_ones("MANNINGS_N", LayerTarget::TWO_D,  mannings_mult_2d);
    copy_or_ones("RAINFALL",   LayerTarget::TWO_D,  rainfall_mult_2d);
    copy_or_ones("SOIL",       LayerTarget::RUNOFF, soil_mult);
    copy_or_ones("CD",         LayerTarget::TWO_D,  cd_mult);
}

} // namespace openswmm::uncertainty
