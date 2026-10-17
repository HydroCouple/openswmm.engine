// SPDX-License-Identifier: Apache-2.0
#include "openswmm_api_common.hpp"
#include <openswmm/engine/openswmm_heat.h>
#ifdef OPENSWMM_HAS_2D
#include "../2d/SurfaceRouter2D.hpp"
#endif

int swmm_heat_get_2d_status(SWMM_Engine engine,SWMM_Heat2DStatus* out) {
    CHECK_HANDLE(engine);
    if(!out || out->struct_size<sizeof(SWMM_Heat2DStatus))return SWMM_ERR_BADPARAM;
    SWMM_Heat2DStatus status{};status.struct_size=sizeof status;
#ifdef OPENSWMM_HAS_2D
    const auto& ctx=to_engine(engine)->context();
    const auto& router=to_engine(engine)->surfaceRouter2D();
    const auto& options=router.options();const auto& heat=router.state().heat;
    status.requested=options.coupled_heat;status.initialized=router.isActive();
    status.surface_active=heat.enabled&&heat.surface;
    status.radiative_active=heat.enabled&&heat.radiative;status.bed_active=heat.enabled&&heat.bed;
    status.bed_model=options.bed_heat_model;status.experimental=1;
    status.water_heat_capacity=options.water_heat_capacity;
    const char* reason=!options.coupled_heat?"Transport only; coupled 2D heat is off.":
        !ctx.options.heat_transport?"Saved; inactive because Heat transport is off.":
        !options.transport_temperature?"Saved; inactive because mesh temperature transport is off.":
        !router.isActive()?"Requested; initialize a model with an enabled surface mesh to measure activation.":
        !heat.enabled?"Saved; inactive because no mesh temperature row is available.":
        "Experimental coupled 2D heat is active; restart and coupled snow/LID owners are not yet qualified.";
    std::strncpy(status.reason,reason,sizeof status.reason-1);
#else
    std::strcpy(status.reason,"This engine was built without 2D routing.");
#endif
    *out=status;return SWMM_OK;
}

int swmm_heat_get_2d_values(SWMM_Engine engine,int kind,int first,int count,double* out) {
    CHECK_HANDLE(engine);
    if(kind<0 || kind>SWMM_HEAT_2D_THERMAL_RESIDUAL || first<0 || count<0 || (count && !out))return SWMM_ERR_BADPARAM;
#ifdef OPENSWMM_HAS_2D
    const auto& router=to_engine(engine)->surfaceRouter2D();
    const auto& state=router.state();const auto& h=state.heat;
    const auto n=router.mesh().n_cells();
    if(first>n || count>n-first)return SWMM_ERR_BADINDEX;
    if(!h.enabled)return SWMM_ERR_LIFECYCLE;
    if(kind>=SWMM_HEAT_2D_BED_TEMPERATURE && kind<=SWMM_HEAT_2D_BED_CAPACITY && !h.bed)return SWMM_ERR_LIFECYCLE;
    const std::vector<double>* values=nullptr;
    switch(kind) {
        case SWMM_HEAT_2D_BED_ENERGY:values=&h.bed_energy;break;
        case SWMM_HEAT_2D_BED_CAPACITY:values=&h.bed_capacity;break;
        case SWMM_HEAT_2D_ATMOSPHERIC_ENERGY:values=&h.atmospheric_j;break;
        case SWMM_HEAT_2D_RADIATIVE_ENERGY:values=&h.radiative_j;break;
        case SWMM_HEAT_2D_SENSIBLE_ENERGY:values=&h.sensible_j;break;
        case SWMM_HEAT_2D_LATENT_ENERGY:values=&h.latent_j;break;
        case SWMM_HEAT_2D_DEEP_ENERGY:values=&h.ground_j;break;
        case SWMM_HEAT_2D_WATER_TO_BED_ENERGY:values=&h.water_bed_j;break;
        case SWMM_HEAT_2D_WATER_THERMAL_INCREMENT:values=&h.water_j;break;
        case SWMM_HEAT_2D_ACCEPTED_EVAPORATION:values=&h.evaporated_m3;break;
    }
    for(int k=0;k<count;++k) {
        const auto i=std::size_t(first+k);
        if(values)out[k]=(*values)[i];
        else if(kind==SWMM_HEAT_2D_BED_TEMPERATURE)out[k]=h.bed_energy[i]/h.bed_capacity[i];
        else if(kind==SWMM_HEAT_2D_WATER_TEMPERATURE)
            out[k]=state.volume[i]>0?state.transport.cell_mass[state.transport.idx(state.transport.temp_row,int(i))]/state.volume[i]:h.reported_temperature[i];
        else out[k]=(h.bed?h.bed_energy[i]-h.initial_bed_energy[i]:0)+h.water_j[i]-h.atmospheric_j[i]-h.ground_j[i];
    }
    return SWMM_OK;
#else
    return SWMM_ERR_PLUGIN;
#endif
}
