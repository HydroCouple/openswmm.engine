"""
Spatial Coordinate Access
=========================

:author: Caleb Buahin
:copyright: Copyright (c) 2026 Caleb Buahin
:license: MIT

Type stubs for :mod:`openswmm.engine._spatial`.

The :class:`Spatial` class provides access to geographic coordinates of
nodes, links, subcatchments, and rain gages.
"""

import numpy as np
import numpy.typing as npt

from ._solver import Solver


class Spatial:
    """Access geographic coordinates for model elements.

    Provides scalar and bulk numpy-array accessors for nodes, links
    (single coordinates and polyline vertices), subcatchments (centroid
    and polygon outlines), and rain gages, plus a project-wide
    coordinate reference system.

    Example::

        from openswmm.engine import Solver
        from openswmm.engine._spatial import Spatial

        with Solver("model.inp", "model.rpt", "model.out") as s:
            sp = Spatial(s)
            x, y = sp.get_node_coord(0)
            sp.set_node_coord(0, x + 100.0, y)

    @ivar _solver: The L{Solver} instance whose engine handle is used for
        every C API call.
    """

    def __init__(self, solver: Solver) -> None:
        """Construct a L{Spatial} accessor bound to a solver.

        @param solver: An active L{Solver} instance. The solver must remain
            alive for the lifetime of this object.
        @type solver: Solver
        """
        ...

    # ====================================================================
    # CRS / projection
    # ====================================================================

    def set_crs(self, crs: str) -> None:
        """Set the project coordinate reference system.

        @param crs: CRS identifier (e.g. an EPSG code such as
            C{"EPSG:4326"} or a WKT string).
        @type crs: str
        @raise RuntimeError: If the C API rejects the CRS string.
        """
        ...

    def get_crs(self) -> str:
        """Return the project coordinate reference system.

        @return: CRS identifier string.
        @rtype: str
        @raise RuntimeError: If the C API call fails.
        """
        ...

    # ====================================================================
    # Node coordinates
    # ====================================================================

    def set_node_coord(self, idx: int, x: float, y: float) -> None:
        """Set the projected (X, Y) coordinate of a node.

        @param idx: Node index.
        @type idx: int
        @param x: X coordinate in the project CRS.
        @type x: float
        @param y: Y coordinate in the project CRS.
        @type y: float
        @raise RuntimeError: If the C API rejects the assignment.
        """
        ...

    def get_node_coord(self, idx: int) -> tuple[float, float]:
        """Return the projected (X, Y) coordinate of a node.

        @param idx: Node index.
        @type idx: int
        @return: Tuple C{(x, y)} of node coordinates.
        @rtype: tuple[float, float]
        @raise RuntimeError: If the C API call fails.
        """
        ...

    def get_node_coords_bulk(
        self,
    ) -> tuple[npt.NDArray[np.float64], npt.NDArray[np.float64]]:
        """Return coordinates for all nodes as NumPy arrays.

        @return: Tuple C{(x_array, y_array)}, each of shape
            C{(n_nodes,)} with dtype C{float64}.
        @rtype: tuple[np.ndarray, np.ndarray]
        @raise RuntimeError: If the C API call fails.
        """
        ...

    def set_node_coords_bulk(
        self,
        x: npt.NDArray[np.float64],
        y: npt.NDArray[np.float64],
    ) -> None:
        """Set coordinates for all nodes from NumPy arrays.

        @param x: X coordinates of shape C{(n_nodes,)}, dtype C{float64}.
        @type x: np.ndarray
        @param y: Y coordinates of shape C{(n_nodes,)}, dtype C{float64}.
        @type y: np.ndarray
        @raise RuntimeError: If the C API rejects the bulk assignment.
        """
        ...

    # ====================================================================
    # Link coordinates
    # ====================================================================

    def set_link_coord(self, idx: int, x: float, y: float) -> None:
        """Set the representative (X, Y) coordinate of a link.

        @param idx: Link index.
        @type idx: int
        @param x: X coordinate in the project CRS.
        @type x: float
        @param y: Y coordinate in the project CRS.
        @type y: float
        @raise RuntimeError: If the C API rejects the assignment.
        """
        ...

    def get_link_coord(self, idx: int) -> tuple[float, float]:
        """Return the representative (X, Y) coordinate of a link.

        @param idx: Link index.
        @type idx: int
        @return: Tuple C{(x, y)} of the link's representative coordinate.
        @rtype: tuple[float, float]
        @raise RuntimeError: If the C API call fails.
        """
        ...

    # ====================================================================
    # Link vertices
    # ====================================================================

    def set_link_vertices(
        self,
        idx: int,
        x: npt.NDArray[np.float64],
        y: npt.NDArray[np.float64],
    ) -> None:
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
        ...

    def get_link_vertex_count(self, idx: int) -> int:
        """Return the number of ordered polyline vertices for a link.

        The count includes the upstream node coordinate at index C{0},
        any interior vertices, and the downstream node coordinate at the
        last index.

        @param idx: Link index.
        @type idx: int
        @return: Vertex count.
        @rtype: int
        @raise RuntimeError: If the C API call fails.
        """
        ...

    def get_link_vertices(
        self, idx: int
    ) -> tuple[npt.NDArray[np.float64], npt.NDArray[np.float64]]:
        """Return ordered polyline vertices of a link as NumPy arrays.

        Ordering is upstream node, interior vertices, downstream node.

        @param idx: Link index.
        @type idx: int
        @return: Tuple C{(x_array, y_array)}, each of shape
            C{(get_link_vertex_count(idx),)} with dtype C{float64}.
        @rtype: tuple[np.ndarray, np.ndarray]
        @raise RuntimeError: If the C API call fails.
        @see: L{get_link_vertex_count}
        """
        ...

    # ====================================================================
    # Subcatchment polygons
    # ====================================================================

    def set_subcatch_coord(self, idx: int, x: float, y: float) -> None:
        """Set the centroid coordinate of a subcatchment.

        @param idx: Subcatchment index.
        @type idx: int
        @param x: X coordinate in the project CRS.
        @type x: float
        @param y: Y coordinate in the project CRS.
        @type y: float
        @raise RuntimeError: If the C API rejects the assignment.
        """
        ...

    def get_subcatch_coord(self, idx: int) -> tuple[float, float]:
        """Return the centroid coordinate of a subcatchment.

        @param idx: Subcatchment index.
        @type idx: int
        @return: Tuple C{(x, y)} centroid coordinate.
        @rtype: tuple[float, float]
        @raise RuntimeError: If the C API call fails.
        """
        ...

    def set_subcatch_polygon(
        self,
        idx: int,
        x: npt.NDArray[np.float64],
        y: npt.NDArray[np.float64],
    ) -> None:
        """Set the polygon vertices of a subcatchment.

        @param idx: Subcatchment index.
        @type idx: int
        @param x: X coordinates of polygon vertices, dtype C{float64}.
        @type x: np.ndarray
        @param y: Y coordinates of polygon vertices, dtype C{float64}.
        @type y: np.ndarray
        @raise RuntimeError: If the C API rejects the polygon.
        """
        ...

    def get_subcatch_polygon_count(self, idx: int) -> int:
        """Return the number of polygon vertices for a subcatchment.

        @param idx: Subcatchment index.
        @type idx: int
        @return: Vertex count.
        @rtype: int
        @raise RuntimeError: If the C API call fails.
        """
        ...

    def get_subcatch_polygon(
        self, idx: int
    ) -> tuple[npt.NDArray[np.float64], npt.NDArray[np.float64]]:
        """Return the polygon vertices of a subcatchment as NumPy arrays.

        @param idx: Subcatchment index.
        @type idx: int
        @return: Tuple C{(x_array, y_array)}, each of shape
            C{(get_subcatch_polygon_count(idx),)} with dtype C{float64}.
        @rtype: tuple[np.ndarray, np.ndarray]
        @raise RuntimeError: If the C API call fails.
        @see: L{get_subcatch_polygon_count}
        """
        ...

    # ====================================================================
    # Bulk geometry access (rain gages)
    # ====================================================================

    def set_gage_coord(self, idx: int, x: float, y: float) -> None:
        """Set the coordinate of a rain gage.

        @param idx: Gage index.
        @type idx: int
        @param x: X coordinate in the project CRS.
        @type x: float
        @param y: Y coordinate in the project CRS.
        @type y: float
        @raise RuntimeError: If the C API rejects the assignment.
        """
        ...

    def get_gage_coord(self, idx: int) -> tuple[float, float]:
        """Return the coordinate of a rain gage.

        @param idx: Gage index.
        @type idx: int
        @return: Tuple C{(x, y)} of the gage's coordinate.
        @rtype: tuple[float, float]
        @raise RuntimeError: If the C API call fails.
        """
        ...
