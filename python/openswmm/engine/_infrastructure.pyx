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

    # ---- Identity lookups -------------------------------------------

    def get_index(self, str transect_id) -> int:
        """Resolve a transect's zero-based index from its string id.

        @param transect_id: The transect's string identifier.
        @rtype: int
        @raise KeyError: If no transect has that id.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef bytes b = transect_id.encode('utf-8')
        cdef int i = swmm_transect_index(h, b)
        if i < 0:
            raise KeyError(transect_id)
        return i

    def get_id(self, int idx) -> str:
        """Return the string id of the transect at C{idx}.

        @rtype: str
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef const char* raw = swmm_transect_id(h, idx)
        return raw.decode('utf-8') if raw != NULL else ""

    def _resolve(self, key) -> int:
        if isinstance(key, str):
            return self.get_index(key)
        cdef int n = len(self)
        cdef int idx = key
        if idx < 0:
            idx += n
        if not 0 <= idx < n:
            raise IndexError(key)
        return idx

    def __iter__(self):
        cdef int n = len(self)
        for i in range(n):
            yield self.get_id(i)

    def __contains__(self, key) -> bool:
        try:
            self._resolve(key)
            return True
        except (KeyError, IndexError):
            return False

    def remove(self, key) -> None:
        """Remove a transect (by index or id), clearing reference sites.

        @param key: Integer index or string id.
        """
        cdef int idx = self._resolve(key)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_transect_remove(h, idx))
        self._solver._bump_generation()

    def rename(self, key, str new_id) -> None:
        """Rename a transect, updating stored references.

        @param key: Integer index or string id.
        @param new_id: New identifier.
        """
        cdef int idx = self._resolve(key)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef bytes b = new_id.encode('utf-8')
        _check(swmm_transect_rename(h, idx, b))
        self._solver._bump_generation()

    # ---- Station geometry -------------------------------------------

    def station_count(self, int idx) -> int:
        """Number of (station, elevation) points in the transect at C{idx}.

        @rtype: int
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        return swmm_transect_get_station_count(h, idx)

    def get_station(self, int idx, int station_idx):
        """Return the C{(station, elevation)} pair at C{station_idx}.

        @rtype: tuple[float, float]
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double station = 0.0, elevation = 0.0
        _check(swmm_transect_get_station(h, idx, station_idx, &station, &elevation))
        return (station, elevation)

    def stations(self, int idx):
        """Return all C{(station, elevation)} points as a list of tuples.

        @rtype: list[tuple[float, float]]
        """
        cdef int n = self.station_count(idx)
        cdef int i
        return [self.get_station(idx, i) for i in range(n)]

    def clear_stations(self, int idx) -> None:
        """Remove all station points from the transect at C{idx}."""
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_transect_clear_stations(h, idx))

    # ---- Roughness / banks / encroachment / modifiers ---------------

    def get_roughness(self, int idx):
        """Return Manning's n as C{(n_left, n_right, n_channel)}.

        @rtype: tuple[float, float, float]
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double nl = 0.0, nr = 0.0, nc = 0.0
        _check(swmm_transect_get_roughness(h, idx, &nl, &nr, &nc))
        return (nl, nr, nc)

    def get_bank_stations(self, int idx):
        """Return the C{(left, right)} bank stations.

        @rtype: tuple[float, float]
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double left = 0.0, right = 0.0
        _check(swmm_transect_get_bank_stations(h, idx, &left, &right))
        return (left, right)

    def set_bank_stations(self, int idx, double left, double right) -> None:
        """Set the left/right bank stations."""
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_transect_set_bank_stations(h, idx, left, right))

    def get_encroachment_stations(self, int idx):
        """Return the C{(left, right)} encroachment stations.

        @rtype: tuple[float, float]
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double left = 0.0, right = 0.0
        _check(swmm_transect_get_encroachment_stations(h, idx, &left, &right))
        return (left, right)

    def set_encroachment_stations(self, int idx, double left, double right) -> None:
        """Set the left/right encroachment stations."""
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_transect_set_encroachment_stations(h, idx, left, right))

    def get_modifiers(self, int idx):
        """Return the geometry modifiers as C{(n_factor, x_factor, y_factor)}.

        @rtype: tuple[float, float, float]
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double nf = 0.0, xf = 0.0, yf = 0.0
        _check(swmm_transect_get_modifiers(h, idx, &nf, &xf, &yf))
        return (nf, xf, yf)

    def set_modifiers(self, int idx, double n_factor,
                      double x_factor, double y_factor) -> None:
        """Set the geometry modifiers (Manning, station, elevation factors)."""
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_transect_set_modifiers(h, idx, n_factor, x_factor, y_factor))

    def get_comments(self, int idx) -> str:
        """Return the descriptive comment string for the transect at C{idx}.

        @rtype: str
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef char buf[1024]
        _check(swmm_transect_get_comments(h, idx, buf, 1024))
        return buf.decode('utf-8')

    def set_comments(self, int idx, str text) -> None:
        """Set the descriptive comment string for the transect at C{idx}."""
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef bytes b = text.encode('utf-8')
        _check(swmm_transect_set_comments(h, idx, b))


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

    def get_params(self, int idx) -> dict:
        """Read back a street cross-section's geometric parameters.

        Inverse of :meth:`set_params`. Values are returned in the same
        (display/project) units they were supplied.

        @param idx: Zero-based street index.
        @type idx: int
        @return: Mapping with keys ``t_crown``, ``h_curb``, ``sx``,
            ``n_road``, ``gutter_depres``, ``gutter_width``, ``sides``,
            ``back_width``, ``back_slope``, ``back_n``.
        @rtype: dict
        @raise EngineError: On C API failure.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double t_crown = 0.0, h_curb = 0.0, sx = 0.0, n_road = 0.0
        cdef double gutter_depres = 0.0, gutter_width = 0.0
        cdef int sides = 0
        cdef double back_width = 0.0, back_slope = 0.0, back_n = 0.0
        _check(swmm_street_get_params(
            h, idx, &t_crown, &h_curb, &sx, &n_road,
            &gutter_depres, &gutter_width, &sides,
            &back_width, &back_slope, &back_n))
        return {
            "t_crown": t_crown, "h_curb": h_curb, "sx": sx, "n_road": n_road,
            "gutter_depres": gutter_depres, "gutter_width": gutter_width,
            "sides": sides, "back_width": back_width,
            "back_slope": back_slope, "back_n": back_n,
        }

    def get_index(self, str street_id) -> int:
        """Resolve a street's zero-based index from its string id.

        @rtype: int
        @raise KeyError: If no street has that id.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef bytes b = street_id.encode('utf-8')
        cdef int i = swmm_street_index(h, b)
        if i < 0:
            raise KeyError(street_id)
        return i

    def get_id(self, int idx) -> str:
        """Return the string id of the street at C{idx}.

        @rtype: str
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef const char* raw = swmm_street_id(h, idx)
        return raw.decode('utf-8') if raw != NULL else ""

    def __iter__(self):
        cdef int n = len(self)
        for i in range(n):
            yield self.get_id(i)

    def __contains__(self, key) -> bool:
        if isinstance(key, str):
            try:
                self.get_index(key)
                return True
            except KeyError:
                return False
        return 0 <= int(key) < len(self)


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

    def get_index(self, str inlet_id) -> int:
        """Resolve an inlet's zero-based index from its string id.

        @rtype: int
        @raise KeyError: If no inlet has that id.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef bytes b = inlet_id.encode('utf-8')
        cdef int i = swmm_inlet_index(h, b)
        if i < 0:
            raise KeyError(inlet_id)
        return i

    def get_id(self, int idx) -> str:
        """Return the string id of the inlet at C{idx}.

        @rtype: str
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef const char* raw = swmm_inlet_id(h, idx)
        return raw.decode('utf-8') if raw != NULL else ""

    def __iter__(self):
        cdef int n = len(self)
        for i in range(n):
            yield self.get_id(i)

    def __contains__(self, key) -> bool:
        if isinstance(key, str):
            try:
                self.get_index(key)
                return True
            except KeyError:
                return False
        return 0 <= int(key) < len(self)


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
        """Add a LID control.

        @param lid_id: Unique identifier for the new LID control.
        @param lid_type: A L{LidType} enum value.
        @return: Zero-based index of the new LID control.
        @rtype: int
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef bytes b = lid_id.encode('utf-8')
        _check(swmm_lid_add(h, b, int(lid_type)))
        self._solver._bump_generation()
        return len(self) - 1

    def get_index(self, str lid_id) -> int:
        """Resolve a LID control's zero-based index from its string id.

        @rtype: int
        @raise KeyError: If no LID control has that id.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef bytes b = lid_id.encode('utf-8')
        cdef int i = swmm_lid_index(h, b)
        if i < 0:
            raise KeyError(lid_id)
        return i

    def get_id(self, int idx) -> str:
        """Return the string id of the LID control at C{idx}.

        @rtype: str
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef const char* raw = swmm_lid_id(h, idx)
        return raw.decode('utf-8') if raw != NULL else ""

    def __iter__(self):
        cdef int n = len(self)
        for i in range(n):
            yield self.get_id(i)

    def __contains__(self, key) -> bool:
        if isinstance(key, str):
            try:
                self.get_index(key)
                return True
            except KeyError:
                return False
        return 0 <= int(key) < len(self)

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
