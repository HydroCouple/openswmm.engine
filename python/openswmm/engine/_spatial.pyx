"""
Spatial Coordinate Access
=========================

:author: Caleb Buahin
:copyright: Copyright (c) HydroCouple 2026
:license: MIT

The :class:`Spatial` class provides access to geographic coordinates of
nodes, links, subcatchments, and rain gages.

.. code-block:: python

    from openswmm.engine import Solver
    from openswmm.engine._spatial import Spatial

    with Solver("model.inp", "model.rpt", "model.out") as s:
        sp = Spatial(s)
        x, y = sp.get_node_coord(0)
        sp.set_node_coord(0, x + 100.0, y)
"""

# cython: language_level=3

import numpy as np
cimport numpy as np

from ._common cimport *


class Spatial:
    """Access geographic coordinates for model elements.

    :param solver: An active :class:`~openswmm.engine.Solver` instance.
    """

    def __init__(self, solver):
        self._solver = solver

    # ------------------------------------------------------------------
    # CRS
    # ------------------------------------------------------------------

    def set_crs(self, str crs):
        """Set the coordinate reference system string.

        :param crs: CRS identifier (e.g. EPSG code or WKT).
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef bytes b = crs.encode('utf-8')
        _check(swmm_spatial_set_crs(h, b))

    def get_crs(self) -> str:
        """Return the coordinate reference system string.

        :returns: CRS identifier.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef char buf[256]
        _check(swmm_spatial_get_crs(h, buf, 256))
        return buf.decode('utf-8')

    # ------------------------------------------------------------------
    # Node coordinates
    # ------------------------------------------------------------------

    def set_node_coord(self, int idx, double x, double y):
        """Set the coordinate of a node.

        :param idx: Node index.
        :param x: X coordinate.
        :param y: Y coordinate.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_spatial_set_node_coord(h, idx, x, y))

    def get_node_coord(self, int idx) -> tuple:
        """Return the coordinate of a node.

        :param idx: Node index.
        :returns: Tuple of (x, y).
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double x = 0.0
        cdef double y = 0.0
        _check(swmm_spatial_get_node_coord(h, idx, &x, &y))
        return (x, y)

    def get_node_coords_bulk(self) -> tuple:
        """Return coordinates for all nodes as NumPy arrays.

        :returns: Tuple of (x_array, y_array), each with dtype ``float64``.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int n = swmm_node_count(h)
        cdef np.ndarray[double, ndim=1] x_buf = np.empty(n, dtype=np.float64)
        cdef np.ndarray[double, ndim=1] y_buf = np.empty(n, dtype=np.float64)
        _check(swmm_spatial_get_node_coords_bulk(
            h, <double*>x_buf.data, <double*>y_buf.data, n))
        return (x_buf, y_buf)

    def set_node_coords_bulk(self, np.ndarray[double, ndim=1] x,
                             np.ndarray[double, ndim=1] y):
        """Set coordinates for all nodes from NumPy arrays.

        :param x: X coordinates array of shape ``(n_nodes,)``.
        :param y: Y coordinates array of shape ``(n_nodes,)``.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int n = swmm_node_count(h)
        _check(swmm_spatial_set_node_coords_bulk(
            h, <const double*>x.data, <const double*>y.data, n))

    # ------------------------------------------------------------------
    # Link coordinates
    # ------------------------------------------------------------------

    def set_link_coord(self, int idx, double x, double y):
        """Set the coordinate of a link.

        :param idx: Link index.
        :param x: X coordinate.
        :param y: Y coordinate.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_spatial_set_link_coord(h, idx, x, y))

    def get_link_coord(self, int idx) -> tuple:
        """Return the coordinate of a link.

        :param idx: Link index.
        :returns: Tuple of (x, y).
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double x = 0.0
        cdef double y = 0.0
        _check(swmm_spatial_get_link_coord(h, idx, &x, &y))
        return (x, y)

    # ------------------------------------------------------------------
    # Link vertices
    # ------------------------------------------------------------------

    def set_link_vertices(self, int idx, np.ndarray[double, ndim=1] x,
                          np.ndarray[double, ndim=1] y):
        """Set the vertices of a link.

        :param idx: Link index.
        :param x: X coordinates of vertices.
        :param y: Y coordinates of vertices.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int n = len(x)
        _check(swmm_spatial_set_link_vertices(
            h, idx, <const double*>x.data, <const double*>y.data, n))

    def get_link_vertex_count(self, int idx) -> int:
        """Return the number of vertices for a link.

        :param idx: Link index.
        :returns: Vertex count.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int count = 0
        _check(swmm_spatial_get_link_vertex_count(h, idx, &count))
        return count

    def get_link_vertices(self, int idx) -> tuple:
        """Return the vertices of a link as NumPy arrays.

        :param idx: Link index.
        :returns: Tuple of (x_array, y_array), each with dtype ``float64``.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int count = 0
        _check(swmm_spatial_get_link_vertex_count(h, idx, &count))
        cdef np.ndarray[double, ndim=1] x_buf = np.empty(count, dtype=np.float64)
        cdef np.ndarray[double, ndim=1] y_buf = np.empty(count, dtype=np.float64)
        _check(swmm_spatial_get_link_vertices(
            h, idx, <double*>x_buf.data, <double*>y_buf.data, count))
        return (x_buf, y_buf)

    # ------------------------------------------------------------------
    # Subcatchment coordinates
    # ------------------------------------------------------------------

    def set_subcatch_coord(self, int idx, double x, double y):
        """Set the centroid coordinate of a subcatchment.

        :param idx: Subcatchment index.
        :param x: X coordinate.
        :param y: Y coordinate.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_spatial_set_subcatch_coord(h, idx, x, y))

    def get_subcatch_coord(self, int idx) -> tuple:
        """Return the centroid coordinate of a subcatchment.

        :param idx: Subcatchment index.
        :returns: Tuple of (x, y).
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double x = 0.0
        cdef double y = 0.0
        _check(swmm_spatial_get_subcatch_coord(h, idx, &x, &y))
        return (x, y)

    # ------------------------------------------------------------------
    # Subcatchment polygon
    # ------------------------------------------------------------------

    def set_subcatch_polygon(self, int idx, np.ndarray[double, ndim=1] x,
                             np.ndarray[double, ndim=1] y):
        """Set the polygon vertices of a subcatchment.

        :param idx: Subcatchment index.
        :param x: X coordinates of polygon vertices.
        :param y: Y coordinates of polygon vertices.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int n = len(x)
        _check(swmm_spatial_set_subcatch_polygon(
            h, idx, <const double*>x.data, <const double*>y.data, n))

    def get_subcatch_polygon_count(self, int idx) -> int:
        """Return the number of polygon vertices for a subcatchment.

        :param idx: Subcatchment index.
        :returns: Vertex count.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int count = 0
        _check(swmm_spatial_get_subcatch_polygon_count(h, idx, &count))
        return count

    def get_subcatch_polygon(self, int idx) -> tuple:
        """Return the polygon vertices of a subcatchment as NumPy arrays.

        :param idx: Subcatchment index.
        :returns: Tuple of (x_array, y_array), each with dtype ``float64``.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int count = 0
        _check(swmm_spatial_get_subcatch_polygon_count(h, idx, &count))
        cdef np.ndarray[double, ndim=1] x_buf = np.empty(count, dtype=np.float64)
        cdef np.ndarray[double, ndim=1] y_buf = np.empty(count, dtype=np.float64)
        _check(swmm_spatial_get_subcatch_polygon(
            h, idx, <double*>x_buf.data, <double*>y_buf.data, count))
        return (x_buf, y_buf)

    # ------------------------------------------------------------------
    # Gage coordinates
    # ------------------------------------------------------------------

    def set_gage_coord(self, int idx, double x, double y):
        """Set the coordinate of a rain gage.

        :param idx: Gage index.
        :param x: X coordinate.
        :param y: Y coordinate.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_spatial_set_gage_coord(h, idx, x, y))

    def get_gage_coord(self, int idx) -> tuple:
        """Return the coordinate of a rain gage.

        :param idx: Gage index.
        :returns: Tuple of (x, y).
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double x = 0.0
        cdef double y = 0.0
        _check(swmm_spatial_get_gage_coord(h, idx, &x, &y))
        return (x, y)
