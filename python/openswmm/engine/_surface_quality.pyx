"""Scoped surface-quality authoring and independent buildup snapshots.

Edit before initialization. GLOBAL < TAG < CELL resolution applies to entire
coverage sets. All indices address cells, with no triangular-mesh assumption.
"""
from dataclasses import dataclass
import numpy as np
cimport numpy as np
from libcpp.vector cimport vector
from ._common cimport SWMM_Engine, _check
from ._access import EngineView
from ._enums import CellScope

cdef extern from "openswmm/engine/openswmm_sq2d.h":
    int swmm_2d_coverage_count(SWMM_Engine engine, int* count) nogil
    int swmm_2d_coverage_set(SWMM_Engine engine, int scope, const char* tag, int cell, const char* const* landuses, const double* percents, int n) nogil
    int swmm_2d_coverage_row_size(SWMM_Engine engine, int index, int* n) nogil
    int swmm_2d_coverage_get(SWMM_Engine engine, int index, int k, int* scope, char* tag, int taglen, int* cell, char* landuse, int lulen, double* percent) nogil
    int swmm_2d_coverage_remove(SWMM_Engine engine, int index) nogil
    int swmm_2d_loading_count(SWMM_Engine engine, int* count) nogil
    int swmm_2d_loading_set(SWMM_Engine engine, int scope, const char* tag, int cell, const char* species, double value) nogil
    int swmm_2d_loading_get(SWMM_Engine engine, int index, int* scope, char* tag, int taglen, int* cell, char* species, int splen, double* value) nogil
    int swmm_2d_loading_remove(SWMM_Engine engine, int index) nogil
    int swmm_2d_curb_length_count(SWMM_Engine engine, int* count) nogil
    int swmm_2d_curb_length_set(SWMM_Engine engine, int scope, const char* tag, int cell, double length) nogil
    int swmm_2d_curb_length_get(SWMM_Engine engine, int index, int* scope, char* tag, int taglen, int* cell, double* length) nogil
    int swmm_2d_curb_length_remove(SWMM_Engine engine, int index) nogil
    int swmm_2d_get_buildup_bulk(SWMM_Engine engine, const char* species, double* out, int n) nogil

cdef extern from "openswmm/engine/openswmm_2d.h":
    int swmm_2d_cell_count(SWMM_Engine, int*)

cdef inline SWMM_Engine _h(view):
    return <SWMM_Engine><size_t>view._address()

@dataclass(frozen=True)
class SurfaceCoverage:
    """A whole land-use coverage set at one scope; pairs carry percentages."""
    scope: CellScope
    tag: str
    cell: int
    landuses: tuple[tuple[str, float], ...]

@dataclass(frozen=True)
class SurfaceLoading:
    """Initial species buildup in project mass per acre/hectare."""
    scope: CellScope
    tag: str
    cell: int
    species: str
    value: float

@dataclass(frozen=True)
class SurfaceCurbLength:
    """Authored curb length in project length units."""
    scope: CellScope
    tag: str
    cell: int
    length: float

class SurfaceQuality(EngineView):
    """``solver.surface2d.quality``: coverages, loadings and curb lengths."""
    @property
    def coverages(self):
        """Immutable whole-scope coverage snapshots; percentages sum to <=100."""
        cdef SWMM_Engine h = _h(self)
        cdef int count=0, index, pairs=0, k, scope=0, cell=0
        cdef double percent=0
        cdef char tag[512]
        cdef char landuse[512]
        _check(swmm_2d_coverage_count(h, &count))
        rows = []
        for index in range(count):
            _check(swmm_2d_coverage_row_size(h, index, &pairs))
            uses = []
            for k in range(pairs):
                _check(swmm_2d_coverage_get(h, index, k, &scope, tag, 512,
                                           &cell, landuse, 512, &percent))
                uses.append((landuse.decode('utf-8'), percent))
            rows.append(SurfaceCoverage(CellScope(scope), tag.decode('utf-8'), cell, tuple(uses)))
        return tuple(rows)

    def set_coverage(self, scope, landuses, *, str tag='', int cell=-1):
        """Replace the complete coverage set at a scope from a name→percent mapping."""
        items = list(landuses.items())
        if not items:
            raise ValueError('Coverage needs at least one land use; remove the row to clear it')
        encoded = [str(name).encode('utf-8') for name, _ in items]
        cdef np.ndarray[double, ndim=1] values = np.asarray([v for _, v in items], dtype=np.float64)
        if not np.isfinite(values).all():
            raise ValueError('Coverage percentages must be finite')
        cdef vector[const char*] names
        cdef bytes encoded_name
        for encoded_name in encoded:
            names.push_back(encoded_name)
        cdef bytes target = tag.encode('utf-8')
        _check(swmm_2d_coverage_set(_h(self), int(CellScope(scope)), target, cell,
                                   names.data(), <double*>values.data, len(items)))

    def remove_coverage(self, int index):
        """Remove an authored scope row; subsequent indices shift."""
        _check(swmm_2d_coverage_remove(_h(self), index))

    @property
    def loadings(self):
        """Initial buildup snapshots, in project mass per acre/hectare."""
        cdef SWMM_Engine h = _h(self)
        cdef int count=0, index, scope=0, cell=0
        cdef double value=0
        cdef char tag[512]
        cdef char species[512]
        _check(swmm_2d_loading_count(h, &count))
        rows = []
        for index in range(count):
            _check(swmm_2d_loading_get(h, index, &scope, tag, 512, &cell, species, 512, &value))
            rows.append(SurfaceLoading(CellScope(scope), tag.decode('utf-8'), cell,
                                        species.decode('utf-8'), value))
        return tuple(rows)

    def set_loading(self, scope, str species, double value, *, str tag='', int cell=-1):
        """Upsert initial species buildup at GLOBAL, TAG or CELL scope."""
        cdef bytes name = species.encode('utf-8')
        cdef bytes target = tag.encode('utf-8')
        _check(swmm_2d_loading_set(_h(self), int(CellScope(scope)), target, cell, name, value))

    def remove_loading(self, int index):
        """Remove one initial-buildup row."""
        _check(swmm_2d_loading_remove(_h(self), index))

    @property
    def curb_lengths(self):
        """Authored curb-length snapshots in project length units."""
        cdef SWMM_Engine h = _h(self)
        cdef int count=0, index, scope=0, cell=0
        cdef double value=0
        cdef char tag[512]
        _check(swmm_2d_curb_length_count(h, &count))
        rows = []
        for index in range(count):
            _check(swmm_2d_curb_length_get(h, index, &scope, tag, 512, &cell, &value))
            rows.append(SurfaceCurbLength(CellScope(scope), tag.decode('utf-8'), cell, value))
        return tuple(rows)

    def set_curb_length(self, scope, double length, *, str tag='', int cell=-1):
        """Upsert a positive curb length at one scope, in project length units."""
        cdef bytes target = tag.encode('utf-8')
        _check(swmm_2d_curb_length_set(_h(self), int(CellScope(scope)), target, cell, length))

    def remove_curb_length(self, int index):
        """Remove an authored curb-length row."""
        _check(swmm_2d_curb_length_remove(_h(self), index))

    def buildup(self, str species):
        """Owned float64 per-cell buildup in project mass per acre/hectare.

        Requires an active resolved surface-quality store. The native engine
        reports LifecycleError before start or for unavailable configurations.
        """
        cdef SWMM_Engine h = _h(self)
        cdef int count=0, rc
        cdef bytes name = species.encode('utf-8')
        cdef const char* p = name
        _check(swmm_2d_cell_count(h, &count))
        cdef np.ndarray[double, ndim=1] values = np.empty(count, dtype=np.float64)
        with self._owner._operation(<size_t>h):
            with nogil:
                rc = swmm_2d_get_buildup_bulk(h, p, <double*>values.data, count)
        _check(rc)
        return values
