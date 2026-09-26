"""Transport configuration and engine capability snapshots.

Dispersion values use project length squared per second; target spacing uses
project length. Boundary/source rows are immutable snapshots because the
native API currently resolves these rows only during model open.
"""
from dataclasses import dataclass
from typing import NamedTuple
from ._common cimport SWMM_Engine, _check
from ._enums import TransportDispersionMode, TransportDomain, TransportClass, TransportState

cdef extern from "openswmm/engine/openswmm_transport.h":
    int swmm_transport_get_configured(SWMM_Engine engine, int* configured)
    int swmm_transport_get_dispersion_mode(SWMM_Engine engine, int* mode)
    int swmm_transport_set_dispersion_mode(SWMM_Engine engine, int mode)
    int swmm_transport_get_dispersion_value(SWMM_Engine engine, double* value)
    int swmm_transport_set_dispersion_value(SWMM_Engine engine, double value)
    int swmm_transport_get_target_dx(SWMM_Engine engine, double* dx)
    int swmm_transport_set_target_dx(SWMM_Engine engine, double dx)
    int swmm_transport_conduit_disp_count(SWMM_Engine engine, int* count)
    int swmm_transport_get_conduit_disp(SWMM_Engine engine, int index, int* link_index, double* value)
    int swmm_transport_boundary_count(SWMM_Engine engine, int* count)
    int swmm_transport_get_boundary(SWMM_Engine engine, int index, char* element, int elem_len, char* species, int spec_len, int* is_ts, double* value, char* ts_name, int ts_len)
    int swmm_transport_source_count(SWMM_Engine engine, int* count)
    int swmm_transport_get_source(SWMM_Engine engine, int index, char* element, int elem_len, char* species, int spec_len, int* is_ts, double* value, char* ts_name, int ts_len)

cdef extern from "openswmm/engine/openswmm_engine.h":
    ctypedef struct SWMM_ThreadInfo:
        int logical_cpus
        int omp_max_threads
        int omp_available
        int perf_cores
        int kokkos_omp_threads
    ctypedef struct SWMM_TransportCell:
        int state
        int count
        char reason[96]
    ctypedef struct SWMM_TransportMatrix:
        SWMM_TransportCell cell[4][4]
    int swmm_get_thread_info(SWMM_ThreadInfo*)
    int swmm_get_effective_threads(SWMM_Engine, int, int*, int*, int*)
    int swmm_get_transport_matrix(SWMM_Engine, SWMM_TransportMatrix*)
    const char* swmm_transport_domain_name(int)
    const char* swmm_transport_class_name(int)

cdef inline SWMM_Engine _h(owner):
    return <SWMM_Engine><size_t>owner.handle

class ThreadInfo(NamedTuple):
    logical_cpus: int
    omp_max_threads: int
    omp_available: bool
    perf_cores: int
    kokkos_omp_threads: int

class EffectiveThreads(NamedTuple):
    global_threads: int
    dynamic_wave: int
    surface2d: int

@dataclass(frozen=True)
class TransportCell:
    state: TransportState
    count: int
    reason: str

class TransportRow(NamedTuple):
    element: str
    species: str
    is_timeseries: bool
    value: float
    timeseries: str

class ConduitDispersion(NamedTuple):
    link_index: int
    value: float

def thread_info():
    """Hardware and OpenMP limits; zero means unknown/not initialized."""
    cdef SWMM_ThreadInfo info
    _check(swmm_get_thread_info(&info))
    return ThreadInfo(info.logical_cpus, info.omp_max_threads,
                      bool(info.omp_available), info.perf_cores, info.kokkos_omp_threads)

def effective_threads(owner, int requested=0):
    """Resolve thread teams for this model; requested=0 selects auto."""
    cdef int general=0, dw=0, surface=0
    _check(swmm_get_effective_threads(_h(owner), requested, &general, &dw, &surface))
    return EffectiveThreads(general, dw, surface)

def transport_matrix(owner):
    """Fresh capability matrix keyed by domain then species class.

    Read after open and after options change. Unavailable entries carry a
    reason instead of implying the process is implemented by a backend.
    """
    cdef SWMM_TransportMatrix matrix
    cdef int d, c
    _check(swmm_get_transport_matrix(_h(owner), &matrix))
    return {TransportDomain(d): {
        TransportClass(c): TransportCell(TransportState(matrix.cell[d][c].state),
            matrix.cell[d][c].count, matrix.cell[d][c].reason.decode('utf-8'))
        for c in range(4)} for d in range(4)}

def transport_domain_name(domain):
    """Native display label for a validated TransportDomain."""
    return swmm_transport_domain_name(int(TransportDomain(domain))).decode('utf-8')

def transport_class_name(species_class):
    """Native display label for a validated TransportClass."""
    return swmm_transport_class_name(int(TransportClass(species_class))).decode('utf-8')

class Transport:
    """``solver.transport`` configuration, with read-only authored rows."""
    def __init__(self, owner):
        self._owner = owner

    @property
    def configured(self):
        """Whether an ARD transport component is configured."""
        cdef int value = 0
        _check(swmm_transport_get_configured(_h(self._owner), &value))
        return bool(value)

    @property
    def dispersion_mode(self):
        """Global dispersion closure, including per-conduit overrides."""
        cdef int value = 0
        _check(swmm_transport_get_dispersion_mode(_h(self._owner), &value))
        return TransportDispersionMode(value)

    @dispersion_mode.setter
    def dispersion_mode(self, value):
        _check(swmm_transport_set_dispersion_mode(_h(self._owner), int(TransportDispersionMode(value))))

    @property
    def dispersion_value(self):
        """Nonnegative coefficient in project length squared per second."""
        cdef double value = 0
        _check(swmm_transport_get_dispersion_value(_h(self._owner), &value))
        return value

    @dispersion_value.setter
    def dispersion_value(self, double value):
        _check(swmm_transport_set_dispersion_value(_h(self._owner), value))

    @property
    def target_dx(self):
        """Nonnegative target spacing in project length; zero uses the default."""
        cdef double value = 0
        _check(swmm_transport_get_target_dx(_h(self._owner), &value))
        return value

    @target_dx.setter
    def target_dx(self, double value):
        _check(swmm_transport_set_target_dx(_h(self._owner), value))

    @property
    def conduit_dispersion(self):
        """Immutable per-conduit (link index, coefficient) snapshots."""
        cdef int count=0, index=0, link=0
        cdef double value=0
        cdef SWMM_Engine h = _h(self._owner)
        _check(swmm_transport_conduit_disp_count(h, &count))
        rows = []
        for index in range(count):
            _check(swmm_transport_get_conduit_disp(h, index, &link, &value))
            rows.append(ConduitDispersion(link, value))
        return tuple(rows)

    @property
    def boundaries(self):
        """Authored rows; value is concentration in species units.

        For timeseries rows read ``timeseries`` instead of ``value``.
        """
        cdef int count=0, index=0, is_ts=0
        cdef double value=0
        cdef char element[256]
        cdef char species[256]
        cdef char series[256]
        cdef SWMM_Engine h = _h(self._owner)
        _check(swmm_transport_boundary_count(h, &count))
        rows = []
        for index in range(count):
            _check(swmm_transport_get_boundary(h, index, element, 256, species, 256,
                                                 &is_ts, &value, series, 256))
            rows.append(TransportRow(element.decode('utf-8'), species.decode('utf-8'),
                        bool(is_ts), value, series.decode('utf-8')))
        return tuple(rows)

    @property
    def sources(self):
        """Authored rows; value is mass rate in species mass units per second.

        For timeseries rows read ``timeseries`` instead of ``value``.
        """
        cdef int count=0, index=0, is_ts=0
        cdef double value=0
        cdef char element[256]
        cdef char species[256]
        cdef char series[256]
        cdef SWMM_Engine h = _h(self._owner)
        _check(swmm_transport_source_count(h, &count))
        rows = []
        for index in range(count):
            _check(swmm_transport_get_source(h, index, element, 256, species, 256,
                                                 &is_ts, &value, series, 256))
            rows.append(TransportRow(element.decode('utf-8'), species.decode('utf-8'),
                        bool(is_ts), value, series.decode('utf-8')))
        return tuple(rows)
