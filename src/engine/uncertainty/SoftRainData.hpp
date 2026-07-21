/**
 * @file SoftRainData.hpp
 * @brief SoA storage for gage-level soft rainfall configuration (SR-1a).
 *
 * @ingroup engine_uncertainty
 */

#ifndef OPENSWMM_ENGINE_SOFT_RAIN_DATA_HPP
#define OPENSWMM_ENGINE_SOFT_RAIN_DATA_HPP

#include "UncertaintyTypes.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace openswmm::uncertainty {

enum class SoftSpreadKind : int8_t {
    SD = 0,
    CV = 1,
    HALFRANGE = 2,
};

/**
 * @brief Spatial coherence mode for a soft-rain gage (design §6, CL-1a).
 *
 * Mirrors the grid-level `Coherence` enum. `FULL` (default) is comonotone;
 * `CORR_LEN` enables a per-gage finite correlation length. `corr_len == 0`
 * (absent) ⇒ comonotone, preserving the pre-CL-1 bit-identical path.
 */
enum class GageCoherence : int8_t {
    FULL     = 0,  ///< Comonotone — one scalar c_i per member (default)
    CORR_LEN = 1,  ///< Spatially correlated field with finite correlation length
};

struct SoftRainData {
    std::vector<DistType>       family;         ///< Distribution family per gage
    std::vector<SoftSpreadKind> spread_kind;    ///< SD | CV | HALFRANGE per gage
    std::vector<double>         spread_const;   ///< Constant spread when spread_ts < 0
    std::vector<int>            spread_ts;      ///< TIMESERIES index or -1
    std::vector<std::string>    spread_ts_name; ///< Deferred timeseries name for late resolution
    std::vector<bool>           configured;     ///< True when the gage has a soft-rain entry
    std::vector<GageCoherence>  coherence;      ///< CL-1a: per-gage coherence mode
    std::vector<double>         corr_len;       ///< CL-1a: per-gage correlation length (m); 0 ⇒ comonotone

    int count() const noexcept { return static_cast<int>(configured.size()); }

    void resize(int n) {
        auto un = static_cast<std::size_t>(n);
        family.assign(un, DistType::UNIFORM);
        spread_kind.assign(un, SoftSpreadKind::CV);
        spread_const.assign(un, 0.0);
        spread_ts.assign(un, -1);
        spread_ts_name.assign(un, std::string{});
        configured.assign(un, false);
        coherence.assign(un, GageCoherence::FULL);
        corr_len.assign(un, 0.0);
    }

    void grow_to(int n) {
        if (n <= count()) return;
        auto un = static_cast<std::size_t>(n);
        family.resize(un, DistType::UNIFORM);
        spread_kind.resize(un, SoftSpreadKind::CV);
        spread_const.resize(un, 0.0);
        spread_ts.resize(un, -1);
        spread_ts_name.resize(un, std::string{});
        configured.resize(un, false);
        coherence.resize(un, GageCoherence::FULL);
        corr_len.resize(un, 0.0);
    }

    void reset_state() noexcept {
        // Configuration-only container: no dynamic state to reset.
    }

    void shrink_to_fit() {
        family.shrink_to_fit();
        spread_kind.shrink_to_fit();
        spread_const.shrink_to_fit();
        spread_ts.shrink_to_fit();
        spread_ts_name.shrink_to_fit();
        configured.shrink_to_fit();
        coherence.shrink_to_fit();
        corr_len.shrink_to_fit();
    }
};

} // namespace openswmm::uncertainty

#endif // OPENSWMM_ENGINE_SOFT_RAIN_DATA_HPP
