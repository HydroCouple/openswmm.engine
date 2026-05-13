"""
Spatial Coordinate Access
=========================

:author: Caleb Buahin
:copyright: Copyright (c) 2026 Caleb Buahin
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

    Provides scalar and bulk numpy-array accessors for nodes, links
    (single coordinates and polyline vertices), subcatchments (centroid
    and polygon outlines), and rain gages, plus a project-wide
    coordinate reference system.

    @ivar _solver: The L{openswmm.engine.Solver} instance whose engine
        handle is used for every C API call.
    """

    def __init__(self, solver):
        """Construct a L{Spatial} accessor bound to a solver.

        @param solver: An active L{openswmm.engine.Solver} instance. The
            solver must remain alive for the lifetime of this object.
        @type solver: Solver
        """
        self._solver = solver

    # ====================================================================
    # CRS / projection
    # ====================================================================

    def set_crs(self, str crs):
        """Set the project coordinate reference system.

        @param crs: CRS identifier (e.g. an EPSG code such as
            C{"EPSG:4326"} or a WKT string).
        @type crs: str
        @raise RuntimeError: If the C API rejects the CRS string.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef bytes b = crs.encode('utf-8')
        _check(swmm_spatial_set_crs(h, b))

    def get_crs(self) -> str:
        """Return the project coordinate reference system.

        @return: CRS identifier string.
        @rtype: str
        @raise RuntimeError: If the C API call fails.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef char buf[256]
        _check(swmm_spatial_get_crs(h, buf, 256))
        return buf.decode('utf-8')

    # ====================================================================
    # Node coordinates
    # ====================================================================

    def set_node_coord(self, int idx, double x, double y):
        """Set the projected (X, Y) coordinate of a node.

        @param idx: Node index.
        @type idx: int
        @param x: X coordinate in the project CRS.
        @type x: float
        @param y: Y coordinate in the project CRS.
        @type y: float
        @raise RuntimeError: If the C API rejects the assignment.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_spatial_set_node_coord(h, idx, x, y))

    def get_node_coord(self, int idx) -> tuple:
        """Return the projected (X, Y) coordinate of a node.

        @param idx: Node index.
        @type idx: int
        @return: Tuple C{(x, y)} of node coordinates.
        @rtype: tuple
        @raise RuntimeError: If the C API call fails.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double x = 0.0
        cdef double y = 0.0
        _check(swmm_spatial_get_node_coord(h, idx, &x, &y))
        return (x, y)

    def get_node_coords_bulk(self) -> tuple:
        """Return coordinates for all nodes as NumPy arrays.

        @return: Tuple C{(x_array, y_array)}, each of shape
            C{(n_nodes,)} with dtype C{float64}.
        @rtype: tuple
        @raise RuntimeError: If the C API call fails.
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

        @param x: X coordinates of shape C{(n_nodes,)}, dtype C{float64}.
        @type x: np.ndarray
        @param y: Y coordinates of shape C{(n_nodes,)}, dtype C{float64}.
        @type y: np.ndarray
        @raise RuntimeError: If the C API rejects the bulk assignment.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int n = swmm_node_count(h)
        _check(swmm_spatial_set_node_coords_bulk(
            h, <const double*>x.data, <const double*>y.data, n))

    # ====================================================================
    # Link coordinates
    # ====================================================================

    def set_link_coord(self, int idx, double x, double y):
        """Set the representative (X, Y) coordinate of a link.

        @param idx: Link index.
        @type idx: int
        @param x: X coordinate in the project CRS.
        @type x: float
        @param y: Y coordinate in the project CRS.
        @type y: float
        @raise RuntimeError: If the C API rejects the assignment.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_spatial_set_link_coord(h, idx, x, y))

    def get_link_coord(self, int idx) -> tuple:
        """Return the representative (X, Y) coordinate of a link.

        @param idx: Link index.
        @type idx: int
        @return: Tuple C{(x, y)} of the link's representative coordinate.
        @rtype: tuple
        @raise RuntimeError: If the C API call fails.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double x = 0.0
        cdef double y = 0.0
        _check(swmm_spatial_get_link_coord(h, idx, &x, &y))
        return (x, y)

    # ====================================================================
    # Link vertices
    # ====================================================================

    def set_link_vertices(self, int idx, np.ndarray[double, ndim=1] x,
                          np.ndarray[double, ndim=1] y):
        """Set the polyline vertices of a link.

        The C{x} and C{y} arrays must have the same length and define the
        ordered polyline geometry of the link.

        @param idx: Link index.
        @type idx: int
        @param x: X coordinates of vertices, dtype C{float64}.
        @type x: np.ndarray
        @param y: Y coordinates of vertices, dtype C{float64}.
        @type y: np.ndarray
        @raise RuntimeError: If the C API rejects the geometry.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int n = len(x)
        _check(swmm_spatial_set_link_vertices(
            h, idx, <const double*>x.data, <const double*>y.data, n))

    def get_link_vertex_count(self, int idx) -> int:
        """Return the number of ordered polyline vertices for a link.

        The returned count includes the upstream node coordinate at index
        C{0}, any interior vertices, and the downstream node coordinate
        at the last index.

        @param idx: Link index.
        @type idx: int
        @return: Vertex count.
        @rtype: int
        @raise RuntimeError: If the C API call fails.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int count = 0
        _check(swmm_spatial_get_link_vertex_count(h, idx, &count))
        return count

    def get_link_vertices(self, int idx) -> tuple:
        """Return ordered polyline vertices of a link as NumPy arrays.

        Ordering is upstream node, interior vertices, downstream node.

        @param idx: Link index.
        @type idx: int
        @return: Tuple C{(x_array, y_array)}, each of shape
            C{(get_link_vertex_count(idx),)} with dtype C{float64}.
        @rtype: tuple
        @raise RuntimeError: If the C API call fails.
        @see: L{get_link_vertex_count}
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int count = 0
        _check(swmm_spatial_get_link_vertex_count(h, idx, &count))
        cdef np.ndarray[double, ndim=1] x_buf = np.empty(count, dtype=np.float64)
        cdef np.ndarray[double, ndim=1] y_buf = np.empty(count, dtype=np.float64)
        _check(swmm_spatial_get_link_vertices(
            h, idx, <double*>x_buf.data, <double*>y_buf.data, count))
        return (x_buf, y_buf)

    # ====================================================================
    # Subcatchment polygons
    # ====================================================================

    def set_subcatch_coord(self, int idx, double x, double y):
        """Set the centroid coordinate of a subcatchment.

        @param idx: Subcatchment index.
        @type idx: int
        @param x: X coordinate in the project CRS.
        @type x: float
        @param y: Y coordinate in the project CRS.
        @type y: float
        @raise RuntimeError: If the C API rejects the assignment.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_spatial_set_subcatch_coord(h, idx, x, y))

    def get_subcatch_coord(self, int idx) -> tuple:
        """Return the centroid coordinate of a subcatchment.

        @param idx: Subcatchment index.
        @type idx: int
        @return: Tuple C{(x, y)} centroid coordinate.
        @rtype: tuple
        @raise RuntimeError: If the C API call fails.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double x = 0.0
        cdef double y = 0.0
        _check(swmm_spatial_get_subcatch_coord(h, idx, &x, &y))
        return (x, y)

    def set_subcatch_polygon(self, int idx, np.ndarray[double, ndim=1] x,
                             np.ndarray[double, ndim=1] y):
        """Set the polygon vertices of a subcatchment.

        @param idx: Subcatchment index.
        @type idx: int
        @param x: X coordinates of polygon vertices, dtype C{float64}.
        @type x: np.ndarray
        @param y: Y coordinates of polygon vertices, dtype C{float64}.
        @type y: np.ndarray
        @raise RuntimeError: If the C API rejects the polygon.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int n = len(x)
        _check(swmm_spatial_set_subcatch_polygon(
            h, idx, <const double*>x.data, <const double*>y.data, n))

    def get_subcatch_polygon_count(self, int idx) -> int:
        """Return the number of polygon vertices for a subcatchment.

        @param idx: Subcatchment index.
        @type idx: int
        @return: Vertex count.
        @rtype: int
        @raise RuntimeError: If the C API call fails.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int count = 0
        _check(swmm_spatial_get_subcatch_polygon_count(h, idx, &count))
        return count

    def get_subcatch_polygon(self, int idx) -> tuple:
        """Return the polygon vertices of a subcatchment as NumPy arrays.

        @param idx: Subcatchment index.
        @type idx: int
        @return: Tuple C{(x_array, y_array)}, each of shape
            C{(get_subcatch_polygon_count(idx),)} with dtype C{float64}.
        @rtype: tuple
        @raise RuntimeError: If the C API call fails.
        @see: L{get_subcatch_polygon_count}
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int count = 0
        _check(swmm_spatial_get_subcatch_polygon_count(h, idx, &count))
        cdef np.ndarray[double, ndim=1] x_buf = np.empty(count, dtype=np.float64)
        cdef np.ndarray[double, ndim=1] y_buf = np.empty(count, dtype=np.float64)
        _check(swmm_spatial_get_subcatch_polygon(
            h, idx, <double*>x_buf.data, <double*>y_buf.data, count))
        return (x_buf, y_buf)

    # ====================================================================
    # Bulk geometry access (rain gages)
    # ====================================================================

    def set_gage_coord(self, int idx, double x, double y):
        """Set the coordinate of a rain gage.

        @param idx: Gage index.
        @type idx: int
        @param x: X coordinate in the project CRS.
        @type x: float
        @param y: Y coordinate in the project CRS.
        @type y: float
        @raise RuntimeError: If the C API rejects the assignment.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_spatial_set_gage_coord(h, idx, x, y))

    def get_gage_coord(self, int idx) -> tuple:
        """Return the coordinate of a rain gage.

        @param idx: Gage index.
        @type idx: int
        @return: Tuple C{(x, y)} of the gage's coordinate.
        @rtype: tuple
        @raise RuntimeError: If the C API call fails.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double x = 0.0
        cdef double y = 0.0
        _check(swmm_spatial_get_gage_coord(h, idx, &x, &y))
        return (x, y)
