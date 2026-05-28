"""
Runtime forcing (Pythonic v1 surface)
=====================================

:author: Caleb Buahin
:copyright: Copyright (c) 2026 Caleb Buahin
:license: MIT

The :class:`Forcing` view, reached via ``solver.forcing``, applies
runtime overrides to node/link/subcatchment/gage state.

.. code-block:: python

    from openswmm.engine import Solver, ForcingMode, ForcingTarget

    with Solver("model.inp") as s:
        # One-shot: replaces the engine-computed value for the next step.
        s.forcing.node_lat_inflow("J1", 0.5)

        # Sticky: persists every step until cleared.
        s.forcing.node_lat_inflow(
            "J1", 0.5, mode=ForcingMode.REPLACE, persist=True)

        # Run.
        for _ in s.steps():
            pass

        # Clear.
        s.forcing.clear(ForcingTarget.NODE, "J1")
        s.forcing.clear_all()
"""

# cython: language_level=3

from ._common cimport *
from ._enums import ForcingMode, ForcingTarget


cdef inline SWMM_Engine _h(solver):
    return <SWMM_Engine><size_t>solver.handle


cdef inline int _resolve_node(solver, key) except -1:
    return _resolve_index(
        _h(solver), key, swmm_node_index, swmm_node_count, "Node")


cdef inline int _resolve_link(solver, key) except -1:
    return _resolve_index(
        _h(solver), key, swmm_link_index, swmm_link_count, "Link")


cdef inline int _resolve_subcatch(solver, key) except -1:
    return _resolve_index(
        _h(solver), key, swmm_subcatch_index, swmm_subcatch_count,
        "Subcatchment")


cdef inline int _resolve_gage(solver, key) except -1:
    return _resolve_index(
        _h(solver), key, swmm_gage_index, swmm_gage_count, "Gage")


cdef inline int _resolve_pollutant(solver, key) except -1:
    return _resolve_index(
        _h(solver), key, swmm_pollutant_index, swmm_pollutant_count, "Pollutant")


class Forcing:
    """Runtime forcing accessor exposed as ``solver.forcing``.

    Every entry point takes:

    * The object as ``int | str`` (or a wrapper — its index is used).
    * ``mode`` defaulting to :attr:`ForcingMode.REPLACE`; can be
      :attr:`ForcingMode.ADD` for additive forcing.
    * ``persist`` defaulting to ``False`` (one-shot for the next step).

    All methods raise :class:`EngineError` on C API failure.
    """

    def __init__(self, solver):
        self._solver = solver

    # ---- Node forcing ---------------------------------------------

    def node_lat_inflow(self, node, double value, *,
                        mode=ForcingMode.REPLACE, bint persist=False) -> None:
        cdef int i = _resolve_node(self._solver, node)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_forcing_node_lat_inflow(
            h, i, value, int(mode), 1 if persist else 0))

    def node_head_boundary(self, node, double value, *,
                           mode=ForcingMode.REPLACE, bint persist=False) -> None:
        cdef int i = _resolve_node(self._solver, node)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_forcing_node_head_boundary(
            h, i, value, int(mode), 1 if persist else 0))

    def node_quality(self, node, pollutant, double mass_rate, *,
                     mode=ForcingMode.REPLACE, bint persist=False) -> None:
        cdef int ni = _resolve_node(self._solver, node)
        cdef int pi = _resolve_pollutant(self._solver, pollutant)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_forcing_node_quality(
            h, ni, pi, mass_rate, int(mode), 1 if persist else 0))

    # ---- Link forcing ---------------------------------------------

    def link_flow(self, link, double value, *,
                  mode=ForcingMode.REPLACE, bint persist=False) -> None:
        cdef int i = _resolve_link(self._solver, link)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_forcing_link_flow(
            h, i, value, int(mode), 1 if persist else 0))

    def link_setting(self, link, double value, *,
                     mode=ForcingMode.REPLACE, bint persist=False) -> None:
        cdef int i = _resolve_link(self._solver, link)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_forcing_link_setting(
            h, i, value, int(mode), 1 if persist else 0))

    # ---- Subcatchment forcing -------------------------------------

    def subcatchment_rainfall(self, sub, double value, *,
                              mode=ForcingMode.REPLACE, bint persist=False) -> None:
        cdef int i = _resolve_subcatch(self._solver, sub)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_forcing_subcatch_rainfall(
            h, i, value, int(mode), 1 if persist else 0))

    def subcatchment_evap(self, sub, double value, *,
                          mode=ForcingMode.REPLACE, bint persist=False) -> None:
        cdef int i = _resolve_subcatch(self._solver, sub)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_forcing_subcatch_evap(
            h, i, value, int(mode), 1 if persist else 0))

    # ---- Gage forcing ---------------------------------------------

    def gage_rainfall(self, gage, double value, *,
                      mode=ForcingMode.REPLACE, bint persist=False) -> None:
        cdef int i = _resolve_gage(self._solver, gage)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_forcing_gage_rainfall(
            h, i, value, int(mode), 1 if persist else 0))

    # ---- Clearing -------------------------------------------------

    def clear(self, target, key) -> None:
        """Clear forcing for one object.

        :param target: :class:`ForcingTarget` (NODE/LINK/SUBCATCH/GAGE).
        :param key: object id or index, resolved against the matching domain.
        """
        cdef int t = int(target)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int i
        if t == int(ForcingTarget.NODE):
            i = _resolve_node(self._solver, key)
        elif t == int(ForcingTarget.LINK):
            i = _resolve_link(self._solver, key)
        elif t == int(ForcingTarget.SUBCATCH):
            i = _resolve_subcatch(self._solver, key)
        elif t == int(ForcingTarget.GAGE):
            i = _resolve_gage(self._solver, key)
        else:
            raise ValueError(f"unknown ForcingTarget {target!r}")
        _check(swmm_forcing_clear(h, t, i))

    def clear_all(self) -> None:
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_forcing_clear_all(h))

    def __repr__(self) -> str:
        return f"<Forcing for {self._solver!r}>"
