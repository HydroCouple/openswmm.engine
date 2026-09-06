# SPDX-License-Identifier: Apache-2.0
#
# Copyright 2026 Caleb Buahin
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""
Infrastructure (Pythonic v1 surface)
====================================

:author: Caleb Buahin
:copyright: Copyright (c) 2026 Caleb Buahin
:license: Apache-2.0

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

from libc.string cimport memcpy, memset

from ._exceptions import ElementNotFoundError
from ._enums import (GrateType, InletCurveKind, InletHostKind, InletPlacement,
                     InletType, ThroatType)
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
        """Add a natural-channel transect *transect_id* and return its index."""
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef bytes b = transect_id.encode('utf-8')
        _check(swmm_transect_add(h, b))
        self._solver._bump_generation()
        return len(self) - 1

    def set_roughness(self, int idx,
                      double n_left, double n_right, double n_channel) -> None:
        """Set the left-bank, right-bank and channel Manning's n for transect *idx*."""
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_transect_set_roughness(h, idx, n_left, n_right, n_channel))

    def add_station(self, int idx, double station, double elevation) -> None:
        """Append a ``(station, elevation)`` point to transect *idx*."""
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
            raise ElementNotFoundError(transect_id)
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
        """Add a street cross-section *street_id* and return its index."""
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
        """Set the geometry and roughness parameters for street *idx*."""
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
            raise ElementNotFoundError(street_id)
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

    def rename(self, key, str new_id) -> None:
        """Rename a street cross-section, updating stored references.

        @param key: Integer index or string id.
        @param new_id: New identifier.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int idx = key if isinstance(key, int) else self.get_index(key)
        cdef bytes b = new_id.encode('utf-8')
        _check(swmm_street_rename(h, idx, b))
        self._solver._bump_generation()


# ---- Inlets ---------------------------------------------------------

class Inlets:
    """``solver.infrastructure.inlets`` view."""

    def __init__(self, solver):
        self._solver = solver

    def __len__(self) -> int:
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        return swmm_inlet_count(h)

    def add(self, str inlet_id, str inlet_type) -> int:
        """Add an inlet *inlet_id* of *inlet_type* and return its index."""
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
        """Set the geometry parameters for inlet *idx*."""
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef bytes b_grate = grate_type.encode('utf-8')
        _check(swmm_inlet_set_params(
            h, idx, length, width, b_grate, open_area, splash_veloc))

    def get_params(self, int idx) -> dict:
        """Read back an inlet's geometric parameters.

        Inverse of :meth:`set_params` — lets an editor load an existing inlet.
        The inlet *type* string (set at :meth:`add`) is returned separately by
        :meth:`get_type`.

        @param idx: Zero-based inlet index.
        @type idx: int
        @return: Mapping with keys ``length``, ``width``, ``grate_type``,
            ``open_area``, ``splash_veloc``.
        @rtype: dict
        @raise EngineError: On C API failure.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double length = 0.0, width = 0.0, open_area = 0.0, splash_veloc = 0.0
        cdef char grate[256]
        _check(swmm_inlet_get_params(
            h, idx, &length, &width, grate, sizeof(grate),
            &open_area, &splash_veloc))
        return {
            "length": length, "width": width,
            "grate_type": grate.decode('utf-8'),
            "open_area": open_area, "splash_veloc": splash_veloc,
        }

    def get_type(self, int idx) -> str:
        """Return the inlet's type string (the value passed to :meth:`add`).

        @param idx: Zero-based inlet index.
        @rtype: str
        @raise EngineError: On C API failure.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef char buf[256]
        _check(swmm_inlet_get_type(h, idx, buf, sizeof(buf)))
        return buf.decode('utf-8')

    def get_index(self, str inlet_id) -> int:
        """Resolve an inlet's zero-based index from its string id.

        @rtype: int
        @raise KeyError: If no inlet has that id.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef bytes b = inlet_id.encode('utf-8')
        cdef int i = swmm_inlet_index(h, b)
        if i < 0:
            raise ElementNotFoundError(inlet_id)
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

    def rename(self, key, str new_id) -> None:
        """Rename an inlet design, updating stored references.

        @param key: Integer index or string id.
        @param new_id: New identifier.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int idx = key if isinstance(key, int) else self.get_index(key)
        cdef bytes b = new_id.encode('utf-8')
        _check(swmm_inlet_rename(h, idx, b))
        self._solver._bump_generation()

    # ---- Full design surface (SWMM_InletDesign) ---------------------

    def get_design(self, key) -> dict:
        """Read every field of an inlet design.

        Superset of :meth:`get_params`, which only covers the grate/slotted
        dimensions. Only the fields the design's C{type} actually uses carry
        meaning; the rest read back as zero.

        @param key: Integer index or string id.
        @return: Mapping with keys ``type`` (L{InletType}), ``grate_length``,
            ``grate_width``, ``grate_type`` (L{GrateType}), ``open_area``,
            ``splash_veloc``, ``curb_length``, ``curb_height``, ``throat``
            (L{ThroatType}), ``slot_length``, ``slot_width``, ``curve_id``,
            ``curve_kind`` (L{InletCurveKind}).
        @rtype: dict
        @raise KeyError: If C{key} is a name and no inlet has it.
        @raise EngineError: On C API failure.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int idx = key if isinstance(key, int) else self.get_index(key)
        cdef SWMM_InletDesign d
        cdef char curve[64]
        _check(swmm_inlet_get_design(h, idx, &d))
        memcpy(curve, d.curve_id, sizeof(curve))
        curve[sizeof(curve) - 1] = 0
        return {
            "type": InletType(d.type),
            "grate_length": d.grate_length,
            "grate_width": d.grate_width,
            "grate_type": GrateType(d.grate_type),
            "open_area": d.open_area,
            "splash_veloc": d.splash_veloc,
            "curb_length": d.curb_length,
            "curb_height": d.curb_height,
            "throat": ThroatType(d.throat),
            "slot_length": d.slot_length,
            "slot_width": d.slot_width,
            "curve_id": curve.decode('utf-8'),
            "curve_kind": InletCurveKind(d.curve_kind),
        }

    def set_design(self, key, design=None, **kwargs) -> None:
        """Overwrite every field of an inlet design (the type may change).

        Accepts the mapping :meth:`get_design` returns, keyword arguments, or
        both (keywords win), so a read-modify-write is a one-liner::

            inlets.set_design("Curb1", throat=ThroatType.INCLINED)

        Only the fields the resulting C{type} uses are validated: the
        dimensions it needs must be positive, ``open_area`` must lie in
        (0, 1] for a C{GENERIC} grate, and a C{CUSTOM} design needs a
        non-empty ``curve_id``. A ``curve_kind`` that contradicts the named
        curve's own C{[CURVES]} type is rejected; when the curve does not
        exist yet the kind is stored as given and re-derived on validation.

        @param key: Integer index or string id.
        @param design: Optional mapping of field values (defaults are read
            from the existing design, so an omitted field is preserved).
        @type design: dict or None
        @raise KeyError: If C{key} is a name and no inlet has it.
        @raise EngineError: On a violated constraint or C API failure.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int idx = key if isinstance(key, int) else self.get_index(key)
        cdef SWMM_InletDesign d
        _check(swmm_inlet_get_design(h, idx, &d))

        values = dict(design or {})
        values.update(kwargs)
        cdef bytes b_curve
        if "type" in values:         d.type         = int(values["type"])
        if "grate_length" in values: d.grate_length = float(values["grate_length"])
        if "grate_width" in values:  d.grate_width  = float(values["grate_width"])
        if "grate_type" in values:   d.grate_type   = int(values["grate_type"])
        if "open_area" in values:    d.open_area    = float(values["open_area"])
        if "splash_veloc" in values: d.splash_veloc = float(values["splash_veloc"])
        if "curb_length" in values:  d.curb_length  = float(values["curb_length"])
        if "curb_height" in values:  d.curb_height  = float(values["curb_height"])
        if "throat" in values:       d.throat       = int(values["throat"])
        if "slot_length" in values:  d.slot_length  = float(values["slot_length"])
        if "slot_width" in values:   d.slot_width   = float(values["slot_width"])
        if "curve_kind" in values:   d.curve_kind   = int(values["curve_kind"])
        if "curve_id" in values:
            b_curve = (values["curve_id"] or "").encode('utf-8')
            if len(b_curve) >= sizeof(d.curve_id):
                raise ValueError("curve_id is longer than 63 bytes")
            memset(d.curve_id, 0, sizeof(d.curve_id))
            memcpy(d.curve_id, <const char*>b_curve, len(b_curve))
        _check(swmm_inlet_set_design(h, idx, &d))

    def get_comment(self, key) -> str:
        """Return the free-text description of an inlet design.

        @param key: Integer index or string id.
        @rtype: str
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int idx = key if isinstance(key, int) else self.get_index(key)
        cdef char buf[1024]
        _check(swmm_inlet_get_comment(h, idx, buf, sizeof(buf)))
        return buf.decode('utf-8')

    def set_comment(self, key, str text) -> None:
        """Set the free-text description of an inlet design.

        @param key: Integer index or string id.
        @param text: Description; round-tripped as a C{';'} comment line.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int idx = key if isinstance(key, int) else self.get_index(key)
        cdef bytes b = (text or "").encode('utf-8')
        _check(swmm_inlet_set_comment(h, idx, b))


# ---- Inlet usage ----------------------------------------------------

class InletUsages:
    """``solver.infrastructure.inlet_usages`` view — inlet placements.

    One row per placement, of either host kind: a conduit carrying an
    C{[INLET_USAGE]} row, or an inlet junction (an C{[INLET_JUNCTIONS]}
    node). Both kinds live in one store, so C{len()} counts them together
    and :meth:`get` reports which kind a row is through its ``host_kind``.
    """

    def __init__(self, solver):
        self._solver = solver

    def __len__(self) -> int:
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int n = swmm_inlet_usage_count(h)
        return n if n > 0 else 0

    def find_link(self, link_idx) -> int:
        """Row index of the usage hosted by conduit C{link_idx}, or C{-1}.

        @param link_idx: Zero-based link index.
        @rtype: int
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        return swmm_inlet_usage_find_link(h, int(link_idx))

    def find_node(self, node_idx) -> int:
        """Row index of the usage hosted by inlet junction C{node_idx}, or C{-1}.

        @param node_idx: Zero-based node index.
        @rtype: int
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        return swmm_inlet_usage_find_node(h, int(node_idx))

    def get(self, int usage_idx) -> dict:
        """Read one usage row.

        @param usage_idx: Zero-based usage-row index.
        @return: Mapping with keys ``host_kind`` (L{InletHostKind}),
            ``host_idx`` (a link index for C{LINK}, a node index for
            C{NODE}), ``design_idx``, ``capture_node_idx``, ``num_inlets``,
            ``pct_clogged``, ``flow_limit``, ``local_depress``,
            ``local_width``, ``placement`` (L{InletPlacement}).
        @rtype: dict
        @raise EngineError: On a bad index or C API failure.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef SWMM_InletUsage u
        _check(swmm_inlet_usage_get(h, usage_idx, &u))
        return {
            "host_kind": InletHostKind(u.host_kind),
            "host_idx": u.host_idx,
            "design_idx": u.design_idx,
            "capture_node_idx": u.capture_node_idx,
            "num_inlets": u.num_inlets,
            "pct_clogged": u.pct_clogged,
            "flow_limit": u.flow_limit,
            "local_depress": u.local_depress,
            "local_width": u.local_width,
            "placement": InletPlacement(u.placement),
        }

    def set(self, int host_kind, int host_idx, int design_idx,
            int capture_node_idx, *, int num_inlets=1,
            double pct_clogged=0.0, double flow_limit=0.0,
            double local_depress=0.0, double local_width=0.0,
            int placement=0) -> int:
        """Create or replace the usage row for a host.

        At most one row exists per host: an existing row for
        C{(host_kind, host_idx)} is overwritten, otherwise a row is appended.
        A C{NODE} host must already be an inlet junction (see
        C{Node.is_inlet}); a C{LINK} host must be a conduit. The capture node
        must exist, must not be the host node, and must not itself be virtual.

        Flow and length values are in the project's display units, as
        authored in C{[INLET_USAGE]}.

        @param host_kind: L{InletHostKind} — C{LINK} or C{NODE}.
        @param host_idx: Zero-based link or node index per C{host_kind}.
        @param design_idx: Zero-based inlet-design index.
        @param capture_node_idx: Receiving (underdrain) node index.
        @param num_inlets: Inlets per side (>= 1).
        @param pct_clogged: Clogged fraction, 0..99.
        @param flow_limit: Max capture per inlet; C{0} = unlimited.
        @param local_depress: Local gutter depression.
        @param local_width: Local depression width.
        @param placement: L{InletPlacement}.
        @return: Index of the created or replaced row.
        @rtype: int
        @raise EngineError: On a violated constraint or C API failure.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef SWMM_InletUsage u
        cdef int row = -1
        u.host_kind = host_kind
        u.host_idx = host_idx
        u.design_idx = design_idx
        u.capture_node_idx = capture_node_idx
        u.num_inlets = num_inlets
        u.pct_clogged = pct_clogged
        u.flow_limit = flow_limit
        u.local_depress = local_depress
        u.local_width = local_width
        u.placement = placement
        _check(swmm_inlet_usage_set(h, &u, &row))
        self._solver._bump_generation()
        return row

    def remove(self, int usage_idx) -> None:
        """Delete a usage row.

        Removing an inlet junction's row leaves the node flagged as an inlet
        with nothing to capture, which the model validator rejects; demote
        the node instead (C{Node.is_inlet = False}) or give it a new row.

        @param usage_idx: Zero-based usage-row index.
        @raise EngineError: On a bad index or C API failure.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_inlet_usage_remove(h, usage_idx))
        self._solver._bump_generation()

    def __iter__(self):
        cdef int n = len(self)
        for i in range(n):
            yield self.get(i)


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
            raise ElementNotFoundError(lid_id)
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

    def rename(self, key, str new_id) -> None:
        """Rename an LID control, updating stored ``[LID_USAGE]`` references.

        @param key: Integer index or string id.
        @param new_id: New identifier.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int idx = key if isinstance(key, int) else self.get_index(key)
        cdef bytes b = new_id.encode('utf-8')
        _check(swmm_lid_rename(h, idx, b))
        self._solver._bump_generation()

    def set_surface(self, int idx, *,
                    double storage, double roughness, double slope) -> None:
        """Set the surface-layer parameters for LID *idx*."""
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_lid_set_surface(h, idx, storage, roughness, slope))

    def set_soil(self, int idx, *,
                 double thick, double porosity, double fc,
                 double wp, double ksat, double kslope) -> None:
        """Set the soil-layer parameters for LID *idx*."""
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_lid_set_soil(h, idx, thick, porosity, fc, wp, ksat, kslope))

    def set_storage(self, int idx, *,
                    double thick, double void_frac, double ksat) -> None:
        """Set the storage-layer parameters for LID *idx*."""
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_lid_set_storage(h, idx, thick, void_frac, ksat))

    def set_drain(self, int idx, *,
                  double coeff, double expon, double offset) -> None:
        """Set the underdrain parameters for LID *idx*."""
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_lid_set_drain(h, idx, coeff, expon, offset))

    def set_pavement(self, int idx, *,
                     double thick, double void_ratio, double frac_imperv,
                     double ksat, double clog_factor=0.0, double regen_days=0.0) -> None:
        """Set the porous-pavement layer (PERM_PAVEMENT LIDs)."""
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_lid_set_pavement(h, idx, thick, void_ratio, frac_imperv,
                                     ksat, clog_factor, regen_days))

    def set_drainmat(self, int idx, *,
                     double thick, double void_frac, double roughness) -> None:
        """Set the drainage-mat layer (GREEN_ROOF LIDs)."""
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_lid_set_drainmat(h, idx, thick, void_frac, roughness))

    def get_type(self, int idx) -> int:
        """Return the LID control's type code (inverse of the ``lid_type``
        argument to :meth:`add`).

        @param idx: Zero-based LID index.
        @return: An integer L{LidType} code (0=BIO_CELL … 7=VEGETATIVE_SWALE).
        @rtype: int
        @raise EngineError: On C API failure.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int t = -1
        _check(swmm_lid_get_type(h, idx, &t))
        return t

    def get_surface(self, int idx) -> dict:
        """Read the surface layer. Inverse of :meth:`set_surface`.

        @return: Mapping ``storage``, ``roughness``, ``slope``.
        @rtype: dict
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double storage = 0.0, roughness = 0.0, slope = 0.0
        _check(swmm_lid_get_surface(h, idx, &storage, &roughness, &slope))
        return {"storage": storage, "roughness": roughness, "slope": slope}

    def get_soil(self, int idx) -> dict:
        """Read the soil layer. Inverse of :meth:`set_soil`.

        @return: Mapping ``thick``, ``porosity``, ``fc``, ``wp``, ``ksat``,
            ``kslope``.
        @rtype: dict
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double thick = 0.0, porosity = 0.0, fc = 0.0, wp = 0.0, ksat = 0.0, kslope = 0.0
        _check(swmm_lid_get_soil(h, idx, &thick, &porosity, &fc, &wp, &ksat, &kslope))
        return {"thick": thick, "porosity": porosity, "fc": fc,
                "wp": wp, "ksat": ksat, "kslope": kslope}

    def get_storage(self, int idx) -> dict:
        """Read the storage layer. Inverse of :meth:`set_storage`.

        @return: Mapping ``thick``, ``void_frac``, ``ksat``.
        @rtype: dict
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double thick = 0.0, void_frac = 0.0, ksat = 0.0
        _check(swmm_lid_get_storage(h, idx, &thick, &void_frac, &ksat))
        return {"thick": thick, "void_frac": void_frac, "ksat": ksat}

    def get_drain(self, int idx) -> dict:
        """Read the underdrain layer. Inverse of :meth:`set_drain`.

        @return: Mapping ``coeff``, ``expon``, ``offset``.
        @rtype: dict
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double coeff = 0.0, expon = 0.0, offset = 0.0
        _check(swmm_lid_get_drain(h, idx, &coeff, &expon, &offset))
        return {"coeff": coeff, "expon": expon, "offset": offset}

    def get_pavement(self, int idx) -> dict:
        """Read the porous-pavement layer. Inverse of :meth:`set_pavement`.

        @return: Mapping ``thick``, ``void_ratio``, ``frac_imperv``, ``ksat``,
            ``clog_factor``, ``regen_days``.
        @rtype: dict
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double thick = 0.0, void_ratio = 0.0, frac_imperv = 0.0
        cdef double ksat = 0.0, clog_factor = 0.0, regen_days = 0.0
        _check(swmm_lid_get_pavement(h, idx, &thick, &void_ratio, &frac_imperv,
                                     &ksat, &clog_factor, &regen_days))
        return {"thick": thick, "void_ratio": void_ratio, "frac_imperv": frac_imperv,
                "ksat": ksat, "clog_factor": clog_factor, "regen_days": regen_days}

    def get_drainmat(self, int idx) -> dict:
        """Read the drainage-mat layer. Inverse of :meth:`set_drainmat`.

        @return: Mapping ``thick``, ``void_frac``, ``roughness``.
        @rtype: dict
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double thick = 0.0, void_frac = 0.0, roughness = 0.0
        _check(swmm_lid_get_drainmat(h, idx, &thick, &void_frac, &roughness))
        return {"thick": thick, "void_frac": void_frac, "roughness": roughness}

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

    def usage_count(self) -> int:
        """Number of ``[LID_USAGE]`` placement rows across all subcatchments.

        @rtype: int
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        return swmm_lid_usage_count(h)

    def usage_get(self, int usage_idx) -> dict:
        """Read one ``[LID_USAGE]`` placement row by global index.

        @param usage_idx: Zero-based global usage index in
            C{[0, usage_count())}.
        @type usage_idx: int
        @return: Mapping with keys ``subcatch_index``, ``lid_index``,
            ``number``, ``area``, ``width``, ``init_sat``, ``from_imperv``,
            ``to_perv``, ``from_perv``.
        @rtype: dict
        @raise EngineError: On C API failure (e.g. index out of range).
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int subcatch_idx = 0, lid_idx = 0, number = 0, to_perv = 0
        cdef double area = 0.0, width = 0.0, init_sat = 0.0
        cdef double from_imperv = 0.0, from_perv = 0.0
        _check(swmm_lid_usage_get(
            h, usage_idx, &subcatch_idx, &lid_idx, &number,
            &area, &width, &init_sat, &from_imperv, &to_perv, &from_perv))
        return {
            "subcatch_index": subcatch_idx,
            "lid_index": lid_idx,
            "number": number,
            "area": area,
            "width": width,
            "init_sat": init_sat,
            "from_imperv": from_imperv,
            "to_perv": to_perv,
            "from_perv": from_perv,
        }

    def usage_remove(self, int usage_idx) -> None:
        """Remove one ``[LID_USAGE]`` placement row by global index.

        @param usage_idx: Zero-based global usage index in
            C{[0, usage_count())}.
        @type usage_idx: int
        @raise EngineError: On C API failure (e.g. index out of range).
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_lid_usage_remove(h, usage_idx))
        self._solver._bump_generation()


# ---- Top-level Infrastructure view ----------------------------------

class Infrastructure:
    """``solver.infrastructure`` — entry point for the five sub-views."""

    def __init__(self, solver):
        self._solver = solver
        self._transects = None
        self._streets = None
        self._inlets = None
        self._inlet_usages = None
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
    def inlet_usages(self) -> InletUsages:
        if self._inlet_usages is None:
            self._inlet_usages = InletUsages(self._solver)
        return self._inlet_usages

    @property
    def lids(self) -> LIDs:
        if self._lids is None:
            self._lids = LIDs(self._solver)
        return self._lids

    def __repr__(self) -> str:
        try:
            return (f"<Infrastructure transects={len(self.transects)} "
                    f"streets={len(self.streets)} inlets={len(self.inlets)} "
                    f"inlet_usages={len(self.inlet_usages)} "
                    f"lids={len(self.lids)}>")
        except Exception:
            return "<Infrastructure (engine closed)>"
