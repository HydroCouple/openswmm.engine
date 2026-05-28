"""
Spatial / CRS / coordinates (Pythonic v1 surface)
=================================================

:author: Caleb Buahin
:copyright: Copyright (c) 2026 Caleb Buahin
:license: MIT

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
    # Node coords
    # ------------------------------------------------------------------

    def node_coord(self, node) -> Tuple[float, float]:
        cdef int i = _resolve_node(self._solver, node)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double x = 0.0, y = 0.0
        _check(swmm_spatial_get_node_coord(h, i, &x, &y))
        return (x, y)

    def set_node_coord(self, node, double x, double y) -> None:
        cdef int i = _resolve_node(self._solver, node)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_spatial_set_node_coord(h, i, x, y))

    def node_coords(self):
        """All node coordinates as a ``(n_nodes, 2)`` ``float64`` array."""
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int n = swmm_node_count(h)
        cdef np.ndarray[double, ndim=1] xs = np.empty(n, dtype=np.float64)
        cdef np.ndarray[double, ndim=1] ys = np.empty(n, dtype=np.float64)
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
        _check(swmm_spatial_set_node_coords_bulk(
            h, <const double*>xs.data, <const double*>ys.data, n))

    # ------------------------------------------------------------------
    # Link coords + vertices
    # ------------------------------------------------------------------

    def link_coord(self, link) -> Tuple[float, float]:
        cdef int i = _resolve_link(self._solver, link)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double x = 0.0, y = 0.0
        _check(swmm_spatial_get_link_coord(h, i, &x, &y))
        return (x, y)

    def set_link_coord(self, link, double x, double y) -> None:
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
        cdef int i = _resolve_subcatch(self._solver, sub)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double x = 0.0, y = 0.0
        _check(swmm_spatial_get_subcatch_coord(h, i, &x, &y))
        return (x, y)

    def set_subcatchment_coord(self, sub, double x, double y) -> None:
        cdef int i = _resolve_subcatch(self._solver, sub)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_spatial_set_subcatch_coord(h, i, x, y))

    def subcatchment_polygon(self, sub):
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
        cdef int i = _resolve_gage(self._solver, gage)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double x = 0.0, y = 0.0
        _check(swmm_spatial_get_gage_coord(h, i, &x, &y))
        return (x, y)

    def set_gage_coord(self, gage, double x, double y) -> None:
        cdef int i = _resolve_gage(self._solver, gage)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_spatial_set_gage_coord(h, i, x, y))

    def __repr__(self) -> str:
        try:
            return f"<Spatial crs={self.crs!r}>"
        except Exception:
            return "<Spatial (no CRS / closed)>"
