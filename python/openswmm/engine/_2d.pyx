# _2d.pyx — Cython wrapper for the 2D surface routing C API.
#
# Provides a Pythonic Surface2D class that wraps the C functions in
# openswmm_2d.h. Supports numpy arrays for bulk data access.

cimport numpy as np
import numpy as np
from libc.stdint cimport uintptr_t

from ._2d cimport *


cdef inline void _check(int rc) except *:
    """Raise RuntimeError if a C API call returns non-zero."""
    if rc != 0:
        raise RuntimeError(f"SWMM 2D API error code {rc}")


cdef class Surface2D:
    """Read/write interface to the optional 2D surface routing module.

    Parameters
    ----------
    engine_ptr : int
        The raw engine handle (SWMM_Engine cast to uintptr_t).
    """

    cdef void* _engine

    def __cinit__(self, uintptr_t engine_ptr):
        self._engine = <void*>engine_ptr

    # ------------------------------------------------------------------
    # Status
    # ------------------------------------------------------------------

    @property
    def is_active(self) -> bool:
        """True if the 2D module is active for this simulation."""
        cdef int active = 0
        _check(swmm_2d_is_active(self._engine, &active))
        return bool(active)

    # ------------------------------------------------------------------
    # Mesh geometry
    # ------------------------------------------------------------------

    @property
    def n_vertices(self) -> int:
        """Number of mesh vertices."""
        cdef int count = 0
        _check(swmm_2d_vertex_count(self._engine, &count))
        return count

    @property
    def n_triangles(self) -> int:
        """Number of mesh triangles."""
        cdef int count = 0
        _check(swmm_2d_triangle_count(self._engine, &count))
        return count

    def get_vertex_coords(self):
        """Return (x, y, z) numpy arrays for all vertices."""
        cdef int n = self.n_vertices
        cdef np.ndarray[double, ndim=1] x = np.empty(n, dtype=np.float64)
        cdef np.ndarray[double, ndim=1] y = np.empty(n, dtype=np.float64)
        cdef np.ndarray[double, ndim=1] z = np.empty(n, dtype=np.float64)
        _check(swmm_2d_vertex_get_xyz_bulk(self._engine, &x[0], &y[0], &z[0]))
        return x, y, z

    def get_triangle_vertices(self, int idx):
        """Return (v0, v1, v2) vertex indices for a triangle."""
        cdef int v0, v1, v2
        _check(swmm_2d_triangle_get_vertices(self._engine, idx, &v0, &v1, &v2))
        return v0, v1, v2

    def get_triangle_area(self, int idx) -> float:
        """Return the area of a triangle."""
        cdef double area
        _check(swmm_2d_triangle_get_area(self._engine, idx, &area))
        return area

    def get_triangle_centroid(self, int idx):
        """Return (cx, cy, cz) centroid coordinates for a triangle."""
        cdef double cx, cy, cz
        _check(swmm_2d_triangle_get_centroid(self._engine, idx, &cx, &cy, &cz))
        return cx, cy, cz

    def get_triangle_mannings(self, int idx) -> float:
        """Return Manning's n for a triangle."""
        cdef double n
        _check(swmm_2d_triangle_get_mannings(self._engine, idx, &n))
        return n

    def get_triangle_neighbours(self, int idx):
        """Return (n0, n1, n2) neighbour triangle indices (-1 = boundary)."""
        cdef int n0, n1, n2
        _check(swmm_2d_triangle_get_neighbours(self._engine, idx, &n0, &n1, &n2))
        return n0, n1, n2

    # ------------------------------------------------------------------
    # Coupling
    # ------------------------------------------------------------------

    @property
    def vertex_coupling_count(self) -> int:
        """Number of vertex-to-node coupling points."""
        cdef int count = 0
        _check(swmm_2d_vertex_coupling_count(self._engine, &count))
        return count

    @property
    def triangle_coupling_count(self) -> int:
        """Number of triangle-to-node coupling points."""
        cdef int count = 0
        _check(swmm_2d_triangle_coupling_count(self._engine, &count))
        return count

    def get_vertex_coupled_node(self, int vertex_idx) -> int:
        """Return the SWMM node index coupled to a vertex (-1 if none)."""
        cdef int node_idx
        _check(swmm_2d_vertex_get_coupled_node(self._engine, vertex_idx,
                                                 &node_idx))
        return node_idx

    def get_triangle_coupled_node(self, int tri_idx) -> int:
        """Return the SWMM node index coupled to a triangle (-1 if none)."""
        cdef int node_idx
        _check(swmm_2d_triangle_get_coupled_node(self._engine, tri_idx,
                                                    &node_idx))
        return node_idx

    # ------------------------------------------------------------------
    # State — per triangle
    # ------------------------------------------------------------------

    def get_depths(self):
        """Return depths for all triangles as a numpy array."""
        cdef int n = self.n_triangles
        cdef np.ndarray[double, ndim=1] arr = np.empty(n, dtype=np.float64)
        _check(swmm_2d_get_depths_bulk(self._engine, &arr[0]))
        return arr

    def get_heads(self):
        """Return total heads for all triangles as a numpy array."""
        cdef int n = self.n_triangles
        cdef np.ndarray[double, ndim=1] arr = np.empty(n, dtype=np.float64)
        _check(swmm_2d_get_heads_bulk(self._engine, &arr[0]))
        return arr

    def get_coupling_fluxes(self):
        """Return coupling fluxes for all triangles as a numpy array."""
        cdef int n = self.n_triangles
        cdef np.ndarray[double, ndim=1] arr = np.empty(n, dtype=np.float64)
        _check(swmm_2d_get_coupling_fluxes_bulk(self._engine, &arr[0]))
        return arr

    def get_depth(self, int idx) -> float:
        """Return depth at a specific triangle."""
        cdef double val
        _check(swmm_2d_get_depth(self._engine, idx, &val))
        return val

    def get_head(self, int idx) -> float:
        """Return total head at a specific triangle."""
        cdef double val
        _check(swmm_2d_get_head(self._engine, idx, &val))
        return val

    def get_rainfall(self, int idx) -> float:
        """Return the current rainfall at a specific triangle."""
        cdef double val
        _check(swmm_2d_get_rainfall(self._engine, idx, &val))
        return val

    def get_net_source(self, int idx) -> float:
        """Return the net source term at a specific triangle."""
        cdef double val
        _check(swmm_2d_get_net_source(self._engine, idx, &val))
        return val

    def get_coupling_flux(self, int idx) -> float:
        """Return the coupling flux at a specific triangle."""
        cdef double val
        _check(swmm_2d_get_coupling_flux(self._engine, idx, &val))
        return val

    # ------------------------------------------------------------------
    # State — per vertex
    # ------------------------------------------------------------------

    def get_vertex_heads(self):
        """Return reconstructed heads at all vertices as a numpy array."""
        cdef int n = self.n_vertices
        cdef np.ndarray[double, ndim=1] arr = np.empty(n, dtype=np.float64)
        _check(swmm_2d_vertex_get_heads_bulk(self._engine, &arr[0]))
        return arr

    # ------------------------------------------------------------------
    # Statistics
    # ------------------------------------------------------------------

    @property
    def max_depth(self) -> float:
        """Maximum depth across all triangles."""
        cdef double val
        _check(swmm_2d_get_max_depth(self._engine, &val))
        return val

    @property
    def total_volume(self) -> float:
        """Total 2D surface volume (sum of depth * area)."""
        cdef double val
        _check(swmm_2d_get_total_volume(self._engine, &val))
        return val

    @property
    def total_exchange_flow(self) -> float:
        """Total exchange flow rate (m³/s, + = into 1D)."""
        cdef double val
        _check(swmm_2d_get_total_exchange_flow(self._engine, &val))
        return val

    @property
    def cvode_steps(self) -> int:
        """Number of CVODE internal steps in last advance."""
        cdef long val
        _check(swmm_2d_get_cvode_steps(self._engine, &val))
        return val

    @property
    def cvode_last_step(self) -> float:
        """Last CVODE internal step size."""
        cdef double val
        _check(swmm_2d_get_cvode_last_step(self._engine, &val))
        return val

    def get_stat_max_depths(self):
        """Return cumulative max depth statistics as a numpy array."""
        cdef int n = self.n_triangles
        cdef np.ndarray[double, ndim=1] arr = np.empty(n, dtype=np.float64)
        _check(swmm_2d_get_stat_max_depths(self._engine, &arr[0]))
        return arr

    # ------------------------------------------------------------------
    # Forcing
    # ------------------------------------------------------------------

    def force_rainfall(self, int idx, double value, int mode=1, int persist=0):
        """Force rainfall on a specific triangle."""
        _check(swmm_2d_force_rainfall(self._engine, idx, value, mode, persist))

    def force_rainfall_uniform(self, double value, int mode=1, int persist=0):
        """Force uniform rainfall on all triangles."""
        _check(swmm_2d_force_rainfall_uniform(self._engine, value, mode, persist))

    def force_coupling_flux(self, int idx, double value, int mode=1,
                             int persist=0):
        """Force coupling flux on a specific triangle."""
        _check(swmm_2d_force_coupling_flux(self._engine, idx, value, mode,
                                            persist))

    def force_clear_all(self):
        """Clear all 2D forcings."""
        _check(swmm_2d_force_clear_all(self._engine))

    # ------------------------------------------------------------------
    # Options
    # ------------------------------------------------------------------

    @property
    def dry_depth(self) -> float:
        """Dry depth threshold (m)."""
        cdef double val
        _check(swmm_2d_get_dry_depth(self._engine, &val))
        return val

    @dry_depth.setter
    def dry_depth(self, double value):
        _check(swmm_2d_set_dry_depth(self._engine, value))

    @property
    def rel_tolerance(self) -> float:
        """CVODE relative tolerance."""
        cdef double val
        _check(swmm_2d_get_rel_tolerance(self._engine, &val))
        return val

    @rel_tolerance.setter
    def rel_tolerance(self, double value):
        _check(swmm_2d_set_rel_tolerance(self._engine, value))

    @property
    def abs_tolerance(self) -> float:
        """CVODE absolute tolerance."""
        cdef double val
        _check(swmm_2d_get_abs_tolerance(self._engine, &val))
        return val

    @abs_tolerance.setter
    def abs_tolerance(self, double value):
        _check(swmm_2d_set_abs_tolerance(self._engine, value))

    # ------------------------------------------------------------------
    # Boundary edges
    # ------------------------------------------------------------------

    @property
    def boundary_edge_count(self) -> int:
        """Number of boundary edges in the 2D mesh."""
        cdef int count = 0
        _check(swmm_2d_boundary_edge_count(self._engine, &count))
        return count

    def get_edge_bc_type(self, int tri_idx, int edge) -> int:
        """Return the boundary condition type for a triangle edge."""
        cdef int bc_type = 0
        _check(swmm_2d_get_edge_bc_type(self._engine, tri_idx, edge, &bc_type))
        return bc_type

    def set_edge_bc_type(self, int tri_idx, int edge, int bc_type):
        """Set the boundary condition type for a triangle edge."""
        _check(swmm_2d_set_edge_bc_type(self._engine, tri_idx, edge, bc_type))

    def get_edge_bc_head(self, int tri_idx, int edge) -> float:
        """Return the boundary head for a triangle edge."""
        cdef double head = 0.0
        _check(swmm_2d_get_edge_bc_head(self._engine, tri_idx, edge, &head))
        return head

    def set_edge_bc_head(self, int tri_idx, int edge, double head):
        """Set the boundary head for a triangle edge."""
        _check(swmm_2d_set_edge_bc_head(self._engine, tri_idx, edge, head))

    def get_edge_bc_slope(self, int tri_idx, int edge) -> float:
        """Return the boundary slope for a triangle edge."""
        cdef double slope = 0.0
        _check(swmm_2d_get_edge_bc_slope(self._engine, tri_idx, edge, &slope))
        return slope

    def set_edge_bc_slope(self, int tri_idx, int edge, double slope):
        """Set the boundary slope for a triangle edge."""
        _check(swmm_2d_set_edge_bc_slope(self._engine, tri_idx, edge, slope))

    def get_edge_bc_cum_flux(self, int tri_idx, int edge) -> float:
        """Return the cumulative boundary flux for a triangle edge."""
        cdef double cum_flux = 0.0
        _check(swmm_2d_get_edge_bc_cum_flux(self._engine, tri_idx, edge, &cum_flux))
        return cum_flux
