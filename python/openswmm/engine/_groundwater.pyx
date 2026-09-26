"""Two-zone groundwater authoring and runtime snapshots.

Authored aquifer and node-bed values use project units. Runtime hydrology uses
SI (metres, seconds, cubic metres); species concentrations use mass per cubic
metre of zone water. Edit before initialize and read active state after start.
"""
from dataclasses import dataclass
from collections.abc import MutableMapping
import numpy as np
cimport numpy as np
from ._common cimport SWMM_Engine, _check
from ._access import EngineView
from ._enums import (CellScope, GroundwaterVariable, GroundwaterLedger,
                     GroundwaterZone, GroundwaterSpeciesLedger)

cdef extern from "openswmm/engine/openswmm_gw2d.h":
    int swmm_gw2d_species_count(SWMM_Engine engine, int* count) nogil
    int swmm_gw2d_species_name(SWMM_Engine engine, int species, char* buf, int buflen) nogil
    int swmm_gw2d_get_cell_conc(SWMM_Engine engine, int zone, int species, double* out, int len, int* written) nogil
    int swmm_gw2d_get_species_ledger(SWMM_Engine engine, int species, int term, double* value) nogil
    int swmm_gw2d_option_get(SWMM_Engine engine, const char* key, char* buf, int buflen) nogil
    int swmm_gw2d_option_set(SWMM_Engine engine, const char* key, const char* value) nogil
    int swmm_gw2d_row_count(SWMM_Engine engine, int* count) nogil
    int swmm_gw2d_row_add(SWMM_Engine engine, int scope, const char* tag, int cell, double ks, double zs, double theta_s, double theta_r, double alpha) nogil
    int swmm_gw2d_row_get(SWMM_Engine engine, int index, int* scope, char* tag, int taglen, int* cell, double* ks, double* zs, double* theta_s, double* theta_r, double* alpha) nogil
    int swmm_gw2d_row_set_property(SWMM_Engine engine, int index, const char* key, double value) nogil
    int swmm_gw2d_row_get_property(SWMM_Engine engine, int index, const char* key, double* value) nogil
    int swmm_gw2d_row_remove(SWMM_Engine engine, int index) nogil
    int swmm_gw2d_node_count(SWMM_Engine engine, int* count) nogil
    int swmm_gw2d_node_add(SWMM_Engine engine, const char* node, int cell, double kc, double dc, double area) nogil
    int swmm_gw2d_node_get(SWMM_Engine engine, int index, char* node, int nodelen, int* cell, double* kc, double* dc, double* area) nogil
    int swmm_gw2d_node_remove(SWMM_Engine engine, int index) nogil
    int swmm_gw2d_node_get_flags(SWMM_Engine engine, int index, int* locate, int* exchange, int* automatic) nogil
    int swmm_gw2d_node_set_exchange(SWMM_Engine engine, int index, int exchange) nogil
    int swmm_gw2d_is_active(SWMM_Engine engine, int* active) nogil
    int swmm_gw2d_get_dimensions(SWMM_Engine engine, int* n_cells, int* m_layers) nogil
    int swmm_gw2d_get_cell(SWMM_Engine engine, int cell, int var, double* value) nogil
    int swmm_gw2d_get_cell_bulk(SWMM_Engine engine, int var, double* out, int len, int* written) nogil
    int swmm_gw2d_get_column(SWMM_Engine engine, int cell, double* theta, int len, int* written) nogil
    int swmm_gw2d_get_ledger(SWMM_Engine engine, int term, double* value) nogil
    int swmm_gw2d_get_continuity_error(SWMM_Engine engine, double* value) nogil
    int swmm_gw2d_get_tier_histogram(SWMM_Engine engine, long* out, int len, int* written) nogil

cdef inline SWMM_Engine _h(view):
    return <SWMM_Engine><size_t>view._address()

@dataclass(frozen=True)
class AquiferRow:
    """Authored aquifer row in project units; cell indices are zero-based."""
    scope: CellScope
    tag: str
    cell: int
    ks: float
    zs: float
    theta_s: float
    theta_r: float
    alpha: float

@dataclass(frozen=True)
class AquiferNode:
    """Node-bed row and enrollment flags; dimensions use project units."""
    node: str
    cell: int
    kc: float
    dc: float
    area: float
    locate: bool
    exchange: bool
    automatic: bool

class AquiferOptions(EngineView, MutableMapping):
    """Text options using the native INP key/value spellings."""
    _keys = ('SOIL_CHAR', 'CLOSURE', 'M_LAYERS', 'CAPILLARY_DIFF', 'C_GW', 'C_COL',
             'FORCE_CLOSED_FORM', 'MODE', 'DUNNE', 'GW_ET', 'NODE_ENROLMENT', 'LINK_SEEPAGE')
    def __len__(self):
        return len(self._keys)
    def __iter__(self):
        return iter(self._keys)
    def __getitem__(self, key):
        cdef bytes name = str(key).encode('utf-8')
        cdef char value[512]
        _check(swmm_gw2d_option_get(_h(self), name, value, 512))
        return value.decode('utf-8')
    def __setitem__(self, key, value):
        cdef bytes name = str(key).encode('utf-8')
        cdef bytes text = str(value).encode('utf-8')
        _check(swmm_gw2d_option_set(_h(self), name, text))
    def __delitem__(self, key):
        raise TypeError('Aquifer options cannot be deleted; assign an explicit value')

class Groundwater(EngineView):
    """``solver.surface2d.groundwater``; authored rows and live SI results."""
    @property
    def transport(self):
        """Groundwater transport authoring (thermal properties, sorption, sources)."""
        self._address()
        from ._gw_transport import GroundwaterTransport
        return GroundwaterTransport(self._owner)

    @property
    def options(self):
        self._address()
        return AquiferOptions(self._owner)

    @property
    def rows(self):
        """Aquifer snapshots in authored order (GLOBAL, TAG or CELL scopes)."""
        cdef int count=0, index, scope=0, cell=0
        cdef double ks=0, zs=0, theta_s=0, theta_r=0, alpha=0
        cdef char tag[512]
        cdef SWMM_Engine h = _h(self)
        _check(swmm_gw2d_row_count(h, &count))
        rows = []
        for index in range(count):
            _check(swmm_gw2d_row_get(h, index, &scope, tag, 512, &cell,
                                    &ks, &zs, &theta_s, &theta_r, &alpha))
            rows.append(AquiferRow(CellScope(scope), tag.decode('utf-8'), cell,
                                   ks, zs, theta_s, theta_r, alpha))
        return tuple(rows)

    def add_row(self, scope, double ks, double zs, double theta_s,
                double theta_r, double alpha, *, str tag='', int cell=-1):
        """Append an aquifer row before initialization.

        ks: in/hr or mm/hr; zs: ft or m; alpha: inverse ft or inverse m.
        theta_s and theta_r are dimensionless water contents.
        """
        cdef bytes name = tag.encode('utf-8')
        _check(swmm_gw2d_row_add(_h(self), int(CellScope(scope)), name, cell,
                                ks, zs, theta_s, theta_r, alpha))

    def row_property(self, int index, str key):
        """Read an optional authored property (HG0, PSI_B, CLOSURE, etc.)."""
        cdef bytes name = key.encode('utf-8')
        cdef double value=0
        _check(swmm_gw2d_row_get_property(_h(self), index, name, &value))
        return value

    def set_row_property(self, int index, str key, double value):
        """Set optional numeric properties; soil and closure accept enum codes."""
        cdef bytes name = key.encode('utf-8')
        _check(swmm_gw2d_row_set_property(_h(self), index, name, value))

    def remove_row(self, int index):
        """Remove an authored row; subsequent indices shift."""
        _check(swmm_gw2d_row_remove(_h(self), index))

    @property
    def nodes(self):
        """Node-bed snapshots, including automatically enrolled rows after init."""
        cdef SWMM_Engine h = _h(self)
        cdef int count=0, index, cell=0, locate=0, exchange=0, automatic=0
        cdef double kc=0, dc=0, area=0
        cdef char node[512]
        _check(swmm_gw2d_node_count(h, &count))
        rows = []
        for index in range(count):
            _check(swmm_gw2d_node_get(h, index, node, 512, &cell, &kc, &dc, &area))
            _check(swmm_gw2d_node_get_flags(h, index, &locate, &exchange, &automatic))
            rows.append(AquiferNode(node.decode('utf-8'), cell, kc, dc, area,
                                    bool(locate), bool(exchange), bool(automatic)))
        return tuple(rows)

    def add_node(self, str node, int cell, double kc, double dc, double area=0):
        """Add a bed: kc in in/hr or mm/hr, dc in ft or m, area in ft² or m².

        cell=-1 locates from node coordinates; kc=0 is direct Darcy exchange;
        area=0 uses the cell area. Edit before initialization.
        """
        cdef bytes name = node.encode('utf-8')
        _check(swmm_gw2d_node_add(_h(self), name, cell, kc, dc, area))

    def remove_node(self, int index):
        """Remove a node-bed row; subsequent indices shift."""
        _check(swmm_gw2d_node_remove(_h(self), index))

    def set_node_exchange(self, int index, bint enabled):
        """Enable or opt out of groundwater exchange for one authored bed."""
        _check(swmm_gw2d_node_set_exchange(_h(self), index, int(enabled)))

    @property
    def active(self):
        cdef int active=0
        _check(swmm_gw2d_is_active(_h(self), &active))
        return bool(active)

    @property
    def dimensions(self):
        """(cell count, sigma layer count); cell topology need not be triangular."""
        cdef int cells=0, layers=0
        _check(swmm_gw2d_get_dimensions(_h(self), &cells, &layers))
        return (cells, layers)

    def cell(self, int cell, variable):
        """One cell's runtime variable in SI; consult GroundwaterVariable."""
        cdef double value=0
        _check(swmm_gw2d_get_cell(_h(self), cell, int(GroundwaterVariable(variable)), &value))
        return value

    def cells(self, variable):
        """Owned float64 snapshot of a runtime variable over all cells."""
        cdef SWMM_Engine h = _h(self)
        cdef int n=0, written=0, rc, selector=int(GroundwaterVariable(variable))
        _check(swmm_gw2d_get_dimensions(h, &n, NULL))
        cdef np.ndarray[double, ndim=1] out = np.empty(n, dtype=np.float64)
        with self._owner._operation(<size_t>h):
            with nogil:
                rc = swmm_gw2d_get_cell_bulk(h, selector, <double*>out.data, n, &written)
        _check(rc)
        return out[:written].copy()

    def column(self, int cell):
        """Sigma-column water contents, surface layer first; requires SIGMA closure."""
        cdef SWMM_Engine h = _h(self)
        cdef int n=0, written=0
        _check(swmm_gw2d_get_dimensions(h, NULL, &n))
        cdef np.ndarray[double, ndim=1] out = np.empty(n, dtype=np.float64)
        _check(swmm_gw2d_get_column(h, cell, <double*>out.data, n, &written))
        return out[:written].copy()

    def ledger(self, term):
        """Water ledger term in cubic metres (storage terms are instantaneous)."""
        cdef double value=0
        _check(swmm_gw2d_get_ledger(_h(self), int(GroundwaterLedger(term)), &value))
        return value

    @property
    def continuity_error(self):
        """Water continuity residual in cubic metres, not a percentage."""
        cdef double value=0
        _check(swmm_gw2d_get_continuity_error(_h(self), &value))
        return value

    @property
    def tier_histogram(self):
        """Owned platform C-long array of cumulative firings per LTS tier."""
        cdef SWMM_Engine h = _h(self)
        cdef int capacity=16, written=0
        cdef np.ndarray out
        while True:
            out = np.empty(capacity, dtype=np.dtype('l'))
            _check(swmm_gw2d_get_tier_histogram(h, <long*>out.data, capacity, &written))
            if written < capacity:
                return out[:written].copy()
            capacity *= 2

    @property
    def species(self):
        """Transported names in native policy order."""
        cdef SWMM_Engine h = _h(self)
        cdef int count=0, index
        cdef char name[512]
        _check(swmm_gw2d_species_count(h, &count))
        names = []
        for index in range(count):
            _check(swmm_gw2d_species_name(h, index, name, 512))
            names.append(name.decode('utf-8'))
        return tuple(names)

    def concentrations(self, zone, int species):
        """Per-cell species mass per m³ of zone water; dry zones report zero."""
        cdef SWMM_Engine h = _h(self)
        cdef int n=0, written=0, selected=int(GroundwaterZone(zone))
        _check(swmm_gw2d_get_dimensions(h, &n, NULL))
        cdef np.ndarray[double, ndim=1] out = np.empty(n, dtype=np.float64)
        _check(swmm_gw2d_get_cell_conc(h, selected, species, <double*>out.data, n, &written))
        return out[:written].copy()

    def species_ledger(self, int species, term):
        """Cumulative transported-species ledger term in the species' mass units."""
        cdef double value=0
        _check(swmm_gw2d_get_species_ledger(_h(self), species,
                                          int(GroundwaterSpeciesLedger(term)), &value))
        return value
