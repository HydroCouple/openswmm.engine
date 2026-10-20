"""Two-zone groundwater authoring and runtime snapshots.

Authored aquifer and node-bed values use project units. Runtime hydrology uses
SI (metres, seconds, cubic metres); species concentrations use mass per cubic
metre of zone water. Edit before initialize and read active state after start.
"""
from libc.stdlib cimport malloc, free
from dataclasses import dataclass
from collections.abc import MutableMapping
import numpy as np
cimport numpy as np
from ._common cimport SWMM_Engine, _check
from ._access import EngineView
from ._enums import (CellScope, GroundwaterVariable, GroundwaterLedger,
                     GroundwaterZone, GroundwaterSpeciesLedger)

cdef extern from "openswmm/engine/openswmm_gw2d.h":
    ctypedef struct SWMM_GwExecutionStatus:
        unsigned int struct_size
        int active
        unsigned int available_operators
        unsigned int executed_operators
        unsigned int host_owned_operations
        int effective_threads
        unsigned long long accepted_batches
        unsigned long long accepted_cells
        char recharge_backend[16]
        char profile[128]
        char detail[256]
    int swmm_gw2d_get_execution_status(SWMM_Engine,SWMM_GwExecutionStatus*) nogil
    ctypedef struct SWMM_GwBoundary:
        int cell
        int edge
        int kind
        double value
    int swmm_gw2d_boundary_count(SWMM_Engine) nogil
    int swmm_gw2d_boundary_get(SWMM_Engine,int,SWMM_GwBoundary*,char*,int) nogil
    int swmm_gw2d_boundary_set(SWMM_Engine,const SWMM_GwBoundary*,const char*) nogil
    int swmm_gw2d_boundary_remove(SWMM_Engine,int) nogil
    int swmm_gw2d_boundary_water(SWMM_Engine,int,double*,double*,double*,double*) nogil
    int swmm_gw2d_boundary_species(SWMM_Engine,int,int,double*,double*,double*,double*) nogil
    int swmm_gw2d_get_layer_quality(SWMM_Engine,int,int,int,double*,double*,double*) nogil

    int swmm_gw2d_species_count(SWMM_Engine engine, int* count) nogil
    int swmm_gw2d_species_name(SWMM_Engine engine, int species, char* buf, int buflen) nogil
    int swmm_gw2d_get_cell_conc(SWMM_Engine engine, int zone, int species, double* out, int len, int* written) nogil
    int swmm_gw2d_get_species_ledger(SWMM_Engine engine, int species, int term, double* value) nogil
    int swmm_gw2d_option_get(SWMM_Engine engine, const char* key, char* buf, int buflen) nogil
    int swmm_gw2d_process_options_set(SWMM_Engine engine, const char* et, const char* link, const char* wilting, int authored) nogil
    int swmm_gw2d_option_set(SWMM_Engine engine, const char* key, const char* value) nogil
    int swmm_gw2d_row_count(SWMM_Engine engine, int* count) nogil
    int swmm_gw2d_row_add(SWMM_Engine engine, int scope, const char* tag, int cell, double ks, double zs, double theta_s, double theta_r, double alpha) nogil
    int swmm_gw2d_row_get(SWMM_Engine engine, int index, int* scope, char* tag, int taglen, int* cell, double* ks, double* zs, double* theta_s, double* theta_r, double* alpha) nogil
    int swmm_gw2d_row_set_property(SWMM_Engine engine, int index, const char* key, double value) nogil
    int swmm_gw2d_row_get_property(SWMM_Engine engine, int index, const char* key, double* value) nogil
    int swmm_gw2d_row_remove(SWMM_Engine engine, int index) nogil
    ctypedef struct SWMM_GwNodeBed:
        const char* node
        int cell
        int locate
        int exchange
        int automatic
        double kc
        double dc
        double area
    ctypedef struct SWMM_LidNodeBottomEdit:
        int node
        int expected_mode
        int mode
    int swmm_gw2d_node_replace(SWMM_Engine,const SWMM_GwNodeBed*,int,const SWMM_GwNodeBed*,int,const SWMM_LidNodeBottomEdit*,int,int)
    int swmm_gw2d_node_count(SWMM_Engine engine, int* count) nogil
    int swmm_gw2d_node_add(SWMM_Engine engine, const char* node, int cell, double kc, double dc, double area) nogil
    int swmm_gw2d_node_get(SWMM_Engine engine, int index, char* node, int nodelen, int* cell, double* kc, double* dc, double* area) nogil
    int swmm_gw2d_node_remove(SWMM_Engine engine, int index) nogil
    int swmm_gw2d_node_get_flags(SWMM_Engine engine, int index, int* locate, int* exchange, int* automatic) nogil
    int swmm_gw2d_node_set_exchange(SWMM_Engine engine, int index, int exchange) nogil
    int swmm_gw2d_link_count(SWMM_Engine engine, int* count) nogil
    int swmm_gw2d_link_add(SWMM_Engine engine, const char* link, double kc, double dc, int exchange) nogil
    int swmm_gw2d_link_get(SWMM_Engine engine, int index, char* link, int linklen, double* kc, double* dc, int* exchange) nogil
    int swmm_gw2d_link_remove(SWMM_Engine engine, int index) nogil
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

@dataclass(frozen=True)
class AquiferLink:
    """[2D_AQUIFER_LINKS] conduit-bed row; kc in seepage units, dc in project length."""
    link: str
    kc: float
    dc: float
    exchange: bool

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
    def set_process_options(self, *, str et, str link, wilting="AUTO", bint authored=True):
        """Atomically set GW_ET, LINK_SEEPAGE and WILTING_SUCTION before a run.

        Custom wilting suction uses project length units; authored restores
        section provenance for undo. Invalid input leaves all options unchanged.
        """
        cdef bytes et_text = et.encode('utf-8')
        cdef bytes link_text = link.encode('utf-8')
        cdef bytes wilting_text = str(wilting).encode('utf-8')
        _check(swmm_gw2d_process_options_set(_h(self), et_text, link_text, wilting_text, authored))

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

    def replace_nodes(self, expected, replacement, *, bottoms=(), apply=True):
        """Atomic compare/replace of the entire bed list and LID bottom owners.

        expected/replacement are AquiferNode rows in project units. bottoms
        contains (node index, expected mode, new mode) triples (0/1/2). apply=False
        validates privately. Stale or invalid edits leave all rows unchanged;
        automatic rows may only be retained unchanged.
        """
        expected=list(expected);replacement=list(replacement);bottoms=list(bottoms)
        cdef int ne=len(expected), nr=len(replacement), nb=len(bottoms), i
        cdef SWMM_GwNodeBed* old=NULL
        cdef SWMM_GwNodeBed* new=NULL
        cdef SWMM_LidNodeBottomEdit* edits=NULL
        names=[]
        for row in expected+replacement:
            name=row.node.encode('utf-8')
            if b'\0' in name:raise ValueError('Node name contains NUL')
            names.append(name)
        try:
            if ne:
                old=<SWMM_GwNodeBed*>malloc(ne*sizeof(SWMM_GwNodeBed))
                if old==NULL:raise MemoryError()
            if nr:
                new=<SWMM_GwNodeBed*>malloc(nr*sizeof(SWMM_GwNodeBed))
                if new==NULL:raise MemoryError()
            if nb:
                edits=<SWMM_LidNodeBottomEdit*>malloc(nb*sizeof(SWMM_LidNodeBottomEdit))
                if edits==NULL:raise MemoryError()
            for i in range(ne):
                old[i].node=names[i]
                old[i].cell=expected[i].cell
                old[i].locate=expected[i].locate
                old[i].exchange=expected[i].exchange
                old[i].automatic=expected[i].automatic
                old[i].kc=expected[i].kc
                old[i].dc=expected[i].dc
                old[i].area=expected[i].area
            for i in range(nr):
                new[i].node=names[ne+i]
                new[i].cell=replacement[i].cell
                new[i].locate=replacement[i].locate
                new[i].exchange=replacement[i].exchange
                new[i].automatic=replacement[i].automatic
                new[i].kc=replacement[i].kc
                new[i].dc=replacement[i].dc
                new[i].area=replacement[i].area
            for i in range(nb):
                edits[i].node,edits[i].expected_mode,edits[i].mode=bottoms[i]
            _check(swmm_gw2d_node_replace(_h(self),old,ne,new,nr,edits,nb,bool(apply)))
        finally:
            free(old);free(new);free(edits)

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
    def links(self):
        """[2D_AQUIFER_LINKS] conduit-bed rows."""
        cdef SWMM_Engine h = _h(self)
        cdef int count=0, index, exchange=0
        cdef double kc=0, dc=0
        cdef char link[512]
        _check(swmm_gw2d_link_count(h, &count))
        rows = []
        for index in range(count):
            _check(swmm_gw2d_link_get(h, index, link, 512, &kc, &dc, &exchange))
            rows.append(AquiferLink(link.decode('utf-8'), kc, dc, bool(exchange)))
        return tuple(rows)

    def add_link(self, str link, double kc=0, double dc=0, bint exchange=True):
        """Add a conduit bed: kc in [LOSSES] seepage units (0 = the conduit's
        seep rate), dc in ft or m (0 = full depth). Edit before initialization."""
        cdef bytes name = link.encode('utf-8')
        _check(swmm_gw2d_link_add(_h(self), name, kc, dc, int(exchange)))

    def remove_link(self, int index):
        """Remove a conduit-bed row; subsequent indices shift."""
        _check(swmm_gw2d_link_remove(_h(self), index))

    @property
    def active(self):
        cdef int active=0
        _check(swmm_gw2d_is_active(_h(self), &active))
        return bool(active)

    @property
    def execution_status(self):
        """Actual recharge execution, distinct from the surface backend.

        Only raw CLOSED_FORM recharge may use the host OpenMP extension.
        Water/receipts, transport, reactions and heat remain host-owned.
        Runtime counters are diagnostics, not persisted simulation inventories.
        """
        cdef SWMM_GwExecutionStatus status
        status.struct_size=sizeof(SWMM_GwExecutionStatus)
        _check(swmm_gw2d_get_execution_status(_h(self),&status))
        return dict(active=bool(status.active),available_operators=status.available_operators,
                    executed_operators=status.executed_operators,host_owned_operations=status.host_owned_operations,
                    effective_threads=status.effective_threads,accepted_batches=status.accepted_batches,
                    accepted_cells=status.accepted_cells,recharge_backend=(<bytes>status.recharge_backend).decode('utf-8','replace'),
                    profile=(<bytes>status.profile).decode('utf-8','replace'),detail=(<bytes>status.detail).decode('utf-8','replace'))

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

    @property
    def sources(self):
        """Runtime external sources and cumulative accepted-transfer receipts."""
        self._address()
        from ._coupling import DomainSources
        return DomainSources(self._owner, 'groundwater')

    def set_head_boundary(self, int cell, int edge, double head_m, concentrations=None):
        self.sources.set_boundary(cell,edge,1,head_m,concentrations)

    def set_flow_boundary(self, int cell, int edge, double flow_m3_s, concentrations=None):
        """Prescribed total outward m³/s; negative supplies boundary water."""
        self.sources.set_boundary(cell,edge,2,flow_m3_s,concentrations)

    def clear_boundary(self, int cell, int edge):
        self.sources.clear_boundary(cell,edge)

    def boundary_flow(self, int cell, int edge):
        """Last accepted outward m³/s; zero for cleared or native no-flow edges."""
        return self.sources.boundary_flow(cell,edge)

    def boundary_receipt(self, int cell, int edge):
        """Cumulative boundary receipt, positive inward; heat includes advection."""
        return self.sources.boundary_receipt(cell,edge)

    def set_water_source(self, cells, flow_m3_s, **kwargs):
        self.sources.set_source(cells, flow_m3_s, **kwargs)

    def set_heat_source(self, cells, heat_w, *, source_id='heat', until_seconds=None):
        self.sources.set_source(cells, heat_w=heat_w, source_id=source_id, until_seconds=until_seconds)

    def set_species_source(self, cells, species_rates, *, source_id='species', until_seconds=None):
        self.sources.set_source(cells, species_rates=species_rates, source_id=source_id, until_seconds=until_seconds)

    def clear_source(self, source_id='external', cell=None):
        self.sources.clear_source(source_id,cell)

    def source_receipt(self, source_id='external', cell=0):
        return self.sources.source_receipt(source_id, cell)

    @property
    def authored_boundaries(self):
        """Exterior edge forcing. HEAD uses project elevation units; FLOW is total inward m3/s."""
        cdef int count=0,index
        cdef char series[8192]
        cdef SWMM_GwBoundary row
        count=swmm_gw2d_boundary_count(_h(self))
        if count<0:raise RuntimeError("Unable to read authored groundwater boundaries")
        result=[]
        for index in range(count):
            _check(swmm_gw2d_boundary_get(_h(self),index,&row,series,sizeof(series)))
            result.append({'cell':row.cell,'edge':row.edge,'kind':'HEAD' if row.kind==0 else 'FLOW',
                           'value':row.value,'series':series.decode('utf-8')})
        return tuple(result)

    def set_authored_boundary(self,int cell,int edge,kind,double value=0,series=''):
        """Set before initialize. Chemistry is authored through groundwater transport boundary quality."""
        cdef SWMM_GwBoundary row
        selected=str(kind).upper()
        if selected not in ('HEAD','FLOW'):raise ValueError('kind must be HEAD or FLOW')
        row.cell=cell;row.edge=edge;row.kind=0 if selected=='HEAD' else 1;row.value=value
        cdef bytes name=str(series).encode('utf-8')
        _check(swmm_gw2d_boundary_set(_h(self),&row,name))

    def remove_authored_boundary(self,int index):
        _check(swmm_gw2d_boundary_remove(_h(self),index))

    def authored_boundary_receipt(self,int index,species=None):
        """Gross requested/accepted water or native advective/imposed species amounts."""
        cdef double a=0,b=0,c=0,d=0
        cdef int row
        if species is None:
            _check(swmm_gw2d_boundary_water(_h(self),index,&a,&b,&c,&d))
            return {'requested_in':a,'requested_out':b,'accepted_in':c,'accepted_out':d}
        row=int(species)
        _check(swmm_gw2d_boundary_species(_h(self),index,row,&a,&b,&c,&d))
        return {'advective_in':a,'advective_out':b,'flux_in':c,'flux_out':d}

    def layer_quality(self,int cell,int layer,int species):
        """Canonical SIGMA inventory; layer is zero-based. Dry retained mass is reported explicitly."""
        cdef double water=0,mass=0,concentration=0
        _check(swmm_gw2d_get_layer_quality(_h(self),cell,layer,species,&water,&mass,&concentration))
        return {'water_m3':water,'total_mass':mass,'dissolved_concentration':concentration}

    def concentrations(self, zone, int species):
        """Per-cell zone concentration. BULK uses total inventory/water; LAYERED uses aqueous plus sorption capacity."""
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
