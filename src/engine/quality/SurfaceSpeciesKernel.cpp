// SPDX-License-Identifier: Apache-2.0
/**
 * @file SurfaceSpeciesKernel.cpp
 * @brief See SurfaceSpeciesKernel.hpp. The bodies below are
 *        SWMMEngine::stepSurfaceQuality and bookWashoffLoads moved verbatim;
 *        only the names of the arrays changed (they now come from the Block).
 */
#include "SurfaceSpeciesKernel.hpp"

#include <algorithm>
#include <cmath>

#include "../core/SimulationContext.hpp"
#include "../core/UnitConversion.hpp"
#include "../core/DateTime.hpp"
#include "../data/TableData.hpp"

namespace openswmm::surfacespecies {

void step(SimulationContext& ctx, Block& b, double dt_runoff,
          double old_runoff_time) noexcept {
    if (b.n <= 0) return;

    //      Matching legacy surfqual_getBuildup + surfqual_getWashoff
    // legacy surfqual_getWashoff (surfqual.c:283-286) returns only on
    //     Nobjects[POLLUT] == 0 || area <= 0.0
    // — there is NO land-use condition, and there must not be one here. Only
    // findWashoffLoads needs a land use; findPondedLoads does not, and that is
    // where WET DEPOSITION lives: rainfall carrying Pollut[p].pptConcen onto
    // the surface (surfqual.c:428), which a deck can have with no [LANDUSES]
    // at all. Requiring a land use skipped the whole step for those decks, so
    // they ran with zero pollutant from any source and reported a 0.000 %
    // runoff-quality continuity error over it — lid1d books 263,134 kg of COD
    // deposition in legacy and 0.000 here. The land-use loop below is already
    // correct at nlu == 0: it simply does not execute, exactly as legacy's
    // findWashoffLoads contributes nothing without coverages.
    if (b.n > 0) {
        int np = b.n;
        int nlu = b.nlu;
        // legacy consts.h MIN_RUNOFF: 2.31481e-8 ft/sec = 0.001 in/hr.
        constexpr double MIN_RUNOFF_FTS = 2.31481e-8;
        double dt_days = dt_runoff / ucf::SEC_PER_DAY;

        const double ucf_landarea_sq = ucf::UCF(ucf::LANDAREA, ctx.options);
        for (int i = 0; i < ctx.n_subcatches(); ++i) {
            auto ui = static_cast<std::size_t>(i);
            // `q` is legacy's Voutflow / tStep — the PRE-LID runoff volume
            // rate. stepSurfaceQuality runs before the LID block subtracts
            // the captured share (A4c precedes A6), exactly as legacy's
            // surfqual_getWashoff reads the Voutflow accumulated by
            // subcatch_getRunoff.
            double q = ctx.subcatches.runoff[ui];  // cfs
            double area_ac = ctx.subcatches.area[ui]; // acres
            const double area_ft2_sc = area_ac / ucf_landarea_sq;

            // legacy's `runoff` argument to findWashoffLoads /
            // landuse_getWashoffQual: the area-averaged subarea runoff RATE
            // in ft/sec (subcatch_getRunoff's return, `runoff / area`), which
            // carries NO fOutlet and no LID adjustment. Using Voutflow/area
            // instead understates it on any deck with inter-subarea routing.
            const double runoff_fts = (ui < ctx.subcatches.subarea_runoff_rate.size())
                                    ? ctx.subcatches.subarea_runoff_rate[ui] : 0.0;

            // legacy surfqual_getWashoff (surfqual.c:305-316): the washoff
            // CONCENTRATION denominator is the pre-LID inflow volume
            //     vOut1 = Voutflow + vLidRain + vLidRunon
            // — rain landing on the LID footprint, and the subcatchment's
            // run-on when the units cover it entirely, are water that dilutes
            // the washoff without carrying any of it. v6 divided by the
            // runoff alone, so a fully LID-covered subcatchment reported its
            // undiluted EMC: variations-on-emc's wRG_O_100 read a flat
            // 100 mg/L where legacy ramps 60 -> 84 -> 92 -> 95 as surface
            // runoff grows against the rain on the rain garden.
            double q_out1 = q;
            {
                const double lid_ft2_sc = (ui < ctx.subcatches.total_lid_area_ft2.size())
                                        ? ctx.subcatches.total_lid_area_ft2[ui] : 0.0;
                if (lid_ft2_sc > 0.0) {
                    q_out1 += ctx.subcatches.rainfall[ui] * lid_ft2_sc;
                    if (area_ft2_sc == lid_ft2_sc)
                        q_out1 += ctx.subcatches.runon_rate[ui] * area_ft2_sc;
                }
            }
            // Keep vOut1 for bookWashoffLoads. By the time the mass bookings
            // can be made the LID units have run and `runoff` is the POST-LID
            // value, so this is the last moment the pre-LID volume exists.
            if (ui < ctx.subcatches.washoff_vout1.size())
                ctx.subcatches.washoff_vout1[ui] = q_out1 * dt_runoff;

            for (int p = 0; p < np; ++p) {
                auto sq_idx = ui * static_cast<std::size_t>(np)
                              + static_cast<std::size_t>(p);
                // THE UNIT CONVENTION, fixed 2026-08-23 after the known-mass
                // audit (QUALITY_LEDGER_UNITS_AUDIT §7): total_washoff_load
                // accumulates in CONCENTRATION MASS UNITS per second (mg/s,
                // or µg/s, or counts/s — whatever the pollutant's units are),
                // and every LEDGER booking converts to USER MASS (lbs/kg) at
                // the seam via mcf_p, exactly as legacy applies Pollut[].mcf
                // at source (landuse.c:585, surfqual.c:352/357/366). Before
                // this, the accumulator mixed three unit systems — EMC in
                // mg/L·ft³/s, EXPON in user-mass/s, RATING in mg/s — and the
                // ledger row printed 16057× legacy on a 100 mg/L EMC deck
                // while the Washoff Summary printed 1/28.3 of it.
                //
                // mcf_p mirrors legacy landuse.c:167-169: UCF(MASS) for mg,
                // /1000 for µg, 1.0 for counts.
                const double mcf_p = (*b.mcf)[static_cast<std::size_t>(p)];
                constexpr double kLperFt3 = 28.317;
                double total_washoff_load = 0.0; // concen. mass units / sec
                // legacy landuse_getAvgBmpEffic(j, p): the coverage-weighted
                // BMP removal fraction, which findPondedLoads applies to the
                // ponded outflow load.
                double avg_bmp_effic = 0.0;

                // Iterate over land uses weighted by coverage
                // Buildup is now stored PER LAND USE: bu_idx(i, lu, p)
                for (int lu = 0; lu < nlu; ++lu) {
                    auto cov_idx = ui * static_cast<std::size_t>(nlu)
                                   + static_cast<std::size_t>(lu);
                    double frac = (cov_idx < ctx.subcatches.coverage.size())
                                  ? ctx.subcatches.coverage[cov_idx] / 100.0 : 0.0;
                    avg_bmp_effic +=
                        frac * (*b.wo_params)[static_cast<std::size_t>(
                                       lu * np + p)]
                                   .bmp_effic / 100.0;
                    if (frac <= 0.0) continue;

                    auto k = static_cast<std::size_t>(lu * np + p);
                    const auto& bp = (*b.bu_params)[k];
                    const auto& wp = (*b.wo_params)[k];
                    auto bu = b.bu_idx(i, lu, p);

                    // Normalizer for this land use (absolute mass = buildup * norm)
                    double norm = (bp.normalizer == 0)
                        ? frac * area_ac : frac * ctx.subcatches.curb_length[ui];

                    // Multispecies Phase 2 (S7 parity): street sweeping, in
                    // legacy's place — after buildup, before washoff
                    // (runoff.c: getBuildup, sweepBuildup, getWashoff). The
                    // events were decided before this step by
                    // SWMMEngine::planSweeping; v6 used to sweep after the
                    // washoff, so on a recession step the land washed off
                    // what legacy had already swept.
                    auto apply_sweep = [&]() {
                        if (!b.sweep_now || !b.led_sweeping || !b.sweep_removal) return;
                        const auto sk = ui * static_cast<std::size_t>(nlu) + static_cast<std::size_t>(lu);
                        if (sk >= b.sweep_now->size() || !(*b.sweep_now)[sk]) return;
                        const double removal_frac = (*b.sweep_removal)[static_cast<std::size_t>(lu)];
                        const double effic = wp.sweep_effic / 100.0;
                        const double old_bu = (*b.buildup)[bu];
                        double new_bu = old_bu * (1.0 - removal_frac * effic);
                        new_bu = std::min(old_bu, new_bu);
                        new_bu = std::max(0.0, new_bu);
                        (*b.buildup)[bu] = new_bu;
                        (*b.led_sweeping)[static_cast<std::size_t>(p)] += (old_bu - new_bu) * norm;
                    };

                    // --- Buildup accumulation (matching legacy
                    //     surfqual_getBuildup). Legacy calls it from
                    //     runoff.c:280 under `if (runoff < MIN_RUNOFF)`:
                    //     pollutant accumulates only while the subcatchment is
                    //     effectively DRY, and stops for as long as it is
                    //     running off. This engine accumulated on every runoff
                    //     step, through the storm as well as between storms, so
                    //     it kept laying down mass that the washoff was
                    //     simultaneously removing — runoff44-sw5 built up
                    //     64.383 lb of COD over the run against legacy's
                    //     18.148. The test is the complement of the washoff
                    //     gate below, on the same ft/sec rate.
                    //
                    //     The snow-only skip is legacy's too (surfqual.c:141):
                    //     a SnowOnly pollutant does not accumulate on bare
                    //     ground, only under a pack deeper than 0.001 in.
                    const bool subcatch_is_dry = (runoff_fts < MIN_RUNOFF_FTS);
                    const bool snow_only_blocked =
                        (static_cast<std::size_t>(p) < (*b.snow_only).size() &&
                         (*b.snow_only)[static_cast<std::size_t>(p)] &&
                         (ui >= ctx.subcatches.snow_depth.size() ||
                          ctx.subcatches.snow_depth[ui] < 0.001 / 12.0));
                    if (bp.type != landuse::BuildupType::NONE &&
                        subcatch_is_dry && !snow_only_blocked) {
                        double mass = (*b.buildup)[bu];  // per-normalizer-unit

                        // Gap #35: EXTERNAL buildup uses a time series for rate.
                        // Matches legacy landuse_getExternalBuildup():
                        //   rate = sf * tseries_lookup(ts, time)
                        //   buildup = MIN(buildup + rate * dt_days, maxBuildup)
                        if (bp.type == landuse::BuildupType::EXTERNAL) {
                            double max_bu = bp.coeff[0];
                            double sf     = bp.coeff[1];   // scaling factor
                            int ts_idx    = static_cast<int>(bp.coeff[2]);
                            double rate   = 0.0;
                            if (ts_idx >= 0 &&
                                ts_idx < static_cast<int>(ctx.tables.tables.size())) {
                                double abs_t = datetime::addSeconds(
                                    ctx.options.start_date, old_runoff_time);
                                rate = sf * table_lookup_cursor(
                                    ctx.tables.tables[static_cast<std::size_t>(ts_idx)],
                                    abs_t);
                            }
                            double new_mass = std::min(mass + rate * dt_days, max_bu);
                            new_mass = std::max(new_mass, 0.0);
                            double buildup_change = new_mass - mass;
                            if (buildup_change != 0.0) {
                                (*b.buildup)[bu] = new_mass;
                                (*b.led_surface_buildup)[
                                    static_cast<std::size_t>(p)] += buildup_change * norm;
                            }
                            apply_sweep();
                            continue;
                        }

                        double days = 0.0;
                        double c0 = bp.coeff[0], c1 = bp.coeff[1], c2 = bp.coeff[2];
                        // Inverse: mass → equivalent days
                        if (mass > 0.0) {
                            switch (bp.type) {
                                case landuse::BuildupType::POWER:
                                    days = (c1 * c2 > 0.0) ? std::pow(mass / c1, 1.0 / c2) : 0.0;
                                    break;
                                case landuse::BuildupType::EXPON:
                                    days = (c0 * c1 > 0.0 && mass < c0) ? -std::log(1.0 - mass / c0) / c1 : 0.0;
                                    break;
                                case landuse::BuildupType::SATUR:
                                    days = (c0 > mass) ? mass * c2 / (c0 - mass) : bp.max_days;
                                    break;
                                default: break;
                            }
                        }
                        days += dt_days;
                        // Forward: days → new mass
                        double new_mass = 0.0;
                        if (days > 0.0) {
                            switch (bp.type) {
                                case landuse::BuildupType::POWER:
                                    new_mass = std::min(c1 * std::pow(days, c2), c0);
                                    break;
                                case landuse::BuildupType::EXPON:
                                    new_mass = c0 * (1.0 - std::exp(-c1 * days));
                                    break;
                                case landuse::BuildupType::SATUR:
                                    new_mass = (c2 + days > 0.0) ? c0 * days / (c2 + days) : 0.0;
                                    break;
                                default: break;
                            }
                        }
                        // legacy surfqual.c:149 `newBuildup = MAX(newBuildup,
                        // oldBuildup)` — buildup never DECREASES across a
                        // step, whatever the inverse/forward round trip
                        // returns.
                        new_mass = std::max(new_mass, mass);
                        double buildup_change = new_mass - mass;
                        (*b.buildup)[bu] = new_mass;

                        // Track net buildup increase for mass balance
                        if (buildup_change > 0.0) {
                            (*b.led_surface_buildup)[
                                static_cast<std::size_t>(p)] += buildup_change * norm;
                        }
                    }
                    apply_sweep();

                    // --- Washoff computation (matching legacy surfqual_getWashoff)
                    // legacy findWashoffLoads returns early below MIN_RUNOFF
                    // (consts.h 2.31481e-8 ft/sec = 0.001 in/hr), tested on
                    // the ft/sec rate — not on a cfs flow against 1e-9.
                    // legacy landuse_getWashoffLoad closes with
                    //   washoffLoad = washoffQual * vOutflow * landuseArea/area
                    // — the load is PROPORTIONAL to vOutflow, the runoff
                    // volume that actually leaves the subcatchment. A
                    // subcatchment covered entirely by LID has Voutflow = 0
                    // and therefore washes off NOTHING, whatever is on the
                    // ground. `q` here is that same number (legacy's
                    // Voutflow/tStep), and requiring it keeps the all-LID case
                    // at zero; where runoff does leave, the magnitude already
                    // matches legacy exactly (runoff44-sw5's Surface Runoff
                    // ledger term is exact), so this only closes the zero end.
                    if (wp.type != landuse::WashoffType::NONE &&
                        runoff_fts >= MIN_RUNOFF_FTS && q > 0.0) {
                        double buildup = (*b.buildup)[bu];

                        // Unit conversions matching legacy landuse_getWashoffLoad:
                        //   EXPON uses unit runoff in display units (in/hr or mm/hr):
                        //     q_expon = (q_cfs / area_ft2) * UCF(RAINFALL)
                        //   RATING and EMC use volumetric flow in display units:
                        //     q_flow = q_cfs * UCF(FLOW)
                        // legacy keeps Subcatch.area in ft2 as
                        // area / UCF(LANDAREA) — 43561.6 ft2 per ACRE and
                        // 107639 per HECTARE. The exact-acre 43560 is
                        // 3.7e-5 off on a US deck and a factor 2.471 off
                        // on an SI one;
                        // on swmm5buildupwashoff (CMS) that made the EXPON
                        // washoff's unit runoff 2.471x too high, so the
                        // 40 kg of SF1 on the ground left in three report
                        // periods instead of ten.
                        double area_ft2 = area_ac / ucf::UCF(ucf::LANDAREA, ctx.options);
                        double q_expon = runoff_fts
                                       * ucf::UCF(ucf::RAINFALL, ctx.options);
                        double q_flow   = q * ucf::UCF(ucf::FLOW, ctx.options);
                        // legacy's land-use area, `landFactor[i].fraction *
                        // Subcatch[j].area` (landuse.c:346).
                        const double landuse_ft2 = frac * area_ft2;

                        // `load` is in CONCENTRATION MASS UNITS per second
                        // (mg/s for a mg/L pollutant). Each branch mirrors
                        // legacy landuse_getWashoffQual × Q, with the
                        // parse-time coefficient pre-multiplies
                        // (landuse.c:332-334) applied here at runtime instead,
                        // because our parser stores the user coefficient raw:
                        //
                        //   EMC:    coeff[mg/L] × LperFT3 → mg/ft³, × Q[cfs].
                        //           Legacy uses INTERNAL flow here, not
                        //           UCF(FLOW) — the previous q_flow form was
                        //           inert under CFS and wrong under any other
                        //           flow unit.
                        //   EXPON:  coeff is PER HOUR in the deck; legacy
                        //           divides by 3600 at parse. Without it the
                        //           load was 3600× the formulation. Buildup
                        //           is stored in user mass, so /mcf brings it
                        //           to concentration mass units (legacy:
                        //           `buildup / Pollut[p].mcf`).
                        //   RATING: coeff × (Q·UCF(FLOW))^expon is already
                        //           legacy's coeff·UCF^e × Q_int^e — unchanged.
                        double load = 0.0; // concen. mass units / sec
                        switch (wp.type) {
                            case landuse::WashoffType::EMC:
                                load = wp.coeff * kLperFt3 * q * frac;
                                break;
                            // EXPON and RATING both close through legacy's
                            // landuse_getWashoffLoad (landuse.c:351):
                            //   washoffLoad = washoffQual * vOutflow
                            //                 * landuseArea / area * mcf
                            // where washoffQual is a CONCENTRATION formed by
                            // dividing the rate by `runoff * landuseArea`.
                            // With vOutflow = q * dt, the step's mass carries
                            // the factor q / (runoff_fts * area) — the share
                            // of the subareas' runoff that actually reaches
                            // the OUTLET, legacy's fOutlet. v6 dropped it, so
                            // any deck whose [SUBAREAS] routes one subarea
                            // into another washed off as if every drop left
                            // directly. On greenroofs — where subcatchment 9
                            // sends 97 % of its impervious runoff to the
                            // pervious subarea — that published 1983 mg/L of
                            // TKN on the first wet period against legacy's 92,
                            // stripped the buildup store, and then ran BELOW
                            // legacy for the rest of the storm; the crossover
                            // at period 17 is the depletion catching up.
                            case landuse::WashoffType::EXPON:
                                if (buildup > 0.0 && mcf_p > 0.0 &&
                                    landuse_ft2 > 0.0) {
                                    double wq = (wp.coeff / 3600.0)
                                              * std::pow(q_expon, wp.expon)
                                              * (buildup * norm) / mcf_p;
                                    wq /= runoff_fts * landuse_ft2;
                                    load = ((wq * q) * landuse_ft2) / area_ft2;
                                }
                                break;
                            case landuse::WashoffType::RATING:
                                if (area_ft2 > 0.0) {
                                    // legacy pre-multiplies the RATING coeff
                                    // by UCF(FLOW)^expon at parse
                                    // (landuse.c:333); our parser keeps the
                                    // user value, so it is applied here.
                                    double wq = wp.coeff
                                              * std::pow(ucf::UCF(ucf::FLOW,
                                                                  ctx.options),
                                                         wp.expon)
                                              * std::pow(runoff_fts * landuse_ft2,
                                                         wp.expon - 1.0);
                                    load = ((wq * q) * landuse_ft2) / area_ft2;
                                }
                                (void)q_flow;
                                break;
                            default: break;
                        }

                        // Cap washoff to available buildup. Buildup is user
                        // mass; the cap must be in the SAME units as `load`
                        // (concen. mass), or the comparison is the mixed-unit
                        // defect all over again — before this fix the EMC/
                        // RATING mg-based load was compared against a lbs cap.
                        double avail = (mcf_p > 0.0)
                            ? (buildup * norm) / mcf_p : 0.0;
                        double max_load = (dt_runoff > 0.0) ? avail / dt_runoff : 0.0;
                        // legacy landuse_getWashoffLoad caps every washoff
                        // type only when a buildup function is modelled.
                        // RATING without buildup supplies its own mass, just
                        // as EMC does; capping it to an empty store erases it.
                        // A capped step washes off the whole store: legacy
                        // subtracts MIN(washoffLoad, buildup) in user mass and
                        // leaves EXACTLY 0. The rate round trip below leaves
                        // a ~1e-13 residue, and the next dry step's power-law
                        // inverse turns that into a nonzero equivalent age.
                        bool capped = false;
                        if (load > max_load && bp.type != landuse::BuildupType::NONE) {
                            load = max_load;
                            capped = true;
                        }

                        // Reduce per-landuse buildup by washoff amount —
                        // buildup is user mass per normalizer unit, so the
                        // reduction converts back (legacy reduces buildup by
                        // washoffLoad, which is already lbs).
                        //
                        // ...unless there is NO buildup function and the
                        // washoff exceeds what is on the ground, in which
                        // case legacy books the load as BUILDUP_LOAD instead
                        // (landuse.c:585-593, "otherwise add washoff to
                        // buildup mass balance totals so that things will
                        // balance"). Without that branch an EMC-only deck
                        // discharges mass the ledger never received: the
                        // Runoff Quality Continuity block reads Surface
                        // Buildup 0.000 against Surface Runoff 113.269 where
                        // legacy reads 113.082 on both. It is also what makes
                        // the printed continuity error meaningful rather than
                        // merely non-zero.
                        const double washed_mass = load * mcf_p * dt_runoff;
                        const bool has_buildup_fn =
                            bp.type != landuse::BuildupType::NONE;
                        if (!has_buildup_fn && washed_mass > buildup * norm) {
                            auto upb2 = static_cast<std::size_t>(p);
                            if (upb2 < (*b.led_surface_buildup).size())
                                (*b.led_surface_buildup)[upb2] +=
                                    washed_mass;
                            (*b.buildup)[bu] = 0.0;
                        } else if (capped) {
                            (*b.buildup)[bu] = 0.0;
                        } else if (norm > 0.0) {
                            double washed = washed_mass / norm;
                            (*b.buildup)[bu] =
                                std::max((*b.buildup)[bu] - washed, 0.0);
                        }

                        // BMP removal — and BOOK it. qual_bmp_removal had no
                        // writer anywhere (Finding 8, the fourth
                        // rendered-but-never-written ledger row after the
                        // snapshot quality vectors, the snow rows and the
                        // subcatchment temperature column). Legacy books it at
                        // surfqual.c:352 in user mass.
                        double bmp_removed = load * (wp.bmp_effic / 100.0);
                        if (bmp_removed > 0.0) {
                            auto upb = static_cast<std::size_t>(p);
                            if (upb < (*b.led_bmp_removal).size())
                                (*b.led_bmp_removal)[upb] +=
                                    bmp_removed * dt_runoff * mcf_p;
                            load -= bmp_removed;
                        }

                        total_washoff_load += load;
                    }
                } // end land use loop

                // Ponded quality tracking — Gap #36: complete-mix balance
                // Matches legacy findPondedLoads() in surfqual.c.
                // Mixes wet deposition with existing ponded mass; distributes
                // to infiltration and runoff outflow proportionally.
                const double lid_ft2_pq =
                    (ui < ctx.subcatches.total_lid_area_ft2.size())
                        ? ctx.subcatches.total_lid_area_ft2[ui] : 0.0;
                const double area_ft2_full_pq =
                    area_ac / ucf::UCF(ucf::LANDAREA, ctx.options);
                // legacy findPondedLoads opens `if (area == lidArea) return;`
                // — a subcatchment that is entirely LID has no ponded surface
                // to mix into.
                const double non_lid_ft2_pq = area_ft2_full_pq - lid_ft2_pq;
                if (sq_idx < (*b.ponded_qual).size() &&
                    non_lid_ft2_pq > 0.0) {
                    constexpr double L_PER_FT3 = 28.317;  // liters per ft3
                    auto up = static_cast<std::size_t>(p);
                    // Every volume here is over the NON-LID area: legacy forms
                    // vRain, the ponded store and the closing pondedQual on
                    // `nonLidArea`, not the full area.
                    const double area_ft2_pq = non_lid_ft2_pq;

                    // Rainfall at this subcatch, INTERNAL ft/sec. This read
                    // the GAGE's rate, which is carried in the project's
                    // rainfall units (in/hr or mm/hr) — legacy's
                    // `Subcatch[j].rainfall` is ft/sec. On a US deck that is
                    // a factor of 12*3600 = 43200, and it went straight into
                    // the wet-deposition volume: runoff44-sw5 booked 843,742
                    // lb of deposited COD against legacy's 19.531, and
                    // reported a 99.998 % runoff-quality continuity error.
                    // `subcatches.rainfall` is the same number legacy uses,
                    // already scaled by the gage and subcatchment factors.
                    double rrate = (ui < ctx.subcatches.rainfall.size())
                        ? ctx.subcatches.rainfall[ui] : 0.0;

                    // Volumes over dt (ft3)
                    double v_rain_pq    = rrate * area_ft2_pq * dt_runoff;
                    double v_runon_pq   = ctx.subcatches.runon_inflow[ui] * dt_runoff;
                    double v_infil_pq   = ctx.subcatches.infil_loss[ui] * area_ft2_pq * dt_runoff;
                    double v_outflow_pq = q * dt_runoff;

                    // Vinflow: rain + runon + the water ALREADY ponded at the
                    // START of the step (legacy subcatch.c:694 forms this
                    // before the subareas are stepped, which is why the
                    // opening depth is kept aside rather than read back here).
                    double v_ponded_pq  =
                        ctx.subcatches.old_ponded_depth[ui] * area_ft2_pq;
                    double v_inflow_pq  = v_rain_pq + v_runon_pq + v_ponded_pq;

                    // Wet deposition mass: c_rain[mg/L] * LperFT3[L/ft3] * v_rain[ft3]
                    double c_rain_pq = (up < (*b.c_rain).size())
                        ? (*b.c_rain)[up] : 0.0;
                    double w_rain_pq = c_rain_pq * L_PER_FT3 * v_rain_pq;

                    // Accumulate wet deposition in mass balance — LEDGER
                    // booking, so convert to user mass at the seam (legacy
                    // books WET_DEPOSITION_LOAD in lbs/kg).
                    if (w_rain_pq > 0.0 && up < (*b.led_wet_deposition).size())
                        (*b.led_wet_deposition)[up] += w_rain_pq * mcf_p;

                    double w_mass_pq = (*b.ponded_qual)[sq_idx];  // mg

                    if (v_inflow_pq <= 0.0) {
                        // Dry surface. The `+=` into qual_final_buildup that
                        // used to sit here was DEAD (Finding 9):
                        // computeFinalQualityMassBalance overwrites the term
                        // with `=` at end-of-run, discarding every value
                        // accumulated here — and it was in mg against that
                        // site's user mass, so surviving would have corrupted
                        // the term. Removed rather than converted. The
                        // residual ponded mass at final time is NOT in the
                        // final-buildup term on either path; that matches the
                        // 4073 computation and is recorded as a small parity
                        // gap against legacy's FINAL_STORED_LOAD.
                        (*b.ponded_qual)[sq_idx] = 0.0;
                    } else {
                        // Complete-mix balance (matching legacy findPondedLoads):
                        // wRunon = the run-on load assembleRunon accumulated
                        // from the donors' oldRunoff x oldQual (surfqual.c:
                        // 442-443, reading the newQual accumulator) — the
                        // run-on WATER was already in v_inflow_pq, so leaving
                        // the load out both starved and DILUTED the mix.
                        double w_runon_pq =
                            (sq_idx < (*b.runon_qual_rate).size())
                                ? (*b.runon_qual_rate)[sq_idx] *
                                      dt_runoff
                                : 0.0;
                        double w_total_pq  = w_mass_pq + w_rain_pq + w_runon_pq;
                        double c_ponded_pq = w_total_pq / v_inflow_pq;  // mg/ft3

                        // Mass lost to infiltration
                        double w_infil_pq = std::min(c_ponded_pq * v_infil_pq, w_total_pq);
                        w_total_pq -= w_infil_pq;
                        // Ledger booking → user mass (legacy INFIL_LOAD, lbs/kg)
                        if (w_infil_pq > 0.0 && up < (*b.led_infil_loss).size())
                            (*b.led_infil_loss)[up] += w_infil_pq * mcf_p;

                        // Mass carried out with runoff outflow
                        double w_outflow_pq = std::min(c_ponded_pq * v_outflow_pq, w_total_pq);
                        w_total_pq -= w_outflow_pq;

                        // legacy reduces the ponded outflow load by the land
                        // uses' area-weighted BMP efficiency, exactly as the
                        // washoff loads above are reduced. Only the washoff
                        // half was booked here, so a BMP removed nothing from
                        // the rain- and run-on-borne share.
                        double bmp_pq = avg_bmp_effic * w_outflow_pq;
                        if (bmp_pq > 0.0) {
                            if (up < (*b.led_bmp_removal).size())
                                (*b.led_bmp_removal)[up] +=
                                    bmp_pq * mcf_p;
                            w_outflow_pq -= bmp_pq;
                        }

                        // Add ponded outflow to total washoff load (mass/sec)
                        if (w_outflow_pq > 0.0 && dt_runoff > 0.0)
                            total_washoff_load += w_outflow_pq / dt_runoff;

                        // legacy closes by RE-DERIVING the ponded store from
                        // the mixed concentration and the depth the step
                        // actually ended at — `cPonded * subcatch_getDepth(j)
                        // * nonLidArea` — not by carrying whatever mass was
                        // left over. The two differ whenever the surface
                        // gained or lost water, and the leftover form drained
                        // the pool to nothing: a subcatchment shed its
                        // rainfall-borne load the instant the rain stopped
                        // instead of releasing it down the recession.
                        (*b.ponded_qual)[sq_idx] = std::max(
                            c_ponded_pq * ctx.subcatches.ponded_depth[ui] *
                                area_ft2_pq,
                            0.0);
                    }
                }

                // Gap #37: LID area wet deposition and runon quality loads.
                // Matches legacy findLidLoads() in surfqual.c.
                // Wet deposition falling on the LID surface contributes to outflow
                // quality. Runon quality loads are counted only when LIDs occupy the
                // full subcatchment (all runon arrives on the LID surface).
                if (ui < ctx.subcatches.total_lid_area_ft2.size()) {
                    double lid_ft2 = ctx.subcatches.total_lid_area_ft2[ui];
                    if (lid_ft2 > 0.0 && dt_runoff > 0.0) {
                        constexpr double L_PER_FT3_37 = 28.317;
                        // legacy `vLidRain = Subcatch[j].rainfall * lidArea *
                        // tStep`, and Subcatch[j].rainfall is INTERNAL ft/sec.
                        // This read ctx.gages.rainfall, which carries the
                        // project's DISPLAY rate — in/hr on a US deck, mm/hr
                        // on an SI one — straight into a volume, exactly the
                        // defect the ponded stage carried until b0924c78. The
                        // inflated volume never reached the vOut1 denominator
                        // (which already used subcatches.rainfall), so it
                        // landed undivided on the numerator: greenroofs'
                        // subcatchment 3 washed off 64,673,001 against
                        // legacy's 1,264.
                        double rrate37 = (ui < ctx.subcatches.rainfall.size())
                                          ? ctx.subcatches.rainfall[ui] : 0.0;
                        auto up37 = static_cast<std::size_t>(p);
                        double c_rain37 = (up37 < (*b.c_rain).size())
                                          ? (*b.c_rain)[up37] : 0.0;
                        // Wet deposition on LID area (mass/sec)
                        double v_lid_rain = rrate37 * lid_ft2 * dt_runoff;  // ft³
                        double w_lid_rain = c_rain37 * L_PER_FT3_37 * v_lid_rain;  // mg
                        if (w_lid_rain > 0.0) {
                            total_washoff_load += w_lid_rain / dt_runoff;
                            // legacy books the LID footprint's share of the
                            // wet deposition too (surfqual.c:496). Only the
                            // ponded stage booked it here, so the ledger was
                            // short by exactly the LID area's rain.
                            if (up37 < (*b.led_wet_deposition).size())
                                (*b.led_wet_deposition)[up37] +=
                                    w_lid_rain * mcf_p;
                        }
                        // Runon quality (only when LIDs cover full subcatchment)
                        double full_ft2 = ctx.subcatches.area[ui]
                                        / ucf::UCF(ucf::LANDAREA, ctx.options);
                        if (std::fabs(lid_ft2 - full_ft2) < 1.0) {
                            // legacy findLidLoads (surfqual.c:560-561) reads
                            // the SAME accumulator findPondedLoads reads —
                            // the donors' oldRunoff x oldQual assembled into
                            // newQual — not the RECEIVER's own concentration,
                            // which this block used before (right only when
                            // donor and receiver happened to share an EMC).
                            double w_lid_runon =
                                (sq_idx < (*b.runon_qual_rate).size())
                                    ? (*b.runon_qual_rate)[sq_idx] *
                                          dt_runoff
                                    : 0.0;                                   // mg
                            if (w_lid_runon > 0.0)
                                total_washoff_load += w_lid_runon / dt_runoff;
                        }
                    }
                }

                // Convert washoff load to the REPORTED concentration, mg/L.
                // load/q is mass/ft³; ÷ LperFT3 gives mass/L, exactly
                // legacy's `newQual[p] = cOut / LperFT3` (surfqual.c:370).
                // For EMC this leaves the value where it already was (the
                // old form skipped LperFT3 in both load and conc, cancelling);
                // for EXPON and RATING it is a real correction — their
                // concentrations were in incompatible units before this.
                double conc = 0.0;
                if (q_out1 > 0.0 && total_washoff_load > 0.0)
                    conc = total_washoff_load / q_out1 / kLperFt3;

                (*b.conc)[sq_idx] = conc;

                // The mass bookings that used to close this loop now live in
                // bookWashoffLoads, called after the LID units have run: they
                // need the POST-LID volumes, which do not exist yet here.
                // Only the concentration is formed at this point, because
                // legacy forms it over the PRE-LID vOut1 (surfqual.c:330).
            } // end pollutant loop

            // --- Co-pollutant washoff (legacy findWashoffLoads,
            // surfqual.c:505-521). A pollutant may be generated as a FRACTION
            // of another's washoff:
            //
            //     k = Pollut[p].coPollut;
            //     if (k >= 0) {
            //         w = Pollut[p].coFraction * OutflowLoad[k];
            //         massbal_updateLoadingTotals(BUILDUP_LOAD, p, w*mcf);
            //         OutflowLoad[p] += w;
            //     }
            //
            // [POLLUTANTS] parses coPollut/coFraction into ctx.pollutants and
            // LanduseSolver::applyCoPollutant implements the relation and is
            // unit-tested — but NOTHING in the engine ever called it, and this
            // loop never mentioned a co-pollutant. Five corpus decks declare
            // Lead as 20 % of TSS and v6 washed off exactly ZERO Lead:
            // pond-design's ORI-19 read 0 against legacy's 5.112 from the
            // first wet period, and the gap propagated to every node and link
            // downstream.
            //
            // It must run AFTER the whole pollutant loop, over the SUMMED
            // loads, exactly as legacy does — and as a single FORWARD pass, so
            // that a co-pollutant of a co-pollutant sees the already-updated
            // value where legacy would. Operating on `conc` is equivalent to
            // operating on OutflowLoad: both divide by the same q_out1 and
            // LperFT3, so the fraction carries through unchanged.
            // Co-pollutants exist only for the pollutant family.
            if (q_out1 > 0.0 && b.co_pollut && b.co_frac) {
                for (int p = 0; p < np; ++p) {
                    const auto up = static_cast<std::size_t>(p);
                    if (up >= (*b.co_pollut).size()) continue;
                    const int k = (*b.co_pollut)[up];
                    if (k < 0 || k >= np) continue;
                    const double frac = (up < (*b.co_frac).size())
                                        ? (*b.co_frac)[up] : 0.0;
                    if (frac <= 0.0) continue;

                    const auto ip = ui * static_cast<std::size_t>(np) + up;
                    const auto ik = ui * static_cast<std::size_t>(np)
                                  + static_cast<std::size_t>(k);
                    if (ip >= (*b.conc).size() ||
                        ik >= (*b.conc).size()) continue;

                    const double add = frac * (*b.conc)[ik];
                    if (add <= 0.0) continue;
                    (*b.conc)[ip] += add;

                    // legacy books the co-pollutant's share as BUILDUP_LOAD so
                    // the ledger balances — this mass was never on the ground.
                    const double mcf_cp = (*b.mcf)[up];
                    const double w = add * q_out1 * 28.317;
                    if (up < (*b.led_surface_buildup).size())
                        (*b.led_surface_buildup)[up] +=
                            w * dt_runoff * mcf_cp;
                }
            }
        } // end subcatch loop
    }
}

void bookPostLid(SimulationContext& ctx, Block& b, double dt_runoff,
                 const std::vector<double>& lid_infil_vol) noexcept {

    const int np = b.n;
    const int ns = ctx.n_subcatches();
    // No land-use condition, for the same reason stepSurfaceQuality has none:
    // the load this books is whatever reached the runoff, and on a deck with
    // no [LANDUSES] that is the wet deposition the ponded stage mixed in.
    // Returning early left the mass created and never booked — lid1d closed at
    // a 94.898 % runoff-quality continuity error with Surface Runoff 0.000
    // against legacy's 110,015.701.
    if (np <= 0 || ns <= 0 || ctx.options.ignore_quality) return;

    constexpr double kLperFt3 = 28.317;

    // legacy VlidInfil — infiltration into the native soil beneath the units,
    // the same sum stepGroundwater forms for the aquifer's recharge.
    // g.infil_loss is a DEPTH over this step, so × area is already ft³.
    for (int i = 0; i < ns; ++i) {
        const auto ui = static_cast<std::size_t>(i);
        if (ctx.subcatches.area[ui] <= 0.0) continue;

        const double v_out1 = (ui < ctx.subcatches.washoff_vout1.size())
                            ? ctx.subcatches.washoff_vout1[ui] : 0.0;
        // POST-LID: `runoff` carries the units' effect by now (A6b), and the
        // drains were accumulated beside it as a CFS this same step.
        const double v_surf_out = ctx.subcatches.runoff[ui] * dt_runoff;
        const double v_drain = (ui < ctx.subcatches.lid_drain_flow.size())
                             ? ctx.subcatches.lid_drain_flow[ui] * dt_runoff
                             : 0.0;
        const double v_out2 = v_surf_out + v_drain;

        const double lid_ft2 = (ui < ctx.subcatches.total_lid_area_ft2.size())
                             ? ctx.subcatches.total_lid_area_ft2[ui] : 0.0;
        const bool has_lid = lid_ft2 > 0.0;
        const double v_lost = has_lid ? std::max(v_out1 - v_out2, 0.0) : 0.0;
        const double v_infil = std::min(lid_infil_vol[ui], v_lost);

        // legacy's third `outNode >= 0 || outSubcatch == subcatchIndex` site
        // (surfqual.c:363): a subcatchment shedding onto a peer has not
        // delivered its load to the system; the receiver books it on
        // discharge, and booking it here counts the same mass twice.
        const bool load_reaches_system =
            (ctx.subcatches.outlet_node[ui] >= 0 ||
             ctx.subcatches.outlet_subcatch[ui] == i);

        for (int p = 0; p < np; ++p) {
            const auto sq_idx = ui * static_cast<std::size_t>(np)
                              + static_cast<std::size_t>(p);
            const auto up = static_cast<std::size_t>(p);
            if (sq_idx >= (*b.conc).size()) continue;
            // stepSurfaceQuality stored legacy's `newQual[p] = cOut/LperFT3`,
            // so cOut reads straight back out — no second array to carry.
            const double c_out = (*b.conc)[sq_idx] * kLperFt3;
            if (c_out <= 0.0) continue;

            // mcf mirrors legacy landuse.c:167-169, as stepSurfaceQuality does.
            const double mcf_p = (*b.mcf)[up];

            if (has_lid) {
                const double m_infil = c_out * v_infil * mcf_p;
                if (m_infil > 0.0 && up < (*b.led_infil_loss).size())
                    (*b.led_infil_loss)[up] += m_infil;
                const double m_bmp = c_out * (v_lost - v_infil) * mcf_p;
                if (m_bmp > 0.0 && up < (*b.led_bmp_removal).size())
                    (*b.led_bmp_removal)[up] += m_bmp;
            }

            // The PER-SUBCATCHMENT total is unconditional, as legacy's is
            // (surfqual.c:356, above its own guard): what this subcatchment
            // washed off, not what the system received.
            (*b.total_load)[sq_idx] += c_out * v_out2 * mcf_p;

            if (load_reaches_system && up < (*b.led_runoff_load).size())
                (*b.led_runoff_load)[up] += c_out * v_surf_out * mcf_p;
        }
    }
}

}  // namespace openswmm::surfacespecies
