/**
 * @file UncertaintyConfig.hpp
 * @brief Container for all uncertainty configuration parsed from [UNCERTAINTY].
 *
 * @details `UncertaintyConfig` collects the full set of active
 *          `UncertaintySourceSpec` entries produced by the `[UNCERTAINTY]`
 *          input section and provides typed accessors for the parameter
 *          families that consumers (SpectralROM, UncertaintyEnsemble) need.
 *
 *          Phase 0 / Phase 1 scope: scalar 2D entries only.
 *          Spatial-field entries and 1D entries are reserved for later phases.
 *
 * @ingroup engine_uncertainty
 */

#ifndef OPENSWMM_ENGINE_UNCERTAINTY_CONFIG_HPP
#define OPENSWMM_ENGINE_UNCERTAINTY_CONFIG_HPP

#include "UncertaintyTypes.hpp"
#include <vector>
#include <optional>
#include <string>

namespace openswmm::uncertainty {

/// Mapping method for grid→target (design §5).
enum class GridMapping : int8_t {
    CENTROID   = 0,  ///< Pixel containing target centroid
    BILINEAR   = 1,  ///< 4-pixel weighted gather (2D cells)
    AREA_MEAN  = 2,  ///< Polygon∩pixel sparse CSR (subcatchments)
};

/// Target layer for a gridded rainfall source (design §3.2).
enum class GridTarget : int8_t {
    TWO_D   = 0,  ///< Per-cell 2D surface rainfall
    RUNOFF  = 1,  ///< Per-subcatchment rainfall
    INFLOWS = 2,  ///< Per-node lateral inflow
};

/**
 * @brief Spatial coherence mode for soft rainfall (design §6, CL-1a).
 *
 * `FULL` (the default) is *comonotone*: every ensemble member shares one
 * scalar coefficient across all space. `CORR_LEN` introduces a finite spatial
 * correlation length via `CorrelatedFieldGenerator`, so a member can be wet in
 * one region and dry in another.
 */
enum class Coherence : int8_t {
    FULL     = 0,  ///< Comonotone — one scalar c_i per member (default)
    CORR_LEN = 1,  ///< Spatially correlated field with finite correlation length
};

/**
 * @brief Specification for one `[SOFT_RAINFALL_GRID]` line (design §3.2, §4.2).
 *
 * Parsed at input time; the GridFileReader is opened and the mapping is
 * precomputed at engine init. No file I/O at parse time beyond existence check.
 */
struct SoftGridSourceSpec {
    std::string    file_path;               ///< Path to the HDF5 grid file
    GridTarget     target  = GridTarget::TWO_D;
    GridMapping    mapping = GridMapping::CENTROID;
    bool           force_location = false;  ///< D4: explicit FORCE_LOCATION keyword
    std::string    nodes_file;              ///< INFLOWS target: node list file path (empty = all)
    Coherence      coherence = Coherence::FULL; ///< CL-1a: spatial coherence mode
    double         corr_len = 0.0;           ///< CL-1a: correlation length (m); 0 ⇒ comonotone
};

/**
 * @brief Aggregated uncertainty configuration.
 *
 * Populated from the `[UNCERTAINTY]` input section by the parser.
 * All fields default to "disabled" (empty / zero perturbation).
 */
struct UncertaintyConfig {

    /// All source specs in parse order.
    std::vector<UncertaintySourceSpec> sources;

    /// All gridded rainfall source specs (from [SOFT_RAINFALL_GRID]).
    std::vector<SoftGridSourceSpec> grid_sources;

    // ------------------------------------------------------------------
    // Convenience accessors — return the first matching spec or nullopt.
    // Phase 0/1 only exposes scalar 2D specs.
    // ------------------------------------------------------------------

    /// First active scalar 2D Manning's-n spec, or nullopt.
    std::optional<UncertaintySourceSpec> mannings_2d() const noexcept {
        for (const auto& s : sources)
            if (s.layer == LayerTarget::TWO_D && s.name == "MANNINGS_N" && s.is_active())
                return s;
        return std::nullopt;
    }

    /// First active scalar 2D rainfall spec, or nullopt.
    std::optional<UncertaintySourceSpec> rainfall_2d() const noexcept {
        for (const auto& s : sources)
            if (s.layer == LayerTarget::TWO_D && s.name == "RAINFALL" && s.is_active())
                return s;
        return std::nullopt;
    }

    /// True if any active 2D uncertainty source is configured.
    bool has_2d() const noexcept {
        for (const auto& s : sources)
            if (s.layer == LayerTarget::TWO_D && s.is_active()) return true;
        return false;
    }

    /// First active 1D Manning's-n spec, or nullopt.
    std::optional<UncertaintySourceSpec> mannings_1d() const noexcept {
        for (const auto& s : sources)
            if (s.layer == LayerTarget::ONE_D && s.name == "MANNINGS_N" && s.is_active())
                return s;
        return std::nullopt;
    }

    /// True if any active 1D uncertainty source is configured.
    bool has_1d() const noexcept {
        for (const auto& s : sources)
            if (s.layer == LayerTarget::ONE_D && s.is_active()) return true;
        return false;
    }

    /// True if any active source is configured.
    bool has_any() const noexcept {
        for (const auto& s : sources)
            if (s.is_active()) return true;
        return false;
    }

    /// True if any active QUALITY uncertainty source is configured.
    bool has_quality() const noexcept {
        for (const auto& s : sources)
            if (s.layer == LayerTarget::QUALITY && s.is_active()) return true;
        return false;
    }

    /// True if any grid source targets the 2D surface layer.
    bool has_2d_grid() const noexcept {
        for (const auto& g : grid_sources)
            if (g.target == GridTarget::TWO_D) return true;
        return false;
    }

    /// All active specs targeting @p layer, in parse order (PR 9c).
    /// Generic accessor for registry consumers — includes user-defined names
    /// that have no dedicated convenience accessor.
    std::vector<UncertaintySourceSpec> specs_for(LayerTarget layer) const {
        std::vector<UncertaintySourceSpec> out;
        for (const auto& s : sources)
            if (s.layer == layer && s.is_active()) out.push_back(s);
        return out;
    }
};

} // namespace openswmm::uncertainty

#endif // OPENSWMM_ENGINE_UNCERTAINTY_CONFIG_HPP
