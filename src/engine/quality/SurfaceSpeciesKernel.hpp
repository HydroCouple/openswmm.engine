// SPDX-License-Identifier: Apache-2.0
/**
 * @file SurfaceSpeciesKernel.hpp
 * @brief One subcatchment surface-quality arithmetic for every species family
 *        (Multispecies Phase 2, S1, 2026-10-15).
 *
 * @details Buildup, washoff, ponded-water mixing, LID-area loads, the washoff
 *          concentration and the post-LID load bookings, moved verbatim out of
 *          SWMMEngine::stepSurfaceQuality / bookWashoffLoads. A Block names
 *          the arrays one species family keeps: the pollutants (SubcatchData,
 *          PollutantData, the mass_balance qual_* rows, the land-use solver)
 *          or the reaction species (MsxSurfaceData). Both families therefore
 *          run the same expressions; before, the reaction species ran a
 *          transcription that had drifted (no fOutlet, a different RC form,
 *          the pre-LID denominator, no ponded mix and no LID rows).
 *
 *          Every per-subcatchment input (runoff, rainfall, areas, ponded
 *          depths, run-on water) is read from the context and is the same for
 *          both families.
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @license  Apache-2.0
 */
#ifndef OPENSWMM_ENGINE_QUALITY_SURFACE_SPECIES_KERNEL_HPP
#define OPENSWMM_ENGINE_QUALITY_SURFACE_SPECIES_KERNEL_HPP

#include <cstddef>
#include <vector>

#include "Landuse.hpp"

namespace openswmm {
struct SimulationContext;
namespace surfacespecies {

/// The arrays one species family keeps, sized for `n` species. Every pointer
/// is required except the optional co-pollutant pair (null for families with
/// no co-pollutants).
struct Block {
    int n   = 0;   ///< species in the family
    int nlu = 0;   ///< land uses
    const std::vector<landuse::BuildupParams>* bu_params = nullptr; ///< [lu*n + s]
    const std::vector<landuse::WashoffParams>* wo_params = nullptr; ///< [lu*n + s]
    std::vector<double>* buildup = nullptr;          ///< [(sc*nlu + lu)*n + s], per normalizer unit
    std::vector<double>* conc = nullptr;             ///< [sc*n + s], washoff concentration
    std::vector<double>* ponded_qual = nullptr;      ///< [sc*n + s], concentration mass
    std::vector<double>* runon_qual_rate = nullptr;  ///< [sc*n + s], concentration mass / s
    std::vector<double>* total_load = nullptr;       ///< [sc*n + s], user mass
    const std::vector<double>* mcf = nullptr;        ///< [s] concentration mass -> user mass
    const std::vector<double>* c_rain = nullptr;     ///< [s] rainfall concentration
    const std::vector<bool>*   snow_only = nullptr;  ///< [s]
    const std::vector<int>*    co_pollut = nullptr;  ///< [s] or null
    const std::vector<double>* co_frac = nullptr;    ///< [s] or null
    // Ledgers [s], user mass.
    std::vector<double>* led_surface_buildup = nullptr;
    std::vector<double>* led_wet_deposition = nullptr;
    std::vector<double>* led_bmp_removal = nullptr;
    std::vector<double>* led_infil_loss = nullptr;
    std::vector<double>* led_runoff_load = nullptr;
    std::vector<double>* led_sweeping = nullptr;
    std::vector<double>* led_ponded_lost = nullptr;   ///< ponded mass written off when dry
    /// Street sweeping this step: `[sc * nlu + lu]` events (shared by every
    /// family, SubcatchData::sweep_now) and the land use's removal fraction.
    const std::vector<unsigned char>* sweep_now = nullptr;
    const std::vector<double>* sweep_removal = nullptr;

    std::size_t bu_idx(int sc, int lu, int s) const noexcept {
        return static_cast<std::size_t>(sc) * static_cast<std::size_t>(nlu * n)
             + static_cast<std::size_t>(lu) * static_cast<std::size_t>(n)
             + static_cast<std::size_t>(s);
    }
};

/// Buildup, washoff and the washoff concentration for one runoff step
/// (legacy surfqual_getBuildup + surfqual_getWashoff). Runs before the LID
/// units; writes `subcatches.washoff_vout1` (shared by every family).
void step(SimulationContext& ctx, Block& b, double dt_runoff,
          double old_runoff_time) noexcept;

/// Post-LID load bookings (legacy surfqual_getWashoff's tail): the captured
/// volume split into LID infiltration and BMP removal, the outflow load, and
/// the runoff ledger. `lid_infil_vol[sc]` is the LID units' infiltration
/// volume this step (ft3).
void bookPostLid(SimulationContext& ctx, Block& b, double dt_runoff,
                 const std::vector<double>& lid_infil_vol) noexcept;

}  // namespace surfacespecies
}  // namespace openswmm

#endif
