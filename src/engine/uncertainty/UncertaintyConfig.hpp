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

namespace openswmm::uncertainty {

/**
 * @brief Aggregated uncertainty configuration.
 *
 * Populated from the `[UNCERTAINTY]` input section by the parser.
 * All fields default to "disabled" (empty / zero perturbation).
 */
struct UncertaintyConfig {

    /// All source specs in parse order.
    std::vector<UncertaintySourceSpec> sources;

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
