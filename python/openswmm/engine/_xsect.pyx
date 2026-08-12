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
Cross-Section Geometry
======================

:author: Caleb Buahin
:copyright: Copyright (c) 2026 Caleb Buahin
:license: Apache-2.0

The :class:`XSectionGeometry` class exposes the engine's cross-section
geometry kernels as a standalone reference implementation — the same code the
routing solvers use, so results agree with a simulation exactly.

A section can be built from shape parameters with no model open, or taken from
a link of a running model (in which case it carries the geometry the engine
actually built, transect tables and all, and stays valid after the solver
closes).

Every method takes a float or a NumPy array; array input dispatches to a single
batched C call and returns an ``ndarray`` of the same shape.

.. code-block:: python

    import numpy as np
    from openswmm.engine import XSectionGeometry, XSectShape

    # A 1 m circular pipe — no model needed.
    pipe = XSectionGeometry(XSectShape.CIRCULAR, 1.0, units="SI")
    pipe.area(0.5)                       # half full
    pipe.area(np.linspace(0, 1.0, 100))  # a rating curve, one C call

    # ...or the real geometry of a link in a model.
    with Solver("model.inp") as s:
        xs = s.links["C1"].xsect.geometry()
        print(xs.full_depth, xs.is_open)
"""

# cython: language_level=3

import numpy as np
cimport numpy as np

from ._common cimport *
from ._enums import XSectShape
from ._exceptions import StaleObjectError

np.import_array()


# =============================================================================
# Helpers
# =============================================================================

_FLOW_UNIT_NAMES = ("CFS", "GPM", "MGD", "CMS", "LPS", "MLD")

# Kernel selectors for _eval(). Keeping the dispatch in one place is what stops
# the scalar and array paths from drifting apart.
cdef enum _Op:
    _OP_AREA_OF_DEPTH
    _OP_WIDTH_OF_DEPTH
    _OP_HYDRAD_OF_DEPTH
    _OP_DEPTH_OF_AREA
    _OP_HYDRAD_OF_AREA
    _OP_SF_OF_AREA
    _OP_AREA_OF_SF
    _OP_DSDA
    _OP_CRITICAL_DEPTH


cdef int _call_scalar(_Op op, SWMM_XSect h, double v, double* out) noexcept nogil:
    if op == _OP_AREA_OF_DEPTH:    return swmm_xsect_area_of_depth(h, v, out)
    if op == _OP_WIDTH_OF_DEPTH:   return swmm_xsect_width_of_depth(h, v, out)
    if op == _OP_HYDRAD_OF_DEPTH:  return swmm_xsect_hydrad_of_depth(h, v, out)
    if op == _OP_DEPTH_OF_AREA:    return swmm_xsect_depth_of_area(h, v, out)
    if op == _OP_HYDRAD_OF_AREA:   return swmm_xsect_hydrad_of_area(h, v, out)
    if op == _OP_SF_OF_AREA:       return swmm_xsect_sectfactor_of_area(h, v, out)
    if op == _OP_AREA_OF_SF:       return swmm_xsect_area_of_sectfactor(h, v, out)
    if op == _OP_DSDA:             return swmm_xsect_dsda(h, v, out)
    return swmm_xsect_critical_depth(h, v, out)


cdef int _call_array(_Op op, SWMM_XSect h, const double* inp, int n,
                     double* out) noexcept nogil:
    if op == _OP_AREA_OF_DEPTH:    return swmm_xsect_area_of_depth_array(h, inp, n, out)
    if op == _OP_WIDTH_OF_DEPTH:   return swmm_xsect_width_of_depth_array(h, inp, n, out)
    if op == _OP_HYDRAD_OF_DEPTH:  return swmm_xsect_hydrad_of_depth_array(h, inp, n, out)
    if op == _OP_DEPTH_OF_AREA:    return swmm_xsect_depth_of_area_array(h, inp, n, out)
    if op == _OP_HYDRAD_OF_AREA:   return swmm_xsect_hydrad_of_area_array(h, inp, n, out)
    if op == _OP_SF_OF_AREA:       return swmm_xsect_sectfactor_of_area_array(h, inp, n, out)
    if op == _OP_AREA_OF_SF:       return swmm_xsect_area_of_sectfactor_array(h, inp, n, out)
    if op == _OP_DSDA:             return swmm_xsect_dsda_array(h, inp, n, out)
    return swmm_xsect_critical_depth_array(h, inp, n, out)


cdef int _units_code(object units) except -1:
    """Map ``'US'``/``'SI'`` to the engine's unit-system code."""
    if not isinstance(units, str):
        raise TypeError(
            f"units must be 'US' or 'SI', got {type(units).__name__}"
        )
    u = units.strip().upper()
    if u == "US":
        return 0
    if u == "SI":
        return 1
    raise ValueError(f"units must be 'US' or 'SI', got {units!r}")


cdef object _as_f64_array(object values, str name):
    arr = np.ascontiguousarray(values, dtype=np.float64)
    if not np.all(np.isfinite(arr)):
        raise ValueError(f"{name} must contain only finite values")
    return arr


cdef object _to_enum(int code):
    try:
        return XSectShape(code)
    except ValueError:
        return code


cdef object _shape_repr(int code):
    cdef const char* nm = swmm_xsect_shape_name(code)
    if nm == NULL:
        return f"<unknown shape {code}>"
    return nm.decode("utf-8")


# =============================================================================
# XSectionGeometry
# =============================================================================

cdef class XSectionGeometry:
    """A cross-section that can be queried for its hydraulic geometry.

    Construct directly for a self-contained shape, or use one of the
    classmethods for shapes whose geometry comes from tabulated data. Every
    query method accepts a float (returning a float) or a NumPy array
    (returning an ``ndarray`` of the same shape).

    All values are in the section's own units: lengths in ft (``units='US'``)
    or m (``units='SI'``), areas squared, section factors to the 8/3 power.
    :meth:`critical_depth` takes a flow in :attr:`flow_units`.

    @ivar full_depth: Depth at which the section is full.
    @ivar full_area: Flow area when full.
    @ivar full_hyd_radius: Hydraulic radius when full.
    @ivar max_width: Width at the widest point.
    @ivar full_section_factor: Section factor when full.
    @ivar max_area: Area at which flow is a maximum.
    @ivar is_open: True when the section is open to the atmosphere.
    """

    cdef SWMM_XSect _h
    cdef readonly object shape
    """The :class:`XSectShape` of this section."""

    def __cinit__(self):
        self._h = NULL

    def __init__(self, shape, geom1, geom2=0.0, geom3=0.0, geom4=0.0, *, units):
        """Build a section from a shape code and its geometry parameters.

        @param shape: An :class:`XSectShape`. IRREGULAR / CUSTOM / STREET_XSECT
            carry no inline geometry — use :meth:`from_transect`,
            :meth:`from_curve` or :meth:`from_street` instead.
        @param geom1: Full depth or diameter.
        @param geom2: Second geometry parameter (shape-dependent).
        @param geom3: Third geometry parameter (shape-dependent).
        @param geom4: Fourth geometry parameter (shape-dependent).
        @keyword units: ``'US'`` or ``'SI'``. Required — there is no default,
            because a silently-assumed unit system is a silently wrong answer.
        @raise ValueError: The shape or geometry is invalid.
        """
        cdef int us = _units_code(units)
        cdef int rc
        cdef SWMM_XSect h = NULL
        cdef int ishape = int(shape)
        rc = swmm_xsect_create(ishape, float(geom1), float(geom2),
                               float(geom3), float(geom4), us, &h)
        if rc != 0:
            raise ValueError(
                f"cannot build a {_shape_repr(ishape)} cross-section from "
                f"geom=({geom1}, {geom2}, {geom3}, {geom4}) in {units!r} units"
            )
        self._h = h
        self.shape = _to_enum(ishape)

    def __dealloc__(self):
        if self._h != NULL:
            swmm_xsect_free(self._h)
            self._h = NULL

    # ---- Alternate constructors -----------------------------------

    @classmethod
    def from_transect(cls, stations, elevations, *, left_bank, right_bank,
                      n_channel, n_left=0.0, n_right=0.0, length_factor=1.0,
                      units):
        """Build an irregular (natural channel) section from transect data.

        Mirrors a ``[TRANSECTS]`` entry.

        @param stations: Station coordinates, ascending.
        @param elevations: Elevation at each station.
        @keyword left_bank: Station where the left overbank ends. Pass the same
            value as *right_bank* for a channel with no overbanks.
        @keyword right_bank: Station where the right overbank begins.
        @keyword n_channel: Manning's n for the main channel. Must be > 0.
        @keyword n_left: Manning's n for the left overbank; 0 → *n_channel*.
        @keyword n_right: Manning's n for the right overbank; 0 → *n_channel*.
        @keyword length_factor: Main-channel / flood-plain length ratio.
        @keyword units: ``'US'`` or ``'SI'``.
        @return: A new :class:`XSectionGeometry`.

        @note The roughness values shape the geometry: the hydraulic-radius
            table is conveyance-weighted across the overbanks and channel, so
            they change what :meth:`hyd_radius` reports.
        """
        cdef int us = _units_code(units)
        st = _as_f64_array(stations, "stations")
        el = _as_f64_array(elevations, "elevations")
        if st.ndim != 1 or el.ndim != 1:
            raise ValueError("stations and elevations must be 1-D")
        if st.shape[0] != el.shape[0]:
            raise ValueError(
                f"stations ({st.shape[0]}) and elevations ({el.shape[0]}) "
                "must have the same length"
            )
        if st.shape[0] < 2:
            raise ValueError("a transect needs at least 2 station/elevation pairs")

        cdef np.ndarray[double, ndim=1] stv = st
        cdef np.ndarray[double, ndim=1] elv = el
        cdef SWMM_XSect h = NULL
        cdef int rc = swmm_xsect_create_irregular(
            <const double*>stv.data, <const double*>elv.data,
            <int>stv.shape[0], float(left_bank), float(right_bank),
            float(n_left), float(n_channel), float(n_right),
            float(length_factor), us, &h)
        if rc != 0:
            raise ValueError(
                "cannot build an irregular cross-section from the given "
                "transect (n_channel must be > 0 and the stations must "
                "enclose a wetted area)"
            )
        return _wrap(h)

    @classmethod
    def from_curve(cls, full_depth, curve_depths, curve_widths, *, units):
        """Build a custom section from a normalized shape curve.

        Mirrors a ``SHAPE``-type ``[CURVES]`` entry scaled to *full_depth*.

        @param full_depth: Full depth of the section (> 0).
        @param curve_depths: Normalized depths (y/yFull in [0, 1]), ascending.
        @param curve_widths: Normalized widths (w/wMax) at each depth.
        @keyword units: ``'US'`` or ``'SI'``.
        @return: A new :class:`XSectionGeometry`.
        """
        cdef int us = _units_code(units)
        cx = _as_f64_array(curve_depths, "curve_depths")
        cy = _as_f64_array(curve_widths, "curve_widths")
        if cx.ndim != 1 or cy.ndim != 1:
            raise ValueError("curve_depths and curve_widths must be 1-D")
        if cx.shape[0] != cy.shape[0]:
            raise ValueError(
                f"curve_depths ({cx.shape[0]}) and curve_widths "
                f"({cy.shape[0]}) must have the same length"
            )
        if cx.shape[0] < 2:
            raise ValueError("a shape curve needs at least 2 points")

        cdef np.ndarray[double, ndim=1] cxv = cx
        cdef np.ndarray[double, ndim=1] cyv = cy
        cdef SWMM_XSect h = NULL
        cdef int rc = swmm_xsect_create_custom(
            float(full_depth), <const double*>cxv.data, <const double*>cyv.data,
            <int>cxv.shape[0], us, &h)
        if rc != 0:
            raise ValueError(
                f"cannot build a custom cross-section with full_depth="
                f"{full_depth} from the given shape curve"
            )
        return _wrap(h)

    @classmethod
    def from_street(cls, width, curb_height, slope, roughness, *,
                    gutter_depression=0.0, gutter_width=0.0, sides=2,
                    back_width=0.0, back_slope=0.0, back_roughness=0.0,
                    units):
        """Build a street section. Mirrors a ``[STREETS]`` entry.

        @param width: Distance from curb to crown.
        @param curb_height: Curb height.
        @param slope: Transverse road slope, in percent.
        @param roughness: Manning's n of the road surface (> 0).
        @keyword gutter_depression: Depressed-gutter depth.
        @keyword gutter_width: Depressed-gutter width.
        @keyword sides: 1 = half street, 2 = full street.
        @keyword back_width: Backing width.
        @keyword back_slope: Backing slope, in percent.
        @keyword back_roughness: Backing Manning's n.
        @keyword units: ``'US'`` or ``'SI'``.
        @return: A new :class:`XSectionGeometry`.
        """
        cdef int us = _units_code(units)
        cdef SWMM_XSect h = NULL
        cdef int rc = swmm_xsect_create_street(
            float(width), float(curb_height), float(slope), float(roughness),
            float(gutter_depression), float(gutter_width), int(sides),
            float(back_width), float(back_slope), float(back_roughness),
            us, &h)
        if rc != 0:
            raise ValueError(
                "cannot build a street cross-section (width, curb_height and "
                "roughness must be > 0 and sides must be 1 or 2)"
            )
        return _wrap(h)

    @classmethod
    def from_link(cls, link):
        """Build a section from a :class:`Link` of an open model.

        Deep-copies the geometry the engine built for the link, so the result
        stays valid after the solver closes. Inherits the model's units.

        @param link: A :class:`~openswmm.engine.Link`.
        @return: A new :class:`XSectionGeometry`.
        @raise LifecycleError: The model is still being built — the derived
            geometry does not exist until the model is finalized or opened.
        @raise BadParamError: The link has no cross-section (e.g. a pump).
        @raise StaleObjectError: *link* predates a change to the model.
        """
        if link._gen != link._solver.generation:
            raise StaleObjectError(
                f"Link wrapper (id={link._captured_id!r}, index={link._index}) "
                "is stale; look it up again from solver.links."
            )
        cdef SWMM_Engine e = <SWMM_Engine><size_t>link._solver.handle
        cdef SWMM_XSect h = NULL
        cdef int rc = swmm_link_create_xsect(e, <int>link._index, &h)
        _check(rc)
        return _wrap(h)

    # ---- Queries --------------------------------------------------

    def area(self, depth):
        """Flow area at *depth*. @param depth: float or ndarray."""
        return self._eval(_OP_AREA_OF_DEPTH, depth, "depth")

    def width(self, depth):
        """Top width of the water surface at *depth*."""
        return self._eval(_OP_WIDTH_OF_DEPTH, depth, "depth")

    def hyd_radius(self, depth):
        """Hydraulic radius (area / wetted perimeter) at *depth*."""
        return self._eval(_OP_HYDRAD_OF_DEPTH, depth, "depth")

    def depth_from_area(self, area):
        """Depth of flow for *area* — the inverse of :meth:`area`."""
        return self._eval(_OP_DEPTH_OF_AREA, area, "area")

    def hyd_radius_from_area(self, area):
        """Hydraulic radius for *area*."""
        return self._eval(_OP_HYDRAD_OF_AREA, area, "area")

    def section_factor(self, area):
        """Section factor (A·R^(2/3)) for *area*."""
        return self._eval(_OP_SF_OF_AREA, area, "area")

    def area_from_section_factor(self, sf):
        """Flow area for section factor *sf* — used to solve for normal depth."""
        return self._eval(_OP_AREA_OF_SF, sf, "section factor")

    def dsda(self, area):
        """Derivative of the section factor with respect to area, dS/dA."""
        return self._eval(_OP_DSDA, area, "area")

    def critical_depth(self, flow):
        """Critical depth for *flow*, in :attr:`flow_units`."""
        return self._eval(_OP_CRITICAL_DEPTH, flow, "flow")

    cdef object _eval(self, _Op op, object values, str name):
        """Scalar → float, array → ndarray, via one C call either way."""
        cdef double out = 0.0
        cdef int rc
        cdef np.ndarray[double, ndim=1] flat
        cdef np.ndarray[double, ndim=1] res
        cdef int n

        cdef double v
        if np.isscalar(values):
            v = float(values)
            with nogil:
                rc = _call_scalar(op, self._h, v, &out)
            if rc != 0:
                raise ValueError(
                    f"{name} {values!r} is out of range for this section "
                    "(must be a finite, non-negative number)"
                )
            return out

        arr = np.asarray(values, dtype=np.float64)
        flat = np.ascontiguousarray(arr.reshape(-1))
        n = <int>flat.shape[0]
        res = np.empty(n, dtype=np.float64)
        if n > 0:
            with nogil:
                rc = _call_array(op, self._h, <const double*>flat.data, n,
                                 <double*>res.data)
            if rc != 0:
                raise ValueError(
                    f"{name} array contains a value out of range for this "
                    "section (values must be finite and non-negative)"
                )
        return res.reshape(arr.shape)

    # ---- Properties -----------------------------------------------

    @property
    def units(self):
        """``'US'`` or ``'SI'`` — the unit system of every value here."""
        cdef int us = 0
        _check(swmm_xsect_get_units(self._h, &us, NULL))
        return "SI" if us == 1 else "US"

    @property
    def flow_units(self):
        """Flow units for :meth:`critical_depth` (``'CFS'``, ``'CMS'``, …)."""
        cdef int fu = 0
        _check(swmm_xsect_get_units(self._h, NULL, &fu))
        return _FLOW_UNIT_NAMES[fu]

    @property
    def full_depth(self):
        cdef double v = 0.0
        _check(swmm_xsect_full_properties(self._h, &v, NULL, NULL, NULL, NULL, NULL))
        return v

    @property
    def full_area(self):
        cdef double v = 0.0
        _check(swmm_xsect_full_properties(self._h, NULL, &v, NULL, NULL, NULL, NULL))
        return v

    @property
    def full_hyd_radius(self):
        cdef double v = 0.0
        _check(swmm_xsect_full_properties(self._h, NULL, NULL, &v, NULL, NULL, NULL))
        return v

    @property
    def max_width(self):
        cdef double v = 0.0
        _check(swmm_xsect_full_properties(self._h, NULL, NULL, NULL, &v, NULL, NULL))
        return v

    @property
    def full_section_factor(self):
        cdef double v = 0.0
        _check(swmm_xsect_full_properties(self._h, NULL, NULL, NULL, NULL, &v, NULL))
        return v

    @property
    def max_area(self):
        cdef double v = 0.0
        _check(swmm_xsect_full_properties(self._h, NULL, NULL, NULL, NULL, NULL, &v))
        return v

    @property
    def is_open(self):
        cdef int v = 0
        _check(swmm_xsect_is_open(self._h, &v))
        return v != 0

    def __repr__(self):
        return (f"XSectionGeometry(shape={self.shape!r}, "
                f"full_depth={self.full_depth:g}, units={self.units!r})")


# =============================================================================
# Module helpers
# =============================================================================

cdef XSectionGeometry _wrap(SWMM_XSect h):
    """Adopt an already-built handle without going through __init__."""
    cdef XSectionGeometry g = XSectionGeometry.__new__(XSectionGeometry)
    cdef int shape = 0
    g._h = h
    _check(swmm_xsect_get_shape(h, &shape))
    g.shape = _to_enum(shape)
    return g


def shape_name(shape):
    """Name of a cross-section shape code, e.g. ``'CIRCULAR'``.

    @param shape: An :class:`XSectShape` or int.
    @return: The shape's name.
    @raise ValueError: *shape* is not a valid code.
    """
    cdef int code = int(shape)
    cdef const char* nm = swmm_xsect_shape_name(code)
    if nm == NULL:
        raise ValueError(f"{shape!r} is not a valid cross-section shape code")
    return nm.decode("utf-8")
