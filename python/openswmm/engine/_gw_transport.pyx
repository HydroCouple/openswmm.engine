"""Groundwater transport authoring; runtime results live on Groundwater.

Setters require BUILDING/OPENED. Cells and local edges are zero-based; explicit
LAYER zones use a one-based layer number. Source flow is m³/s; parameter rows
use SI thermal/material units, and sorption uses L/kg and 1/day.
"""
from dataclasses import dataclass
from collections.abc import MutableMapping
from ._access import EngineView
from ._common cimport SWMM_Engine, _check
from ._enums import CellScope, GroundwaterTransportZone

cdef extern from "openswmm/engine/openswmm_gw_transport.h":
    ctypedef struct SWMM_GwParams:
        int scope
        int cell
        double rho_s
        double c_s
        double lambda_s
        double a_s
        double alpha_L
        double alpha_T
        double D_m
        double D_v
        double geo_flux
    int swmm_gw_transport_option_get(SWMM_Engine engine, const char* key, char* buf, int buflen)
    int swmm_gw_transport_option_set(SWMM_Engine engine, const char* key, const char* value)
    int swmm_gw_transport_authored(SWMM_Engine engine)
    int swmm_gw_params_count(SWMM_Engine engine)
    int swmm_gw_params_get(SWMM_Engine engine, int idx, SWMM_GwParams* row, char* tag_buf, int tag_len)
    int swmm_gw_params_set(SWMM_Engine engine, const SWMM_GwParams* row, const char* tag)
    int swmm_gw_params_remove(SWMM_Engine engine, int idx)
    int swmm_gw_sorption_count(SWMM_Engine engine)
    int swmm_gw_sorption_get(SWMM_Engine engine, int idx, int* scope, char* tag_buf, int tag_len, int* cell, char* species_buf, int species_len, double* kd, double* decay)
    int swmm_gw_sorption_set(SWMM_Engine engine, int scope, const char* tag, int cell, const char* species, double kd, double decay)
    int swmm_gw_sorption_remove(SWMM_Engine engine, int idx)
    int swmm_gw_init_quality_count(SWMM_Engine engine)
    int swmm_gw_init_quality_get(SWMM_Engine engine, int idx, int* scope, char* tag_buf, int tag_len, int* cell, int* zone, int* layer, char* species_buf, int species_len, double* value)
    int swmm_gw_init_quality_set(SWMM_Engine engine, int scope, const char* tag, int cell, int zone, int layer, const char* species, double value)
    int swmm_gw_init_quality_remove(SWMM_Engine engine, int idx)
    int swmm_gw_init_quality_file_get(SWMM_Engine engine, char* buf, int buflen)
    int swmm_gw_init_quality_file_set(SWMM_Engine engine, const char* path)
    int swmm_gw_boundary_quality_count(SWMM_Engine engine)
    int swmm_gw_boundary_quality_get(SWMM_Engine engine, int idx, int* cell, int* edge, char* species_buf, int species_len, char* kind_buf, int kind_len, double* value, char* ts_buf, int ts_len)
    int swmm_gw_boundary_quality_set(SWMM_Engine engine, int cell, int edge, const char* species, const char* kind, double value, const char* ts_name)
    int swmm_gw_boundary_quality_remove(SWMM_Engine engine, int idx)
    int swmm_gw_source_count(SWMM_Engine engine)
    int swmm_gw_source_get(SWMM_Engine engine, int idx, char* name_buf, int name_len, int* scope, char* tag_buf, int tag_len, int* cell, double* flow, char* flow_ts_buf, int flow_ts_len)
    int swmm_gw_source_set(SWMM_Engine engine, const char* name, int scope, const char* tag, int cell, double flow, const char* flow_ts)
    int swmm_gw_source_remove(SWMM_Engine engine, int idx)
    int swmm_gw_source_species_count(SWMM_Engine engine, int src_idx)
    int swmm_gw_source_species_get(SWMM_Engine engine, int src_idx, int term_idx, char* species_buf, int species_len, char* kind_buf, int kind_len, double* value, char* ts_buf, int ts_len)
    int swmm_gw_source_species_set(SWMM_Engine engine, int src_idx, const char* species, const char* kind, double value, const char* ts_name)
    int swmm_gw_source_species_remove(SWMM_Engine engine, int src_idx, int term_idx)

cdef inline SWMM_Engine _h(view):
    return <SWMM_Engine><size_t>view._address()

@dataclass(frozen=True)
class GroundwaterParameters:
    """Immutable authored row; replace fields and submit it with the matching setter."""
    scope: CellScope = CellScope.GLOBAL
    cell: int = -1
    rho_s: float = 2650.0
    c_s: float = 880.0
    lambda_s: float = 2.0
    a_s: float = 0.0
    alpha_L: float = 1.0
    alpha_T: float = 0.1
    D_m: float = 1e-9
    D_v: float = 1e-9
    geo_flux: float = 0.065
    tag: str = ''


@dataclass(frozen=True)
class GroundwaterSorption:
    """Immutable authored row; replace fields and submit it with the matching setter."""
    scope: CellScope = CellScope.GLOBAL
    tag: str = ''
    cell: int = -1
    species: str = ''
    kd: float = 0.0
    decay: float = -1.0


@dataclass(frozen=True)
class GroundwaterInitialQuality:
    """Immutable authored row; replace fields and submit it with the matching setter."""
    scope: CellScope = CellScope.GLOBAL
    tag: str = ''
    cell: int = -1
    zone: GroundwaterTransportZone = GroundwaterTransportZone.SAT
    layer: int = -1
    species: str = ''
    value: float = 0.0


@dataclass(frozen=True)
class GroundwaterBoundary:
    """Immutable authored row; replace fields and submit it with the matching setter."""
    cell: int = 0
    edge: int = 0
    species: str = ''
    kind: str = 'CONC'
    value: float = 0.0
    timeseries: str = ''


@dataclass(frozen=True)
class GroundwaterSource:
    """Immutable authored row; replace fields and submit it with the matching setter."""
    name: str = ''
    scope: CellScope = CellScope.CELL
    tag: str = ''
    cell: int = 0
    flow: float = 0.0
    flow_timeseries: str = ''


@dataclass(frozen=True)
class GroundwaterSourceTerm:
    """Immutable authored row; replace fields and submit it with the matching setter."""
    species: str = ''
    kind: str = 'CONC'
    value: float = 0.0
    timeseries: str = ''


class GroundwaterTransportOptions(EngineView, MutableMapping):
    """Native GW_TRANSPORT_OPTIONS text keys; values preserve INP spelling."""
    _keys = ('TRANSPORT_POLLUTANTS', 'TRANSPORT_MSX', 'TRANSPORT_AGE',
             'TRANSPORT_TEMPERATURE', 'DISPERSION', 'CONDUCTION',
             'SURFACE_THERMAL_BC', 'DEEP_THERMAL_BC', 'THERMAL_MIXING', 'C_DIFF')
    def __len__(self):
        return len(self._keys)
    def __iter__(self):
        return iter(self._keys)
    def __getitem__(self, key):
        cdef bytes name = str(key).encode('utf-8')
        cdef bytearray buffer = bytearray(256)
        while True:
            _check(swmm_gw_transport_option_get(_h(self), name, buffer, len(buffer)))
            value = bytes(buffer).split(b'\0', 1)[0]
            if len(value) < len(buffer) - 1:
                return value.decode('utf-8')
            buffer = bytearray(len(buffer) * 2)
    def __setitem__(self, key, value):
        cdef bytes name = str(key).encode('utf-8')
        cdef bytes text = str(value).encode('utf-8')
        _check(swmm_gw_transport_option_set(_h(self), name, text))
    def __delitem__(self, key):
        raise TypeError('Transport options cannot be deleted; assign an explicit value')

class GroundwaterTransport(EngineView):
    """``surface2d.groundwater.transport`` authored row tables.

    Getters return immutable snapshots, setters upsert by native row keys, and
    removing a row shifts subsequent indices. Check the engine capability
    matrix for which species and processes are active in a selected backend.
    """
    @property
    def options(self):
        self._address()
        return GroundwaterTransportOptions(self._owner)

    @property
    def authored(self):
        """Whether any groundwater transport section was authored."""
        return bool(swmm_gw_transport_authored(_h(self)))

    @property
    def parameters(self):
        """Thermal/material/dispersion parameter snapshots, all in native SI units."""
        cdef SWMM_Engine h = _h(self)
        cdef SWMM_GwParams row
        cdef char tag[512]
        rows = []
        for index in range(swmm_gw_params_count(h)):
            _check(swmm_gw_params_get(h, index, &row, tag, 512))
            rows.append(GroundwaterParameters(CellScope(row.scope), row.cell,
                row.rho_s, row.c_s, row.lambda_s, row.a_s, row.alpha_L, row.alpha_T, row.D_m, row.D_v, row.geo_flux, tag.decode('utf-8')))
        return tuple(rows)

    def set_parameters(self, row):
        """Upsert material parameters by scope/tag/cell; edit before initialization."""
        cdef SWMM_GwParams value
        cdef bytes tag = row.tag.encode('utf-8')
        value.scope = int(CellScope(row.scope))
        value.cell = row.cell
        value.rho_s = row.rho_s
        value.c_s = row.c_s
        value.lambda_s = row.lambda_s
        value.a_s = row.a_s
        value.alpha_L = row.alpha_L
        value.alpha_T = row.alpha_T
        value.D_m = row.D_m
        value.D_v = row.D_v
        value.geo_flux = row.geo_flux
        _check(swmm_gw_params_set(_h(self), &value, tag))

    def remove_parameters(self, int index):
        _check(swmm_gw_params_remove(_h(self), index))

    @property
    def sorption(self):
        """Immutable authored row snapshots in native order."""
        cdef SWMM_Engine h = _h(self)
        cdef int scope=0
        cdef char tag[512]
        cdef int cell=0
        cdef char species[512]
        cdef double kd=0
        cdef double decay=0
        cdef int count = swmm_gw_sorption_count(h)
        if count < 0:
            raise IndexError("Invalid native row owner/index")
        rows = []
        for index in range(count):
            _check(swmm_gw_sorption_get(h, index, &scope, tag, 512, &cell, species, 512, &kd, &decay))
            rows.append(GroundwaterSorption(CellScope(scope), tag.decode('utf-8'), cell, species.decode('utf-8'), kd, decay))
        return tuple(rows)

    def set_sorption(self, row):
        """Upsert an authored row; requires BUILDING or OPENED."""
        cdef bytes tag = row.tag.encode('utf-8')
        cdef bytes species = row.species.encode('utf-8')
        _check(swmm_gw_sorption_set(_h(self), int(CellScope(row.scope)), tag, row.cell, species, row.kd, row.decay))

    def remove_sorption(self, int index):
        """Remove a row; subsequent indices shift."""
        _check(swmm_gw_sorption_remove(_h(self), index))

    @property
    def initial_quality(self):
        """Immutable authored row snapshots in native order."""
        cdef SWMM_Engine h = _h(self)
        cdef int scope=0
        cdef char tag[512]
        cdef int cell=0
        cdef int zone=0
        cdef int layer=0
        cdef char species[512]
        cdef double value=0
        cdef int count = swmm_gw_init_quality_count(h)
        if count < 0:
            raise IndexError("Invalid native row owner/index")
        rows = []
        for index in range(count):
            _check(swmm_gw_init_quality_get(h, index, &scope, tag, 512, &cell, &zone, &layer, species, 512, &value))
            rows.append(GroundwaterInitialQuality(CellScope(scope), tag.decode('utf-8'), cell, GroundwaterTransportZone(zone), layer, species.decode('utf-8'), value))
        return tuple(rows)

    def set_initial_quality(self, row):
        """Upsert an authored row; requires BUILDING or OPENED."""
        cdef bytes tag = row.tag.encode('utf-8')
        cdef bytes species = row.species.encode('utf-8')
        _check(swmm_gw_init_quality_set(_h(self), int(CellScope(row.scope)), tag, row.cell, int(GroundwaterTransportZone(row.zone)), row.layer, species, row.value))

    def remove_initial_quality(self, int index):
        """Remove a row; subsequent indices shift."""
        _check(swmm_gw_init_quality_remove(_h(self), index))

    @property
    def boundaries(self):
        """Immutable authored row snapshots in native order."""
        cdef SWMM_Engine h = _h(self)
        cdef int cell=0
        cdef int edge=0
        cdef char species[512]
        cdef char kind[512]
        cdef double value=0
        cdef char timeseries[512]
        cdef int count = swmm_gw_boundary_quality_count(h)
        if count < 0:
            raise IndexError("Invalid native row owner/index")
        rows = []
        for index in range(count):
            _check(swmm_gw_boundary_quality_get(h, index, &cell, &edge, species, 512, kind, 512, &value, timeseries, 512))
            rows.append(GroundwaterBoundary(cell, edge, species.decode('utf-8'), kind.decode('utf-8'), value, timeseries.decode('utf-8')))
        return tuple(rows)

    def set_boundary(self, row):
        """Upsert an authored row; requires BUILDING or OPENED."""
        cdef bytes species = row.species.encode('utf-8')
        cdef bytes kind = row.kind.encode('utf-8')
        cdef bytes timeseries = row.timeseries.encode('utf-8')
        _check(swmm_gw_boundary_quality_set(_h(self), row.cell, row.edge, species, kind, row.value, timeseries))

    def remove_boundary(self, int index):
        """Remove a row; subsequent indices shift."""
        _check(swmm_gw_boundary_quality_remove(_h(self), index))

    @property
    def sources(self):
        """Immutable authored row snapshots in native order."""
        cdef SWMM_Engine h = _h(self)
        cdef char name[512]
        cdef int scope=0
        cdef char tag[512]
        cdef int cell=0
        cdef double flow=0
        cdef char flow_timeseries[512]
        cdef int count = swmm_gw_source_count(h)
        if count < 0:
            raise IndexError("Invalid native row owner/index")
        rows = []
        for index in range(count):
            _check(swmm_gw_source_get(h, index, name, 512, &scope, tag, 512, &cell, &flow, flow_timeseries, 512))
            rows.append(GroundwaterSource(name.decode('utf-8'), CellScope(scope), tag.decode('utf-8'), cell, flow, flow_timeseries.decode('utf-8')))
        return tuple(rows)

    def set_source(self, row):
        """Upsert an authored row; requires BUILDING or OPENED."""
        cdef bytes name = row.name.encode('utf-8')
        cdef bytes tag = row.tag.encode('utf-8')
        cdef bytes flow_timeseries = row.flow_timeseries.encode('utf-8')
        _check(swmm_gw_source_set(_h(self), name, int(CellScope(row.scope)), tag, row.cell, row.flow, flow_timeseries))

    def remove_source(self, int index):
        """Remove a row; subsequent indices shift."""
        _check(swmm_gw_source_remove(_h(self), index))

    def source_species(self, int source_index):
        """Immutable authored row snapshots in native order."""
        cdef SWMM_Engine h = _h(self)
        cdef char species[512]
        cdef char kind[512]
        cdef double value=0
        cdef char timeseries[512]
        cdef int count = swmm_gw_source_species_count(h, source_index)
        if count < 0:
            raise IndexError("Invalid native row owner/index")
        rows = []
        for index in range(count):
            _check(swmm_gw_source_species_get(h, source_index, index, species, 512, kind, 512, &value, timeseries, 512))
            rows.append(GroundwaterSourceTerm(species.decode('utf-8'), kind.decode('utf-8'), value, timeseries.decode('utf-8')))
        return tuple(rows)

    def set_source_species(self, int source_index, row):
        """Upsert an authored row; requires BUILDING or OPENED."""
        cdef bytes species = row.species.encode('utf-8')
        cdef bytes kind = row.kind.encode('utf-8')
        cdef bytes timeseries = row.timeseries.encode('utf-8')
        _check(swmm_gw_source_species_set(_h(self), source_index, species, kind, row.value, timeseries))

    def remove_source_species(self, int source_index, int index):
        """Remove a row; subsequent indices shift."""
        _check(swmm_gw_source_species_remove(_h(self), source_index, index))

    @property
    def initial_quality_file(self):
        """Authored sidecar path; changing it records a reference for the next open."""
        cdef bytearray buffer = bytearray(256)
        while True:
            _check(swmm_gw_init_quality_file_get(_h(self), buffer, len(buffer)))
            value = bytes(buffer).split(b'\0', 1)[0]
            if len(value) < len(buffer) - 1:
                return value.decode('utf-8')
            buffer = bytearray(len(buffer) * 2)

    @initial_quality_file.setter
    def initial_quality_file(self, path):
        import os
        cdef bytes value = os.fsencode(path) if path is not None else b''
        _check(swmm_gw_init_quality_file_set(_h(self), value))
