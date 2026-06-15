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

from ._enums import SurfaceForcingMode, ForcingPersist, SurfaceBoundaryType


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
        """Return (x, y, z) NumPy arrays for all vertices. GIL is released
        during the C call.

        @return: Tuple C{(x, y, z)}, each of shape C{(n_vertices,)} with
            dtype C{float64}.
        @rtype: tuple[np.ndarray, np.ndarray, np.ndarray]
        @raise RuntimeError: If the C API call fails.
        """
        ...

    def set_vertex_z(self, idx: int, z: float) -> None:
        """Set the ground elevation of a mesh vertex.

        Updates derived geometry for every triangle incident to this
        vertex (centroid Z, per-edge midpoint Z). XY-derived fields are
        unaffected. When called during a running simulation, solver
        state (head, depth) is intentionally not rewritten — the implied
        depth = head - bed therefore changes by the same amount as bed.

        @param idx: Vertex index (0-based).
        @type idx: int
        @param z: New ground elevation (project vertical units).
        @type z: float
        @raise RuntimeError: If the C API call fails.
        """
        ...

    def get_vertex_xyz(self, idx: int) -> tuple[float, float, float]:
        """Scalar ``(x, y, z)`` for one mesh vertex (project units).

        @rtype: tuple[float, float, float]
        """
        ...
    def get_vertex_head(self, idx: int) -> float:
        """Scalar water-surface head at one mesh vertex (project units).

        @rtype: float
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
    #
    # Every bulk getter in this section releases the GIL for the C call.
    # ====================================================================

    def get_depths(self) -> npt.NDArray[np.float64]:
        """Return depths for all triangles as a NumPy array. GIL is
        released during the C call.

        @return: Array of shape C{(n_triangles,)} with dtype C{float64}.
        @rtype: np.ndarray
        @raise RuntimeError: If the C API call fails.
        """
        ...

    def get_heads(self) -> npt.NDArray[np.float64]:
        """Return total heads for all triangles as a NumPy array. GIL is
        released during the C call.

        @return: Array of shape C{(n_triangles,)} with dtype C{float64}.
        @rtype: np.ndarray
        @raise RuntimeError: If the C API call fails.
        """
        ...

    def get_coupling_fluxes(self) -> npt.NDArray[np.float64]:
        """Return coupling fluxes for all triangles as a NumPy array. GIL
        is released during the C call.

        @return: Array of shape C{(n_triangles,)} with dtype C{float64}.
            Positive values denote flux into the 2D surface.
        @rtype: np.ndarray
        @raise RuntimeError: If the C API call fails.
        """
        ...

    def get_edge_flux_bulk(self) -> npt.NDArray[np.float64]:
        """Return normal edge fluxes for all triangle edges. GIL is
        released during the C call.

        Indexed as C{[tri*3 + localEdge]}.
        @return: Array of shape C{(n_triangles*3,)} with dtype C{float64}.
        """
        ...

    def get_edge_geometry_bulk(self) -> tuple[npt.NDArray[np.float64], npt.NDArray[np.float64], npt.NDArray[np.float64]]:
        """Return time-invariant edge lengths and outward unit normal components.
        GIL is released during the C call.

        Returns C{(length, nx, ny)}, each indexed as C{[tri*3 + localEdge]}.

        @return: Tuple C{(length, nx, ny)}, each of shape C{(n_triangles*3,)}.
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
        GIL is released during the C call.

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
        """Return cumulative maximum-depth envelope for all triangles.

        @return: Array of shape C{(n_triangles,)} with dtype C{float64}, in m.
        @rtype: np.ndarray
        @raise RuntimeError: If the C API call fails.
        """
        ...

    def get_stat_max_velocities(self) -> npt.NDArray[np.float64]:
        """Return cumulative maximum velocity-magnitude envelope per triangle.

        @return: Array of shape C{(n_triangles,)} with dtype C{float64}, in m/s.
        @rtype: np.ndarray
        @raise RuntimeError: If the C API call fails.
        """
        ...

    def get_stat_max_continuity_err(self) -> npt.NDArray[np.float64]:
        """Return cumulative maximum M{abs(continuity residual)} envelope per triangle.

        @return: Array of shape C{(n_triangles,)} with dtype C{float64}, in
            C{m^3/s}.
        @rtype: np.ndarray
        @raise RuntimeError: If the C API call fails.
        """
        ...

    @property
    def continuity_error(self) -> float:
        """Global 2D surface continuity error.

        @return: M{(total_in - total_out) / total_in}, the domain mass-balance
            error as a fraction.
        @rtype: float
        @raise RuntimeError: If the 2D module did not run.
        """
        ...

    def get_mass_balance(self) -> dict[str, float]:
        """Return the global 2D mass-balance terms.

        @return: Mapping with keys C{init_storage}, C{final_storage},
            C{rainfall_in}, C{coupling_1d_to_2d_in}, C{coupling_2d_to_1d_out},
            C{outfall_in}, C{boundary_in}, C{boundary_out}, C{evap_out}
            (all C{m^3}) and C{continuity_error} (fraction).
        @rtype: dict[str, float]
        @raise RuntimeError: If the 2D module did not run.
        """
        ...

    # ====================================================================
    # Boundary conditions - forcing
    # ====================================================================

    def force_rainfall(
        self,
        idx: int,
        value: float,
        *,
        mode: SurfaceForcingMode = ...,
        persist: ForcingPersist = ...,
    ) -> None:
        """Force rainfall on a specific triangle.

        @param idx: Triangle index.
        @type idx: int
        @param value: Rainfall rate (m/s).
        @type value: float
        @param mode: How the value is applied (C{OVERRIDE} or C{ADD}).
        @type mode: L{SurfaceForcingMode}
        @param persist: C{PERSIST} to hold until cleared; C{RESET} for a
            single step.
        @type persist: L{ForcingPersist}
        @raise RuntimeError: If the C API rejects the forcing.
        """
        ...

    def force_rainfall_uniform(
        self,
        value: float,
        *,
        mode: SurfaceForcingMode = ...,
        persist: ForcingPersist = ...,
    ) -> None:
        """Force uniform rainfall on all triangles.

        @param value: Rainfall rate (m/s).
        @type value: float
        @param mode: How the value is applied (C{OVERRIDE} or C{ADD}).
        @type mode: L{SurfaceForcingMode}
        @param persist: C{PERSIST} to hold until cleared; C{RESET} for a
            single step.
        @type persist: L{ForcingPersist}
        @raise RuntimeError: If the C API rejects the forcing.
        """
        ...

    def force_evap(
        self,
        idx: int,
        value: float,
        *,
        mode: SurfaceForcingMode = ...,
        persist: ForcingPersist = ...,
    ) -> None:
        """Force evaporation on a specific triangle.

        The rate is a demand: wet cells lose depth at this rate, shutting
        off smoothly as a cell dries (depths never go negative). The default
        rate is 0 unless forced. Negative values are treated as zero.

        @param idx: Triangle index.
        @type idx: int
        @param value: Evaporation rate (m/s; same SI convention as rainfall).
        @type value: float
        @param mode: How the value is applied (C{OVERRIDE} or C{ADD}).
        @type mode: L{SurfaceForcingMode}
        @param persist: C{PERSIST} to hold until cleared; C{RESET} for a
            single step.
        @type persist: L{ForcingPersist}
        @raise RuntimeError: If the C API rejects the forcing.
        """
        ...

    def force_evap_uniform(
        self,
        value: float,
        *,
        mode: SurfaceForcingMode = ...,
        persist: ForcingPersist = ...,
    ) -> None:
        """Force uniform evaporation on all triangles.

        @param value: Evaporation rate (m/s; same SI convention as rainfall).
        @type value: float
        @param mode: How the value is applied (C{OVERRIDE} or C{ADD}).
        @type mode: L{SurfaceForcingMode}
        @param persist: C{PERSIST} to hold until cleared; C{RESET} for a
            single step.
        @type persist: L{ForcingPersist}
        @raise RuntimeError: If the C API rejects the forcing.
        """
        ...

    def force_coupling_flux(
        self,
        idx: int,
        value: float,
        *,
        mode: SurfaceForcingMode = ...,
        persist: ForcingPersist = ...,
    ) -> None:
        """Force a coupling flux on a specific triangle.

        @param idx: Triangle index.
        @type idx: int
        @param value: Coupling flux value (m/s, positive = into 2D).
        @type value: float
        @param mode: How the value is applied (C{OVERRIDE} or C{ADD}).
        @type mode: L{SurfaceForcingMode}
        @param persist: C{PERSIST} to hold until cleared; C{RESET} for a
            single step.
        @type persist: L{ForcingPersist}
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

    def get_edge_bc_type(self, tri_idx: int, edge: int) -> SurfaceBoundaryType:
        """Return the boundary condition type for a triangle edge.

        @param tri_idx: Triangle index.
        @type tri_idx: int
        @param edge: Edge index in C{0}-C{2}.
        @type edge: int
        @return: Boundary condition type.
        @rtype: L{SurfaceBoundaryType}
        @raise RuntimeError: If the C API call fails.
        """
        ...

    def set_edge_bc_type(
        self, tri_idx: int, edge: int, bc_type: SurfaceBoundaryType
    ) -> None:
        """Set the boundary condition type for a triangle edge.

        @param tri_idx: Triangle index.
        @type tri_idx: int
        @param edge: Edge index in C{0}-C{2}.
        @type edge: int
        @param bc_type: Boundary condition type.
        @type bc_type: L{SurfaceBoundaryType}
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

    def get_edge_bc_flow(self, tri_idx: int, edge: int) -> float:
        """Return the prescribed flow per metre for a SPECIFIED_FLOW edge.

        @param tri_idx: Triangle index.
        @type tri_idx: int
        @param edge: Edge index in C{0}-C{2}.
        @type edge: int
        @return: Prescribed flow per metre of edge (C{m^3/s/m}).
        @rtype: float
        @raise RuntimeError: If the C API call fails.
        """
        ...

    def set_edge_bc_flow(self, tri_idx: int, edge: int, flow: float) -> None:
        """Set the prescribed flow per metre for a SPECIFIED_FLOW edge.

        @param tri_idx: Triangle index.
        @type tri_idx: int
        @param edge: Edge index in C{0}-C{2}.
        @type edge: int
        @param flow: Prescribed flow per metre of edge (C{m^3/s/m}).
        @type flow: float
        @raise RuntimeError: If the C API rejects the assignment.
        """
        ...

    def set_edge_bc_tseries_name(
        self, tri_idx: int, edge: int, name: str
    ) -> None:
        """Set the timeseries name driving a SPECIFIED_STAGE edge.

        @param tri_idx: Triangle index.
        @type tri_idx: int
        @param edge: Edge index in C{0}-C{2}.
        @type edge: int
        @param name: Timeseries name, or C{""} to clear.
        @type name: str
        @raise RuntimeError: If the C API rejects the assignment.
        """
        ...

    def set_edge_bc_flow_tseries_name(
        self, tri_idx: int, edge: int, name: str
    ) -> None:
        """Set the timeseries name driving a SPECIFIED_FLOW edge.

        @param tri_idx: Triangle index.
        @type tri_idx: int
        @param edge: Edge index in C{0}-C{2}.
        @type edge: int
        @param name: Timeseries name, or C{""} to clear.
        @type name: str
        @raise RuntimeError: If the C API rejects the assignment.
        """
        ...

    def set_edge_bc_rating_curve_name(
        self, tri_idx: int, edge: int, name: str
    ) -> None:
        """Set the rating-curve name driving a RATING_CURVE edge.

        @param tri_idx: Triangle index.
        @type tri_idx: int
        @param edge: Edge index in C{0}-C{2}.
        @type edge: int
        @param name: Rating-curve name, or C{""} to clear.
        @type name: str
        @raise RuntimeError: If the C API rejects the assignment.
        """
        ...

    # ====================================================================
    # Edge conveyance factor
    # ====================================================================

    def get_edge_conveyance(self, tri: int, edge: int) -> float:
        """Return the per-edge conveyance factor in C{[0, 1]}.

        @param tri: Triangle index in C{[0, triangle_count)}.
        @type tri: int
        @param edge: Local edge index in C{{0, 1, 2}}.
        @type edge: int
        @return: Conveyance factor (1.0 = unrestricted, 0.0 = wall).
        @rtype: float
        @raise RuntimeError: If the C API call fails.
        """
        ...

    def set_edge_conveyance(self, tri: int, edge: int, conveyance: float) -> None:
        """Set the per-edge conveyance factor in C{[0, 1]}.

        For interior edges the value is mirrored to the partner slot on
        the neighbouring triangle so mass conservation is preserved.

        @param tri: Triangle index in C{[0, triangle_count)}.
        @type tri: int
        @param edge: Local edge index in C{{0, 1, 2}}.
        @type edge: int
        @param conveyance: New value in C{[0, 1]}.
        @type conveyance: float
        @raise RuntimeError: If the C API rejects the assignment.
        """
        ...

    def get_edge_conveyance_bulk(self) -> npt.NDArray[np.float64]:
        """Return a NumPy array of all per-edge conveyance factors.

        Length is C{triangle_count * 3}, indexed C{[tri*3 + edge]}.
        @rtype: np.ndarray
        """
        ...

    def reset_edge_conveyance(self) -> None:
        """Reset every edge's conveyance factor to 1.0 (unrestricted)."""
        ...
