/**
 * @file PollutantData.hpp
 * @brief Structure-of-Arrays (SoA) storage for pollutants and water quality.
 *
 * @details Replaces the global `Pollut[]` array and `TPollut` struct from
 *          src/solver/objects.h. Per-object quality arrays (concentrations at
 *          nodes/links/subcatchments) are stored as flat 2D arrays indexed as:
 *              [object_index * n_pollutants + pollutant_index]
 *
 * @see Legacy reference: src/solver/objects.h — TPollut
 * @see Legacy reference: src/solver/globals.h — Pollut[], Npolluts
 * @ingroup engine_data
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_ENGINE_POLLUTANT_DATA_HPP
#define OPENSWMM_ENGINE_POLLUTANT_DATA_HPP

#include <vector>
#include <string>
#include <cstdint>

namespace openswmm {

// ============================================================================
// Concentration units
// ============================================================================

/**
 * @brief Pollutant concentration units.
 * @see Legacy: MassUnitsType in src/solver/enums.h
 */
enum class MassUnits : int8_t {
    MG_PER_L  = 0,  ///< Milligrams per liter
    UG_PER_L  = 1,  ///< Micrograms per liter
    COUNTS_PER_L = 2 ///< Counts per liter (bacteria)
};

// ============================================================================
// PollutantData — pollutant definitions
// ============================================================================

/**
 * @brief Static properties for each pollutant species.
 *
 * @details Holds definition-level information only. Concentration state at
 *          each node/link/subcatch is stored in NodeQuality, LinkQuality,
 *          SubcatchQuality (flat arrays in SimulationContext).
 *
 * @ingroup engine_data
 */
struct PollutantData {

    // -----------------------------------------------------------------------
    // Pollutant definition arrays (indexed by pollutant index)
    // -----------------------------------------------------------------------

    /**
     * @brief Concentration units for each pollutant.
     * @see Legacy: Pollut[i].units
     */
    std::vector<MassUnits>  units;

    /**
     * @brief Molecular weight (g/mol). Used for unit conversions.
     * @see Legacy: Pollut[i].mwt
     */
    std::vector<double>     mwt;

    /**
     * @brief First-order decay coefficient (1/day).
     * @see Legacy: Pollut[i].kDecay
     */
    std::vector<double>     k_decay;

    /**
     * @brief Rain concentration (project mass/volume units).
     * @see Legacy: Pollut[i].cRain
     */
    std::vector<double>     c_rain;

    /**
     * @brief Groundwater concentration.
     * @see Legacy: Pollut[i].cGW
     */
    std::vector<double>     c_gw;

    /**
     * @brief RDII concentration.
     * @see Legacy: Pollut[i].cRDII
     */
    std::vector<double>     c_rdii;

    /**
     * @brief Initial concentration everywhere in the network.
     * @see Legacy: Pollut[i].initConc
     */
    std::vector<double>     init_conc;

    /**
     * @brief Co-pollutant index (-1 if none).
     * @details Used for co-fraction (fraction of co-pollutant).
     * @see Legacy: Pollut[i].coPollut
     */
    std::vector<int>        co_pollut;

    /**
     * @brief Co-fraction (fraction of co-pollutant concentration).
     * @see Legacy: Pollut[i].coFraction
     */
    std::vector<double>     co_frac;

    /**
     * @brief True if the pollutant should be printed in output.
     * @see Legacy: Pollut[i].snowOnly
     */
    std::vector<bool>       snow_only;

    // -----------------------------------------------------------------------
    // Per-node quality state — flat 2D: [node * n_pollutants + pollutant]
    // -----------------------------------------------------------------------

    /**
     * @brief Current quality concentration at each node.
     * @details Size = n_nodes * n_pollutants.
     * @see Legacy: Node[i].newQual[]
     */
    std::vector<double>     node_conc;

    /** @brief Previous-step quality at each node. */
    std::vector<double>     node_conc_old;

    // -----------------------------------------------------------------------
    // Per-link quality state — flat 2D: [link * n_pollutants + pollutant]
    // -----------------------------------------------------------------------

    /**
     * @brief Current quality concentration in each link.
     * @details Size = n_links * n_pollutants.
     * @see Legacy: Link[i].newQual[]
     */
    std::vector<double>     link_conc;

    /** @brief Previous-step quality in each link. */
    std::vector<double>     link_conc_old;

    // -----------------------------------------------------------------------
    // Per-subcatch quality state — flat 2D: [subcatch * n_pollutants + pollutant]
    // -----------------------------------------------------------------------

    /**
     * @brief Current quality concentration in subcatchment runoff.
     * @details Size = n_subcatches * n_pollutants.
     * @see Legacy: Subcatch[i].newQual[]
     */
    std::vector<double>     subcatch_conc;

    /** @brief Previous-step quality in subcatchment runoff. */
    std::vector<double>     subcatch_conc_old;

    // -----------------------------------------------------------------------
    // Capacity management
    // -----------------------------------------------------------------------

    int n_pollutants() const noexcept { return static_cast<int>(units.size()); }

    /** @brief Resize pollutant definition arrays to hold `n` pollutants. */
    void resize_pollutants(int n) {
        const auto un = static_cast<std::size_t>(n);
        units.assign(un, MassUnits::MG_PER_L);
        mwt.assign(un, 1.0);
        k_decay.assign(un, 0.0);
        c_rain.assign(un, 0.0);
        c_gw.assign(un, 0.0);
        c_rdii.assign(un, 0.0);
        init_conc.assign(un, 0.0);
        co_pollut.assign(un, -1);
        co_frac.assign(un, 0.0);
        snow_only.assign(un, false);
    }

    /**
     * @brief Allocate per-object quality arrays.
     *
     * @details Called after both pollutant count and object counts are known.
     * @param n_nodes      Number of nodes.
     * @param n_links      Number of links.
     * @param n_subcatches Number of subcatchments.
     */
    void resize_quality(int n_nodes, int n_links, int n_subcatches) {
        const auto np = static_cast<std::size_t>(n_pollutants());
        node_conc.assign(static_cast<std::size_t>(n_nodes) * np, 0.0);
        node_conc_old.assign(static_cast<std::size_t>(n_nodes) * np, 0.0);
        link_conc.assign(static_cast<std::size_t>(n_links) * np, 0.0);
        link_conc_old.assign(static_cast<std::size_t>(n_links) * np, 0.0);
        subcatch_conc.assign(static_cast<std::size_t>(n_subcatches) * np, 0.0);
        subcatch_conc_old.assign(static_cast<std::size_t>(n_subcatches) * np, 0.0);
    }

    void save_quality_state() noexcept {
        node_conc_old = node_conc;
        link_conc_old = link_conc;
        subcatch_conc_old = subcatch_conc;
    }

    void reset_quality_state() noexcept {
        std::fill(node_conc.begin(),    node_conc.end(),    0.0);
        std::fill(node_conc_old.begin(),node_conc_old.end(),0.0);
        std::fill(link_conc.begin(),    link_conc.end(),    0.0);
        std::fill(link_conc_old.begin(),link_conc_old.end(),0.0);
        std::fill(subcatch_conc.begin(),    subcatch_conc.end(),    0.0);
        std::fill(subcatch_conc_old.begin(),subcatch_conc_old.end(),0.0);
    }
};

} /* namespace openswmm */

#endif /* OPENSWMM_ENGINE_POLLUTANT_DATA_HPP */
