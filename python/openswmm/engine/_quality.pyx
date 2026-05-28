"""
Water quality (Pythonic v1 surface)
===================================

:author: Caleb Buahin
:copyright: Copyright (c) 2026 Caleb Buahin
:license: MIT

``solver.quality`` exposes landuse, buildup, washoff, and treatment
configuration with enum-typed function selectors and ``int | str``
object keys.

.. code-block:: python

    from openswmm.engine import Solver, BuildupFunc, WashoffFunc

    with Solver("model.inp") as s:
        s.quality.landuses.add("RESIDENTIAL")
        s.quality.set_buildup(
            "RESIDENTIAL", "TSS",
            func=BuildupFunc.POW, c1=10.0, c2=0.1, c3=2.0, normalizer=0)
        info = s.quality.get_buildup("RESIDENTIAL", "TSS")
        print(info["func"], info["c1"])
"""

# cython: language_level=3

from collections.abc import Iterator
from typing import Dict, NamedTuple, Optional, Union

from ._common cimport *
from ._enums import BuildupFunc, WashoffFunc


cdef inline SWMM_Engine _h(solver):
    return <SWMM_Engine><size_t>solver.handle


cdef inline int _resolve_landuse(solver, key) except -1:
    return _resolve_index(
        _h(solver), key, swmm_landuse_index, swmm_landuse_count, "Landuse")


cdef inline int _resolve_pollutant(solver, key) except -1:
    return _resolve_index(
        _h(solver), key, swmm_pollutant_index, swmm_pollutant_count, "Pollutant")


cdef inline int _resolve_node(solver, key) except -1:
    return _resolve_index(
        _h(solver), key, swmm_node_index, swmm_node_count, "Node")


# =============================================================================
# Landuse sub-collection
# =============================================================================

class Landuse:
    """One ``[LANDUSES]`` entry. Reach via ``solver.quality.landuses[key]``."""

    def __init__(self, solver, int index):
        self._solver = solver
        self._index = index

    @property
    def id(self) -> str:
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef const char* raw = swmm_landuse_id(h, self._index)
        return raw.decode('utf-8') if raw != NULL else ""

    @property
    def index(self) -> int:
        return self._index

    @property
    def sweep_interval(self) -> float:
        """Interval between street-sweeping events (days)."""
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_landuse_get_sweep_interval(h, self._index, &v))
        return v

    @sweep_interval.setter
    def sweep_interval(self, double value) -> None:
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_landuse_set_sweep_interval(h, self._index, value))

    @property
    def sweep_removal(self) -> float:
        """Sweeper removal fraction (0..1)."""
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_landuse_get_sweep_removal(h, self._index, &v))
        return v

    @sweep_removal.setter
    def sweep_removal(self, double value) -> None:
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_landuse_set_sweep_removal(h, self._index, value))

    def __repr__(self) -> str:
        try:
            return f"<Landuse id={self.id!r} index={self._index}>"
        except Exception:
            return f"<Landuse index={self._index} (closed)>"


class Landuses:
    """``solver.quality.landuses`` — collection of :class:`Landuse`."""

    def __init__(self, solver):
        self._solver = solver

    def __len__(self) -> int:
        return swmm_landuse_count(_h(self._solver))

    def __iter__(self) -> Iterator[Landuse]:
        cdef int n = swmm_landuse_count(_h(self._solver))
        for i in range(n):
            yield Landuse(self._solver, i)

    def __getitem__(self, key) -> Landuse:
        cdef int i = _resolve_landuse(self._solver, key)
        return Landuse(self._solver, i)

    def __contains__(self, key) -> bool:
        try:
            _resolve_landuse(self._solver, key)
            return True
        except (KeyError, IndexError, TypeError):
            return False

    def get_index(self, str landuse_id) -> int:
        cdef bytes b = landuse_id.encode('utf-8')
        cdef int i = swmm_landuse_index(_h(self._solver), b)
        if i < 0:
            raise KeyError(landuse_id)
        return i

    def get_id(self, int idx) -> str:
        if not (0 <= idx < len(self)):
            raise IndexError(idx)
        cdef const char* raw = swmm_landuse_id(_h(self._solver), idx)
        return raw.decode('utf-8') if raw != NULL else ""

    def add(self, str landuse_id) -> Landuse:
        cdef bytes b = landuse_id.encode('utf-8')
        _check(swmm_landuse_add(_h(self._solver), b))
        self._solver._bump_generation()
        cdef int idx = swmm_landuse_index(_h(self._solver), b)
        return Landuse(self._solver, idx)

    def __repr__(self) -> str:
        try:
            return f"<Landuses n={len(self)}>"
        except Exception:
            return "<Landuses (engine closed)>"


# =============================================================================
# Quality view
# =============================================================================

class Quality:
    """``solver.quality`` — landuse / buildup / washoff / treatment configuration."""

    def __init__(self, solver):
        self._solver = solver
        self._landuses = None

    @property
    def landuses(self) -> Landuses:
        if self._landuses is None:
            self._landuses = Landuses(self._solver)
        return self._landuses

    # ------------------------------------------------------------------
    # Buildup
    # ------------------------------------------------------------------

    def set_buildup(self, landuse, pollutant, *,
                    func, double c1, double c2, double c3,
                    int normalizer=0) -> None:
        """Configure buildup for ``(landuse, pollutant)``.

        :param func: :class:`BuildupFunc` enum.
        :param normalizer: 0 = per acre, 1 = per curb length.
        """
        cdef int lu = _resolve_landuse(self._solver, landuse)
        cdef int p = _resolve_pollutant(self._solver, pollutant)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_buildup_set(h, lu, p, int(func), c1, c2, c3, normalizer))

    def get_buildup(self, landuse, pollutant) -> Dict[str, object]:
        """Return ``{"func": BuildupFunc, "c1": ..., "c2": ..., "c3": ...,
        "normalizer": int}``."""
        cdef int lu = _resolve_landuse(self._solver, landuse)
        cdef int p = _resolve_pollutant(self._solver, pollutant)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int func = 0, normalizer = 0
        cdef double c1 = 0.0, c2 = 0.0, c3 = 0.0
        _check(swmm_buildup_get(h, lu, p, &func, &c1, &c2, &c3, &normalizer))
        return {
            "func": BuildupFunc(func),
            "c1": c1, "c2": c2, "c3": c3,
            "normalizer": normalizer,
        }

    # ------------------------------------------------------------------
    # Washoff
    # ------------------------------------------------------------------

    def set_washoff(self, landuse, pollutant, *,
                    func, double coeff, double expon,
                    double sweep_effic=0.0, double bmp_effic=0.0) -> None:
        cdef int lu = _resolve_landuse(self._solver, landuse)
        cdef int p = _resolve_pollutant(self._solver, pollutant)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_washoff_set(
            h, lu, p, int(func), coeff, expon, sweep_effic, bmp_effic))

    def get_washoff(self, landuse, pollutant) -> Dict[str, object]:
        cdef int lu = _resolve_landuse(self._solver, landuse)
        cdef int p = _resolve_pollutant(self._solver, pollutant)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int func = 0
        cdef double coeff = 0.0, expon = 0.0, sweep = 0.0, bmp = 0.0
        _check(swmm_washoff_get(
            h, lu, p, &func, &coeff, &expon, &sweep, &bmp))
        return {
            "func": WashoffFunc(func),
            "coeff": coeff, "expon": expon,
            "sweep_effic": sweep, "bmp_effic": bmp,
        }

    # ------------------------------------------------------------------
    # Treatment
    # ------------------------------------------------------------------

    def set_treatment(self, node, pollutant, str expression) -> None:
        cdef int ni = _resolve_node(self._solver, node)
        cdef int pi = _resolve_pollutant(self._solver, pollutant)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef bytes b = expression.encode('utf-8')
        _check(swmm_treatment_set(h, ni, pi, b))

    def get_treatment(self, node, pollutant) -> str:
        cdef int ni = _resolve_node(self._solver, node)
        cdef int pi = _resolve_pollutant(self._solver, pollutant)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef char buf[1024]
        _check(swmm_treatment_get(h, ni, pi, buf, 1024))
        return buf.decode('utf-8')

    def clear_treatment(self, node, pollutant) -> None:
        cdef int ni = _resolve_node(self._solver, node)
        cdef int pi = _resolve_pollutant(self._solver, pollutant)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_treatment_clear(h, ni, pi))

    def __repr__(self) -> str:
        try:
            return f"<Quality landuses={len(self.landuses)}>"
        except Exception:
            return "<Quality (engine closed)>"
