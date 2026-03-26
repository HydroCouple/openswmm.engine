/**
 * @file QualityData.hpp
 * @brief SoA stores for land uses, buildup, washoff, and treatment.
 *
 * @details These stores persist across read/write for .inp round-trip fidelity.
 *          They are separate from the computational Landuse/Treatment modules
 *          which hold runtime state.
 *
 * @ingroup engine_data
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_ENGINE_QUALITY_DATA_HPP
#define OPENSWMM_ENGINE_QUALITY_DATA_HPP

#include <vector>
#include <string>

namespace openswmm {

// ============================================================================
// Land use definitions
// ============================================================================

struct LanduseData {
    int count() const { return static_cast<int>(sweep_interval.size()); }

    std::vector<double> sweep_interval;  ///< Days between sweeps
    std::vector<double> sweep_removal;   ///< Max removal fraction (0-100)
    std::vector<double> last_swept;      ///< Days since last swept

    void resize(int n) {
        auto un = static_cast<std::size_t>(n);
        sweep_interval.assign(un, 0.0);
        sweep_removal.assign(un, 0.0);
        last_swept.assign(un, 0.0);
    }
};

// ============================================================================
// Buildup function per (landuse x pollutant)
// ============================================================================

struct BuildupData {
    /// Index: [landuse * n_pollutants + pollutant]
    std::vector<int>    func_type;   ///< 0=NONE,1=POW,2=EXP,3=SAT,4=EXT
    std::vector<double> coeff1;
    std::vector<double> coeff2;
    std::vector<double> coeff3;
    std::vector<int>    normalizer;  ///< 0=PER_AREA, 1=PER_CURB

    int n_landuses = 0;
    int n_pollutants = 0;

    void resize(int nlu, int npoll) {
        n_landuses = nlu; n_pollutants = npoll;
        auto total = static_cast<std::size_t>(nlu * npoll);
        func_type.assign(total, 0);
        coeff1.assign(total, 0.0);
        coeff2.assign(total, 0.0);
        coeff3.assign(total, 0.0);
        normalizer.assign(total, 0);
    }
};

// ============================================================================
// Washoff function per (landuse x pollutant)
// ============================================================================

struct WashoffData {
    /// Index: [landuse * n_pollutants + pollutant]
    std::vector<int>    func_type;   ///< 0=NONE,1=EXP,2=RC,3=EMC
    std::vector<double> coeff;
    std::vector<double> expon;
    std::vector<double> sweep_effic; ///< 0-100
    std::vector<double> bmp_effic;   ///< 0-100

    int n_landuses = 0;
    int n_pollutants = 0;

    void resize(int nlu, int npoll) {
        n_landuses = nlu; n_pollutants = npoll;
        auto total = static_cast<std::size_t>(nlu * npoll);
        func_type.assign(total, 0);
        coeff.assign(total, 0.0);
        expon.assign(total, 0.0);
        sweep_effic.assign(total, 0.0);
        bmp_effic.assign(total, 0.0);
    }
};

// ============================================================================
// Treatment expression per (node x pollutant)
// ============================================================================

struct TreatmentData {
    /// Index: [node * n_pollutants + pollutant]
    std::vector<std::string> expressions;  ///< e.g. "R = 0.5 * exp(-0.1 * DT)"

    int n_nodes = 0;
    int n_pollutants = 0;

    void resize(int nn, int npoll) {
        n_nodes = nn; n_pollutants = npoll;
        expressions.assign(static_cast<std::size_t>(nn * npoll), "");
    }

    bool hasAny() const {
        for (const auto& e : expressions) if (!e.empty()) return true;
        return false;
    }
};

} // namespace openswmm

#endif // OPENSWMM_ENGINE_QUALITY_DATA_HPP
