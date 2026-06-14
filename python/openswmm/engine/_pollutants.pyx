"""
Pollutant access (Pythonic v1 surface)
======================================

:author: Caleb Buahin
:copyright: Copyright (c) 2026 Caleb Buahin
:license: MIT

The :class:`Pollutants` collection and :class:`Pollutant` wrapper
mirror the shape of :doc:`nodes` / :doc:`links`.

.. code-block:: python

    from openswmm.engine import Solver, ConcentrationUnits

    with Solver("model.inp") as s:
        tss = s.pollutants["TSS"]
        print(tss.units, tss.kdecay, tss.init_conc)
        tss.kdecay = 0.05

        # Inject runtime concentration at a node.
        s.pollutants.set_node_quality("J1", "TSS", 12.0)
"""

# cython: language_level=3

from ._common cimport *
from ._enums import ConcentrationUnits
from ._exceptions import ElementNotFoundError, StaleObjectError


cdef inline SWMM_Engine _h(solver):
    return <SWMM_Engine><size_t>solver.handle


cdef inline int _resolve_pollutant(solver, key) except -1:
    return _resolve_index(
        _h(solver), key, swmm_pollutant_index, swmm_pollutant_count, "Pollutant")


cdef inline int _resolve_node(solver, key) except -1:
    return _resolve_index(
        _h(solver), key, swmm_node_index, swmm_node_count, "Node")


cdef inline int _resolve_link(solver, key) except -1:
    return _resolve_index(
        _h(solver), key, swmm_link_index, swmm_link_count, "Link")


cdef inline void _check_fresh(p) except *:
    if p._gen != p._solver.generation:
        raise StaleObjectError(
            f"Pollutant wrapper (id={p._captured_id!r}, index={p._index}) is stale; "
            "look it up again from solver.pollutants.")


# =============================================================================
# Pollutant wrapper
# =============================================================================

cdef class Pollutant:
    """A single pollutant."""

    cdef readonly object _solver
    cdef readonly int _index
    cdef readonly long long _gen
    cdef readonly str _captured_id

    def __init__(self, solver, int index):
        self._solver = solver
        self._index = index
        self._gen = solver.generation
        cdef const char* raw = swmm_pollutant_id(_h(solver), index)
        self._captured_id = raw.decode('utf-8') if raw != NULL else ""

    # ---- Identity ---------------------------------------------------

    @property
    def id(self) -> str:
        _check_fresh(self)
        cdef const char* raw = swmm_pollutant_id(_h(self._solver), self._index)
        return raw.decode('utf-8') if raw != NULL else ""

    @property
    def index(self) -> int:
        _check_fresh(self)
        return self._index

    @property
    def solver(self):
        return self._solver

    @property
    def units(self):
        _check_fresh(self)
        cdef int v = 0
        _check(swmm_pollutant_get_units(_h(self._solver), self._index, &v))
        return ConcentrationUnits(v)

    # ---- Decay / fate properties -----------------------------------

    @property
    def kdecay(self) -> float:
        _check_fresh(self)
        cdef double v = 0.0
        _check(swmm_pollutant_get_kdecay(_h(self._solver), self._index, &v))
        return v

    @kdecay.setter
    def kdecay(self, double value) -> None:
        _check_fresh(self)
        _check(swmm_pollutant_set_kdecay(_h(self._solver), self._index, value))

    @property
    def mwt(self) -> float:
        _check_fresh(self)
        cdef double v = 0.0
        _check(swmm_pollutant_get_mwt(_h(self._solver), self._index, &v))
        return v

    @mwt.setter
    def mwt(self, double value) -> None:
        _check_fresh(self)
        _check(swmm_pollutant_set_mwt(_h(self._solver), self._index, value))

    # ---- Inflow concentrations -------------------------------------

    @property
    def rain_conc(self) -> float:
        _check_fresh(self)
        cdef double v = 0.0
        _check(swmm_pollutant_get_rain_conc(_h(self._solver), self._index, &v))
        return v

    @rain_conc.setter
    def rain_conc(self, double value) -> None:
        _check_fresh(self)
        _check(swmm_pollutant_set_rain_conc(_h(self._solver), self._index, value))

    @property
    def gw_conc(self) -> float:
        _check_fresh(self)
        cdef double v = 0.0
        _check(swmm_pollutant_get_gw_conc(_h(self._solver), self._index, &v))
        return v

    @gw_conc.setter
    def gw_conc(self, double value) -> None:
        _check_fresh(self)
        _check(swmm_pollutant_set_gw_conc(_h(self._solver), self._index, value))

    @property
    def init_conc(self) -> float:
        _check_fresh(self)
        cdef double v = 0.0
        _check(swmm_pollutant_get_init_conc(_h(self._solver), self._index, &v))
        return v

    @init_conc.setter
    def init_conc(self, double value) -> None:
        _check_fresh(self)
        _check(swmm_pollutant_set_init_conc(_h(self._solver), self._index, value))

    @property
    def rdii_conc(self) -> float:
        _check_fresh(self)
        cdef double v = 0.0
        _check(swmm_pollutant_get_rdii_conc(_h(self._solver), self._index, &v))
        return v

    @rdii_conc.setter
    def rdii_conc(self, double value) -> None:
        _check_fresh(self)
        _check(swmm_pollutant_set_rdii_conc(_h(self._solver), self._index, value))

    @property
    def dwf_conc(self) -> float:
        """Dry-weather-flow concentration of this pollutant (pollutant units).

        Mirrors L{rdii_conc}: the concentration applied to dry-weather
        sanitary inflows. Settable while the simulation is running so
        diurnal/seasonal sanitary quality can be prescribed mid-run.
        """
        _check_fresh(self)
        cdef double v = 0.0
        _check(swmm_pollutant_get_dwf_conc(_h(self._solver), self._index, &v))
        return v

    @dwf_conc.setter
    def dwf_conc(self, double value) -> None:
        _check_fresh(self)
        _check(swmm_pollutant_set_dwf_conc(_h(self._solver), self._index, value))

    # ---- Snow-only flag --------------------------------------------

    @property
    def snow_only(self) -> bool:
        _check_fresh(self)
        cdef int v = 0
        _check(swmm_pollutant_get_snow_only(_h(self._solver), self._index, &v))
        return v != 0

    @snow_only.setter
    def snow_only(self, bint value) -> None:
        _check_fresh(self)
        _check(swmm_pollutant_set_snow_only(
            _h(self._solver), self._index, 1 if value else 0))

    # ---- Co-pollutant link -----------------------------------------

    @property
    def co_pollutant(self):
        """``(Pollutant, fraction)`` or ``None`` if no co-pollutant."""
        _check_fresh(self)
        cdef int co_idx = -1
        cdef double frac = 0.0
        _check(swmm_pollutant_get_co_pollutant(
            _h(self._solver), self._index, &co_idx, &frac))
        if co_idx < 0:
            return None
        return (Pollutant(self._solver, co_idx), frac)

    def set_co_pollutant(self, co, double fraction) -> None:
        """Link this pollutant to ``co`` (id, index, or Pollutant) with
        the given ``fraction``."""
        _check_fresh(self)
        cdef int co_idx
        if isinstance(co, Pollutant):
            co_idx = co._index
        else:
            co_idx = _resolve_pollutant(self._solver, co)
        _check(swmm_pollutant_set_co_pollutant(
            _h(self._solver), self._index, co_idx, fraction))

    # ---- Equality / repr ------------------------------------------

    def __eq__(self, other):
        if not isinstance(other, Pollutant):
            return NotImplemented
        return (self._solver is other._solver
                and self._index == other._index)

    def __hash__(self):
        return hash((id(self._solver), self._index))

    def __repr__(self) -> str:
        try:
            return f"<Pollutant id={self._captured_id!r} index={self._index}>"
        except Exception:
            return f"<Pollutant index={self._index} (stale or closed)>"


# =============================================================================
# Pollutants collection
# =============================================================================

cdef class Pollutants:
    """Indexable, iterable collection of :class:`Pollutant` wrappers."""

    cdef object _solver

    def __init__(self, solver):
        self._solver = solver

    # ---- Container protocol ----------------------------------------

    def __len__(self) -> int:
        return swmm_pollutant_count(_h(self._solver))

    def __iter__(self):
        cdef int n = swmm_pollutant_count(_h(self._solver))
        for i in range(n):
            yield Pollutant(self._solver, i)

    def __getitem__(self, key) -> Pollutant:
        cdef int i = _resolve_pollutant(self._solver, key)
        return Pollutant(self._solver, i)

    def __contains__(self, key) -> bool:
        try:
            _resolve_pollutant(self._solver, key)
            return True
        except (KeyError, IndexError, TypeError):
            return False

    # ---- Identity lookups -----------------------------------------

    def get_index(self, str pollut_id) -> int:
        cdef bytes b = pollut_id.encode('utf-8')
        cdef int i = swmm_pollutant_index(_h(self._solver), b)
        if i < 0:
            raise ElementNotFoundError(pollut_id)
        return i

    def get_id(self, int idx) -> str:
        if not (0 <= idx < len(self)):
            raise IndexError(idx)
        cdef const char* raw = swmm_pollutant_id(_h(self._solver), idx)
        return raw.decode('utf-8') if raw != NULL else ""

    # ---- Editing --------------------------------------------------

    def add(self, str pollut_id, units=ConcentrationUnits.MG_PER_L) -> Pollutant:
        cdef bytes b = pollut_id.encode('utf-8')
        _check(swmm_pollutant_add(_h(self._solver), b, int(units)))
        self._solver._bump_generation()
        cdef int new_idx = swmm_pollutant_index(_h(self._solver), b)
        return Pollutant(self._solver, new_idx)

    # ---- Runtime quality injection (node + link) -----------------

    def set_node_quality(self, node, pollutant, double conc) -> None:
        """Set runtime concentration at ``node`` for ``pollutant``."""
        cdef int ni = _resolve_node(self._solver, node)
        cdef int pi = _resolve_pollutant(self._solver, pollutant)
        _check(swmm_node_set_quality(_h(self._solver), ni, pi, conc))

    def set_link_quality(self, link, pollutant, double conc) -> None:
        """Set runtime concentration at ``link`` for ``pollutant``."""
        cdef int li = _resolve_link(self._solver, link)
        cdef int pi = _resolve_pollutant(self._solver, pollutant)
        _check(swmm_link_set_quality(_h(self._solver), li, pi, conc))

    def __repr__(self) -> str:
        try:
            return f"<Pollutants n={len(self)}>"
        except Exception:
            return "<Pollutants (engine closed)>"
