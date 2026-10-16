# SPDX-License-Identifier: Apache-2.0
# cython: language_level=3
"""Snow configuration and accepted state. New process quantities always use SI.

Unavailable UEB quantities remain NaN.
"""
from ._common cimport SWMM_Engine, _check, swmm_snowpack_index, swmm_subcatch_index
from libc.stdlib cimport malloc, free
from ._access import resolve_owner
from ._exceptions import StaleObjectError

cdef extern from "openswmm/engine/openswmm_snow.h":
    ctypedef struct SWMM_SnowLedger:
        double elapsed_s
        double initial_m3
        double precipitation_m3
        double liquid_m3
        double throughfall_m3
        double sublimation_m3
        double removed_m3
        double transferred_m3
        double stored_m3
        double residual_m3
        double net_energy_kj
        double initial_energy_kj
        double stored_energy_kj
        double energy_residual_kj
        double geometry_energy_kj
        double plowed_energy_kj
    ctypedef struct SWMM_UebSnowpack:
        int model
        double forest_fraction
        double drift_factor
        double ground_heat_wm2
        double extinction_depth_m
        double initial_energy_kjm2
    ctypedef struct SWMM_RadiationConfig:
        int explicit_site
        double latitude
        double longitude
        double timezone
        double elevation_m
        int shortwave_mode
        int longwave_mode
        int shortwave_authored
        int longwave_authored
    ctypedef struct SWMM_ClimateCloudConfig:
        int mode
        double fraction
        double shortwave_k
        double shortwave_n
        double longwave_k
    ctypedef struct SWMM_ClimateMetSummary:
        int records
        double first_date
        double last_date
        double minimum[5]
        double maximum[5]
    ctypedef struct SWMM_SnowpackEdit:
        const char* name
        double surface[3][7]
        double removal[6]
        const char* removal_subcatch
        SWMM_UebSnowpack ueb
    int swmm_climate_get_radiation(SWMM_Engine,SWMM_RadiationConfig*)
    int swmm_climate_set_radiation(SWMM_Engine,const SWMM_RadiationConfig*)
    int swmm_climate_get_radiation_source(SWMM_Engine,int,char*,int)
    int swmm_climate_set_radiation_source(SWMM_Engine,int,const char*)
    int swmm_climate_get_cloud(SWMM_Engine,SWMM_ClimateCloudConfig*,char*,int)
    int swmm_climate_preview_meteorology(SWMM_Engine,const char*,SWMM_ClimateMetSummary*,char*,int)
    int swmm_snow_apply_configuration_with_cloud(SWMM_Engine,const char*,const char*,const SWMM_ClimateCloudConfig*,const char*,char*,int)
    int swmm_snowpack_apply(SWMM_Engine,const SWMM_SnowpackEdit*,int,char*,int)
    int swmm_snow_get_model(SWMM_Engine,int*)
    int swmm_snow_set_model(SWMM_Engine,int)
    int swmm_snow_get_step(SWMM_Engine,double*)
    int swmm_snow_set_step(SWMM_Engine,double)
    int swmm_ueb_option_get(SWMM_Engine,const char*,char*,int)
    int swmm_ueb_option_set(SWMM_Engine,const char*,const char*)
    int swmm_ueb_get_monthly_ranges(SWMM_Engine,double*)
    int swmm_ueb_set_monthly_ranges(SWMM_Engine,const double*)
    int swmm_snowpack_get_ueb(SWMM_Engine,int,SWMM_UebSnowpack*)
    int swmm_snowpack_set_ueb(SWMM_Engine,int,const SWMM_UebSnowpack*)
    int swmm_subcatch_get_snow_process_state(SWMM_Engine,int,int,SWMM_SnowState*)
    int swmm_snow_get_ueb_ledger(SWMM_Engine,SWMM_SnowLedger*)
    int swmm_snow_apply_configuration(SWMM_Engine,const char*,const char*,char*,int)

cdef SWMM_Engine _h(view) except NULL:
    if view._generation != view._owner.generation:
        raise StaleObjectError("Snow view was invalidated; reacquire it from the solver")
    return <SWMM_Engine><size_t>view._owner.handle

cdef bytes _text(value):
    result=str(value).encode('utf-8')
    if b'\0' in result:
        raise ValueError("Snow text must not contain NUL")
    return result

cdef dict _state(SWMM_SnowState v):
    return {'model': v.model, 'swe_m': v.swe_m, 'cover_fraction': v.cover_fraction, 'melt_ms': v.melt_ms, 'surface_input_ms': v.surface_input_ms, 'energy_kjm2': v.energy_kjm2, 'surface_temperature_c': v.surface_temperature_c, 'bulk_temperature_c': v.bulk_temperature_c, 'albedo': v.albedo, 'surface_age': v.surface_age, 'sublimation_ms': v.sublimation_ms, 'water_age_s': v.water_age_s}

class Snow:
    """Shared model, UEB parameters, named packs, subcatchment state and ledger."""
    def __init__(self, owner):
        self._owner=resolve_owner(owner)
        self._generation=self._owner.generation

    @property
    def model(self):
        cdef int v
        _check(swmm_snow_get_model(_h(self),&v))
        return 'UEB' if v==1 else 'DEGREE_DAY'

    @model.setter
    def model(self,value):
        models={'UEB':1,'DEGREE_DAY':0}
        if str(value).upper() not in models:
            raise ValueError('Snow model must be UEB or DEGREE_DAY')
        code=models[str(value).upper()]
        _check(swmm_snow_set_model(_h(self),code))

    @property
    def step_seconds(self):
        cdef double v
        _check(swmm_snow_get_step(_h(self),&v))
        return v

    @step_seconds.setter
    def step_seconds(self,double value):
        _check(swmm_snow_set_step(_h(self),value))

    def option(self,key):
        cdef char value[4096]
        _check(swmm_ueb_option_get(_h(self),_text(key),value,sizeof(value)))
        return value.decode('utf-8')

    def set_option(self,key,value):
        _check(swmm_ueb_option_set(_h(self),_text(key),_text(value)))

    @property
    def monthly_ranges(self):
        cdef double values[12]
        _check(swmm_ueb_get_monthly_ranges(_h(self),values))
        return [values[i] for i in range(12)]

    @monthly_ranges.setter
    def monthly_ranges(self,values):
        cdef double buffer[12]
        rows=list(values)
        if len(rows)!=12:raise ValueError("Expected twelve monthly ranges in Celsius")
        for i in range(12):buffer[i]=rows[i]
        _check(swmm_ueb_set_monthly_ranges(_h(self),buffer))

    def pack(self,name):
        """Read the named pack model and SI site parameters without changing it."""
        cdef SWMM_Engine h=_h(self)
        cdef int index=swmm_snowpack_index(h,_text(name))
        cdef SWMM_UebSnowpack v
        _check(swmm_snowpack_get_ueb(h,index,&v))
        return {'model': v.model, 'forest_fraction': v.forest_fraction, 'drift_factor': v.drift_factor, 'ground_heat_wm2': v.ground_heat_wm2, 'extinction_depth_m': v.extinction_depth_m, 'initial_energy_kjm2': v.initial_energy_kjm2}

    def set_pack(self,name,changes=None,**fields):
        """Atomically edit a named pack from an SI field dictionary or keywords."""
        changes=dict(changes or {})
        changes.update(fields)
        current=self.pack(name)
        unknown=changes.keys()-current.keys()
        if unknown:raise ValueError(f"Unknown snowpack fields: {sorted(unknown)}")
        current.update(changes)
        cdef SWMM_UebSnowpack v
        v.model=current['model']
        v.forest_fraction=current['forest_fraction']
        v.drift_factor=current['drift_factor']
        v.ground_heat_wm2=current['ground_heat_wm2']
        v.extinction_depth_m=current['extinction_depth_m']
        v.initial_energy_kjm2=current['initial_energy_kjm2']
        cdef SWMM_Engine h=_h(self)
        _check(swmm_snowpack_set_ueb(h,swmm_snowpack_index(h,_text(name)),&v))

    def state(self,subcatch,int surface=2):
        """Read accepted SI snow state; surface 0 plowable, 1 impervious, 2 pervious."""
        cdef SWMM_Engine h=_h(self)
        cdef int index=swmm_subcatch_index(h,_text(subcatch)) if isinstance(subcatch,str) else subcatch
        cdef SWMM_SnowState v
        _check(swmm_subcatch_get_snow_process_state(h,index,surface,&v))
        return _state(v)

    def ledger(self):
        cdef SWMM_SnowLedger v
        _check(swmm_snow_get_ueb_ledger(_h(self),&v))
        return {'elapsed_s': v.elapsed_s, 'initial_m3': v.initial_m3, 'precipitation_m3': v.precipitation_m3, 'liquid_m3': v.liquid_m3, 'throughfall_m3': v.throughfall_m3, 'sublimation_m3': v.sublimation_m3, 'removed_m3': v.removed_m3, 'transferred_m3': v.transferred_m3, 'stored_m3': v.stored_m3, 'residual_m3': v.residual_m3, 'net_energy_kj': v.net_energy_kj, 'initial_energy_kj': v.initial_energy_kj, 'stored_energy_kj': v.stored_energy_kj, 'energy_residual_kj': v.energy_residual_kj, 'geometry_energy_kj': v.geometry_energy_kj, 'plowed_energy_kj': v.plowed_energy_kj}

    def apply_configuration(self,temperature_rows='',ueb_rows='', *, cloud=None):
        """Replace snow/radiation TEMPERATURE keys and UEB_OPTIONS atomically.

        Omitted keys use defaults. cloud=None preserves the shared cloud record;
        pass a complete cloud dictionary (as returned by cloud) to replace it in
        the same transaction. Classic air-temperature forcing is unchanged.
        """
        cdef char diagnostic[4096]
        diagnostic[0]=0
        cdef SWMM_ClimateCloudConfig v
        cdef int code
        if cloud is None:
            code=swmm_snow_apply_configuration(_h(self),_text(temperature_rows),_text(ueb_rows),diagnostic,sizeof(diagnostic))
        else:
            v.mode=cloud['mode']
            v.fraction=cloud['fraction']
            v.shortwave_k=cloud['shortwave_k']
            v.shortwave_n=cloud['shortwave_n']
            v.longwave_k=cloud['longwave_k']
            code=swmm_snow_apply_configuration_with_cloud(_h(self),_text(temperature_rows),_text(ueb_rows),&v,_text(cloud.get('series','')),diagnostic,sizeof(diagnostic))
        if code:
            if diagnostic[0]:raise ValueError(diagnostic.decode('utf-8'))
            _check(code)


    @property
    def radiation(self):
        """Shared radiation configuration; elevation uses metres, modes 0/1/2."""
        cdef SWMM_RadiationConfig v
        _check(swmm_climate_get_radiation(_h(self),&v))
        return {'explicit_site': v.explicit_site, 'latitude': v.latitude, 'longitude': v.longitude, 'timezone': v.timezone, 'elevation_m': v.elevation_m, 'shortwave_mode': v.shortwave_mode, 'longwave_mode': v.longwave_mode, 'shortwave_authored': v.shortwave_authored, 'longwave_authored': v.longwave_authored}

    @radiation.setter
    def radiation(self, values):
        """Replace the complete shared radiation record atomically."""
        cdef SWMM_RadiationConfig v
        v.explicit_site=values['explicit_site']
        v.latitude=values['latitude']
        v.longitude=values['longitude']
        v.timezone=values['timezone']
        v.elevation_m=values['elevation_m']
        v.shortwave_mode=values['shortwave_mode']
        v.longwave_mode=values['longwave_mode']
        v.shortwave_authored=values['shortwave_authored']
        v.longwave_authored=values['longwave_authored']
        _check(swmm_climate_set_radiation(_h(self),&v))

    def radiation_source(self, int channel):
        """Source name: channel 0 shortwave, 1 longwave, 2 meteorology file."""
        cdef char value[4096]
        _check(swmm_climate_get_radiation_source(_h(self),channel,value,sizeof(value)))
        return value.decode('utf-8')

    def set_radiation_source(self, int channel, value):
        """Bind a source name; select its mode separately in radiation."""
        _check(swmm_climate_set_radiation_source(_h(self),channel,_text(value)))

    @property
    def cloud(self):
        """Cloud modes 0 default, 1 constant, 2 series; fraction is dimensionless."""
        cdef SWMM_ClimateCloudConfig v
        cdef char series[4096]
        _check(swmm_climate_get_cloud(_h(self),&v,series,sizeof(series)))
        return {'mode': v.mode, 'fraction': v.fraction, 'shortwave_k': v.shortwave_k, 'shortwave_n': v.shortwave_n, 'longwave_k': v.longwave_k, 'series': series.decode('utf-8')}

    def preview_meteorology(self, path):
        """Validate hourly forcing; raw OLE dates and extrema in C, %, m/s, W/m2."""
        import os
        cdef SWMM_ClimateMetSummary v
        cdef char diagnostic[4096]
        diagnostic[0]=0
        cdef int code=swmm_climate_preview_meteorology(_h(self),_text(os.fspath(path)),&v,diagnostic,sizeof(diagnostic))
        if code:
            if diagnostic[0]:raise ValueError(diagnostic.decode('utf-8'))
            _check(code)
        return {'records': v.records, 'first_date': v.first_date, 'last_date': v.last_date,
                'minimum': [v.minimum[i] for i in range(5)], 'maximum': [v.maximum[i] for i in range(5)]}

    def apply_packs(self, rows):
        """Atomic named-pack upsert. Each row supplies surface (3x7), removal (6),
        removal_subcatch and ueb fields. Legacy arrays use INP units and ordering;
        ueb fields use SI. Omitted named packs and assignments are preserved.
        Successful edits invalidate existing views; reacquire them from Solver.
        """
        rows=list(rows)
        cdef int count=len(rows), i, j, k
        cdef SWMM_SnowpackEdit* values=NULL
        cdef char diagnostic[4096]
        diagnostic[0]=0
        # Own encoded strings until the synchronous native call returns.
        names=[_text(row['name']) for row in rows]
        destinations=[_text(row.get('removal_subcatch','')) for row in rows]
        if count:
            values=<SWMM_SnowpackEdit*>malloc(count*sizeof(SWMM_SnowpackEdit))
            if values==NULL:raise MemoryError()
        try:
            for i in range(count):
                row=rows[i]
                surface=list(row['surface']); removal=list(row['removal'])
                if len(surface)!=3 or any(len(x)!=7 for x in surface) or len(removal)!=6:
                    raise ValueError('surface must be 3x7 and removal must have six values')
                values[i].name=names[i]
                values[i].removal_subcatch=destinations[i]
                for j in range(3):
                    for k in range(7):values[i].surface[j][k]=surface[j][k]
                for j in range(6):values[i].removal[j]=removal[j]
                ueb=row['ueb']
                values[i].ueb.model=ueb['model']
                values[i].ueb.forest_fraction=ueb['forest_fraction']
                values[i].ueb.drift_factor=ueb['drift_factor']
                values[i].ueb.ground_heat_wm2=ueb['ground_heat_wm2']
                values[i].ueb.extinction_depth_m=ueb['extinction_depth_m']
                values[i].ueb.initial_energy_kjm2=ueb['initial_energy_kjm2']
            code=swmm_snowpack_apply(_h(self),values,count,diagnostic,sizeof(diagnostic))
            if code:
                if diagnostic[0]:raise ValueError(diagnostic.decode('utf-8'))
                _check(code)
            if count:self._owner._bump_generation()
        finally:free(values)
