"""Groundwater transport authoring; runtime results live on Groundwater.

Setters require BUILDING/OPENED. Cells and local edges are zero-based; explicit
LAYER zones use a one-based layer number. Source flow is m³/s; parameter rows
use SI thermal/material units, and sorption uses L/kg and 1/day.
"""
from dataclasses import dataclass
from libc.math cimport isfinite
from ._exceptions import BadParamError
from collections.abc import MutableMapping
from ._access import EngineView
from ._common cimport SWMM_Engine, _check, _TextBufs, _tb_open, _tb_retry, _tb_str, _tb_close
from ._enums import CellScope, GroundwaterTransportZone, ErrorCode

cdef extern from "openswmm/engine/openswmm_2d.h":
    int swmm_2d_cell_count(SWMM_Engine engine, int* count)

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
    int swmm_gw_source_scale_get(SWMM_Engine engine, int idx, double* value)
    int swmm_gw_source_scale_set(SWMM_Engine engine, int idx, double value)
    int swmm_gw_source_xy_get(SWMM_Engine engine, int idx, int* by_xy, double* x, double* y)
    int swmm_gw_source_xy_set(SWMM_Engine engine, int idx, double x, double y)
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
    by_xy: bool = False
    x: float = 0.0
    y: float = 0.0


@dataclass(frozen=True)
class GroundwaterSourceTerm:
    """Immutable authored row; replace fields and submit it with the matching setter."""
    species: str = ''
    kind: str = 'CONC'
    value: float = 0.0
    timeseries: str = ''


class GroundwaterTransportOptions(EngineView, MutableMapping):
    """Native GW_TRANSPORT_OPTIONS text keys; values preserve INP spelling."""
    _keys = ('UNSAT_TRANSPORT', 'TRANSPORT_POLLUTANTS', 'TRANSPORT_MSX', 'TRANSPORT_AGE',
             'TRANSPORT_TEMPERATURE', 'DISPERSION', 'CONDUCTION',
             'SURFACE_THERMAL_BC', 'DEEP_THERMAL_BC', 'THERMAL_MIXING', 'C_DIFF',
             'THERMAL_MODEL', 'THERMAL_LAYERS', 'LATERAL_CONDUCTION', 'THERMAL_MAX_STEP',
             'WATER_THERMAL_CONDUCTIVITY', 'AIR_THERMAL_CONDUCTIVITY',
             'SURFACE_THERMAL_RESISTANCE', 'SOIL_ET_HEAT')
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
        cdef _TextBufs t
        rows = []
        _tb_open(&t, 1)
        try:
            for index in range(swmm_gw_params_count(h)):
                while True:
                    _check(swmm_gw_params_get(h, index, &row, t.p[0], t.size))
                    if not _tb_retry(&t):
                        break
                rows.append(GroundwaterParameters(CellScope(row.scope), row.cell,
                    row.rho_s, row.c_s, row.lambda_s, row.a_s, row.alpha_L, row.alpha_T, row.D_m, row.D_v, row.geo_flux, _tb_str(&t, 0)))
        finally:
            _tb_close(&t)
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
        cdef int cell=0
        cdef double kd=0
        cdef double decay=0
        cdef _TextBufs t
        cdef int count = swmm_gw_sorption_count(h)
        if count < 0:
            raise IndexError("Invalid native row owner/index")
        rows = []
        _tb_open(&t, 2)
        try:
            for index in range(count):
                while True:
                    _check(swmm_gw_sorption_get(h, index, &scope, t.p[0], t.size, &cell, t.p[1], t.size, &kd, &decay))
                    if not _tb_retry(&t):
                        break
                rows.append(GroundwaterSorption(CellScope(scope), _tb_str(&t, 0), cell, _tb_str(&t, 1), kd, decay))
        finally:
            _tb_close(&t)
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
        cdef int cell=0
        cdef int zone=0
        cdef int layer=0
        cdef double value=0
        cdef _TextBufs t
        cdef int count = swmm_gw_init_quality_count(h)
        if count < 0:
            raise IndexError("Invalid native row owner/index")
        rows = []
        _tb_open(&t, 2)
        try:
            for index in range(count):
                while True:
                    _check(swmm_gw_init_quality_get(h, index, &scope, t.p[0], t.size, &cell, &zone, &layer, t.p[1], t.size, &value))
                    if not _tb_retry(&t):
                        break
                rows.append(GroundwaterInitialQuality(CellScope(scope), _tb_str(&t, 0), cell, GroundwaterTransportZone(zone), layer, _tb_str(&t, 1), value))
        finally:
            _tb_close(&t)
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
        cdef double value=0
        cdef _TextBufs t
        cdef int count = swmm_gw_boundary_quality_count(h)
        if count < 0:
            raise IndexError("Invalid native row owner/index")
        rows = []
        _tb_open(&t, 3)
        try:
            for index in range(count):
                while True:
                    _check(swmm_gw_boundary_quality_get(h, index, &cell, &edge, t.p[0], t.size, t.p[1], t.size, &value, t.p[2], t.size))
                    if not _tb_retry(&t):
                        break
                rows.append(GroundwaterBoundary(cell, edge, _tb_str(&t, 0), _tb_str(&t, 1), value, _tb_str(&t, 2)))
        finally:
            _tb_close(&t)
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
        cdef int by_xy=0
        cdef double x=0, y=0
        cdef int scope=0
        cdef int cell=0
        cdef double flow=0
        cdef _TextBufs t
        cdef int count = swmm_gw_source_count(h)
        if count < 0:
            raise IndexError("Invalid native row owner/index")
        rows = []
        _tb_open(&t, 3)
        try:
            for index in range(count):
                while True:
                    _check(swmm_gw_source_get(h, index, t.p[0], t.size, &scope, t.p[1], t.size, &cell, &flow, t.p[2], t.size))
                    if not _tb_retry(&t):
                        break
                x = y = 0
                _check(swmm_gw_source_xy_get(h, index, &by_xy, &x, &y))
                rows.append(GroundwaterSource(_tb_str(&t, 0), CellScope(scope), _tb_str(&t, 1), cell, flow, _tb_str(&t, 2), bool(by_xy), x, y))
        finally:
            _tb_close(&t)
        return tuple(rows)

    def set_source(self, row):
        """Upsert an authored row; requires BUILDING or OPENED."""
        cdef bytes name = row.name.encode('utf-8')
        cdef bytes tag = row.tag.encode('utf-8')
        cdef bytes flow_timeseries = row.flow_timeseries.encode('utf-8')
        cdef SWMM_Engine h = _h(self)
        cdef int count = 0
        cdef int index = -1
        if row.by_xy:
            _check(swmm_2d_cell_count(h, &count))
            if (CellScope(row.scope) != CellScope.CELL or count == 0 or
                    not isfinite(row.x) or not isfinite(row.y)):
                raise BadParamError(ErrorCode.BADPARAM, "XY sources require CELL scope, a mesh and finite coordinates")
        _check(swmm_gw_source_set(h, name, int(CellScope(row.scope)), tag, row.cell, row.flow, flow_timeseries))
        if row.by_xy:
            for index, source in enumerate(self.sources):
                if source.name == row.name:
                    _check(swmm_gw_source_xy_set(h, index, row.x, row.y))
                    break

    def remove_source(self, int index):
        """Remove a row; subsequent indices shift."""
        _check(swmm_gw_source_remove(_h(self), index))

    def source_scale(self, int source_index):
        """Return the dimensionless multiplier for a source's flow and MASS terms."""
        cdef double value = 0
        _check(swmm_gw_source_scale_get(_h(self), source_index, &value))
        return value

    def set_source_scale(self, int source_index, double value):
        """Set a finite, nonnegative source multiplier before initialization.

        The multiplier applies to flow and MASS terms; CONC is unchanged.
        """
        _check(swmm_gw_source_scale_set(_h(self), source_index, value))

    def source_xy(self, int source_index):
        """Return authored (X, Y) in project map units, or None for a non-XY source.

        The native sources table reports an XY source as CELL scope with its
        resolved cell. This accessor preserves its original map coordinates.
        """
        cdef int by_xy = 0
        cdef double x = 0
        cdef double y = 0
        _check(swmm_gw_source_xy_get(_h(self), source_index, &by_xy, &x, &y))
        return (x, y) if by_xy else None

    def set_source_xy(self, int source_index, double x, double y):
        """Locate a source by finite X/Y coordinates in project map units.

        Requires BUILDING or OPENED and a mesh with cells. The nearest cell
        centroid selects the CELL scope; the writer retains XY coordinates.
        Flow, scale and species terms remain unchanged.
        """
        _check(swmm_gw_source_xy_set(_h(self), source_index, x, y))

    def source_species(self, int source_index):
        """Immutable authored row snapshots in native order."""
        cdef SWMM_Engine h = _h(self)
        cdef double value=0
        cdef _TextBufs t
        cdef int count = swmm_gw_source_species_count(h, source_index)
        if count < 0:
            raise IndexError("Invalid native row owner/index")
        rows = []
        _tb_open(&t, 3)
        try:
            for index in range(count):
                while True:
                    _check(swmm_gw_source_species_get(h, source_index, index, t.p[0], t.size, t.p[1], t.size, &value, t.p[2], t.size))
                    if not _tb_retry(&t):
                        break
                rows.append(GroundwaterSourceTerm(_tb_str(&t, 0), _tb_str(&t, 1), value, _tb_str(&t, 2)))
        finally:
            _tb_close(&t)
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
