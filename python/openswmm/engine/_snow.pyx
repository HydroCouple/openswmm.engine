# SPDX-License-Identifier: Apache-2.0
# cython: language_level=3
"""Snow configuration and accepted state. New process quantities always use SI.

Unavailable UEB quantities remain NaN. Mesh rows use zero-based cell indices;
section-text editing retains the INP reader's one-based cell identifiers.
"""
from ._common cimport SWMM_Engine, _check, swmm_snowpack_index, swmm_subcatch_index
from libc.stdlib cimport malloc, free
from ._access import resolve_owner
from ._exceptions import StaleObjectError

cdef extern from "openswmm/engine/openswmm_snow.h":
    ctypedef struct SWMM_SnowState:
        int model
        double swe_m
        double cover_fraction
        double melt_ms
        double surface_input_ms
        double energy_kjm2
        double surface_temperature_c
        double bulk_temperature_c
        double albedo
        double surface_age
        double sublimation_ms
        double water_age_s
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

cdef extern from "openswmm/engine/openswmm_2d.h":
    int swmm_2d_cell_count(SWMM_Engine,int*)
cdef extern from "openswmm/engine/openswmm_snow2d.h":
    ctypedef struct SWMM_MeshSnowLedger:
        double completed_s
        double initial_m3
        double raw_precipitation_m3
        double precipitation_m3
        double liquid_m3
        double sublimation_m3
        double stored_m3
        double residual_m3
        double net_energy_kj
        double initial_energy_kj
        double stored_energy_kj
        double energy_residual_kj
    int swmm_snow2d_get_configuration(SWMM_Engine,int,char*,int,int*)
    int swmm_snow2d_apply_configuration(SWMM_Engine,const char*,const char*,const char*,char*,int)
    int swmm_snow2d_get_states(SWMM_Engine,int,int,SWMM_SnowState*)
    int swmm_snow2d_get_ledgers(SWMM_Engine,int,int,SWMM_MeshSnowLedger*)

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

    def apply_configuration(self,temperature_rows='',ueb_rows=''):
        """Atomically replace snow/radiation rows; omitted keys use defaults."""
        cdef char diagnostic[4096]
        cdef int code=swmm_snow_apply_configuration(_h(self),_text(temperature_rows),_text(ueb_rows),diagnostic,sizeof(diagnostic))
        if code:raise ValueError(diagnostic.decode('utf-8'))

class MeshSnow:
    """Mesh snow assignments and independent accepted cell snapshots."""
    def __init__(self,owner):
        self._owner=resolve_owner(owner)
        self._generation=self._owner.generation

    def configuration(self):
        cdef SWMM_Engine h=_h(self)
        cdef int size=0,section
        cdef char* buffer=NULL
        result={}
        for section,name in enumerate(('options','defaults','cells')):
            _check(swmm_snow2d_get_configuration(h,section,NULL,0,&size))
            buffer=<char*>malloc(size)
            if buffer==NULL:raise MemoryError()
            try:
                _check(swmm_snow2d_get_configuration(h,section,buffer,size,&size))
                result[name]=buffer.decode('utf-8')
            finally:free(buffer)
        return result

    def apply_configuration(self,options='',defaults='',cells=''):
        """Atomically replace all three section bodies, preserving named packs."""
        cdef char diagnostic[4096]
        cdef int code=swmm_snow2d_apply_configuration(_h(self),_text(options),_text(defaults),_text(cells),diagnostic,sizeof(diagnostic))
        if code:raise ValueError(diagnostic.decode('utf-8'))

    def states(self):
        cdef int count=0
        cdef SWMM_Engine h=_h(self)
        cdef SWMM_SnowState* values=NULL
        _check(swmm_2d_cell_count(h,&count))
        if not count:return []
        values=<SWMM_SnowState*>malloc(count*sizeof(SWMM_SnowState))
        if values==NULL:raise MemoryError()
        try:
            _check(swmm_snow2d_get_states(h,0,count,values))
            return [_state(values[i]) for i in range(count)]
        finally:free(values)

    def ledgers(self):
        cdef int count=0
        cdef SWMM_Engine h=_h(self)
        cdef SWMM_MeshSnowLedger* values=NULL
        _check(swmm_2d_cell_count(h,&count))
        if not count:return []
        values=<SWMM_MeshSnowLedger*>malloc(count*sizeof(SWMM_MeshSnowLedger))
        if values==NULL:raise MemoryError()
        try:
            _check(swmm_snow2d_get_ledgers(h,0,count,values))
            return [{'completed_s': values[i].completed_s, 'initial_m3': values[i].initial_m3, 'raw_precipitation_m3': values[i].raw_precipitation_m3, 'precipitation_m3': values[i].precipitation_m3, 'liquid_m3': values[i].liquid_m3, 'sublimation_m3': values[i].sublimation_m3, 'stored_m3': values[i].stored_m3, 'residual_m3': values[i].residual_m3, 'net_energy_kj': values[i].net_energy_kj, 'initial_energy_kj': values[i].initial_energy_kj, 'stored_energy_kj': values[i].stored_energy_kj, 'energy_residual_kj': values[i].energy_residual_kj} for i in range(count)]
        finally:free(values)
