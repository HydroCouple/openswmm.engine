"""
2D Surface Routing
==================

:author: Caleb Buahin
:copyright: Copyright (c) HydroCouple 2026
:license: MIT

Type stubs for :mod:`openswmm.engine._2d`.

The :class:`Surface2D` class provides read/write access to the optional 2D
surface routing module (requires ``OPENSWMM_BUILD_2D=ON`` and SUNDIALS).
"""

import numpy as np
import numpy.typing as npt


class Surface2D:
    """Read/write interface to the optional 2D surface routing module.

    Args:
        engine_ptr: The raw engine handle (``SWMM_Engine`` cast to ``int``
            via ``solver.handle``).

    Example::

        from openswmm.engine import Solver
        from openswmm.engine._2d import Surface2D

        with Solver("model.inp", "model.rpt", "model.out") as s:
            mesh = Surface2D(s.handle)
            if mesh.is_active:
                depths = mesh.get_depths()
    """

    def __init__(self, engine_ptr: int) -> None: ...

    # ------------------------------------------------------------------
    # Status
    # ------------------------------------------------------------------

    @property
    def is_active(self) -> bool:
        """``True`` if the 2D module is active for this simulation."""
        ...

    # ------------------------------------------------------------------
    # Mesh geometry
    # ------------------------------------------------------------------

    @property
    def n_vertices(self) -> int:
        """Number of mesh vertices."""
        ...

    @property
    def n_triangles(self) -> int:
        """Number of mesh triangles."""
        ...

    def get_vertex_coords(
        self,
    ) -> tuple[
        npt.NDArray[np.float64],
        npt.NDArray[np.float64],
        npt.NDArray[np.float64],
    ]:
        """Return (x, y, z) NumPy arrays for all vertices.

        Returns:
            Tuple of (x, y, z) arrays, each of shape ``(n_vertices,)``
            with dtype ``float64``.
        """
        ...

    def get_triangle_vertices(self, idx: int) -> tuple[int, int, int]:
        """Return (v0, v1, v2) vertex indices for a triangle.

        Args:
            idx: Triangle index.

        Returns:
            Tuple of three vertex indices.
        """
        ...

    def get_triangle_area(self, idx: int) -> float:
        """Return the area of a triangle.

        Args:
            idx: Triangle index.

        Returns:
            Triangle area.
        """
        ...

    def get_triangle_centroid(self, idx: int) -> tuple[float, float, float]:
        """Return (cx, cy, cz) centroid coordinates for a triangle.

        Args:
            idx: Triangle index.

        Returns:
            Tuple of (cx, cy, cz) centroid coordinates.
        """
        ...

    def get_triangle_mannings(self, idx: int) -> float:
        """Return Manning's n for a triangle.

        Args:
            idx: Triangle index.

        Returns:
            Manning's n value.
        """
        ...

    def get_triangle_neighbours(self, idx: int) -> tuple[int, int, int]:
        """Return (n0, n1, n2) neighbour triangle indices.

        Args:
            idx: Triangle index.

        Returns:
            Tuple of three neighbour indices; -1 indicates a boundary.
        """
        ...

    # ------------------------------------------------------------------
    # Coupling
    # ------------------------------------------------------------------

    @property
    def vertex_coupling_count(self) -> int:
        """Number of vertex-to-node coupling points."""
        ...

    @property
    def triangle_coupling_count(self) -> int:
        """Number of triangle-to-node coupling points."""
        ...

    def get_vertex_coupled_node(self, vertex_idx: int) -> int:
        """Return the SWMM node index coupled to a vertex.

        Args:
            vertex_idx: Vertex index.

        Returns:
            Node index, or -1 if no coupling.
        """
        ...

    def get_triangle_coupled_node(self, tri_idx: int) -> int:
        """Return the SWMM node index coupled to a triangle.

        Args:
            tri_idx: Triangle index.

        Returns:
            Node index, or -1 if no coupling.
        """
        ...

    # ------------------------------------------------------------------
    # State — per triangle
    # ------------------------------------------------------------------

    def get_depths(self) -> npt.NDArray[np.float64]:
        """Return depths for all triangles as a NumPy array.

        Returns:
            Array of shape ``(n_triangles,)`` with dtype ``float64``.
        """
        ...

    def get_heads(self) -> npt.NDArray[np.float64]:
        """Return total heads for all triangles as a NumPy array.

        Returns:
            Array of shape ``(n_triangles,)`` with dtype ``float64``.
        """
        ...

    def get_coupling_fluxes(self) -> npt.NDArray[np.float64]:
        """Return coupling fluxes for all triangles as a NumPy array.

        Returns:
            Array of shape ``(n_triangles,)`` with dtype ``float64``.
        """
        ...

    def get_depth(self, idx: int) -> float:
        """Return depth at a specific triangle.

        Args:
            idx: Triangle index.

        Returns:
            Water depth.
        """
        ...

    def get_head(self, idx: int) -> float:
        """Return total head at a specific triangle.

        Args:
            idx: Triangle index.

        Returns:
            Total head.
        """
        ...

    def get_rainfall(self, idx: int) -> float:
        """Return the current rainfall at a specific triangle.

        Args:
            idx: Triangle index.

        Returns:
            Rainfall rate.
        """
        ...

    def get_net_source(self, idx: int) -> float:
        """Return the net source term at a specific triangle.

        Args:
            idx: Triangle index.

        Returns:
            Net source term.
        """
        ...

    def get_coupling_flux(self, idx: int) -> float:
        """Return the coupling flux at a specific triangle.

        Args:
            idx: Triangle index.

        Returns:
            Coupling flux (positive = into 2D surface).
        """
        ...

    # ------------------------------------------------------------------
    # State — per vertex
    # ------------------------------------------------------------------

    def get_vertex_heads(self) -> npt.NDArray[np.float64]:
        """Return reconstructed heads at all vertices as a NumPy array.

        Returns:
            Array of shape ``(n_vertices,)`` with dtype ``float64``.
        """
        ...

    # ------------------------------------------------------------------
    # Statistics
    # ------------------------------------------------------------------

    @property
    def max_depth(self) -> float:
        """Maximum depth across all triangles."""
        ...

    @property
    def total_volume(self) -> float:
        """Total 2D surface volume (sum of depth × area)."""
        ...

    @property
    def total_exchange_flow(self) -> float:
        """Total exchange flow rate (m³/s; positive = into 1D network)."""
        ...

    @property
    def cvode_steps(self) -> int:
        """Number of CVODE internal steps in the last advance."""
        ...

    @property
    def cvode_last_step(self) -> float:
        """Last CVODE internal step size."""
        ...

    def get_stat_max_depths(self) -> npt.NDArray[np.float64]:
        """Return cumulative maximum depth statistics for all triangles.

        Returns:
            Array of shape ``(n_triangles,)`` with dtype ``float64``.
        """
        ...

    # ------------------------------------------------------------------
    # Forcing
    # ------------------------------------------------------------------

    def force_rainfall(
        self, idx: int, value: float, mode: int = 1, persist: int = 0
    ) -> None:
        """Force rainfall on a specific triangle.

        Args:
            idx: Triangle index.
            value: Rainfall rate.
            mode: Forcing mode (1 = ADD, 0 = REPLACE).
            persist: 1 to hold until cleared; 0 for single-step.
        """
        ...

    def force_rainfall_uniform(
        self, value: float, mode: int = 1, persist: int = 0
    ) -> None:
        """Force uniform rainfall on all triangles.

        Args:
            value: Rainfall rate.
            mode: Forcing mode (1 = ADD, 0 = REPLACE).
            persist: 1 to hold until cleared; 0 for single-step.
        """
        ...

    def force_coupling_flux(
        self, idx: int, value: float, mode: int = 1, persist: int = 0
    ) -> None:
        """Force coupling flux on a specific triangle.

        Args:
            idx: Triangle index.
            value: Coupling flux value.
            mode: Forcing mode (1 = ADD, 0 = REPLACE).
            persist: 1 to hold until cleared; 0 for single-step.
        """
        ...

    def force_clear_all(self) -> None:
        """Clear all 2D forcings."""
        ...

    # ------------------------------------------------------------------
    # Options
    # ------------------------------------------------------------------

    @property
    def dry_depth(self) -> float:
        """Dry depth threshold (m); triangles below this are considered dry."""
        ...

    @dry_depth.setter
    def dry_depth(self, value: float) -> None: ...

    @property
    def rel_tolerance(self) -> float:
        """CVODE relative tolerance."""
        ...

    @rel_tolerance.setter
    def rel_tolerance(self, value: float) -> None: ...

    @property
    def abs_tolerance(self) -> float:
        """CVODE absolute tolerance."""
        ...

    @abs_tolerance.setter
    def abs_tolerance(self, value: float) -> None: ...

    # ------------------------------------------------------------------
    # Boundary edges
    # ------------------------------------------------------------------

    @property
    def boundary_edge_count(self) -> int:
        """Number of boundary edges in the 2D mesh."""
        ...

    def get_edge_bc_type(self, tri_idx: int, edge: int) -> int:
        """Return the boundary condition type for a triangle edge.

        Args:
            tri_idx: Triangle index.
            edge: Edge index (0–2).

        Returns:
            Boundary condition type code.
        """
        ...

    def set_edge_bc_type(self, tri_idx: int, edge: int, bc_type: int) -> None:
        """Set the boundary condition type for a triangle edge.

        Args:
            tri_idx: Triangle index.
            edge: Edge index (0–2).
            bc_type: Boundary condition type code.
        """
        ...

    def get_edge_bc_head(self, tri_idx: int, edge: int) -> float:
        """Return the boundary head for a triangle edge.

        Args:
            tri_idx: Triangle index.
            edge: Edge index (0–2).

        Returns:
            Boundary head value.
        """
        ...

    def set_edge_bc_head(self, tri_idx: int, edge: int, head: float) -> None:
        """Set the boundary head for a triangle edge.

        Args:
            tri_idx: Triangle index.
            edge: Edge index (0–2).
            head: Boundary head value.
        """
        ...

    def get_edge_bc_slope(self, tri_idx: int, edge: int) -> float:
        """Return the boundary slope for a triangle edge.

        Args:
            tri_idx: Triangle index.
            edge: Edge index (0–2).

        Returns:
            Boundary slope.
        """
        ...

    def set_edge_bc_slope(
        self, tri_idx: int, edge: int, slope: float
    ) -> None:
        """Set the boundary slope for a triangle edge.

        Args:
            tri_idx: Triangle index.
            edge: Edge index (0–2).
            slope: Boundary slope.
        """
        ...

    def get_edge_bc_cum_flux(self, tri_idx: int, edge: int) -> float:
        """Return the cumulative boundary flux for a triangle edge.

        Args:
            tri_idx: Triangle index.
            edge: Edge index (0–2).

        Returns:
            Cumulative boundary flux.
        """
        ...
