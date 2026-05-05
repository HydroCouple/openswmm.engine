"""
Spatial Coordinate Access
=========================

:author: Caleb Buahin
:copyright: Copyright (c) HydroCouple 2026
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

    Args:
        solver: An active :class:`Solver` instance. The solver must remain
                alive for the lifetime of this object.

    Example::

        from openswmm.engine import Solver
        from openswmm.engine._spatial import Spatial

        with Solver("model.inp", "model.rpt", "model.out") as s:
            sp = Spatial(s)
            x, y = sp.get_node_coord(0)
            sp.set_node_coord(0, x + 100.0, y)
    """

    def __init__(self, solver: Solver) -> None: ...

    # ------------------------------------------------------------------
    # CRS
    # ------------------------------------------------------------------

    def set_crs(self, crs: str) -> None:
        """Set the coordinate reference system string.

        Args:
            crs: CRS identifier (e.g. EPSG code or WKT string).
        """
        ...

    def get_crs(self) -> str:
        """Return the coordinate reference system string.

        Returns:
            CRS identifier.
        """
        ...

    # ------------------------------------------------------------------
    # Node coordinates
    # ------------------------------------------------------------------

    def set_node_coord(self, idx: int, x: float, y: float) -> None:
        """Set the coordinate of a node.

        Args:
            idx: Node index.
            x: X coordinate.
            y: Y coordinate.
        """
        ...

    def get_node_coord(self, idx: int) -> tuple[float, float]:
        """Return the coordinate of a node.

        Args:
            idx: Node index.

        Returns:
            Tuple of (x, y).
        """
        ...

    def get_node_coords_bulk(
        self,
    ) -> tuple[npt.NDArray[np.float64], npt.NDArray[np.float64]]:
        """Return coordinates for all nodes as NumPy arrays.

        Returns:
            Tuple of (x_array, y_array), each of shape ``(n_nodes,)``
            with dtype ``float64``.
        """
        ...

    def set_node_coords_bulk(
        self,
        x: npt.NDArray[np.float64],
        y: npt.NDArray[np.float64],
    ) -> None:
        """Set coordinates for all nodes from NumPy arrays.

        Args:
            x: X coordinates array of shape ``(n_nodes,)``.
            y: Y coordinates array of shape ``(n_nodes,)``.
        """
        ...

    # ------------------------------------------------------------------
    # Link coordinates
    # ------------------------------------------------------------------

    def set_link_coord(self, idx: int, x: float, y: float) -> None:
        """Set the coordinate of a link.

        Args:
            idx: Link index.
            x: X coordinate.
            y: Y coordinate.
        """
        ...

    def get_link_coord(self, idx: int) -> tuple[float, float]:
        """Return the coordinate of a link.

        Args:
            idx: Link index.

        Returns:
            Tuple of (x, y).
        """
        ...

    # ------------------------------------------------------------------
    # Link vertices
    # ------------------------------------------------------------------

    def set_link_vertices(
        self,
        idx: int,
        x: npt.NDArray[np.float64],
        y: npt.NDArray[np.float64],
    ) -> None:
        """Set the polyline vertices of a link.

        Args:
            idx: Link index.
            x: X coordinates of vertices.
            y: Y coordinates of vertices.
        """
        ...

    def get_link_vertex_count(self, idx: int) -> int:
        """Return the number of ordered polyline vertices for a link.

        The count includes the upstream node coordinate at index 0,
        interior vertices, and the downstream node coordinate at the last
        index.

        Args:
            idx: Link index.

        Returns:
            Vertex count.
        """
        ...

    def get_link_vertices(
        self, idx: int
    ) -> tuple[npt.NDArray[np.float64], npt.NDArray[np.float64]]:
        """Return ordered polyline vertices of a link as NumPy arrays.

        Ordering is: upstream node, interior vertices, downstream node.

        Args:
            idx: Link index.

        Returns:
            Tuple of (x_array, y_array), each with dtype ``float64``.
        """
        ...

    # ------------------------------------------------------------------
    # Subcatchment coordinates
    # ------------------------------------------------------------------

    def set_subcatch_coord(self, idx: int, x: float, y: float) -> None:
        """Set the centroid coordinate of a subcatchment.

        Args:
            idx: Subcatchment index.
            x: X coordinate.
            y: Y coordinate.
        """
        ...

    def get_subcatch_coord(self, idx: int) -> tuple[float, float]:
        """Return the centroid coordinate of a subcatchment.

        Args:
            idx: Subcatchment index.

        Returns:
            Tuple of (x, y).
        """
        ...

    # ------------------------------------------------------------------
    # Subcatchment polygon
    # ------------------------------------------------------------------

    def set_subcatch_polygon(
        self,
        idx: int,
        x: npt.NDArray[np.float64],
        y: npt.NDArray[np.float64],
    ) -> None:
        """Set the polygon vertices of a subcatchment.

        Args:
            idx: Subcatchment index.
            x: X coordinates of polygon vertices.
            y: Y coordinates of polygon vertices.
        """
        ...

    def get_subcatch_polygon_count(self, idx: int) -> int:
        """Return the number of polygon vertices for a subcatchment.

        Args:
            idx: Subcatchment index.

        Returns:
            Vertex count.
        """
        ...

    def get_subcatch_polygon(
        self, idx: int
    ) -> tuple[npt.NDArray[np.float64], npt.NDArray[np.float64]]:
        """Return the polygon vertices of a subcatchment as NumPy arrays.

        Args:
            idx: Subcatchment index.

        Returns:
            Tuple of (x_array, y_array), each with dtype ``float64``.
        """
        ...

    # ------------------------------------------------------------------
    # Gage coordinates
    # ------------------------------------------------------------------

    def set_gage_coord(self, idx: int, x: float, y: float) -> None:
        """Set the coordinate of a rain gage.

        Args:
            idx: Gage index.
            x: X coordinate.
            y: Y coordinate.
        """
        ...

    def get_gage_coord(self, idx: int) -> tuple[float, float]:
        """Return the coordinate of a rain gage.

        Args:
            idx: Gage index.

        Returns:
            Tuple of (x, y).
        """
        ...
