"""
Infrastructure (Pythonic v1 surface)
====================================

:author: Caleb Buahin
:copyright: Copyright (c) 2026 Caleb Buahin
:license: MIT

``solver.infrastructure`` exposes the four hydraulic-infrastructure
families: transects, streets, inlets, and LID controls/usage. The C
API supports ``add`` + ``count`` + per-row parameter setters but no
generic id→index resolver for these families, so the Python view stays
flat (``add_*`` / ``*_count``) rather than dressing it as a collection.

.. code-block:: python

    from openswmm.engine import Solver, LidType

    with Solver("model.inp") as s:
        s.infrastructure.transects.add("T1")
        s.infrastructure.transects.set_roughness(0, 0.05, 0.05, 0.03)
        s.infrastructure.transects.add_station(0, 0.0, 100.0)

        s.infrastructure.lids.add("BC1", LidType.BIO_CELL)
        s.infrastructure.lids.set_surface(0, storage=0.0, roughness=0.0, slope=0.5)
        s.infrastructure.lids.usage_add("S1", "BC1", number=1, area=100.0,
                                        width=10.0, init_sat=0.0, from_imperv=25.0)
"""

# cython: language_level=3

from ._common cimport *


cdef inline SWMM_Engine _h(solver):
    return <SWMM_Engine><size_t>solver.handle


cdef inline int _resolve_subcatch(solver, key) except -1:
    return _resolve_index(_h(solver), key,
                          swmm_subcatch_index, swmm_subcatch_count,
                          "Subcatchment")


# ---- Transects ------------------------------------------------------

class Transects:
    """``solver.infrastructure.transects`` view."""

    def __init__(self, solver):
        self._solver = solver

    def __len__(self) -> int:
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        return swmm_transect_count(h)

    def add(self, str transect_id) -> int:
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef bytes b = transect_id.encode('utf-8')
        _check(swmm_transect_add(h, b))
        self._solver._bump_generation()
        return len(self) - 1

    def set_roughness(self, int idx,
                      double n_left, double n_right, double n_channel) -> None:
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_transect_set_roughness(h, idx, n_left, n_right, n_channel))

    def add_station(self, int idx, double station, double elevation) -> None:
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_transect_add_station(h, idx, station, elevation))


# ---- Streets --------------------------------------------------------

class Streets:
    """``solver.infrastructure.streets`` view."""

    def __init__(self, solver):
        self._solver = solver

    def __len__(self) -> int:
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        return swmm_street_count(h)

    def add(self, str street_id) -> int:
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef bytes b = street_id.encode('utf-8')
        _check(swmm_street_add(h, b))
        self._solver._bump_generation()
        return len(self) - 1

    def set_params(self, int idx, *,
                   double t_crown, double h_curb, double sx, double n_road,
                   double gutter_depres=0.0, double gutter_width=0.0,
                   int sides=2,
                   double back_width=0.0, double back_slope=0.0,
                   double back_n=0.0) -> None:
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_street_set_params(
            h, idx, t_crown, h_curb, sx, n_road,
            gutter_depres, gutter_width, sides,
            back_width, back_slope, back_n))


# ---- Inlets ---------------------------------------------------------

class Inlets:
    """``solver.infrastructure.inlets`` view."""

    def __init__(self, solver):
        self._solver = solver

    def __len__(self) -> int:
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        return swmm_inlet_count(h)

    def add(self, str inlet_id, str inlet_type) -> int:
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef bytes b_id = inlet_id.encode('utf-8')
        cdef bytes b_type = inlet_type.encode('utf-8')
        _check(swmm_inlet_add(h, b_id, b_type))
        self._solver._bump_generation()
        return len(self) - 1

    def set_params(self, int idx, *,
                   double length=0.0, double width=0.0,
                   str grate_type="",
                   double open_area=0.0, double splash_veloc=0.0) -> None:
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef bytes b_grate = grate_type.encode('utf-8')
        _check(swmm_inlet_set_params(
            h, idx, length, width, b_grate, open_area, splash_veloc))


# ---- LID controls + usage ------------------------------------------

class LIDs:
    """``solver.infrastructure.lids`` view.

    Handles both the ``[LID_CONTROLS]`` family (the ``add`` /
    ``set_*`` methods) and the ``[LID_USAGE]`` placement records
    (``usage_add``).
    """

    def __init__(self, solver):
        self._solver = solver

    def __len__(self) -> int:
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        return swmm_lid_count(h)

    def add(self, str lid_id, lid_type) -> int:
        """``lid_type`` is a :class:`LidType` enum."""
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef bytes b = lid_id.encode('utf-8')
        _check(swmm_lid_add(h, b, int(lid_type)))
        self._solver._bump_generation()
        return len(self) - 1

    def set_surface(self, int idx, *,
                    double storage, double roughness, double slope) -> None:
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_lid_set_surface(h, idx, storage, roughness, slope))

    def set_soil(self, int idx, *,
                 double thick, double porosity, double fc,
                 double wp, double ksat, double kslope) -> None:
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_lid_set_soil(h, idx, thick, porosity, fc, wp, ksat, kslope))

    def set_storage(self, int idx, *,
                    double thick, double void_frac, double ksat) -> None:
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_lid_set_storage(h, idx, thick, void_frac, ksat))

    def set_drain(self, int idx, *,
                  double coeff, double expon, double offset) -> None:
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_lid_set_drain(h, idx, coeff, expon, offset))

    def usage_add(self, subcatchment, lid, *,
                  int number, double area, double width,
                  double init_sat=0.0, double from_imperv=0.0) -> None:
        """Place ``lid`` (id or index) on ``subcatchment`` (id or index)."""
        cdef int si = _resolve_subcatch(self._solver, subcatchment)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        # No id→index resolver for LID controls; accept int or look up by
        # scanning isn't supported by the C API. Hard-require int here.
        if isinstance(lid, str):
            raise TypeError(
                "LID lookup by id is not supported by the C API; pass the "
                "integer index returned from .add(id, type) instead.")
        cdef int li = int(lid)
        _check(swmm_lid_usage_add(
            h, si, li, number, area, width, init_sat, from_imperv))


# ---- Top-level Infrastructure view ----------------------------------

class Infrastructure:
    """``solver.infrastructure`` — entry point for the four sub-views."""

    def __init__(self, solver):
        self._solver = solver
        self._transects = None
        self._streets = None
        self._inlets = None
        self._lids = None

    @property
    def transects(self) -> Transects:
        if self._transects is None:
            self._transects = Transects(self._solver)
        return self._transects

    @property
    def streets(self) -> Streets:
        if self._streets is None:
            self._streets = Streets(self._solver)
        return self._streets

    @property
    def inlets(self) -> Inlets:
        if self._inlets is None:
            self._inlets = Inlets(self._solver)
        return self._inlets

    @property
    def lids(self) -> LIDs:
        if self._lids is None:
            self._lids = LIDs(self._solver)
        return self._lids

    def __repr__(self) -> str:
        try:
            return (f"<Infrastructure transects={len(self.transects)} "
                    f"streets={len(self.streets)} inlets={len(self.inlets)} "
                    f"lids={len(self.lids)}>")
        except Exception:
            return "<Infrastructure (engine closed)>"
