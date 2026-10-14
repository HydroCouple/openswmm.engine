// SPDX-License-Identifier: Apache-2.0
#include "NativeSnow.hpp"
#include "UebSubcatchments.hpp"
#include "../../core/SimulationContext.hpp"
#include "../../core/UnitConversion.hpp"
#include <algorithm>
#include <limits>
#include <cmath>
namespace openswmm::surface {
bool hasUebSnow(const SimulationContext& c) {
    if(c.options.ignore_snow_melt)return false;
    for(int pack:c.subcatches.snowpack)
        if(pack>=0 && c.snowpacks.config.modelFor(pack)==SnowKind::Ueb)return true;
    return false;
}
std::array<double,10> nativeSnowResults(const SimulationContext& c,const snow::SnowSoA& s,int sc) {
    std::array<double,10> out;out.fill(std::numeric_limits<double>::quiet_NaN());
    const auto& rt=c.snowpacks.runtime;
    if(sc<0 || sc>=s.n_subcatch || !s.external[sc] || (sc+1)*3>int(rt.columns.size()))return out;
    out.fill(0);bool advanced=false;
    for(int k=0;k<3;++k) {
        const int i=sc*3+k;const double a=s.fArea[i];if(a<=0)continue;
        const auto& l=rt.columns[i].accepted;const auto& p=l.point;
        const auto& f=rt.last[i];
        out[9]=std::max(out[9],rt.sample_end[i]);advanced=advanced||rt.sample_seconds[i]>0;
        out[0]+=a*l.water;out[1]+=a*(l.cover>0?l.cover:1)*p.energy;
        const double bulk=ueb::bulkTemperature(p,c.snowpacks.config.parameters);
        out[2]+=a*(p.history.empty()?std::min(0.,bulk):p.history.back().surface);
        out[3]+=a*bulk;out[4]+=a*f.point.albedo;out[5]+=a*l.cover;
        out[6]+=a*s.imelt[i]*.3048;out[7]+=a*s.external_net[i]*.3048;
        out[8]+=a*(rt.sample_seconds[i]>0?f.sublimation/rt.sample_seconds[i]:0);
    }
    if(!advanced)out[4]=std::numeric_limits<double>::quiet_NaN();
    return out;
}
double nativeSnowDepth(const snow::SnowSoA& s,int sc) {
    double water=0;for(int k=0;k<3;++k){const int i=sc*3+k;water+=s.wsnow[i]*s.fArea[i];}return water;
}
double nativeSnowVolume(const snow::SnowSoA& s,int sc,double area) {
    double water=0;for(int k=0;k<3;++k){const int i=sc*3+k;water+=(s.wsnow[i]+s.fw[i])*s.fArea[i];}return water*area;
}
std::string initializeNativeSnow(SimulationContext& ctx_,snow::SnowSolver& snow_) {
    snow_.init(ctx_.n_subcatches());
    {
        auto& soa = snow_.state();
        int unit_sys_snow = ucf::getUnitSystem(static_cast<int>(ctx_.options.flow_units));

        // Transfer global snowmelt parameters
        soa.tipm = ctx_.options.snow_ati_wt;
        soa.rnm  = ctx_.options.snow_nrg_ratio;

        // Transfer ADC curves from options
        for (int k = 0; k < 10; ++k) {
            soa.adc_imperv[k] = ctx_.options.adc_imperv[k];
            soa.adc_perv[k]   = ctx_.options.adc_perv[k];
        }

        // Transfer per-subcatchment snowpack parameters
        for (int i = 0; i < ctx_.n_subcatches(); ++i) {
            auto ui = static_cast<std::size_t>(i);
            int sp_idx = ctx_.subcatches.snowpack[ui];
            if (sp_idx < 0) continue;
            auto usp = static_cast<std::size_t>(sp_idx);

            // Surface types: 0=PLOWABLE, 1=IMPERVIOUS, 2=PERVIOUS
            const std::array<double, 7>* surfaces[3] = {nullptr, nullptr, nullptr};
            if (usp < ctx_.snowpacks.plowable.size())   surfaces[0] = &ctx_.snowpacks.plowable[usp];
            if (usp < ctx_.snowpacks.impervious.size())  surfaces[1] = &ctx_.snowpacks.impervious[usp];
            if (usp < ctx_.snowpacks.pervious.size())    surfaces[2] = &ctx_.snowpacks.pervious[usp];

            for (int k = 0; k < snow::N_SUBAREAS; ++k) {
                auto idx = static_cast<std::size_t>(i * snow::N_SUBAREAS + k);
                if (!surfaces[k]) continue;
                const auto& p = *surfaces[k];
                // p[0]=Cmin, p[1]=Cmax, p[2]=Tbase, p[3]=FWF, p[4]=SD0, p[5]=FW0, p[6]=SNN0
                // legacy setMeltParams (snow.c): the melt coefficients are
                // per DEGREE, so an SI deck's mm/hr-degC also carries
                // UCF(TEMPERATURE) = 1.8; the initial free water is capped
                // at FWF x SD0; ATI starts at the base temperature.
                soa.dhmin[idx]  = p[0] * ucf::Ucf[ucf::TEMPERATURE][unit_sys_snow]
                                       / ucf::Ucf[ucf::RAINFALL][unit_sys_snow];
                soa.dhmax[idx]  = p[1] * ucf::Ucf[ucf::TEMPERATURE][unit_sys_snow]
                                       / ucf::Ucf[ucf::RAINFALL][unit_sys_snow];
                soa.tbase[idx]  = p[2];
                if (unit_sys_snow == 1) // SI: convert °C to °F
                    soa.tbase[idx] = (9. / 5.) * soa.tbase[idx] + 32.0;
                soa.fwfrac[idx] = p[3];
                soa.wsnow[idx]  = p[4] / ucf::Ucf[ucf::RAINDEPTH][unit_sys_snow];
                soa.fw[idx]     = std::min(p[5], p[3] * p[4])
                                  / ucf::Ucf[ucf::RAINDEPTH][unit_sys_snow];
                soa.ati[idx]    = soa.tbase[idx];
                // F6: the 7th field is SNN0 on the PLOWABLE row and SD100 —
                // the depth at which areal coverage reaches 100 % — on the
                // other two. Legacy splits it exactly this way
                // (`snow.c:348-352`). Pinning `si` to the INITIAL pack depth
                // instead made `asc` identically 1 on every deck, so
                // `rain·(1 − asc)` was always zero and no rain ever reached
                // the ground under a pack unless the deck wrote an explicit
                // ADC row.
                if (k == snow::SNOW_PLOWABLE) {
                    soa.snn[ui] = p[6];
                    // The plowable surface has no SD100 of its own; legacy
                    // leaves its `si` at the init value (0).
                } else {
                    soa.si[idx] = p[6] / ucf::Ucf[ucf::RAINDEPTH][unit_sys_snow];
                }
            }

            // Fractional area of each snow surface (legacy snow_initSnowpack,
            // snow.c:178-182). Without this the SoA fArea stays zero, so
            // plowSnow() and the melt area-weighting treat every surface as
            // having no area — packs could never accumulate (Gap: M3 repair).
            //   plowable = snn * fracImperv
            //   imperv   = (1 - snn) * fracImperv
            //   pervious = 1 - fracImperv
            {
                double fimp = ctx_.subcatches.frac_imperv[ui];
                double snn  = soa.snn[ui];
                auto base = static_cast<std::size_t>(i * snow::N_SUBAREAS);
                soa.fArea[base + snow::SNOW_PLOWABLE] = snn * fimp;
                soa.fArea[base + snow::SNOW_IMPERV]   = (1.0 - snn) * fimp;
                soa.fArea[base + snow::SNOW_PERV]     = 1.0 - fimp;
                // Match legacy: zero initial state where the surface has no
                // area (snow.c:184-197).
                for (int k = 0; k < snow::N_SUBAREAS; ++k) {
                    auto idx = base + static_cast<std::size_t>(k);
                    if (soa.fArea[idx] <= 0.0) {
                        soa.wsnow[idx] = 0.0;
                        soa.fw[idx]    = 0.0;
                        soa.si[idx]    = 0.0;
                    }
                }
            }

            // Transfer plowing/removal parameters
            if (usp < ctx_.snowpacks.removal.size()) {
                const auto& r = ctx_.snowpacks.removal[usp];
                auto sf = static_cast<std::size_t>(i * 5);
                soa.weplow[ui]  = r[0] / ucf::Ucf[ucf::RAINDEPTH][unit_sys_snow];
                soa.sfrac[sf+0] = r[1]; // fraction removed
                soa.sfrac[sf+1] = r[2]; // fraction to impervious
                soa.sfrac[sf+2] = r[3]; // fraction to pervious
                soa.sfrac[sf+3] = r[4]; // fraction immediate melt
                soa.sfrac[sf+4] = r[5]; // fraction to other subcatchment

                // Resolve target subcatchment for plowed snow
                if (usp < ctx_.snowpacks.removal_subcatch.size() &&
                    !ctx_.snowpacks.removal_subcatch[usp].empty()) {
                    soa.to_subcatch[ui] = ctx_.subcatch_names.find(
                        ctx_.snowpacks.removal_subcatch[usp]);
                }
            }
        }
    }
    if(auto error=ueb::initializeSubcatchments(ctx_,snow_.state());!error.empty())return error;
    for(int sc=0;sc<ctx_.n_subcatches();++sc)ctx_.subcatches.snow_depth[sc]=nativeSnowDepth(snow_.state(),sc);
    return {};
}
}
