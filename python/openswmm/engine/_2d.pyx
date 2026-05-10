"""
2D Surface Routing
==================

:author: Caleb Buahin
:copyright: Copyright (c) 2026 Caleb Buahin
:license: MIT

Cython wrapper for the 2D surface routing C API.

The :class:`Surface2D` class wraps the C functions declared in
``openswmm_2d.h`` and supports NumPy arrays for bulk data access. The
2D module is optional and is only available when the package is built
with ``OPENSWMM_BUILD_2D=ON`` (which also requires SUNDIALS / CVODE).
"""

cimport numpy as np
import numpy as np
from libc.stdint cimport uintptr_t

from ._2d cimport *


cdef inline void _check(int rc) except *:
    """Raise L{RuntimeError} if a C API call returns non-zero.

    @param rc: Return code from a C{swmm_2d_*} call.
    @type rc: int
    @raise RuntimeError: If C{rc != 0}.
    """
    if rc != 0:
        raise RuntimeError(f"SWMM 2D API error code {rc}")


cdef class Surface2D:
    """Read/write interface to the optional 2D surface routing module.

    The module solves the depth-averaged shallow-water equations on an
    unstructured triangular mesh and is integrated in time with CVODE
    from SUNDIALS. Two-way coupling with the 1D drainage network is
    supported per-vertex and per-triangle.

    @ivar _engine: Internal pointer to the underlying C{SWMM_Engine}
        handle (managed by the Cython extension).
    """

    cdef void* _engine

    def __cinit__(self, uintptr_t engine_ptr):
        """Construct a L{Surface2D} accessor from a raw engine handle.

        @param engine_ptr: The raw engine handle (C{SWMM_Engine} cast to
            C{uintptr_t}).
        @type engine_ptr: int
        """
        self._engine = <void*>engine_ptr

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
        cdef int active = 0
        _check(swmm_2d_is_active(self._engine, &active))
        return bool(active)

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
        cdef int count = 0
        _check(swmm_2d_vertex_count(self._engine, &count))
        return count

    @property
    def n_triangles(self) -> int:
        """Number of mesh triangles.

        @return: Triangle count.
        @rtype: int
        @raise RuntimeError: If the C API call fails.
        """
        cdef int count = 0
        _check(swmm_2d_triangle_count(self._engine, &count))
        return count

    def get_vertex_coords(self):
        """Return (x, y, z) NumPy arrays for all vertices.

        @return: Tuple C{(x, y, z)}, each of shape C{(n_vertices,)} with
            dtype C{float64}.
        @rtype: tuple
        @raise RuntimeError: If the C API call fails.
        """
        cdef int n = self.n_vertices
        cdef np.ndarray[double, ndim=1] x = np.empty(n, dtype=np.float64)
        cdef np.ndarray[double, ndim=1] y = np.empty(n, dtype=np.float64)
        cdef np.ndarray[double, ndim=1] z = np.empty(n, dtype=np.float64)
        _check(swmm_2d_vertex_get_xyz_bulk(self._engine, &x[0], &y[0], &z[0]))
        return x, y, z

    def get_triangle_vertices(self, int idx):
        """Return the (v0, v1, v2) vertex indices for a triangle.

        @param idx: Triangle index.
        @type idx: int
        @return: Tuple of three vertex indices.
        @rtype: tuple
        @raise RuntimeError: If the C API call fails.
        """
        cdef int v0, v1, v2
        _check(swmm_2d_triangle_get_vertices(self._engine, idx, &v0, &v1, &v2))
        return v0, v1, v2

    def get_triangle_area(self, int idx) -> float:
        """Return the area of a triangle.

        @param idx: Triangle index.
        @type idx: int
        @return: Triangle area in project units squared.
        @rtype: float
        @raise RuntimeError: If the C API call fails.
        """
        cdef double area
        _check(swmm_2d_triangle_get_area(self._engine, idx, &area))
        return area

    def get_triangle_centroid(self, int idx):
        """Return the (cx, cy, cz) centroid coordinates for a triangle.

        @param idx: Triangle index.
        @type idx: int
        @return: Tuple C{(cx, cy, cz)} centroid coordinates.
        @rtype: tuple
        @raise RuntimeError: If the C API call fails.
        """
        cdef double cx, cy, cz
        _check(swmm_2d_triangle_get_centroid(self._engine, idx, &cx, &cy, &cz))
        return cx, cy, cz

    def get_triangle_mannings(self, int idx) -> float:
        """Return Manning's M{n} for a triangle.

        @param idx: Triangle index.
        @type idx: int
        @return: Manning's M{n} roughness.
        @rtype: float
        @raise RuntimeError: If the C API call fails.
        """
        cdef double n
        _check(swmm_2d_triangle_get_mannings(self._engine, idx, &n))
        return n

    def get_triangle_neighbours(self, int idx):
        """Return the (n0, n1, n2) neighbour triangle indices.

        @param idx: Triangle index.
        @type idx: int
        @return: Tuple of three neighbour triangle indices; C{-1}
            indicates a boundary edge.
        @rtype: tuple
        @raise RuntimeError: If the C API call fails.
        """
        cdef int n0, n1, n2
        _check(swmm_2d_triangle_get_neighbours(self._engine, idx, &n0, &n1, &n2))
        return n0, n1, n2

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
        cdef int count = 0
        _check(swmm_2d_vertex_coupling_count(self._engine, &count))
        return count

    @property
    def triangle_coupling_count(self) -> int:
        """Number of triangle-to-node coupling points.

        @return: Coupling count.
        @rtype: int
        @raise RuntimeError: If the C API call fails.
        """
        cdef int count = 0
        _check(swmm_2d_triangle_coupling_count(self._engine, &count))
        return count

    def get_vertex_coupled_node(self, int vertex_idx) -> int:
        """Return the SWMM node index coupled to a vertex.

        @param vertex_idx: Vertex index.
        @type vertex_idx: int
        @return: Node index, or C{-1} if no coupling exists.
        @rtype: int
        @raise RuntimeError: If the C API call fails.
        """
        cdef int node_idx
        _check(swmm_2d_vertex_get_coupled_node(self._engine, vertex_idx,
                                                 &node_idx))
        return node_idx

    def get_triangle_coupled_node(self, int tri_idx) -> int:
        """Return the SWMM node index coupled to a triangle.

        @param tri_idx: Triangle index.
        @type tri_idx: int
        @return: Node index, or C{-1} if no coupling exists.
        @rtype: int
        @raise RuntimeError: If the C API call fails.
        """
        cdef int node_idx
        _check(swmm_2d_triangle_get_coupled_node(self._engine, tri_idx,
                                                    &node_idx))
        return node_idx

    # ====================================================================
    # State (depth/velocity) - per triangle bulk arrays
    # ====================================================================

    def get_depths(self):
        """Return depths for all triangles as a NumPy array.

        @return: Array of shape C{(n_triangles,)} with dtype C{float64}.
        @rtype: np.ndarray
        @raise RuntimeError: If the C API call fails.
        """
        cdef int n = self.n_triangles
        cdef np.ndarray[double, ndim=1] arr = np.empty(n, dtype=np.float64)
        _check(swmm_2d_get_depths_bulk(self._engine, &arr[0]))
        return arr

    def get_heads(self):
        """Return total heads for all triangles as a NumPy array.

        @return: Array of shape C{(n_triangles,)} with dtype C{float64}.
        @rtype: np.ndarray
        @raise RuntimeError: If the C API call fails.
        """
        cdef int n = self.n_triangles
        cdef np.ndarray[double, ndim=1] arr = np.empty(n, dtype=np.float64)
        _check(swmm_2d_get_heads_bulk(self._engine, &arr[0]))
        return arr

    def get_coupling_fluxes(self):
        """Return coupling fluxes for all triangles as a NumPy array.

        @return: Array of shape C{(n_triangles,)} with dtype C{float64}.
            Positive values denote flux into the 2D surface.
        @rtype: np.ndarray
        @raise RuntimeError: If the C API call fails.
        """
        cdef int n = self.n_triangles
        cdef np.ndarray[double, ndim=1] arr = np.empty(n, dtype=np.float64)
        _check(swmm_2d_get_coupling_fluxes_bulk(self._engine, &arr[0]))
        return arr

    # ====================================================================
    # State (depth/velocity) - per triangle scalar
    # ====================================================================

    def get_depth(self, int idx) -> float:
        """Return the water depth at a specific triangle.

        @param idx: Triangle index.
        @type idx: int
        @return: Water depth.
        @rtype: float
        @raise RuntimeError: If the C API call fails.
        """
        cdef double val
        _check(swmm_2d_get_depth(self._engine, idx, &val))
        return val

    def get_head(self, int idx) -> float:
        """Return the total head at a specific triangle.

        @param idx: Triangle index.
        @type idx: int
        @return: Total head.
        @rtype: float
        @raise RuntimeError: If the C API call fails.
        """
        cdef double val
        _check(swmm_2d_get_head(self._engine, idx, &val))
        return val

    def get_rainfall(self, int idx) -> float:
        """Return the current rainfall at a specific triangle.

        @param idx: Triangle index.
        @type idx: int
        @return: Rainfall rate.
        @rtype: float
        @raise RuntimeError: If the C API call fails.
        """
        cdef double val
        _check(swmm_2d_get_rainfall(self._engine, idx, &val))
        return val

    def get_net_source(self, int idx) -> float:
        """Return the net source term at a specific triangle.

        @param idx: Triangle index.
        @type idx: int
        @return: Net source term.
        @rtype: float
        @raise RuntimeError: If the C API call fails.
        """
        cdef double val
        _check(swmm_2d_get_net_source(self._engine, idx, &val))
        return val

    def get_coupling_flux(self, int idx) -> float:
        """Return the coupling flux at a specific triangle.

        @param idx: Triangle index.
        @type idx: int
        @return: Coupling flux value (positive = into 2D surface).
        @rtype: float
        @raise RuntimeError: If the C API call fails.
        """
        cdef double val
        _check(swmm_2d_get_coupling_flux(self._engine, idx, &val))
        return val

    # ====================================================================
    # Bulk array access - per vertex state
    # ====================================================================

    def get_vertex_heads(self):
        """Return reconstructed heads at all vertices as a NumPy array.

        @return: Array of shape C{(n_vertices,)} with dtype C{float64}.
        @rtype: np.ndarray
        @raise RuntimeError: If the C API call fails.
        """
        cdef int n = self.n_vertices
        cdef np.ndarray[double, ndim=1] arr = np.empty(n, dtype=np.float64)
        _check(swmm_2d_vertex_get_heads_bulk(self._engine, &arr[0]))
        return arr

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
        cdef double val
        _check(swmm_2d_get_max_depth(self._engine, &val))
        return val

    @property
    def total_volume(self) -> float:
        """Total 2D surface volume (sum of M{depth x area}).

        @return: Total volume.
        @rtype: float
        @raise RuntimeError: If the C API call fails.
        """
        cdef double val
        _check(swmm_2d_get_total_volume(self._engine, &val))
        return val

    @property
    def total_exchange_flow(self) -> float:
        """Total exchange flow rate.

        @return: Exchange flow rate in C{m^3/s} (positive = into 1D
            network).
        @rtype: float
        @raise RuntimeError: If the C API call fails.
        """
        cdef double val
        _check(swmm_2d_get_total_exchange_flow(self._engine, &val))
        return val

    @property
    def cvode_steps(self) -> int:
        """Number of CVODE internal steps in the last advance.

        @return: Step count.
        @rtype: int
        @raise RuntimeError: If the C API call fails.
        """
        cdef long val
        _check(swmm_2d_get_cvode_steps(self._engine, &val))
        return val

    @property
    def cvode_last_step(self) -> float:
        """Last CVODE internal step size.

        @return: Step size.
        @rtype: float
        @raise RuntimeError: If the C API call fails.
        """
        cdef double val
        _check(swmm_2d_get_cvode_last_step(self._engine, &val))
        return val

    def get_stat_max_depths(self):
        """Return cumulative maximum-depth statistics for all triangles.

        @return: Array of shape C{(n_triangles,)} with dtype C{float64}.
        @rtype: np.ndarray
        @raise RuntimeError: If the C API call fails.
        """
        cdef int n = self.n_triangles
        cdef np.ndarray[double, ndim=1] arr = np.empty(n, dtype=np.float64)
        _check(swmm_2d_get_stat_max_depths(self._engine, &arr[0]))
        return arr

    # ====================================================================
    # Boundary conditions - forcing
    # ====================================================================

    def force_rainfall(self, int idx, double value, int mode=1, int persist=0):
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
        _check(swmm_2d_force_rainfall(self._engine, idx, value, mode, persist))

    def force_rainfall_uniform(self, double value, int mode=1, int persist=0):
        """Force uniform rainfall on all triangles.

        @param value: Rainfall rate.
        @type value: float
        @param mode: Forcing mode (C{1} = ADD, C{0} = REPLACE).
        @type mode: int
        @param persist: C{1} to hold until cleared; C{0} for single-step.
        @type persist: int
        @raise RuntimeError: If the C API rejects the forcing.
        """
        _check(swmm_2d_force_rainfall_uniform(self._engine, value, mode, persist))

    def force_coupling_flux(self, int idx, double value, int mode=1,
                             int persist=0):
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
        _check(swmm_2d_force_coupling_flux(self._engine, idx, value, mode,
                                            persist))

    def force_clear_all(self):
        """Clear all 2D forcings.

        @raise RuntimeError: If the C API call fails.
        """
        _check(swmm_2d_force_clear_all(self._engine))

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
        cdef double val
        _check(swmm_2d_get_dry_depth(self._engine, &val))
        return val

    @dry_depth.setter
    def dry_depth(self, double value):
        """Set the dry-depth threshold.

        @param value: New threshold depth (m).
        @type value: float
        @raise RuntimeError: If the C API rejects the value.
        """
        _check(swmm_2d_set_dry_depth(self._engine, value))

    @property
    def rel_tolerance(self) -> float:
        """CVODE relative tolerance.

        @return: Relative tolerance.
        @rtype: float
        @raise RuntimeError: If the C API call fails.
        """
        cdef double val
        _check(swmm_2d_get_rel_tolerance(self._engine, &val))
        return val

    @rel_tolerance.setter
    def rel_tolerance(self, double value):
        """Set the CVODE relative tolerance.

        @param value: New relative tolerance.
        @type value: float
        @raise RuntimeError: If the C API rejects the value.
        """
        _check(swmm_2d_set_rel_tolerance(self._engine, value))

    @property
    def abs_tolerance(self) -> float:
        """CVODE absolute tolerance.

        @return: Absolute tolerance.
        @rtype: float
        @raise RuntimeError: If the C API call fails.
        """
        cdef double val
        _check(swmm_2d_get_abs_tolerance(self._engine, &val))
        return val

    @abs_tolerance.setter
    def abs_tolerance(self, double value):
        """Set the CVODE absolute tolerance.

        @param value: New absolute tolerance.
        @type value: float
        @raise RuntimeError: If the C API rejects the value.
        """
        _check(swmm_2d_set_abs_tolerance(self._engine, value))

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
        cdef int count = 0
        _check(swmm_2d_boundary_edge_count(self._engine, &count))
        return count

    def get_edge_bc_type(self, int tri_idx, int edge) -> int:
        """Return the boundary condition type for a triangle edge.

        @param tri_idx: Triangle index.
        @type tri_idx: int
        @param edge: Edge index in C{0}-C{2}.
        @type edge: int
        @return: Boundary condition type code.
        @rtype: int
        @raise RuntimeError: If the C API call fails.
        """
        cdef int bc_type = 0
        _check(swmm_2d_get_edge_bc_type(self._engine, tri_idx, edge, &bc_type))
        return bc_type

    def set_edge_bc_type(self, int tri_idx, int edge, int bc_type):
        """Set the boundary condition type for a triangle edge.

        @param tri_idx: Triangle index.
        @type tri_idx: int
        @param edge: Edge index in C{0}-C{2}.
        @type edge: int
        @param bc_type: Boundary condition type code.
        @type bc_type: int
        @raise RuntimeError: If the C API rejects the assignment.
        """
        _check(swmm_2d_set_edge_bc_type(self._engine, tri_idx, edge, bc_type))

    def get_edge_bc_head(self, int tri_idx, int edge) -> float:
        """Return the boundary head for a triangle edge.

        @param tri_idx: Triangle index.
        @type tri_idx: int
        @param edge: Edge index in C{0}-C{2}.
        @type edge: int
        @return: Boundary head value.
        @rtype: float
        @raise RuntimeError: If the C API call fails.
        """
        cdef double head = 0.0
        _check(swmm_2d_get_edge_bc_head(self._engine, tri_idx, edge, &head))
        return head

    def set_edge_bc_head(self, int tri_idx, int edge, double head):
        """Set the boundary head for a triangle edge.

        @param tri_idx: Triangle index.
        @type tri_idx: int
        @param edge: Edge index in C{0}-C{2}.
        @type edge: int
        @param head: Boundary head value.
        @type head: float
        @raise RuntimeError: If the C API rejects the assignment.
        """
        _check(swmm_2d_set_edge_bc_head(self._engine, tri_idx, edge, head))

    def get_edge_bc_slope(self, int tri_idx, int edge) -> float:
        """Return the boundary slope for a triangle edge.

        @param tri_idx: Triangle index.
        @type tri_idx: int
        @param edge: Edge index in C{0}-C{2}.
        @type edge: int
        @return: Boundary slope.
        @rtype: float
        @raise RuntimeError: If the C API call fails.
        """
        cdef double slope = 0.0
        _check(swmm_2d_get_edge_bc_slope(self._engine, tri_idx, edge, &slope))
        return slope

    def set_edge_bc_slope(self, int tri_idx, int edge, double slope):
        """Set the boundary slope for a triangle edge.

        @param tri_idx: Triangle index.
        @type tri_idx: int
        @param edge: Edge index in C{0}-C{2}.
        @type edge: int
        @param slope: Boundary slope.
        @type slope: float
        @raise RuntimeError: If the C API rejects the assignment.
        """
        _check(swmm_2d_set_edge_bc_slope(self._engine, tri_idx, edge, slope))

    def get_edge_bc_cum_flux(self, int tri_idx, int edge) -> float:
        """Return the cumulative boundary flux for a triangle edge.

        @param tri_idx: Triangle index.
        @type tri_idx: int
        @param edge: Edge index in C{0}-C{2}.
        @type edge: int
        @return: Cumulative boundary flux through the edge.
        @rtype: float
        @raise RuntimeError: If the C API call fails.
        """
        cdef double cum_flux = 0.0
        _check(swmm_2d_get_edge_bc_cum_flux(self._engine, tri_idx, edge, &cum_flux))
        return cum_flux
