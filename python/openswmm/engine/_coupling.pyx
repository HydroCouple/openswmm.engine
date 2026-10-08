"""Runtime exchange sources. All water rates are total SI m³/s per cell.

Concentrations follow each domain's species getter (including °C and seconds).
Species rates use concentration-unit*m³/s; heat is W supplied to water only.
Different source IDs add; reusing an ID/cell replaces its held prescription.
"""
from dataclasses import dataclass, field
import numpy as np
cimport numpy as np
from libcpp.vector cimport vector
from ._common cimport SWMM_Engine, _check
from ._access import EngineView


@dataclass(frozen=True)
class RuntimeSource:
    """One cell prescription. `until_seconds=None` holds until replaced/cleared."""
    source_id: str
    domain: str
    cell: int
    flow_m3_s: float = 0.0
    concentrations: dict = field(default_factory=dict)
    species_rates: dict = field(default_factory=dict)
    heat_w: float = 0.0
    until_seconds: object = None

@dataclass(frozen=True)
class RuntimeBoundary:
    domain: str
    cell: int
    edge: int
    kind: int
    value: float = 0.0
    concentrations: dict = field(default_factory=dict)

@dataclass(frozen=True)
class RuntimeForcing:
    channel: str
    cell: int = -1
    rate_m_s: float = 0.0
    mode: str = 'replace'
    concentrations: object = None
    until_seconds: object = None

@dataclass(frozen=True)
class RuntimeClear:
    source_id: str
    domain: str
    cell: int = -1

@dataclass(frozen=True)
class CouplingFrame:
    sources: tuple = ()
    boundaries: tuple = ()
    forcings: tuple = ()
    clears: tuple = ()

@dataclass(frozen=True)
class SourceReceipt:
    requested_m3: float
    applied_m3: float
    rejected_m3: float
    requested_heat_j: float
    applied_heat_j: float
    rejected_heat_j: float
    requested_species: dict
    applied_species: dict
    last_applied_m3_s: float
    last_interval_seconds: float

    @property
    def rejected_species(self):
        return {k:self.requested_species[k]-v for k,v in self.applied_species.items()}

    @property
    def limiter_reasons(self):
        reasons=[]
        if self.rejected_m3: reasons.append('insufficient_water')
        if self.rejected_heat_j: reasons.append('dry_cell_heat')
        if any(self.rejected_species.values()): reasons.append('insufficient_species_or_water')
        return tuple(reasons)

@dataclass(frozen=True)
class BoundaryReceipt(SourceReceipt):
    """Boundary/paired transfer receipt; heat fields include advection."""
    @property
    def limiter_reasons(self):
        return ('water_or_receiver_capacity',) if self.rejected_m3 else ()

class Coupling(EngineView):
    def __init__(self, owner, provider=''):
        super().__init__(owner)
        if not isinstance(provider,str) or '/' in provider or '\0' in provider:
            raise ValueError('provider must be a string without slash or NUL')
        self._provider_id=provider

    @property
    def provider_id(self):
        return self._provider_id

    def for_provider(self, name):
        self._address()
        return Coupling(self._owner,name)

    @property
    def surface(self):
        self._address()
        return DomainSources(self._owner,'surface',self.provider_id)

    @property
    def groundwater(self):
        self._address()
        return DomainSources(self._owner,'groundwater',self.provider_id)

    def set_sources(self, records):
        return self.apply_frame(CouplingFrame(sources=tuple(records)))

    @property
    def capabilities(self):
        cdef unsigned flags=0
        cdef SWMM_Engine h=<SWMM_Engine><size_t>self._address()
        with self._owner._operation(<size_t>h):
            _check(swmm_coupling_capabilities(h,&flags))
        return dict(surface_sources=bool(flags&1), groundwater_sources=bool(flags&2),
                    surface_water_heat=bool(flags&4), surface_boundaries=bool(flags&8),
                    groundwater_water_heat=bool(flags&16), groundwater_soil_heat=False,
                    groundwater_lateral_boundaries=bool(flags&32), atomic_frames=bool(flags&64),
                    rainfall_quality=bool(flags&128), paired_infiltration=bool(flags&256),
                    boundary_receipts=bool(flags&512), rollback=False)

    def apply_frame(self, frame):
        """Atomically update sources, boundaries and forcing across both domains."""
        if not isinstance(frame,CouplingFrame): raise TypeError("expected CouplingFrame")
        cdef SWMM_Engine h=<SWMM_Engine><size_t>self._address()
        cdef vector[SWMM_CouplingSource] rows
        cdef SWMM_CouplingSource row
        cdef np.ndarray conc, rates
        cdef bytes name
        cdef vector[SWMM_CouplingBoundary] boundaries
        cdef vector[SWMM_CouplingForcing] forcings
        cdef vector[SWMM_CouplingClear] clears
        cdef SWMM_CouplingBoundary boundary
        cdef SWMM_CouplingForcing forcing
        cdef SWMM_CouplingClear clear
        cdef SWMM_CouplingFrame native
        cdef bytes provider=self.provider_id.encode('utf-8')
        keep=[]
        with self._owner._operation(<size_t>h):
            for record in frame.sources:
                if not isinstance(record, RuntimeSource):
                    raise TypeError('records must contain RuntimeSource objects')
                if record.domain not in ('surface', 'groundwater'):
                    raise ValueError('domain must be surface or groundwater')
                if not record.source_id or '\0' in record.source_id:
                    raise ValueError('source_id must be nonempty and contain no NUL')
                if record.source_id.startswith(('boundary/','provider/')):
                    raise ValueError('boundary/ source IDs are reserved for hydraulic boundaries')
                if record.until_seconds is not None and (
                        not np.isfinite(record.until_seconds) or record.until_seconds <= 0):
                    raise ValueError('until_seconds must be a finite future time; use None to hold')
                if isinstance(record.cell, bool) or int(record.cell)!=record.cell:
                    raise ValueError('cell must be an integer')
                view=self._owner.surface2d if record.domain=='surface' else self._owner.groundwater2d
                names=list(view.species)
                if record.heat_w!=0.0 and '__TEMPERATURE__' not in names:
                    raise ValueError(f'Water temperature transport is not enabled for {record.domain}; soil heat conduction is unsupported')
                unknown=(set(record.concentrations)|set(record.species_rates))-set(names)
                if unknown: raise ValueError(f'Unknown or disabled species: {sorted(unknown)}')
                conc=np.array([record.concentrations.get(n,0.0) for n in names],dtype=np.float64)
                rates=np.array([record.species_rates.get(n,0.0) for n in names],dtype=np.float64)
                name=record.source_id.encode('utf-8')
                keep.extend((name,conc,rates))
                row.struct_size=sizeof(SWMM_CouplingSource)
                row.id=name
                row.domain=0 if record.domain=='surface' else 1
                row.cell=int(record.cell)
                row.flow_m3_s=record.flow_m3_s
                row.heat_w=record.heat_w
                row.until_seconds=0.0 if record.until_seconds is None else record.until_seconds
                row.species_count=len(names)
                row.concentrations=<double*>conc.data
                row.rates=<double*>rates.data
                rows.push_back(row)
            for record in frame.boundaries:
                if not isinstance(record,RuntimeBoundary): raise TypeError('expected RuntimeBoundary')
                if record.domain not in ('surface','groundwater'): raise ValueError('invalid domain')
                if isinstance(record.cell,bool) or int(record.cell)!=record.cell or isinstance(record.edge,bool) or int(record.edge)!=record.edge:
                    raise ValueError('cell and edge must be integers')
                names=list((self._owner.surface2d if record.domain=='surface' else self._owner.groundwater2d).species)
                if set(record.concentrations)-set(names): raise ValueError('unknown or disabled species')
                conc=np.array([record.concentrations.get(n,0.0) for n in names],dtype=np.float64)
                keep.append(conc)
                boundary.struct_size=sizeof(SWMM_CouplingBoundary)
                boundary.domain=0 if record.domain=='surface' else 1
                boundary.cell=record.cell; boundary.edge=record.edge; boundary.kind=record.kind
                boundary.value=record.value; boundary.species_count=0 if record.kind==0 else len(names)
                boundary.concentrations=<double*>conc.data
                boundaries.push_back(boundary)
            for record in frame.forcings:
                if not isinstance(record,RuntimeForcing): raise TypeError('expected RuntimeForcing')
                channels={'rainfall':1,'evaporation':2,'infiltration':3,'rain_quality':4}
                modes={'clear':0,'replace':1,'add':2}
                if record.channel not in channels or record.mode not in modes: raise ValueError('invalid forcing channel/mode')
                if isinstance(record.cell,bool) or int(record.cell)!=record.cell: raise ValueError('cell must be an integer')
                names=list(self._owner.surface2d.species) if record.concentrations is not None else []
                if set(record.concentrations or {})-set(names): raise ValueError('unknown or disabled species')
                conc=np.array([(record.concentrations or {}).get(n,0.0) for n in names],dtype=np.float64)
                keep.append(conc)
                forcing.struct_size=sizeof(SWMM_CouplingForcing)
                forcing.channel=channels[record.channel]; forcing.cell=record.cell; forcing.mode=modes[record.mode]
                forcing.rate_m_s=record.rate_m_s
                forcing.until_seconds=0.0 if record.until_seconds is None else record.until_seconds
                forcing.species_count=0 if record.mode=='clear' else len(names); forcing.concentrations=<double*>conc.data
                forcings.push_back(forcing)
            for record in frame.clears:
                if not isinstance(record,RuntimeClear): raise TypeError('expected RuntimeClear')
                if record.domain not in ('surface','groundwater'): raise ValueError('invalid domain')
                if isinstance(record.cell,bool) or int(record.cell)!=record.cell: raise ValueError('cell must be an integer')
                if not record.source_id or '\0' in record.source_id: raise ValueError('invalid source_id')
                name=record.source_id.encode('utf-8'); keep.append(name)
                clear.domain=0 if record.domain=='surface' else 1; clear.cell=record.cell; clear.id=name
                clears.push_back(clear)
            native.struct_size=sizeof(SWMM_CouplingFrame); native.version=1; native.provider=provider
            native.source_count=rows.size(); native.sources=rows.data()
            native.boundary_count=boundaries.size(); native.boundaries=boundaries.data()
            native.forcing_count=forcings.size(); native.forcings=forcings.data()
            native.clear_count=clears.size(); native.clears=clears.data()
            if not self.provider_id and not boundaries.size() and not forcings.size() and not clears.size():
                _check(swmm_coupling_set_sources(h,rows.data(),rows.size()))
            else:
                _check(swmm_coupling_apply_frame(h,&native))

    def advance_to(self, seconds):
        return self._owner.advance_to(seconds)

class DomainSources(EngineView):
    def __init__(self, owner, domain, provider=""):
        if domain not in ('surface', 'groundwater'):
            raise ValueError('domain must be surface or groundwater')
        super().__init__(owner)
        self.domain=domain
        self.provider_id=provider

    @property
    def water_totals(self):
        """Gross accepted source inflow/outflow in m³ since start."""
        cdef SWMM_Engine h=<SWMM_Engine><size_t>self._address()
        cdef double incoming=0.0, outgoing=0.0
        with self._owner._operation(<size_t>h):
            _check(swmm_coupling_get_water_totals(h,0 if self.domain=='surface' else 1,&incoming,&outgoing))
        return {'external_in':incoming,'external_out':outgoing}

    def set_source(self, cells, flow_m3_s=0.0, *, source_id='external',
                   concentrations=None, species_rates=None, heat_w=0.0, until_seconds=None):
        """Scalar or bulk: each scalar value broadcasts to every listed cell.

        Rate arrays are per cell, not a total to distribute. Omitted species
        concentrations are zero. Set temperature via `__TEMPERATURE__`.
        """
        self._address()
        selected=np.atleast_1d(cells)
        if selected.ndim!=1: raise ValueError('cells must be a scalar or one-dimensional')
        def broadcast(value):
            return np.broadcast_to(np.asarray(value),selected.shape)
        flows=broadcast(flow_m3_s); heats=broadcast(heat_w)
        conc={k:broadcast(v) for k,v in (concentrations or {}).items()}
        rates={k:broadcast(v) for k,v in (species_rates or {}).items()}
        records=[RuntimeSource(source_id,self.domain,cell,flows[i],
                 {k:v[i] for k,v in conc.items()}, {k:v[i] for k,v in rates.items()},
                 heats[i],until_seconds) for i,cell in enumerate(selected)]
        Coupling(self._owner,self.provider_id).set_sources(records)

    def set_boundary(self, int cell, int edge, int kind, double value, concentrations=None):
        if self.provider_id:
            return self.set_boundaries([RuntimeBoundary(self.domain,cell,edge,kind,value,concentrations or {})])
        cdef SWMM_Engine h=<SWMM_Engine><size_t>self._address()
        cdef np.ndarray values
        with self._owner._operation(<size_t>h):
            view=self._owner.groundwater2d if self.domain=='groundwater' else self._owner.surface2d
            names=list(view.species)
            conc=concentrations or {}
            unknown=set(conc)-set(names)
            if unknown: raise ValueError(f'Unknown or disabled species: {sorted(unknown)}')
            values=np.array([conc.get(n,0.0) for n in names],dtype=np.float64)
            if self.domain=='groundwater':
                _check(swmm_gw2d_set_runtime_boundary(h,cell,edge,kind,value,<double*>values.data,len(names)))
            else:
                _check(swmm_2d_set_runtime_boundary(h,cell,edge,kind,value,<double*>values.data,len(names)))

    def clear_boundary(self, int cell, int edge):
        if self.provider_id or self.domain=='surface':
            return self.set_boundaries([RuntimeBoundary(self.domain,cell,edge,0)])
        cdef SWMM_Engine h=<SWMM_Engine><size_t>self._address()
        with self._owner._operation(<size_t>h):
            _check(swmm_gw2d_clear_runtime_boundary(h,cell,edge))

    def boundary_flow(self, int cell, int edge):
        if self.domain!='groundwater': raise ValueError('use surface boundary_flow')
        cdef SWMM_Engine h=<SWMM_Engine><size_t>self._address()
        cdef double flow=0.0
        with self._owner._operation(<size_t>h):
            _check(swmm_gw2d_get_runtime_boundary_flow(h,cell,edge,&flow))
        return flow

    def clear_source(self, source_id='external', cell=None):
        if self.provider_id:
            return Coupling(self._owner,self.provider_id).apply_frame(CouplingFrame(clears=(RuntimeClear(source_id,self.domain,-1 if cell is None else cell),)))
        cdef SWMM_Engine h=<SWMM_Engine><size_t>self._address()
        cdef bytes name=source_id.encode('utf-8')
        if not name or b'\0' in name: raise ValueError('invalid source_id')
        with self._owner._operation(<size_t>h):
            if cell is None:
                _check(swmm_coupling_clear_source(h,0 if self.domain=='surface' else 1,name))
            else:
                if isinstance(cell,bool) or int(cell)!=cell: raise ValueError('cell must be an integer')
                _check(swmm_coupling_clear_source_cell(h,0 if self.domain=='surface' else 1,name,cell))

    def source_receipt(self, source_id='external', cell=0):
        cdef SWMM_Engine h=<SWMM_Engine><size_t>self._address()
        cdef SWMM_CouplingReceipt receipt
        cdef bytes name=(f'provider/{self.provider_id}/{source_id}' if self.provider_id else source_id).encode('utf-8')
        if not name or b'\0' in name: raise ValueError('invalid source_id')
        cdef np.ndarray requested,applied
        with self._owner._operation(<size_t>h):
            view=self._owner.surface2d if self.domain=='surface' else self._owner.groundwater2d
            names=list(view.species)
            requested=np.zeros(len(names),dtype=np.float64); applied=requested.copy()
            _check(swmm_coupling_get_receipt(h,0 if self.domain=='surface' else 1,name,cell,
                   &receipt,<double*>requested.data,<double*>applied.data,len(names)))
        return SourceReceipt(receipt.requested_m3,receipt.applied_m3,receipt.rejected_m3,
                   receipt.requested_heat_j,receipt.applied_heat_j,receipt.rejected_heat_j,
                   dict(zip(names,requested)),dict(zip(names,applied)),
                   receipt.last_applied_m3_s,receipt.last_interval_seconds)

    def set_boundaries(self, records):
        records=tuple(records)
        if any(r.domain!=self.domain for r in records): raise ValueError('boundary domain mismatch')
        return Coupling(self._owner,self.provider_id).apply_frame(CouplingFrame(boundaries=records))

    def set_forcings(self, records):
        if self.domain!='surface': raise ValueError('forcing addresses surface cells')
        return Coupling(self._owner,self.provider_id).apply_frame(CouplingFrame(forcings=tuple(records)))

    def source_receipts(self, cells, source_id='external'):
        cdef SWMM_Engine h=<SWMM_Engine><size_t>self._address()
        cdef vector[int] selected
        cdef vector[SWMM_CouplingReceipt] receipts
        cdef bytes name=(f'provider/{self.provider_id}/{source_id}' if self.provider_id else source_id).encode('utf-8')
        cdef np.ndarray requested,applied
        cdef int i
        if not name or b'\0' in name: raise ValueError('invalid source_id')
        for cell in cells:
            if isinstance(cell,bool) or int(cell)!=cell: raise ValueError('cell must be an integer')
            selected.push_back(cell)
        receipts.resize(selected.size())
        with self._owner._operation(<size_t>h):
            names=list((self._owner.surface2d if self.domain=='surface' else self._owner.groundwater2d).species)
            requested=np.zeros((selected.size(),len(names)),dtype=np.float64); applied=requested.copy()
            _check(swmm_coupling_get_receipts(h,0 if self.domain=='surface' else 1,name,selected.data(),selected.size(),receipts.data(),<double*>requested.data,<double*>applied.data,len(names)))
        return [SourceReceipt(receipts[i].requested_m3,receipts[i].applied_m3,receipts[i].rejected_m3,
                receipts[i].requested_heat_j,receipts[i].applied_heat_j,receipts[i].rejected_heat_j,
                dict(zip(names,requested[i])),dict(zip(names,applied[i])),receipts[i].last_applied_m3_s,receipts[i].last_interval_seconds) for i in range(selected.size())]

    def boundary_receipt(self, int cell, int edge):
        return self._transfer_receipt(cell,edge,False)

    def infiltration_receipt(self, int cell):
        if self.domain!='surface': raise ValueError('infiltration receipts use surface species')
        return self._transfer_receipt(cell,0,True)

    def _transfer_receipt(self, int cell, int edge, infiltration):
        cdef SWMM_Engine h=<SWMM_Engine><size_t>self._address()
        cdef SWMM_CouplingReceipt receipt
        cdef np.ndarray requested,applied
        with self._owner._operation(<size_t>h):
            names=list((self._owner.surface2d if self.domain=='surface' else self._owner.groundwater2d).species)
            requested=np.zeros(len(names),dtype=np.float64); applied=requested.copy()
            if infiltration:
                _check(swmm_coupling_get_infiltration_receipt(h,cell,&receipt,<double*>requested.data,<double*>applied.data,len(names)))
            else:
                _check(swmm_coupling_get_boundary_receipt(h,0 if self.domain=='surface' else 1,cell,edge,&receipt,<double*>requested.data,<double*>applied.data,len(names)))
        return BoundaryReceipt(receipt.requested_m3,receipt.applied_m3,receipt.rejected_m3,
               receipt.requested_heat_j,receipt.applied_heat_j,receipt.rejected_heat_j,
               dict(zip(names,requested)),dict(zip(names,applied)),receipt.last_applied_m3_s,receipt.last_interval_seconds)

    def species_ledger(self, species):
        if self.domain!='surface': raise ValueError('surface ledger only')
        cdef SWMM_Engine h=<SWMM_Engine><size_t>self._address()
        cdef double value
        cdef int term,row
        with self._owner._operation(<size_t>h):
            row=list(self._owner.surface2d.species).index(species)
            result={}
            for term,key in enumerate(('storage','external_in','external_out','boundary_in','boundary_out','rainfall_in','infiltration_out','coupling_in','coupling_out','exfiltration_in')):
                _check(swmm_2d_get_species_ledger(h,row,term,&value)); result[key]=value
        return result
