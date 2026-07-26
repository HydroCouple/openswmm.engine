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
from ._enums import SurfaceForcingMode, ForcingPersist, SurfaceBoundaryType


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

    def prepare_for_edit(self) -> None:
        """Make the parsed 2D mesh editable without a full ``initialize()``.

        Mesh-edit setters (vertex Z, triangle Manning's *n*, triangle/vertex
        tags, vertex→node coupling) work as soon as the mesh is parsed, but
        per-edge boundary-condition and conveyance edits additionally need the
        authored ``[2D_BOUNDARY_CONDITIONS]`` / ``[2D_EDGE_CONVEYANCE]`` rows
        drained into live storage first. Call this once before editing those
        so the changes take effect and are written on save. No-op when the
        engine is already initialized/drained.

        @raise RuntimeError: If no 2D mesh is present.
        """
        _check(swmm_2d_prepare_for_edit(self._engine))

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
        cdef void* eng = self._engine
        cdef double* px = <double*>x.data
        cdef double* py = <double*>y.data
        cdef double* pz = <double*>z.data
        cdef int err
        with nogil:
            err = swmm_2d_vertex_get_xyz_bulk(eng, px, py, pz)
        _check(err)
        return x, y, z

    def set_vertex_z(self, int idx, double z) -> None:
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
        _check(swmm_2d_set_vertex_z(self._engine, idx, z))

    def get_vertex_xyz(self, int idx):
        """Return the C{(x, y, z)} coordinates of one mesh vertex.

        Scalar counterpart to :meth:`get_vertex_coords` (which returns
        whole-mesh arrays). Values are in project coordinate/vertical units.

        @param idx: Vertex index (0-based).
        @type idx: int
        @return: C{(x, y, z)}.
        @rtype: tuple[float, float, float]
        @raise RuntimeError: If the C API call fails.
        """
        cdef double x = 0.0, y = 0.0, z = 0.0
        _check(swmm_2d_vertex_get_xyz(self._engine, idx, &x, &y, &z))
        return (x, y, z)

    def get_vertex_head(self, int idx) -> float:
        """Return the water-surface head at one mesh vertex.

        Scalar counterpart to :meth:`get_heads`. Value is in project
        vertical units.

        @param idx: Vertex index (0-based).
        @type idx: int
        @return: Head at the vertex.
        @rtype: float
        @raise RuntimeError: If the C API call fails.
        """
        cdef double head = 0.0
        _check(swmm_2d_vertex_get_head(self._engine, idx, &head))
        return head

    def get_vertex_tag(self, int idx) -> str:
        """Return the descriptive tag of a vertex (C{[2D_VERTICES]} TAG).

        Distinct from the 1D<->2D coupling node (see
        :meth:`get_vertex_coupled_node`).

        @param idx: Vertex index (0-based).
        @type idx: int
        @return: The tag string; empty when the vertex has no tag.
        @rtype: str
        @raise RuntimeError: If the C API call fails.
        """
        cdef char buf[256]
        _check(swmm_2d_get_vertex_tag(self._engine, idx, buf, 256))
        return buf.decode('utf-8')

    def set_vertex_tag(self, int idx, str tag) -> None:
        """Set the descriptive tag of a vertex (C{[2D_VERTICES]} TAG).

        @param idx: Vertex index (0-based).
        @type idx: int
        @param tag: New tag; an empty string clears it.
        @type tag: str
        @raise RuntimeError: If the C API rejects the assignment.
        """
        cdef bytes b = (tag or "").encode('utf-8')
        _check(swmm_2d_set_vertex_tag(self._engine, idx, b))

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

    def set_triangle_mannings(self, int idx, double n) -> None:
        """Set Manning's M{n} for a triangle.

        Persists in the C{MANNINGS_N} column of C{[2D_TRIANGLES]} on save.

        @param idx: Triangle index (0-based).
        @type idx: int
        @param n: New Manning's roughness; must be strictly positive.
        @type n: float
        @raise RuntimeError: If the C API rejects the value (e.g. C{n <= 0}).
        """
        _check(swmm_2d_set_triangle_mannings(self._engine, idx, n))

    def get_triangle_tag(self, int idx) -> str:
        """Return the descriptive tag of a triangle (C{[2D_TRIANGLES]} TAG).

        @param idx: Triangle index (0-based).
        @type idx: int
        @return: The tag string; empty when the triangle has no tag.
        @rtype: str
        @raise RuntimeError: If the C API call fails.
        """
        cdef char buf[256]
        _check(swmm_2d_get_triangle_tag(self._engine, idx, buf, 256))
        return buf.decode('utf-8')

    def set_triangle_tag(self, int idx, str tag) -> None:
        """Set the descriptive tag of a triangle (C{[2D_TRIANGLES]} TAG).

        @param idx: Triangle index (0-based).
        @type idx: int
        @param tag: New tag; an empty string clears it.
        @type tag: str
        @raise RuntimeError: If the C API rejects the assignment.
        """
        cdef bytes b = (tag or "").encode('utf-8')
        _check(swmm_2d_set_triangle_tag(self._engine, idx, b))

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

    def set_vertex_coupled_node(self, int vertex_idx, str node_name) -> None:
        """Couple a mesh vertex to a 1D SWMM node by name.

        Establishes the per-vertex 1D<->2D exchange point. Pass an empty
        string to clear the coupling. The name is resolved against the 1D
        node registry by the C API.

        @param vertex_idx: Vertex index (0-based).
        @type vertex_idx: int
        @param node_name: Target 1D node id, or C{""} to clear.
        @type node_name: str
        @raise RuntimeError: If the C API rejects the assignment (e.g. the
            node name does not resolve).
        """
        cdef bytes b = (node_name or "").encode('utf-8')
        _check(swmm_2d_set_vertex_coupled_node(self._engine, vertex_idx, b))

    def get_vertex_coupling_cd(self, int vertex_idx) -> float:
        """Return the coupling discharge coefficient of a vertex.

        Corresponds to the C{[2D_VERTEX_NODE_MAP]} CD column
        (default 0.65).

        @param vertex_idx: Vertex index (0-based).
        @type vertex_idx: int
        @return: Discharge coefficient.
        @rtype: float
        @raise RuntimeError: If the C API call fails.
        """
        cdef double cd = 0.0
        _check(swmm_2d_get_vertex_coupling_cd(self._engine, vertex_idx, &cd))
        return cd

    def set_vertex_coupling_cd(self, int vertex_idx, double cd) -> None:
        """Set the coupling discharge coefficient of a vertex.

        Corresponds to the C{[2D_VERTEX_NODE_MAP]} CD column and is
        persisted by the C{.inp} writer.

        @param vertex_idx: Vertex index (0-based).
        @type vertex_idx: int
        @param cd: Discharge coefficient; must be > 0.
        @type cd: float
        @raise RuntimeError: If the C API rejects the value (e.g. a
            non-positive coefficient).
        """
        _check(swmm_2d_set_vertex_coupling_cd(self._engine, vertex_idx, cd))

    def get_vertex_coupling_area(self, int vertex_idx) -> float:
        """Return the coupling exchange area of a vertex.

        Corresponds to the C{[2D_VERTEX_NODE_MAP]} AREA column in m^2
        (default 1.0).

        @param vertex_idx: Vertex index (0-based).
        @type vertex_idx: int
        @return: Exchange area in m^2.
        @rtype: float
        @raise RuntimeError: If the C API call fails.
        """
        cdef double area = 0.0
        _check(swmm_2d_get_vertex_coupling_area(self._engine, vertex_idx,
                                                  &area))
        return area

    def set_vertex_coupling_area(self, int vertex_idx, double area) -> None:
        """Set the coupling exchange area of a vertex.

        Corresponds to the C{[2D_VERTEX_NODE_MAP]} AREA column (m^2) and
        is persisted by the C{.inp} writer.

        @param vertex_idx: Vertex index (0-based).
        @type vertex_idx: int
        @param area: Exchange area in m^2; must be > 0.
        @type area: float
        @raise RuntimeError: If the C API rejects the value (e.g. a
            non-positive area).
        """
        _check(swmm_2d_set_vertex_coupling_area(self._engine, vertex_idx,
                                                  area))

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
        cdef void* eng = self._engine
        cdef double* p = <double*>arr.data
        cdef int err
        with nogil:
            err = swmm_2d_get_depths_bulk(eng, p)
        _check(err)
        return arr

    def get_heads(self):
        """Return total heads for all triangles as a NumPy array. The GIL
        is released during the C call.

        @return: Array of shape C{(n_triangles,)} with dtype C{float64}.
        @rtype: np.ndarray
        @raise RuntimeError: If the C API call fails.
        """
        cdef int n = self.n_triangles
        cdef np.ndarray[double, ndim=1] arr = np.empty(n, dtype=np.float64)
        cdef void* eng = self._engine
        cdef double* p = <double*>arr.data
        cdef int err
        with nogil:
            err = swmm_2d_get_heads_bulk(eng, p)
        _check(err)
        return arr

    def get_coupling_fluxes(self):
        """Return coupling fluxes for all triangles as a NumPy array. The
        GIL is released during the C call.

        @return: Array of shape C{(n_triangles,)} with dtype C{float64}.
            Positive values denote flux into the 2D surface.
        @rtype: np.ndarray
        @raise RuntimeError: If the C API call fails.
        """
        cdef int n = self.n_triangles
        cdef np.ndarray[double, ndim=1] arr = np.empty(n, dtype=np.float64)
        cdef void* eng = self._engine
        cdef double* p = <double*>arr.data
        cdef int err
        with nogil:
            err = swmm_2d_get_coupling_fluxes_bulk(eng, p)
        _check(err)
        return arr

    def get_edge_flux_bulk(self):
        """Return normal edge fluxes for all triangle edges as a NumPy array.
        The GIL is released during the C call.

        The array is indexed as C{[tri*3 + localEdge]} where C{localEdge}
        is the edge opposite vertex C{localEdge} (0, 1, or 2). Positive
        flux flows outward through the edge's outward normal.

        @return: Array of shape C{(n_triangles*3,)} with dtype C{float64}.
        @rtype: np.ndarray
        @raise RuntimeError: If the C API call fails.
        """
        cdef int n = self.n_triangles * 3
        cdef np.ndarray[double, ndim=1] arr = np.empty(n, dtype=np.float64)
        cdef void* eng = self._engine
        cdef double* p = <double*>arr.data
        cdef int err
        with nogil:
            err = swmm_2d_get_edge_flux_bulk(eng, p)
        _check(err)
        return arr

    def get_edge_geometry_bulk(self):
        """Return time-invariant edge lengths and outward unit normal components.
        The GIL is released during the C call.

        Returns arrays indexed as C{[tri*3 + localEdge]}.  Use together
        with L{get_edge_flux_bulk} to reconstruct cell-centred velocity via
        the RT0 scheme.

        @return: Tuple C{(length, nx, ny)}, each of shape
            C{(n_triangles*3,)} with dtype C{float64}.
        @rtype: tuple
        @raise RuntimeError: If the C API call fails.
        """
        cdef int n = self.n_triangles * 3
        cdef np.ndarray[double, ndim=1] length = np.empty(n, dtype=np.float64)
        cdef np.ndarray[double, ndim=1] nx = np.empty(n, dtype=np.float64)
        cdef np.ndarray[double, ndim=1] ny = np.empty(n, dtype=np.float64)
        cdef void* eng = self._engine
        cdef double* pL = <double*>length.data
        cdef double* pX = <double*>nx.data
        cdef double* pY = <double*>ny.data
        cdef int err
        with nogil:
            err = swmm_2d_edge_get_geometry_bulk(eng, pL, pX, pY)
        _check(err)
        return length, nx, ny

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
        cdef void* eng = self._engine
        cdef double* p = <double*>arr.data
        cdef int err
        with nogil:
            err = swmm_2d_vertex_get_heads_bulk(eng, p)
        _check(err)
        return arr

    def get_vertex_render_depths(self):
        """Return render-oriented signed water depths at all vertices.

        This is the wet-masked, depth-weighted free-surface reconstruction
        C{eta_v - z_v} (m): dry-cell bed elevations never contribute (unlike
        L{get_vertex_heads}, whose dry-cell value is the bed elevation). The
        value is negative over the dry side of partially wet cells (sub-cell
        shoreline intercept) and C{0} where no incident cell is wet. This is
        the field GUIs should interpolate for water-surface rendering and
        profiles.

        @return: Array of shape C{(n_vertices,)} with dtype C{float64}.
        @rtype: np.ndarray
        @raise RuntimeError: If the C API call fails.
        """
        cdef int n = self.n_vertices
        cdef np.ndarray[double, ndim=1] arr = np.empty(n, dtype=np.float64)
        cdef void* eng = self._engine
        cdef double* p = <double*>arr.data
        cdef int err
        with nogil:
            err = swmm_2d_vertex_get_render_depths_bulk(eng, p)
        _check(err)
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
        """Return cumulative maximum-depth envelope for all triangles.

        @return: Array of shape C{(n_triangles,)} with dtype C{float64}, in m.
        @rtype: np.ndarray
        @raise RuntimeError: If the C API call fails.
        """
        cdef int n = self.n_triangles
        cdef np.ndarray[double, ndim=1] arr = np.empty(n, dtype=np.float64)
        _check(swmm_2d_get_stat_max_depths(self._engine, &arr[0]))
        return arr

    def get_stat_max_velocities(self):
        """Return cumulative maximum velocity-magnitude envelope for all triangles.

        @return: Array of shape C{(n_triangles,)} with dtype C{float64}, in m/s.
        @rtype: np.ndarray
        @raise RuntimeError: If the C API call fails.
        """
        cdef int n = self.n_triangles
        cdef np.ndarray[double, ndim=1] arr = np.empty(n, dtype=np.float64)
        _check(swmm_2d_get_stat_max_velocities(self._engine, &arr[0]))
        return arr

    def get_stat_max_continuity_err(self):
        """Return cumulative maximum M{abs(continuity residual)} envelope per triangle.

        @return: Array of shape C{(n_triangles,)} with dtype C{float64}, in
            C{m^3/s}.
        @rtype: np.ndarray
        @raise RuntimeError: If the C API call fails.
        """
        cdef int n = self.n_triangles
        cdef np.ndarray[double, ndim=1] arr = np.empty(n, dtype=np.float64)
        _check(swmm_2d_get_stat_max_continuity_err(self._engine, &arr[0]))
        return arr

    @property
    def continuity_error(self) -> float:
        """Global 2D surface continuity error.

        @return: M{(total_in - total_out) / total_in}, the domain mass-balance
            error as a fraction.
        @rtype: float
        @raise RuntimeError: If the 2D module did not run.
        """
        cdef double val
        _check(swmm_2d_get_continuity_error(self._engine, &val))
        return val

    def get_mass_balance(self):
        """Return the global 2D mass-balance terms.

        @return: Mapping with keys C{init_storage}, C{final_storage},
            C{rainfall_in}, C{coupling_1d_to_2d_in}, C{coupling_2d_to_1d_out},
            C{outfall_in}, C{outfall_out}, C{boundary_in}, C{boundary_out},
            C{evap_out} (all C{m^3}) and C{continuity_error} (fraction).
        @rtype: dict[str, float]
        @raise RuntimeError: If the 2D module did not run.
        """
        cdef double init_storage = 0.0
        cdef double final_storage = 0.0
        cdef double rainfall_in = 0.0
        cdef double coupling_in = 0.0
        cdef double coupling_out = 0.0
        cdef double outfall_in = 0.0
        cdef double outfall_out = 0.0
        cdef double boundary_in = 0.0
        cdef double boundary_out = 0.0
        cdef double evap_out = 0.0
        cdef double err = 0.0
        _check(swmm_2d_get_mass_balance(self._engine,
                                        &init_storage, &final_storage,
                                        &rainfall_in, &coupling_in,
                                        &coupling_out, &outfall_in,
                                        &outfall_out, &boundary_in,
                                        &boundary_out, &evap_out))
        _check(swmm_2d_get_continuity_error(self._engine, &err))
        return {
            "init_storage": init_storage,
            "final_storage": final_storage,
            "rainfall_in": rainfall_in,
            "coupling_1d_to_2d_in": coupling_in,
            "coupling_2d_to_1d_out": coupling_out,
            "outfall_in": outfall_in,
            "outfall_out": outfall_out,
            "boundary_in": boundary_in,
            "boundary_out": boundary_out,
            "evap_out": evap_out,
            "continuity_error": err,
        }

    # ====================================================================
    # Boundary conditions - forcing
    # ====================================================================

    def force_rainfall(self, int idx, double value, *,
                       mode=SurfaceForcingMode.OVERRIDE,
                       persist=ForcingPersist.RESET):
        """Force rainfall on a specific triangle.

        @param idx: Triangle index.
        @type idx: int
        @param value: Rainfall rate (m/s).
        @type value: float
        @param mode: How the value is applied. C{OVERRIDE} replaces the
            computed rainfall; C{ADD} adds to it.
        @type mode: L{SurfaceForcingMode}
        @param persist: C{PERSIST} holds the forcing until cleared; C{RESET}
            applies it for a single step.
        @type persist: L{ForcingPersist}
        @raise RuntimeError: If the C API rejects the forcing.
        """
        _check(swmm_2d_force_rainfall(self._engine, idx, value,
                                      int(mode), int(persist)))

    def force_rainfall_uniform(self, double value, *,
                               mode=SurfaceForcingMode.OVERRIDE,
                               persist=ForcingPersist.RESET):
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
        _check(swmm_2d_force_rainfall_uniform(self._engine, value,
                                              int(mode), int(persist)))

    def force_evap(self, int idx, double value, *,
                   mode=SurfaceForcingMode.OVERRIDE,
                   persist=ForcingPersist.RESET):
        """Force evaporation on a specific triangle.

        The rate is a demand: wet cells lose depth at this rate, shutting
        off smoothly as a cell dries (depths never go negative). The default
        rate is 0 unless forced. Negative values are treated as zero.

        @param idx: Triangle index.
        @type idx: int
        @param value: Evaporation rate (m/s; same SI convention as rainfall).
        @type value: float
        @param mode: How the value is applied. C{OVERRIDE} replaces the
            computed rate; C{ADD} adds to it.
        @type mode: L{SurfaceForcingMode}
        @param persist: C{PERSIST} holds the forcing until cleared; C{RESET}
            applies it for a single step.
        @type persist: L{ForcingPersist}
        @raise RuntimeError: If the C API rejects the forcing.
        """
        _check(swmm_2d_force_evap(self._engine, idx, value,
                                  int(mode), int(persist)))

    def force_evap_uniform(self, double value, *,
                           mode=SurfaceForcingMode.OVERRIDE,
                           persist=ForcingPersist.RESET):
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
        _check(swmm_2d_force_evap_uniform(self._engine, value,
                                          int(mode), int(persist)))

    def force_coupling_flux(self, int idx, double value, *,
                            mode=SurfaceForcingMode.OVERRIDE,
                            persist=ForcingPersist.RESET):
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
        _check(swmm_2d_force_coupling_flux(self._engine, idx, value,
                                           int(mode), int(persist)))

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

    def get_edge_bc_type(self, int tri_idx, int edge):
        """Return the boundary condition type for a triangle edge.

        @param tri_idx: Triangle index.
        @type tri_idx: int
        @param edge: Edge index in C{0}-C{2}.
        @type edge: int
        @return: Boundary condition type.
        @rtype: L{SurfaceBoundaryType}
        @raise RuntimeError: If the C API call fails.
        """
        cdef int bc_type = 0
        _check(swmm_2d_get_edge_bc_type(self._engine, tri_idx, edge, &bc_type))
        return SurfaceBoundaryType(bc_type)

    def set_edge_bc_type(self, int tri_idx, int edge, bc_type):
        """Set the boundary condition type for a triangle edge.

        @param tri_idx: Triangle index.
        @type tri_idx: int
        @param edge: Edge index in C{0}-C{2}.
        @type edge: int
        @param bc_type: Boundary condition type.
        @type bc_type: L{SurfaceBoundaryType}
        @raise RuntimeError: If the C API rejects the assignment.
        """
        _check(swmm_2d_set_edge_bc_type(self._engine, tri_idx, edge,
                                        int(bc_type)))

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

    def get_edge_bc_flow(self, int tri_idx, int edge) -> float:
        """Return the prescribed flow per metre for a SPECIFIED_FLOW edge.

        @param tri_idx: Triangle index.
        @type tri_idx: int
        @param edge: Edge index in C{0}-C{2}.
        @type edge: int
        @return: Prescribed flow per metre of edge (C{m^3/s/m}).
        @rtype: float
        @raise RuntimeError: If the C API call fails.
        """
        cdef double flow = 0.0
        _check(swmm_2d_get_edge_bc_flow(self._engine, tri_idx, edge, &flow))
        return flow

    def set_edge_bc_flow(self, int tri_idx, int edge, double flow):
        """Set the prescribed flow per metre for a SPECIFIED_FLOW edge.

        @param tri_idx: Triangle index.
        @type tri_idx: int
        @param edge: Edge index in C{0}-C{2}.
        @type edge: int
        @param flow: Prescribed flow per metre of edge (C{m^3/s/m}).
        @type flow: float
        @raise RuntimeError: If the C API rejects the assignment.
        """
        _check(swmm_2d_set_edge_bc_flow(self._engine, tri_idx, edge, flow))

    def set_edge_bc_tseries_name(self, int tri_idx, int edge, str name):
        """Set the timeseries name driving a SPECIFIED_STAGE edge.

        The name is resolved against the model's timeseries registry on the
        next forcing-step lookup. Pass an empty string to clear the slot
        (reverting to the constant C{edge_bc_head}).

        @param tri_idx: Triangle index.
        @type tri_idx: int
        @param edge: Edge index in C{0}-C{2}.
        @type edge: int
        @param name: Timeseries name, or C{""} to clear.
        @type name: str
        @raise RuntimeError: If the C API rejects the assignment.
        """
        cdef bytes b = name.encode("utf-8")
        _check(swmm_2d_set_edge_bc_tseries_name(self._engine, tri_idx, edge, b))

    def set_edge_bc_flow_tseries_name(self, int tri_idx, int edge, str name):
        """Set the timeseries name driving a SPECIFIED_FLOW edge.

        Same resolution contract as L{set_edge_bc_tseries_name}. Pass an
        empty string to clear the slot.

        @param tri_idx: Triangle index.
        @type tri_idx: int
        @param edge: Edge index in C{0}-C{2}.
        @type edge: int
        @param name: Timeseries name, or C{""} to clear.
        @type name: str
        @raise RuntimeError: If the C API rejects the assignment.
        """
        cdef bytes b = name.encode("utf-8")
        _check(swmm_2d_set_edge_bc_flow_tseries_name(self._engine, tri_idx, edge, b))

    def set_edge_bc_rating_curve_name(self, int tri_idx, int edge, str name):
        """Set the rating-curve name driving a RATING_CURVE edge.

        The stage-to-flow lookup is resolved against the model's curve
        registry on the next forcing-step lookup. Pass an empty string to
        clear the slot.

        @param tri_idx: Triangle index.
        @type tri_idx: int
        @param edge: Edge index in C{0}-C{2}.
        @type edge: int
        @param name: Rating-curve name, or C{""} to clear.
        @type name: str
        @raise RuntimeError: If the C API rejects the assignment.
        """
        cdef bytes b = name.encode("utf-8")
        _check(swmm_2d_set_edge_bc_rating_curve_name(self._engine, tri_idx, edge, b))

    # ------------------------------------------------------------------
    # Edge conveyance factor (§11A of docs/2dModelStrategy.md)
    # ------------------------------------------------------------------

    def get_edge_conveyance(self, int tri, int edge) -> float:
        """Return the per-edge conveyance factor in C{[0, 1]}.

        @param tri: Triangle index in C{[0, triangle_count)}.
        @type tri: int
        @param edge: Local edge index in C{{0, 1, 2}}.
        @type edge: int
        @return: Conveyance factor (1.0 = unrestricted, 0.0 = wall).
        @rtype: float
        @raise RuntimeError: If the C API call fails.
        """
        cdef double c = 1.0
        _check(swmm_2d_get_edge_conveyance(self._engine, tri, edge, &c))
        return c

    def set_edge_conveyance(self, int tri, int edge, double conveyance):
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
        _check(swmm_2d_set_edge_conveyance(self._engine, tri, edge, conveyance))

    def get_edge_conveyance_bulk(self):
        """Return a NumPy array of all per-edge conveyance factors.

        Length is C{triangle_count * 3}, indexed C{[tri*3 + edge]}.
        """
        import numpy as np
        cdef int nt = 0
        _check(swmm_2d_triangle_count(self._engine, &nt))
        cdef double[::1] out = np.empty(nt * 3, dtype=np.float64)
        cdef double* p = &out[0]
        cdef int err
        with nogil:
            err = swmm_2d_get_edge_conveyance_bulk(self._engine, p)
        _check(err)
        return np.asarray(out)

    def reset_edge_conveyance(self):
        """Reset every edge's conveyance factor to 1.0 (unrestricted)."""
        _check(swmm_2d_reset_edge_conveyance(self._engine))
