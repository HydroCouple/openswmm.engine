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
Spatial / CRS / coordinates (Pythonic v1 surface)
=================================================

:author: Caleb Buahin
:copyright: Copyright (c) 2026 Caleb Buahin
:license: Apache-2.0

Reach via ``solver.spatial``. Provides CRS read/write, per-object
coordinate access, link vertices, and subcatchment polygons. Every
object selector accepts ``int | str``.

.. code-block:: python

    from openswmm.engine import Solver

    with Solver("model.inp") as s:
        s.spatial.crs                     # 'EPSG:4326' or WKT
        s.spatial.node_coord("J1")        # (x, y) tuple
        s.spatial.node_coords()           # np.ndarray shape (n_nodes, 2)
        s.spatial.link_vertices("C1")     # np.ndarray (n, 2)
        s.spatial.subcatchment_polygon("S1")  # np.ndarray (n, 2)
"""

# cython: language_level=3

from typing import Tuple

import numpy as np
cimport numpy as np

from ._common cimport *


cdef inline SWMM_Engine _h(solver):
    return <SWMM_Engine><size_t>solver.handle


cdef inline int _resolve_node(solver, key) except -1:
    return _resolve_index(_h(solver), key, swmm_node_index, swmm_node_count, "Node")


cdef inline int _resolve_link(solver, key) except -1:
    return _resolve_index(_h(solver), key, swmm_link_index, swmm_link_count, "Link")


cdef inline int _resolve_subcatch(solver, key) except -1:
    return _resolve_index(_h(solver), key,
                          swmm_subcatch_index, swmm_subcatch_count,
                          "Subcatchment")


cdef inline int _resolve_gage(solver, key) except -1:
    return _resolve_index(_h(solver), key, swmm_gage_index, swmm_gage_count, "Gage")


class Spatial:
    """``solver.spatial`` — geographic coordinates and CRS."""

    def __init__(self, solver):
        self._solver = solver

    # ------------------------------------------------------------------
    # CRS
    # ------------------------------------------------------------------

    @property
    def crs(self) -> str:
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef char buf[256]
        _check(swmm_spatial_get_crs(h, buf, 256))
        return buf.decode('utf-8')

    @crs.setter
    def crs(self, str value) -> None:
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef bytes b = value.encode('utf-8')
        _check(swmm_spatial_set_crs(h, b))

    # ------------------------------------------------------------------
    # [MAP] and GUI annotation sections
    # ------------------------------------------------------------------

    @property
    def map_dimensions(self) -> Tuple[float, float, float, float]:
        """``[MAP] DIMENSIONS`` as ``(x1, y1, x2, y2)``."""
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double x1 = 0.0, y1 = 0.0, x2 = 0.0, y2 = 0.0
        _check(swmm_spatial_get_map_dimensions(h, &x1, &y1, &x2, &y2))
        return (x1, y1, x2, y2)

    @map_dimensions.setter
    def map_dimensions(self, value) -> None:
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        x1, y1, x2, y2 = value
        _check(swmm_spatial_set_map_dimensions(h, x1, y1, x2, y2))

    @property
    def map_units(self) -> str:
        """``[MAP] UNITS`` (upper case; ``''`` when not stated)."""
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef char buf[64]
        _check(swmm_spatial_get_map_units(h, buf, 64))
        return buf.decode('utf-8')

    @map_units.setter
    def map_units(self, str value) -> None:
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef bytes b = value.encode('utf-8')
        _check(swmm_spatial_set_map_units(h, b))

    def get_annotation(self, str section) -> str:
        """Lines of ``LABELS``, ``BACKDROP``, ``PROFILE`` or ``PROFILES``,
        verbatim and joined by newlines (the engine never interprets them)."""
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef bytes sec = section.encode('utf-8')
        cdef int required = 0
        _check(swmm_spatial_get_annotation(h, sec, NULL, 0, &required))
        cdef bytearray buf = bytearray(required)
        _check(swmm_spatial_get_annotation(h, sec, buf, required, NULL))
        return bytes(buf).split(b'\0', 1)[0].decode('utf-8')

    def set_annotation(self, str section, text) -> None:
        """Replace an annotation section's lines; ``''`` or ``None`` removes it."""
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef bytes sec = section.encode('utf-8')
        cdef bytes t = (text or '').encode('utf-8')
        _check(swmm_spatial_set_annotation(h, sec, t))

    # ------------------------------------------------------------------
    # Node coords
    # ------------------------------------------------------------------

    def node_coord(self, node) -> Tuple[float, float]:
        """Return the ``(x, y)`` coordinate of *node*."""
        cdef int i = _resolve_node(self._solver, node)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double x = 0.0, y = 0.0
        _check(swmm_spatial_get_node_coord(h, i, &x, &y))
        return (x, y)

    def set_node_coord(self, node, double x, double y) -> None:
        """Set the ``(x, y)`` coordinate of *node*."""
        cdef int i = _resolve_node(self._solver, node)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_spatial_set_node_coord(h, i, x, y))

    def node_coords(self):
        """All node coordinates as a ``(n_nodes, 2)`` ``float64`` array."""
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int n = swmm_node_count(h)
        cdef np.ndarray[double, ndim=1] xs = np.empty(n, dtype=np.float64)
        cdef np.ndarray[double, ndim=1] ys = np.empty(n, dtype=np.float64)
        if n > 0:  # the C bulk calls refuse a zero count
            _check(swmm_spatial_get_node_coords_bulk(
                h, <double*>xs.data, <double*>ys.data, n))
        return np.column_stack((xs, ys))

    def set_node_coords(self, coords) -> None:
        """``coords`` is shape ``(n_nodes, 2)`` ``float64``."""
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int n = swmm_node_count(h)
        cdef np.ndarray[double, ndim=2] arr = np.ascontiguousarray(
            coords, dtype=np.float64)
        if arr.shape[0] != n or arr.shape[1] != 2:
            raise ValueError(
                f"node coords array must have shape ({n}, 2), got "
                f"({arr.shape[0]}, {arr.shape[1]})")
        cdef np.ndarray[double, ndim=1] xs = np.ascontiguousarray(arr[:, 0])
        cdef np.ndarray[double, ndim=1] ys = np.ascontiguousarray(arr[:, 1])
        if n > 0:
            _check(swmm_spatial_set_node_coords_bulk(
                h, <const double*>xs.data, <const double*>ys.data, n))

    # ------------------------------------------------------------------
    # Link coords + vertices
    # ------------------------------------------------------------------

    def link_coord(self, link) -> Tuple[float, float]:
        """Return the ``(x, y)`` mid-point coordinate of *link*."""
        cdef int i = _resolve_link(self._solver, link)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double x = 0.0, y = 0.0
        _check(swmm_spatial_get_link_coord(h, i, &x, &y))
        return (x, y)

    def set_link_coord(self, link, double x, double y) -> None:
        """Set the ``(x, y)`` mid-point coordinate of *link*."""
        cdef int i = _resolve_link(self._solver, link)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_spatial_set_link_coord(h, i, x, y))

    def link_vertices(self, link):
        """``(n, 2)`` ``float64`` numpy array of polyline vertices."""
        cdef int i = _resolve_link(self._solver, link)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int n = 0
        _check(swmm_spatial_get_link_vertex_count(h, i, &n))
        cdef np.ndarray[double, ndim=1] xs = np.empty(n, dtype=np.float64)
        cdef np.ndarray[double, ndim=1] ys = np.empty(n, dtype=np.float64)
        _check(swmm_spatial_get_link_vertices(
            h, i, <double*>xs.data, <double*>ys.data, n))
        return np.column_stack((xs, ys))

    def set_link_vertices(self, link, vertices) -> None:
        """Set the polyline vertices of *link* from an ``(N, 2)`` array."""
        cdef int i = _resolve_link(self._solver, link)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef np.ndarray[double, ndim=2] arr = np.ascontiguousarray(
            vertices, dtype=np.float64)
        if arr.ndim != 2 or arr.shape[1] != 2:
            raise ValueError(
                f"link vertices must be shape (n, 2); got "
                f"({arr.shape[0]}, {arr.shape[1]})")
        cdef int n = arr.shape[0]
        cdef np.ndarray[double, ndim=1] xs = np.ascontiguousarray(arr[:, 0])
        cdef np.ndarray[double, ndim=1] ys = np.ascontiguousarray(arr[:, 1])
        _check(swmm_spatial_set_link_vertices(
            h, i, <const double*>xs.data, <const double*>ys.data, n))

    # ------------------------------------------------------------------
    # Subcatchment coords + polygons
    # ------------------------------------------------------------------

    def subcatchment_coord(self, sub) -> Tuple[float, float]:
        """Return the ``(x, y)`` centroid coordinate of *sub*."""
        cdef int i = _resolve_subcatch(self._solver, sub)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double x = 0.0, y = 0.0
        _check(swmm_spatial_get_subcatch_coord(h, i, &x, &y))
        return (x, y)

    def set_subcatchment_coord(self, sub, double x, double y) -> None:
        """Set the ``(x, y)`` centroid coordinate of *sub*."""
        cdef int i = _resolve_subcatch(self._solver, sub)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_spatial_set_subcatch_coord(h, i, x, y))

    def subcatchment_polygon(self, sub):
        """Return the boundary polygon of *sub* as an ``(N, 2)`` array."""
        cdef int i = _resolve_subcatch(self._solver, sub)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int n = 0
        _check(swmm_spatial_get_subcatch_polygon_count(h, i, &n))
        cdef np.ndarray[double, ndim=1] xs = np.empty(n, dtype=np.float64)
        cdef np.ndarray[double, ndim=1] ys = np.empty(n, dtype=np.float64)
        _check(swmm_spatial_get_subcatch_polygon(
            h, i, <double*>xs.data, <double*>ys.data, n))
        return np.column_stack((xs, ys))

    def set_subcatchment_polygon(self, sub, polygon) -> None:
        """Set the boundary polygon of *sub* from an ``(N, 2)`` array."""
        cdef int i = _resolve_subcatch(self._solver, sub)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef np.ndarray[double, ndim=2] arr = np.ascontiguousarray(
            polygon, dtype=np.float64)
        if arr.ndim != 2 or arr.shape[1] != 2:
            raise ValueError(
                f"polygon must be shape (n, 2); got "
                f"({arr.shape[0]}, {arr.shape[1]})")
        cdef int n = arr.shape[0]
        cdef np.ndarray[double, ndim=1] xs = np.ascontiguousarray(arr[:, 0])
        cdef np.ndarray[double, ndim=1] ys = np.ascontiguousarray(arr[:, 1])
        _check(swmm_spatial_set_subcatch_polygon(
            h, i, <const double*>xs.data, <const double*>ys.data, n))

    # ------------------------------------------------------------------
    # Gage coords
    # ------------------------------------------------------------------

    def gage_coord(self, gage) -> Tuple[float, float]:
        """Return the ``(x, y)`` coordinate of rain *gage*."""
        cdef int i = _resolve_gage(self._solver, gage)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double x = 0.0, y = 0.0
        _check(swmm_spatial_get_gage_coord(h, i, &x, &y))
        return (x, y)

    def set_gage_coord(self, gage, double x, double y) -> None:
        """Set the ``(x, y)`` coordinate of rain *gage*."""
        cdef int i = _resolve_gage(self._solver, gage)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_spatial_set_gage_coord(h, i, x, y))

    def __repr__(self) -> str:
        try:
            return f"<Spatial crs={self.crs!r}>"
        except Exception:
            return "<Spatial (no CRS / closed)>"
