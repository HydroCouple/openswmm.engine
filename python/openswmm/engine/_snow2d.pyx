"""Optional mesh snow configuration and accepted SI snapshots."""
from ._common cimport SWMM_Engine, _check
from ._snow cimport SWMM_SnowState, _h, _text, _state
from ._access import resolve_owner
from libc.stdlib cimport malloc, free

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
    ctypedef struct SWMM_MeshSnowOverrides:
        double forest
        double drift
        double ground_heat_wm2
        double extinction_depth_m
        double slope_deg
        double aspect_deg
    int swmm_snow2d_get_options(SWMM_Engine,int*,double*)
    int swmm_snow2d_defaults_count(SWMM_Engine)
    int swmm_snow2d_cells_count(SWMM_Engine)
    int swmm_snow2d_get_row(SWMM_Engine,int,int,int*,char*,int,char*,int,SWMM_MeshSnowOverrides*)
    int swmm_snow2d_get_configuration(SWMM_Engine,int,char*,int,int*)
    int swmm_snow2d_apply_configuration(SWMM_Engine,const char*,const char*,const char*,char*,int)
    int swmm_snow2d_get_states(SWMM_Engine,int,int,SWMM_SnowState*)
    int swmm_snow2d_get_ledgers(SWMM_Engine,int,int,SWMM_MeshSnowLedger*)
    int swmm_snow2d_get_geometry_energy(SWMM_Engine,int,int,double*)

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
        diagnostic[0]=0
        cdef int code=swmm_snow2d_apply_configuration(_h(self),_text(options),_text(defaults),_text(cells),diagnostic,sizeof(diagnostic))
        if code:
            if diagnostic[0]:raise ValueError(diagnostic.decode('utf-8'))
            _check(code)

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
        cdef double* geometry=NULL
        _check(swmm_2d_cell_count(h,&count))
        if not count:return []
        values=<SWMM_MeshSnowLedger*>malloc(count*sizeof(SWMM_MeshSnowLedger))
        if values==NULL:raise MemoryError()
        geometry=<double*>malloc(count*sizeof(double))
        if geometry==NULL:
            free(values)
            raise MemoryError()
        try:
            _check(swmm_snow2d_get_ledgers(h,0,count,values))
            _check(swmm_snow2d_get_geometry_energy(h,0,count,geometry))
            return [{'completed_s': values[i].completed_s, 'initial_m3': values[i].initial_m3, 'raw_precipitation_m3': values[i].raw_precipitation_m3, 'precipitation_m3': values[i].precipitation_m3, 'liquid_m3': values[i].liquid_m3, 'sublimation_m3': values[i].sublimation_m3, 'stored_m3': values[i].stored_m3, 'residual_m3': values[i].residual_m3, 'net_energy_kj': values[i].net_energy_kj, 'initial_energy_kj': values[i].initial_energy_kj, 'stored_energy_kj': values[i].stored_energy_kj, 'energy_residual_kj': values[i].energy_residual_kj, 'geometry_energy_kj': geometry[i]} for i in range(count)]
        finally:
            free(geometry)
            free(values)

    @property
    def options(self):
        """Mode AUTO/YES/NO and step in seconds (zero inherits shared step)."""
        cdef int mode
        cdef double step
        _check(swmm_snow2d_get_options(_h(self), &mode, &step))
        return {'mode': ('AUTO', 'YES', 'NO')[mode], 'step_seconds': step}

    def rows(self, section='defaults'):
        """Authored rows; NaN overrides inherit and cell indices are zero based."""
        sections={'defaults': 1, 'cells': 2}
        if section not in sections:raise ValueError('section must be defaults or cells')
        cdef SWMM_Engine h=_h(self)
        cdef int which=sections[section], cell, count
        cdef char tag[4096], pack[4096]
        cdef SWMM_MeshSnowOverrides v
        count=swmm_snow2d_defaults_count(h) if which==1 else swmm_snow2d_cells_count(h)
        result=[]
        for i in range(count):
            _check(swmm_snow2d_get_row(h,which,i,&cell,tag,sizeof(tag),pack,sizeof(pack),&v))
            result.append({'cell': cell, 'tag': tag.decode('utf-8'), 'pack': pack.decode('utf-8'),
                           'forest': v.forest, 'drift': v.drift, 'ground_heat_wm2': v.ground_heat_wm2,
                           'extinction_depth_m': v.extinction_depth_m, 'slope_deg': v.slope_deg, 'aspect_deg': v.aspect_deg})
        return result
