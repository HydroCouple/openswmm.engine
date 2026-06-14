"""
Subcatchment access (Pythonic v1 surface)
=========================================

:author: Caleb Buahin
:copyright: Copyright (c) 2026 Caleb Buahin
:license: MIT

The :class:`Subcatchments` collection and :class:`Subcatchment` wrapper
follow the same shape as :mod:`openswmm.engine._nodes`. Each wrapper
exposes a property surface for geometry, runtime state, infiltration
parameters, coverage, and statistics.

.. code-block:: python

    from openswmm.engine import Solver, InfilModel

    with Solver("model.inp") as s:
        s1 = s.subcatchments["S1"]
        print(s1.area, s1.imperv_pct, s1.slope)

        # Infiltration as a tagged-union view.
        print(s1.infiltration.model)            # InfilModel enum
        s1.infiltration.set_horton(3.0, 0.5, 4.0, 7.0)

        # Coverage as a MutableMapping over landuse ids.
        s1.coverage["RESIDENTIAL"] = 0.6

        # Runtime state + bulk numpy.
        print(s1.runoff, s1.rainfall, s1.evap)
        depths = s.subcatchments.runoffs
"""

# cython: language_level=3

import numpy as np
cimport numpy as np

from collections.abc import MutableMapping

from ._common cimport *
from ._enums import InfilModel, AquiferParam
from ._exceptions import ElementNotFoundError, StaleObjectError


# =============================================================================
# Helpers
# =============================================================================

cdef inline SWMM_Engine _h(solver):
    return <SWMM_Engine><size_t>solver.handle


cdef inline int _resolve_subcatch(solver, object key) except -1:
    return _resolve_index(
        _h(solver), key, swmm_subcatch_index, swmm_subcatch_count, "Subcatchment")


cdef inline void _check_fresh(sub) except *:
    if sub._gen != sub._solver.generation:
        raise StaleObjectError(
            f"Subcatchment wrapper (id={sub._captured_id!r}, index={sub._index}) "
            "is stale; look it up again from solver.subcatchments."
        )


cdef int _resolve_pollutant(solver, key) except -1:
    return _resolve_index(
        _h(solver), key, swmm_pollutant_index, swmm_pollutant_count, "Pollutant")


cdef int _resolve_landuse(solver, key) except -1:
    return _resolve_index(
        _h(solver), key, swmm_landuse_index, swmm_landuse_count, "Landuse")


# =============================================================================
# Sub-views
# =============================================================================

cdef class SubcatchmentStatsView:
    """Per-subcatchment cumulative statistics."""
    cdef object _sub

    def __init__(self, sub):
        self._sub = sub

    @property
    def precip(self) -> float:
        _check_fresh(self._sub)
        cdef double v = 0.0
        _check(swmm_subcatch_get_stat_precip(
            _h(self._sub._solver), self._sub._index, &v))
        return v

    @property
    def runoff_vol(self) -> float:
        _check_fresh(self._sub)
        cdef double v = 0.0
        _check(swmm_subcatch_get_stat_runoff_vol(
            _h(self._sub._solver), self._sub._index, &v))
        return v

    @property
    def max_runoff(self) -> float:
        _check_fresh(self._sub)
        cdef double v = 0.0
        _check(swmm_subcatch_get_stat_max_runoff(
            _h(self._sub._solver), self._sub._index, &v))
        return v

    def __repr__(self) -> str:
        return f"<SubcatchmentStatsView for {self._sub!r}>"


cdef class InfiltrationView:
    """Tagged-union view of the per-subcatchment infiltration model.

    .. code-block:: python

        s1.infiltration.model            # InfilModel.HORTON / GREEN_AMPT / CURVE_NUMBER

        # Read whichever set of parameters matches the active model.
        if s1.infiltration.model in (InfilModel.HORTON, InfilModel.MOD_HORTON):
            f0, fmin, decay, dry_time = s1.infiltration.horton
        elif s1.infiltration.model in (InfilModel.GREEN_AMPT, InfilModel.MOD_GREEN_AMPT):
            suction, ksat, deficit = s1.infiltration.green_ampt
        else:
            cn = s1.infiltration.curve_number

        # Writers — each forces the model to the corresponding kind.
        s1.infiltration.set_horton(3.0, 0.5, 4.0, 7.0)
        s1.infiltration.set_green_ampt(3.5, 0.06, 0.26)
        s1.infiltration.set_curve_number(85.0)
    """
    cdef object _sub

    def __init__(self, sub):
        self._sub = sub

    @property
    def model(self):
        _check_fresh(self._sub)
        cdef int v = 0
        _check(swmm_subcatch_get_infil_model(
            _h(self._sub._solver), self._sub._index, &v))
        return InfilModel(v)

    @property
    def horton(self) -> tuple:
        """``(f0, fmin, decay, dry_time)`` — Horton parameters."""
        _check_fresh(self._sub)
        cdef double a = 0.0, b = 0.0, c = 0.0, d = 0.0
        _check(swmm_subcatch_get_infil_horton(
            _h(self._sub._solver), self._sub._index, &a, &b, &c, &d))
        return (a, b, c, d)

    def set_horton(self,
                   double f0, double fmin,
                   double decay, double dry_time) -> None:
        _check_fresh(self._sub)
        _check(swmm_subcatch_set_infil_horton(
            _h(self._sub._solver), self._sub._index, f0, fmin, decay, dry_time))

    @property
    def green_ampt(self) -> tuple:
        """``(suction, conductivity, initial_deficit)`` Green-Ampt parameters."""
        _check_fresh(self._sub)
        cdef double s = 0.0, k = 0.0, d = 0.0
        _check(swmm_subcatch_get_infil_green_ampt(
            _h(self._sub._solver), self._sub._index, &s, &k, &d))
        return (s, k, d)

    def set_green_ampt(self,
                       double suction, double conductivity,
                       double initial_deficit) -> None:
        _check_fresh(self._sub)
        _check(swmm_subcatch_set_infil_green_ampt(
            _h(self._sub._solver), self._sub._index,
            suction, conductivity, initial_deficit))

    @property
    def curve_number(self) -> float:
        _check_fresh(self._sub)
        cdef double v = 0.0
        _check(swmm_subcatch_get_infil_curve_number(
            _h(self._sub._solver), self._sub._index, &v))
        return v

    def set_curve_number(self, double cn) -> None:
        _check_fresh(self._sub)
        _check(swmm_subcatch_set_infil_curve_number(
            _h(self._sub._solver), self._sub._index, cn))

    def __repr__(self) -> str:
        try:
            return f"<InfiltrationView model={self.model.name}>"
        except Exception:
            return "<InfiltrationView (stale or closed)>"


class CoverageView(MutableMapping):
    """``subcatchment.coverage`` — landuse-id → fraction mapping.

    .. code-block:: python

        s1.coverage["RESIDENTIAL"] = 0.6
        s1.coverage["COMMERCIAL"] = 0.4
        sum(s1.coverage.values())    # ≤ 1.0
    """

    def __init__(self, sub):
        self._sub = sub

    def __getitem__(self, key):
        _check_fresh(self._sub)
        cdef int lu = _resolve_landuse(self._sub._solver, key)
        cdef double v = 0.0
        _check(swmm_subcatch_get_coverage(
            _h(self._sub._solver), self._sub._index, lu, &v))
        return v

    def __setitem__(self, key, value):
        _check_fresh(self._sub)
        cdef int lu = _resolve_landuse(self._sub._solver, key)
        _check(swmm_subcatch_set_coverage(
            _h(self._sub._solver), self._sub._index, lu, float(value)))

    def __delitem__(self, key):
        raise TypeError(
            "coverage entries can't be deleted; set fraction to 0.0 instead")

    def __iter__(self):
        # Iterate landuses; yield ids that have nonzero coverage. This is the
        # honest semantics — the C side stores a dense per-landuse array.
        n = swmm_landuse_count(_h(self._sub._solver))
        for i in range(n):
            raw = swmm_landuse_id(_h(self._sub._solver), i)
            lid = raw.decode('utf-8') if raw != NULL else ""
            if not lid:
                continue
            if self[lid] != 0.0:
                yield lid

    def __len__(self) -> int:
        return sum(1 for _ in self)


# =============================================================================
# Subcatchment wrapper
# =============================================================================

cdef class Subcatchment:
    """A single subcatchment."""

    cdef readonly object _solver
    cdef readonly int _index
    cdef readonly long long _gen
    cdef readonly str _captured_id
    cdef object _stats
    cdef object _infiltration
    cdef object _coverage

    def __init__(self, solver, int index):
        self._solver = solver
        self._index = index
        self._gen = solver.generation
        cdef const char* raw = swmm_subcatch_id(_h(solver), index)
        self._captured_id = raw.decode('utf-8') if raw != NULL else ""
        self._stats = None
        self._infiltration = None
        self._coverage = None

    # ---- Identity ---------------------------------------------------

    @property
    def id(self) -> str:
        _check_fresh(self)
        cdef const char* raw = swmm_subcatch_id(_h(self._solver), self._index)
        return raw.decode('utf-8') if raw != NULL else ""

    @property
    def tag(self) -> str:
        """The subcatchment's free-form tag string (INP C{[TAGS]} section).

        Empty string when the subcatchment has no tag. Assigning C{None} or
        C{""} clears it. The tag is keyed by index and persists across
        L{rename}.

        @rtype: str
        """
        _check_fresh(self)
        cdef char buf[256]
        _check(swmm_subcatch_get_tag(_h(self._solver), self._index, buf, 256))
        return buf.decode('utf-8')

    @tag.setter
    def tag(self, value) -> None:
        _check_fresh(self)
        cdef bytes b = (value or "").encode('utf-8')
        _check(swmm_subcatch_set_tag(_h(self._solver), self._index, b))

    @property
    def index(self) -> int:
        _check_fresh(self)
        return self._index

    @property
    def solver(self):
        return self._solver

    # ---- Geometry / properties -------------------------------------

    @property
    def area(self) -> float:
        _check_fresh(self)
        cdef double v = 0.0
        _check(swmm_subcatch_get_area(_h(self._solver), self._index, &v))
        return v

    @area.setter
    def area(self, double value) -> None:
        _check_fresh(self)
        _check(swmm_subcatch_set_area(_h(self._solver), self._index, value))

    @property
    def width(self) -> float:
        _check_fresh(self)
        cdef double v = 0.0
        _check(swmm_subcatch_get_width(_h(self._solver), self._index, &v))
        return v

    @width.setter
    def width(self, double value) -> None:
        _check_fresh(self)
        _check(swmm_subcatch_set_width(_h(self._solver), self._index, value))

    @property
    def slope(self) -> float:
        _check_fresh(self)
        cdef double v = 0.0
        _check(swmm_subcatch_get_slope(_h(self._solver), self._index, &v))
        return v

    @slope.setter
    def slope(self, double value) -> None:
        _check_fresh(self)
        _check(swmm_subcatch_set_slope(_h(self._solver), self._index, value))

    @property
    def imperv_pct(self) -> float:
        _check_fresh(self)
        cdef double v = 0.0
        _check(swmm_subcatch_get_imperv_pct(_h(self._solver), self._index, &v))
        return v

    @imperv_pct.setter
    def imperv_pct(self, double value) -> None:
        _check_fresh(self)
        _check(swmm_subcatch_set_imperv_pct(_h(self._solver), self._index, value))

    @property
    def n_imperv(self) -> float:
        _check_fresh(self)
        cdef double v = 0.0
        _check(swmm_subcatch_get_n_imperv(_h(self._solver), self._index, &v))
        return v

    @n_imperv.setter
    def n_imperv(self, double value) -> None:
        _check_fresh(self)
        _check(swmm_subcatch_set_n_imperv(_h(self._solver), self._index, value))

    @property
    def n_perv(self) -> float:
        _check_fresh(self)
        cdef double v = 0.0
        _check(swmm_subcatch_get_n_perv(_h(self._solver), self._index, &v))
        return v

    @n_perv.setter
    def n_perv(self, double value) -> None:
        _check_fresh(self)
        _check(swmm_subcatch_set_n_perv(_h(self._solver), self._index, value))

    @property
    def ds_imperv(self) -> float:
        _check_fresh(self)
        cdef double v = 0.0
        _check(swmm_subcatch_get_ds_imperv(_h(self._solver), self._index, &v))
        return v

    @ds_imperv.setter
    def ds_imperv(self, double value) -> None:
        _check_fresh(self)
        _check(swmm_subcatch_set_ds_imperv(_h(self._solver), self._index, value))

    @property
    def ds_perv(self) -> float:
        _check_fresh(self)
        cdef double v = 0.0
        _check(swmm_subcatch_get_ds_perv(_h(self._solver), self._index, &v))
        return v

    @ds_perv.setter
    def ds_perv(self, double value) -> None:
        _check_fresh(self)
        _check(swmm_subcatch_set_ds_perv(_h(self._solver), self._index, value))

    # ---- Topology --------------------------------------------------

    @property
    def gage(self):
        """The :class:`Gage` wrapper assigned to this subcatchment."""
        _check_fresh(self)
        cdef int v = 0
        _check(swmm_subcatch_get_gage(_h(self._solver), self._index, &v))
        from ._gages import Gage
        return Gage(self._solver, v)

    @gage.setter
    def gage(self, value) -> None:
        _check_fresh(self)
        from ._gages import Gage
        cdef int gi
        if isinstance(value, Gage):
            gi = value._index
        else:
            gi = _resolve_index(
                _h(self._solver), value,
                swmm_gage_index, swmm_gage_count, "Gage")
        _check(swmm_subcatch_set_gage(_h(self._solver), self._index, gi))

    @property
    def outlet(self):
        """The downstream :class:`Node` (or :class:`Subcatchment` if the
        outlet is another subcatchment).

        Returns whichever assignment is currently active; checks the
        node outlet first, then falls back to the subcatchment outlet.
        """
        _check_fresh(self)
        cdef int v = 0
        cdef int rc = swmm_subcatch_get_outlet(_h(self._solver), self._index, &v)
        if rc == 0 and v >= 0:
            from ._nodes import Node
            return Node(self._solver, v)
        # Try outlet-subcatchment.
        _check(swmm_subcatch_get_outlet_subcatch(
            _h(self._solver), self._index, &v))
        if v >= 0:
            return Subcatchment(self._solver, v)
        return None

    def set_outlet_node(self, node) -> None:
        """Route runoff to ``node`` (a :class:`Node` wrapper, index, or id)."""
        _check_fresh(self)
        from ._nodes import Node
        cdef int ni
        if isinstance(node, Node):
            ni = node._index
        else:
            ni = _resolve_index(
                _h(self._solver), node,
                swmm_node_index, swmm_node_count, "Node")
        _check(swmm_subcatch_set_outlet(_h(self._solver), self._index, ni))

    def set_outlet_subcatchment(self, sub) -> None:
        """Route runoff to another :class:`Subcatchment`."""
        _check_fresh(self)
        cdef int si
        if isinstance(sub, Subcatchment):
            si = sub._index
        else:
            si = _resolve_subcatch(self._solver, sub)
        _check(swmm_subcatch_set_outlet_subcatch(
            _h(self._solver), self._index, si))

    # ---- Runtime state ---------------------------------------------

    @property
    def runoff(self) -> float:
        _check_fresh(self)
        cdef double v = 0.0
        _check(swmm_subcatch_get_runoff(_h(self._solver), self._index, &v))
        return v

    @property
    def groundwater(self) -> float:
        _check_fresh(self)
        cdef double v = 0.0
        _check(swmm_subcatch_get_groundwater(_h(self._solver), self._index, &v))
        return v

    @property
    def rainfall(self) -> float:
        _check_fresh(self)
        cdef double v = 0.0
        _check(swmm_subcatch_get_rainfall(_h(self._solver), self._index, &v))
        return v

    @rainfall.setter
    def rainfall(self, double value) -> None:
        _check_fresh(self)
        _check(swmm_subcatch_set_rainfall(_h(self._solver), self._index, value))

    @property
    def snow_depth(self) -> float:
        _check_fresh(self)
        cdef double v = 0.0
        _check(swmm_subcatch_get_snow_depth(_h(self._solver), self._index, &v))
        return v

    @property
    def evap(self) -> float:
        _check_fresh(self)
        cdef double v = 0.0
        _check(swmm_subcatch_get_evap(_h(self._solver), self._index, &v))
        return v

    @property
    def infil(self) -> float:
        _check_fresh(self)
        cdef double v = 0.0
        _check(swmm_subcatch_get_infil(_h(self._solver), self._index, &v))
        return v

    # ---- Quality ---------------------------------------------------

    def quality(self, pollutant) -> float:
        _check_fresh(self)
        cdef int p = _resolve_pollutant(self._solver, pollutant)
        cdef double v = 0.0
        _check(swmm_subcatch_get_quality(
            _h(self._solver), self._index, p, &v))
        return v

    def ponded_quality(self, pollutant) -> float:
        _check_fresh(self)
        cdef int p = _resolve_pollutant(self._solver, pollutant)
        cdef double v = 0.0
        _check(swmm_subcatch_get_ponded_quality(
            _h(self._solver), self._index, p, &v))
        return v

    def set_ponded_quality(self, pollutant, double mass) -> None:
        _check_fresh(self)
        cdef int p = _resolve_pollutant(self._solver, pollutant)
        _check(swmm_subcatch_set_ponded_quality(
            _h(self._solver), self._index, p, mass))

    # ---- State injection (data assimilation) -----------------------

    def set_gw_state(self, double theta=-1.0, double lower_depth=-1.0) -> None:
        """Inject the groundwater state on this subcatchment (RUNNING only).

        State injection for data assimilation / external coupling. The
        subcatchment must have groundwater. Mass-balance reports reflect the
        resulting storage discontinuity, mirroring hotstart loading.

        @param theta: Upper-zone moisture content (0..porosity); pass a
            negative value to leave it unchanged.
        @type theta: float
        @param lower_depth: Saturated-zone depth above the aquifer bottom in
            project length units (ft US, m SI); negative leaves it unchanged.
        @type lower_depth: float
        """
        _check_fresh(self)
        _check(swmm_subcatch_set_gw_state(
            _h(self._solver), self._index, theta, lower_depth))

    def get_gw_state(self) -> tuple:
        """Read the groundwater state on this subcatchment.

        @return: ``(theta, lower_depth)`` — upper-zone moisture content and
            saturated-zone depth (project length units).
        @rtype: tuple of float
        """
        _check_fresh(self)
        cdef double theta = 0.0
        cdef double lower_depth = 0.0
        _check(swmm_subcatch_get_gw_state(
            _h(self._solver), self._index, &theta, &lower_depth))
        return (theta, lower_depth)

    def set_snow_state(self, int surface, double swe=-1.0, double fw=-1.0,
                       double ati=-1000.0, double coldc=-1.0) -> None:
        """Inject the snow-pack state on one snow surface (RUNNING only).

        State injection for data assimilation (e.g. observed SWE). The
        subcatchment must have a snow pack.

        @param surface: Snow subarea: 0 plowable, 1 impervious, 2 pervious.
        @type surface: int
        @param swe: Snow water equivalent in project depth units (in US,
            mm SI); negative leaves it unchanged.
        @type swe: float
        @param fw: Free water in project depth units; negative leaves it
            unchanged.
        @type fw: float
        @param ati: Antecedent temperature index (deg F US, deg C SI). Pass
            ``<= -999`` to leave it unchanged (negative temperatures valid).
        @type ati: float
        @param coldc: Cold content in project depth units of melt equivalent;
            negative leaves it unchanged.
        @type coldc: float
        """
        _check_fresh(self)
        _check(swmm_subcatch_set_snow_state(
            _h(self._solver), self._index, surface, swe, fw, ati, coldc))

    def get_snow_state(self, int surface) -> tuple:
        """Read the snow-pack state on one snow surface.

        @param surface: Snow subarea: 0 plowable, 1 impervious, 2 pervious.
        @type surface: int
        @return: ``(swe, fw, ati, coldc)`` in project units (SWE/free
            water/cold content as depths, ATI as temperature).
        @rtype: tuple of float
        """
        _check_fresh(self)
        cdef double swe = 0.0
        cdef double fw = 0.0
        cdef double ati = 0.0
        cdef double coldc = 0.0
        _check(swmm_subcatch_get_snow_state(
            _h(self._solver), self._index, surface, &swe, &fw, &ati, &coldc))
        return (swe, fw, ati, coldc)

    # ---- Sub-views -------------------------------------------------

    @property
    def stats(self) -> SubcatchmentStatsView:
        if self._stats is None:
            self._stats = SubcatchmentStatsView(self)
        return self._stats

    @property
    def infiltration(self) -> InfiltrationView:
        if self._infiltration is None:
            self._infiltration = InfiltrationView(self)
        return self._infiltration

    @property
    def coverage(self) -> CoverageView:
        if self._coverage is None:
            self._coverage = CoverageView(self)
        return self._coverage

    # ---- Equality / repr ------------------------------------------

    def __eq__(self, other):
        if not isinstance(other, Subcatchment):
            return NotImplemented
        return (self._solver is other._solver
                and self._index == other._index)

    def __hash__(self):
        return hash((id(self._solver), self._index))

    def __repr__(self) -> str:
        try:
            return f"<Subcatchment id={self._captured_id!r} index={self._index}>"
        except Exception:
            return f"<Subcatchment index={self._index} (stale or closed)>"


# =============================================================================
# Subcatchments collection
# =============================================================================

cdef class Subcatchments:
    """Indexable, iterable collection of :class:`Subcatchment` wrappers."""

    cdef object _solver

    def __init__(self, solver):
        self._solver = solver

    # ---- Container protocol ----------------------------------------

    def __len__(self) -> int:
        return swmm_subcatch_count(_h(self._solver))

    def __iter__(self):
        cdef int n = swmm_subcatch_count(_h(self._solver))
        for i in range(n):
            yield Subcatchment(self._solver, i)

    def __getitem__(self, key) -> Subcatchment:
        cdef int i = _resolve_subcatch(self._solver, key)
        return Subcatchment(self._solver, i)

    def __contains__(self, key) -> bool:
        try:
            _resolve_subcatch(self._solver, key)
            return True
        except (KeyError, IndexError, TypeError):
            return False

    # ---- Identity lookups -----------------------------------------

    def get_index(self, str sub_id) -> int:
        cdef bytes b = sub_id.encode('utf-8')
        cdef int i = swmm_subcatch_index(_h(self._solver), b)
        if i < 0:
            raise ElementNotFoundError(sub_id)
        return i

    def get_id(self, int idx) -> str:
        if not (0 <= idx < len(self)):
            raise IndexError(idx)
        cdef const char* raw = swmm_subcatch_id(_h(self._solver), idx)
        return raw.decode('utf-8') if raw != NULL else ""

    # ---- Editing (bumps generation) -------------------------------

    def add(self, str sub_id) -> Subcatchment:
        cdef bytes b = sub_id.encode('utf-8')
        _check(swmm_subcatch_add(_h(self._solver), b))
        self._solver._bump_generation()
        cdef int new_idx = swmm_subcatch_index(_h(self._solver), b)
        return Subcatchment(self._solver, new_idx)

    def rename(self, key, str new_id) -> None:
        cdef int i = _resolve_subcatch(self._solver, key)
        cdef bytes b = new_id.encode('utf-8')
        _check(swmm_subcatch_rename(_h(self._solver), i, b))
        self._solver._bump_generation()

    # ---- Bulk numpy properties ------------------------------------

    @property
    def runoffs(self):
        cdef SWMM_Engine h = _h(self._solver)
        cdef int n = swmm_subcatch_count(h)
        cdef np.ndarray[double, ndim=1] buf = np.empty(n, dtype=np.float64)
        cdef int err
        with nogil:
            err = swmm_subcatch_get_runoff_bulk(h, <double*>buf.data, n)
        _check(err)
        return buf

    @property
    def rainfalls(self):
        cdef SWMM_Engine h = _h(self._solver)
        cdef int n = swmm_subcatch_count(h)
        cdef np.ndarray[double, ndim=1] buf = np.empty(n, dtype=np.float64)
        cdef int err
        with nogil:
            err = swmm_subcatch_get_rainfall_bulk(h, <double*>buf.data, n)
        _check(err)
        return buf

    @property
    def evaps(self):
        cdef SWMM_Engine h = _h(self._solver)
        cdef int n = swmm_subcatch_count(h)
        cdef np.ndarray[double, ndim=1] buf = np.empty(n, dtype=np.float64)
        cdef int err
        with nogil:
            err = swmm_subcatch_get_evap_bulk(h, <double*>buf.data, n)
        _check(err)
        return buf

    @property
    def infils(self):
        cdef SWMM_Engine h = _h(self._solver)
        cdef int n = swmm_subcatch_count(h)
        cdef np.ndarray[double, ndim=1] buf = np.empty(n, dtype=np.float64)
        cdef int err
        with nogil:
            err = swmm_subcatch_get_infil_bulk(h, <double*>buf.data, n)
        _check(err)
        return buf

    @property
    def snow_depths(self):
        cdef SWMM_Engine h = _h(self._solver)
        cdef int n = swmm_subcatch_count(h)
        cdef np.ndarray[double, ndim=1] buf = np.empty(n, dtype=np.float64)
        cdef int err
        with nogil:
            err = swmm_subcatch_get_snow_depth_bulk(h, <double*>buf.data, n)
        _check(err)
        return buf

    def qualities(self, pollutant):
        cdef SWMM_Engine h = _h(self._solver)
        cdef int n = swmm_subcatch_count(h)
        cdef int p = _resolve_pollutant(self._solver, pollutant)
        cdef np.ndarray[double, ndim=1] buf = np.empty(n, dtype=np.float64)
        cdef int err
        with nogil:
            err = swmm_subcatch_get_quality_bulk(h, p, <double*>buf.data, n)
        _check(err)
        return buf

    @property
    def ids(self):
        return np.asarray(self._ids_list(), dtype=object)

    def _ids_list(self, int stride=64):
        cdef SWMM_Engine h = _h(self._solver)
        cdef int n = swmm_subcatch_count(h)
        cdef np.ndarray[char, ndim=1, mode="c"] buf = np.zeros(
            n * stride, dtype=np.int8)
        cdef int err
        with nogil:
            err = swmm_subcatch_get_ids_bulk(h, <char*>buf.data, stride, n)
        _check(err)
        raw = bytes(buf)
        out = []
        for i in range(n):
            slot = raw[i * stride:(i + 1) * stride]
            nul = slot.find(b"\x00")
            if nul >= 0:
                slot = slot[:nul]
            out.append(slot.decode("utf-8"))
        return out

    def __repr__(self) -> str:
        try:
            return f"<Subcatchments n={len(self)}>"
        except Exception:
            return "<Subcatchments (engine closed)>"


# =============================================================================
# Aquifers and snowpacks (model-global named objects)
# =============================================================================

cdef class _NamedObjects:
    """Shared base for the simple name-keyed C{Aquifers} / C{Snowpacks}
    collections.

    Each entry is just a string id; the C API exposes only
    count/index/id/add for these objects, so the collection yields ids and
    supports membership + add. Subclasses bind the four C functions.
    """

    cdef object _solver

    def __init__(self, solver):
        self._solver = solver

    cdef int _count(self) except -1:
        raise NotImplementedError

    cdef int _index(self, bytes b) except? -2:
        raise NotImplementedError

    cdef const char* _id(self, int idx):
        raise NotImplementedError

    cdef int _add(self, bytes b) except -1:
        raise NotImplementedError

    def __len__(self) -> int:
        return self._count()

    def get_index(self, str obj_id) -> int:
        """Resolve the zero-based index of an object from its string id.

        @rtype: int
        @raise KeyError: If no object has that id.
        """
        cdef bytes b = obj_id.encode('utf-8')
        cdef int i = self._index(b)
        if i < 0:
            raise ElementNotFoundError(obj_id)
        return i

    def get_id(self, int idx) -> str:
        """Return the string id of the object at C{idx}.

        @rtype: str
        """
        cdef const char* raw = self._id(idx)
        return raw.decode('utf-8') if raw != NULL else ""

    def __iter__(self):
        cdef int n = self._count()
        for i in range(n):
            yield self.get_id(i)

    def __contains__(self, key) -> bool:
        if isinstance(key, str):
            try:
                self.get_index(key)
                return True
            except KeyError:
                return False
        return 0 <= int(key) < self._count()

    def add(self, str obj_id) -> int:
        """Append a new object and return its zero-based index.

        @param obj_id: Unique identifier for the new object.
        @rtype: int
        """
        cdef bytes b = obj_id.encode('utf-8')
        self._add(b)
        self._solver._bump_generation()
        return self._count() - 1


cdef class Aquifers(_NamedObjects):
    """C{solver.aquifers} — name-keyed collection of C{[AQUIFERS]} entries."""

    cdef int _count(self) except -1:
        return swmm_aquifer_count(_h(self._solver))

    cdef int _index(self, bytes b) except? -2:
        return swmm_aquifer_index(_h(self._solver), b)

    cdef const char* _id(self, int idx):
        return swmm_aquifer_id(_h(self._solver), idx)

    cdef int _add(self, bytes b) except -1:
        _check(swmm_aquifer_add(_h(self._solver), b))
        return 0

    def get_param(self, aquifer, param) -> float:
        """Get an aquifer parameter (input-file units).

        @param aquifer: Aquifer index or string id.
        @param param: An L{AquiferParam} code.
        @rtype: float
        """
        cdef int idx = aquifer if isinstance(aquifer, int) else self.get_index(aquifer)
        cdef double value = 0.0
        _check(swmm_aquifer_get_param(_h(self._solver), idx, int(param), &value))
        return value

    def set_param(self, aquifer, param, double value) -> None:
        """Set an aquifer parameter (input-file units).

        Flux-coefficient parameters (conductivity, slopes, evap/loss
        coefficients) take effect on the next step when set mid-run; the
        structural / initial-condition parameters are pre-start-only and raise
        L{LifecycleError} while the simulation is running.

        @param aquifer: Aquifer index or string id.
        @param param: An L{AquiferParam} code.
        @param value: New value in input-file units.
        """
        cdef int idx = aquifer if isinstance(aquifer, int) else self.get_index(aquifer)
        _check(swmm_aquifer_set_param(_h(self._solver), idx, int(param), value))

    def __repr__(self) -> str:
        try:
            return f"<Aquifers n={len(self)}>"
        except Exception:
            return "<Aquifers (engine closed)>"


cdef class Snowpacks(_NamedObjects):
    """C{solver.snowpacks} — name-keyed collection of C{[SNOWPACKS]} entries."""

    cdef int _count(self) except -1:
        return swmm_snowpack_count(_h(self._solver))

    cdef int _index(self, bytes b) except? -2:
        return swmm_snowpack_index(_h(self._solver), b)

    cdef const char* _id(self, int idx):
        return swmm_snowpack_id(_h(self._solver), idx)

    cdef int _add(self, bytes b) except -1:
        _check(swmm_snowpack_add(_h(self._solver), b))
        return 0

    def __repr__(self) -> str:
        try:
            return f"<Snowpacks n={len(self)}>"
        except Exception:
            return "<Snowpacks (engine closed)>"
