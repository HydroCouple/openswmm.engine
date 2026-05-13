"""
2D Surface Routing
==================

:author: Caleb Buahin
:copyright: Copyright (c) 2026 Caleb Buahin
:license: MIT

Type stubs for :mod:`openswmm.engine._2d`.

The :class:`Surface2D` class provides read/write access to the optional 2D
surface routing module (requires ``OPENSWMM_BUILD_2D=ON`` and SUNDIALS).
"""

import numpy as np
import numpy.typing as npt


class Surface2D:
    """Read/write interface to the optional 2D surface routing module.

    The module solves the depth-averaged shallow-water equations on an
    unstructured triangular mesh and is integrated in time with CVODE
    from SUNDIALS. Two-way coupling with the 1D drainage network is
    supported per-vertex and per-triangle.

    Example::

        from openswmm.engine import Solver
        from openswmm.engine._2d import Surface2D

        with Solver("model.inp", "model.rpt", "model.out") as s:
            mesh = Surface2D(s.handle)
            if mesh.is_active:
                depths = mesh.get_depths()

    @ivar _engine: Internal pointer to the underlying C{SWMM_Engine}
        handle (managed by the Cython extension).
    """

    def __init__(self, engine_ptr: int) -> None:
        """Construct a L{Surface2D} accessor from a raw engine handle.

        @param engine_ptr: The raw engine handle (C{SWMM_Engine} cast to
            an integer via C{solver.handle}).
        @type engine_ptr: int
        """
        ...

    # ====================================================================
    # Mesh definition - status
    # ====================================================================

    @property
    def is_active(self) -> bool:
        """C{True} if the 2D module is active for this simulation.

        @return: Activation flag.
        @rtype: bool
        @raise RuntimeError: If the C API call fails.
        """
        ...

    # ====================================================================
    # Mesh definition - geometry
    # ====================================================================

    @property
    def n_vertices(self) -> int:
        """Number of mesh vertices.

        @return: Vertex count.
        @rtype: int
        @raise RuntimeError: If the C API call fails.
        """
        ...

    @property
    def n_triangles(self) -> int:
        """Number of mesh triangles.

        @return: Triangle count.
        @rtype: int
        @raise RuntimeError: If the C API call fails.
        """
        ...

    def get_vertex_coords(
        self,
    ) -> tuple[
        npt.NDArray[np.float64],
        npt.NDArray[np.float64],
        npt.NDArray[np.float64],
    ]:
        """Return (x, y, z) NumPy arrays for all vertices.

        @return: Tuple C{(x, y, z)}, each of shape C{(n_vertices,)} with
            dtype C{float64}.
        @rtype: tuple[np.ndarray, np.ndarray, np.ndarray]
        @raise RuntimeError: If the C API call fails.
        """
        ...

    def get_triangle_vertices(self, idx: int) -> tuple[int, int, int]:
        """Return the (v0, v1, v2) vertex indices for a triangle.

        @param idx: Triangle index.
        @type idx: int
        @return: Tuple of three vertex indices.
        @rtype: tuple[int, int, int]
        @raise RuntimeError: If the C API call fails.
        """
        ...

    def get_triangle_area(self, idx: int) -> float:
        """Return the area of a triangle.

        @param idx: Triangle index.
        @type idx: int
        @return: Triangle area in project units squared.
        @rtype: float
        @raise RuntimeError: If the C API call fails.
        """
        ...

    def get_triangle_centroid(self, idx: int) -> tuple[float, float, float]:
        """Return the (cx, cy, cz) centroid coordinates for a triangle.

        @param idx: Triangle index.
        @type idx: int
        @return: Tuple C{(cx, cy, cz)} centroid coordinates.
        @rtype: tuple[float, float, float]
        @raise RuntimeError: If the C API call fails.
        """
        ...

    def get_triangle_mannings(self, idx: int) -> float:
        """Return Manning's M{n} for a triangle.

        @param idx: Triangle index.
        @type idx: int
        @return: Manning's M{n} roughness.
        @rtype: float
        @raise RuntimeError: If the C API call fails.
        """
        ...

    def get_triangle_neighbours(self, idx: int) -> tuple[int, int, int]:
        """Return the (n0, n1, n2) neighbour triangle indices.

        @param idx: Triangle index.
        @type idx: int
        @return: Tuple of three neighbour triangle indices; C{-1}
            indicates a boundary edge.
        @rtype: tuple[int, int, int]
        @raise RuntimeError: If the C API call fails.
        """
        ...

    # ====================================================================
    # Coupling
    # ====================================================================

    @property
    def vertex_coupling_count(self) -> int:
        """Number of vertex-to-node coupling points.

        @return: Coupling count.
        @rtype: int
        @raise RuntimeError: If the C API call fails.
        """
        ...

    @property
    def triangle_coupling_count(self) -> int:
        """Number of triangle-to-node coupling points.

        @return: Coupling count.
        @rtype: int
        @raise RuntimeError: If the C API call fails.
        """
        ...

    def get_vertex_coupled_node(self, vertex_idx: int) -> int:
        """Return the SWMM node index coupled to a vertex.

        @param vertex_idx: Vertex index.
        @type vertex_idx: int
        @return: Node index, or C{-1} if no coupling exists.
        @rtype: int
        @raise RuntimeError: If the C API call fails.
        """
        ...

    def get_triangle_coupled_node(self, tri_idx: int) -> int:
        """Return the SWMM node index coupled to a triangle.

        @param tri_idx: Triangle index.
        @type tri_idx: int
        @return: Node index, or C{-1} if no coupling exists.
        @rtype: int
        @raise RuntimeError: If the C API call fails.
        """
        ...

    # ====================================================================
    # State (depth/velocity) - per triangle bulk arrays
    # ====================================================================

    def get_depths(self) -> npt.NDArray[np.float64]:
        """Return depths for all triangles as a NumPy array.

        @return: Array of shape C{(n_triangles,)} with dtype C{float64}.
        @rtype: np.ndarray
        @raise RuntimeError: If the C API call fails.
        """
        ...

    def get_heads(self) -> npt.NDArray[np.float64]:
        """Return total heads for all triangles as a NumPy array.

        @return: Array of shape C{(n_triangles,)} with dtype C{float64}.
        @rtype: np.ndarray
        @raise RuntimeError: If the C API call fails.
        """
        ...

    def get_coupling_fluxes(self) -> npt.NDArray[np.float64]:
        """Return coupling fluxes for all triangles as a NumPy array.

        @return: Array of shape C{(n_triangles,)} with dtype C{float64}.
            Positive values denote flux into the 2D surface.
        @rtype: np.ndarray
        @raise RuntimeError: If the C API call fails.
        """
        ...

    # ====================================================================
    # State (depth/velocity) - per triangle scalar
    # ====================================================================

    def get_depth(self, idx: int) -> float:
        """Return the water depth at a specific triangle.

        @param idx: Triangle index.
        @type idx: int
        @return: Water depth.
        @rtype: float
        @raise RuntimeError: If the C API call fails.
        """
        ...

    def get_head(self, idx: int) -> float:
        """Return the total head at a specific triangle.

        @param idx: Triangle index.
        @type idx: int
        @return: Total head.
        @rtype: float
        @raise RuntimeError: If the C API call fails.
        """
        ...

    def get_rainfall(self, idx: int) -> float:
        """Return the current rainfall at a specific triangle.

        @param idx: Triangle index.
        @type idx: int
        @return: Rainfall rate.
        @rtype: float
        @raise RuntimeError: If the C API call fails.
        """
        ...

    def get_net_source(self, idx: int) -> float:
        """Return the net source term at a specific triangle.

        @param idx: Triangle index.
        @type idx: int
        @return: Net source term.
        @rtype: float
        @raise RuntimeError: If the C API call fails.
        """
        ...

    def get_coupling_flux(self, idx: int) -> float:
        """Return the coupling flux at a specific triangle.

        @param idx: Triangle index.
        @type idx: int
        @return: Coupling flux value (positive = into 2D surface).
        @rtype: float
        @raise RuntimeError: If the C API call fails.
        """
        ...

    # ====================================================================
    # Bulk array access - per vertex state
    # ====================================================================

    def get_vertex_heads(self) -> npt.NDArray[np.float64]:
        """Return reconstructed heads at all vertices as a NumPy array.

        @return: Array of shape C{(n_vertices,)} with dtype C{float64}.
        @rtype: np.ndarray
        @raise RuntimeError: If the C API call fails.
        """
        ...

    # ====================================================================
    # State (depth/velocity) - statistics
    # ====================================================================

    @property
    def max_depth(self) -> float:
        """Maximum depth across all triangles.

        @return: Maximum depth.
        @rtype: float
        @raise RuntimeError: If the C API call fails.
        """
        ...

    @property
    def total_volume(self) -> float:
        """Total 2D surface volume (sum of M{depth x area}).

        @return: Total volume.
        @rtype: float
        @raise RuntimeError: If the C API call fails.
        """
        ...

    @property
    def total_exchange_flow(self) -> float:
        """Total exchange flow rate.

        @return: Exchange flow rate in C{m^3/s} (positive = into 1D
            network).
        @rtype: float
        @raise RuntimeError: If the C API call fails.
        """
        ...

    @property
    def cvode_steps(self) -> int:
        """Number of CVODE internal steps in the last advance.

        @return: Step count.
        @rtype: int
        @raise RuntimeError: If the C API call fails.
        """
        ...

    @property
    def cvode_last_step(self) -> float:
        """Last CVODE internal step size.

        @return: Step size.
        @rtype: float
        @raise RuntimeError: If the C API call fails.
        """
        ...

    def get_stat_max_depths(self) -> npt.NDArray[np.float64]:
        """Return cumulative maximum-depth statistics for all triangles.

        @return: Array of shape C{(n_triangles,)} with dtype C{float64}.
        @rtype: np.ndarray
        @raise RuntimeError: If the C API call fails.
        """
        ...

    # ====================================================================
    # Boundary conditions - forcing
    # ====================================================================

    def force_rainfall(
        self, idx: int, value: float, mode: int = 1, persist: int = 0
    ) -> None:
        """Force rainfall on a specific triangle.

        @param idx: Triangle index.
        @type idx: int
        @param value: Rainfall rate.
        @type value: float
        @param mode: Forcing mode (C{1} = ADD, C{0} = REPLACE).
        @type mode: int
        @param persist: C{1} to hold until cleared; C{0} for single-step.
        @type persist: int
        @raise RuntimeError: If the C API rejects the forcing.
        """
        ...

    def force_rainfall_uniform(
        self, value: float, mode: int = 1, persist: int = 0
    ) -> None:
        """Force uniform rainfall on all triangles.

        @param value: Rainfall rate.
        @type value: float
        @param mode: Forcing mode (C{1} = ADD, C{0} = REPLACE).
        @type mode: int
        @param persist: C{1} to hold until cleared; C{0} for single-step.
        @type persist: int
        @raise RuntimeError: If the C API rejects the forcing.
        """
        ...

    def force_coupling_flux(
        self, idx: int, value: float, mode: int = 1, persist: int = 0
    ) -> None:
        """Force a coupling flux on a specific triangle.

        @param idx: Triangle index.
        @type idx: int
        @param value: Coupling flux value.
        @type value: float
        @param mode: Forcing mode (C{1} = ADD, C{0} = REPLACE).
        @type mode: int
        @param persist: C{1} to hold until cleared; C{0} for single-step.
        @type persist: int
        @raise RuntimeError: If the C API rejects the forcing.
        """
        ...

    def force_clear_all(self) -> None:
        """Clear all 2D forcings.

        @raise RuntimeError: If the C API call fails.
        """
        ...

    # ====================================================================
    # Mesh definition - solver options
    # ====================================================================

    @property
    def dry_depth(self) -> float:
        """Dry-depth threshold (m); triangles below this are treated as dry.

        @return: Threshold depth.
        @rtype: float
        @raise RuntimeError: If the C API call fails.
        """
        ...

    @dry_depth.setter
    def dry_depth(self, value: float) -> None:
        """Set the dry-depth threshold.

        @param value: New threshold depth (m).
        @type value: float
        @raise RuntimeError: If the C API rejects the value.
        """
        ...

    @property
    def rel_tolerance(self) -> float:
        """CVODE relative tolerance.

        @return: Relative tolerance.
        @rtype: float
        @raise RuntimeError: If the C API call fails.
        """
        ...

    @rel_tolerance.setter
    def rel_tolerance(self, value: float) -> None:
        """Set the CVODE relative tolerance.

        @param value: New relative tolerance.
        @type value: float
        @raise RuntimeError: If the C API rejects the value.
        """
        ...

    @property
    def abs_tolerance(self) -> float:
        """CVODE absolute tolerance.

        @return: Absolute tolerance.
        @rtype: float
        @raise RuntimeError: If the C API call fails.
        """
        ...

    @abs_tolerance.setter
    def abs_tolerance(self, value: float) -> None:
        """Set the CVODE absolute tolerance.

        @param value: New absolute tolerance.
        @type value: float
        @raise RuntimeError: If the C API rejects the value.
        """
        ...

    # ====================================================================
    # Boundary conditions - boundary edges
    # ====================================================================

    @property
    def boundary_edge_count(self) -> int:
        """Number of boundary edges in the 2D mesh.

        @return: Boundary edge count.
        @rtype: int
        @raise RuntimeError: If the C API call fails.
        """
        ...

    def get_edge_bc_type(self, tri_idx: int, edge: int) -> int:
        """Return the boundary condition type for a triangle edge.

        @param tri_idx: Triangle index.
        @type tri_idx: int
        @param edge: Edge index in C{0}-C{2}.
        @type edge: int
        @return: Boundary condition type code.
        @rtype: int
        @raise RuntimeError: If the C API call fails.
        """
        ...

    def set_edge_bc_type(self, tri_idx: int, edge: int, bc_type: int) -> None:
        """Set the boundary condition type for a triangle edge.

        @param tri_idx: Triangle index.
        @type tri_idx: int
        @param edge: Edge index in C{0}-C{2}.
        @type edge: int
        @param bc_type: Boundary condition type code.
        @type bc_type: int
        @raise RuntimeError: If the C API rejects the assignment.
        """
        ...

    def get_edge_bc_head(self, tri_idx: int, edge: int) -> float:
        """Return the boundary head for a triangle edge.

        @param tri_idx: Triangle index.
        @type tri_idx: int
        @param edge: Edge index in C{0}-C{2}.
        @type edge: int
        @return: Boundary head value.
        @rtype: float
        @raise RuntimeError: If the C API call fails.
        """
        ...

    def set_edge_bc_head(self, tri_idx: int, edge: int, head: float) -> None:
        """Set the boundary head for a triangle edge.

        @param tri_idx: Triangle index.
        @type tri_idx: int
        @param edge: Edge index in C{0}-C{2}.
        @type edge: int
        @param head: Boundary head value.
        @type head: float
        @raise RuntimeError: If the C API rejects the assignment.
        """
        ...

    def get_edge_bc_slope(self, tri_idx: int, edge: int) -> float:
        """Return the boundary slope for a triangle edge.

        @param tri_idx: Triangle index.
        @type tri_idx: int
        @param edge: Edge index in C{0}-C{2}.
        @type edge: int
        @return: Boundary slope.
        @rtype: float
        @raise RuntimeError: If the C API call fails.
        """
        ...

    def set_edge_bc_slope(
        self, tri_idx: int, edge: int, slope: float
    ) -> None:
        """Set the boundary slope for a triangle edge.

        @param tri_idx: Triangle index.
        @type tri_idx: int
        @param edge: Edge index in C{0}-C{2}.
        @type edge: int
        @param slope: Boundary slope.
        @type slope: float
        @raise RuntimeError: If the C API rejects the assignment.
        """
        ...

    def get_edge_bc_cum_flux(self, tri_idx: int, edge: int) -> float:
        """Return the cumulative boundary flux for a triangle edge.

        @param tri_idx: Triangle index.
        @type tri_idx: int
        @param edge: Edge index in C{0}-C{2}.
        @type edge: int
        @return: Cumulative boundary flux through the edge.
        @rtype: float
        @raise RuntimeError: If the C API call fails.
        """
        ...
