# SPDX-License-Identifier: Apache-2.0
#
# Copyright 2026 Caleb Buahin
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""
2D Surface Routing
==================

:author: Caleb Buahin
:copyright: Copyright (c) 2026 Caleb Buahin
:license: Apache-2.0

Cython wrapper for the 2D surface routing C API.

The :class:`Surface2D` class wraps the C functions declared in
``openswmm_2d.h`` and supports NumPy arrays for bulk data access. The
2D module is optional and is only available when the package is built
with ``OPENSWMM_BUILD_2D=ON``.
"""

cimport numpy as np
import numpy as np
from libc.stdint cimport uintptr_t

from collections import namedtuple
from collections.abc import MutableMapping

from ._2d cimport *
from ._enums import (SurfaceForcingMode, ForcingPersist, SurfaceBoundaryType,
                     SurfaceInfilMethod, SurfaceInfilDest)


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
    unstructured triangular mesh and is integrated in time with the
    explicit local-inertial finite-volume marcher. Two-way coupling with
    the 1D drainage network is supported per-vertex and per-triangle.

    @ivar _engine: Internal pointer to the underlying C{SWMM_Engine}
        handle (managed by the Cython extension).
    """

    cdef void* _engine
    cdef object _infiltration

    def __cinit__(self, uintptr_t engine_ptr):
        """Construct a L{Surface2D} accessor from a raw engine handle.

        @param engine_ptr: The raw engine handle (C{SWMM_Engine} cast to
            C{uintptr_t}).
        @type engine_ptr: int
        """
        self._engine = <void*>engine_ptr
        self._infiltration = None

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

    def get_triangle_init_depth(self, int idx) -> float:
        """Return the initial water depth of a triangle.

        The value is in B{mesh length units} — feet on a US-C{FLOW_UNITS}
        project, metres on SI or on a mesh file that declared
        C{;; UNITS: SI (m)} — the same convention as the vertex Z column.

        @param idx: Triangle index (0-based).
        @type idx: int
        @return: Initial depth; C{0.0} means the triangle starts dry.
        @rtype: float
        @raise RuntimeError: If the C API call fails.
        """
        cdef double d
        _check(swmm_2d_triangle_get_init_depth(self._engine, idx, &d))
        return d

    def set_triangle_init_depth(self, int idx, double depth) -> None:
        """Set the initial water depth of a triangle.

        Applied to the solver state when the 2D surface initializes, and
        persisted in the C{INIT_DEPTH} column of C{[2D_TRIANGLES]} on save.

        @param idx: Triangle index (0-based).
        @type idx: int
        @param depth: Initial depth in mesh length units; must be M{>= 0}.
        @type depth: float
        @raise RuntimeError: If the C API rejects the value (e.g. negative).
        """
        _check(swmm_2d_set_triangle_init_depth(self._engine, idx, depth))

    def get_triangle_init_velocity(self, int idx):
        """Return the C{(u, v)} initial velocity of a triangle.

        @param idx: Triangle index (0-based).
        @type idx: int
        @return: Tuple C{(u, v)} in m/s; C{(0.0, 0.0)} when at rest.
        @rtype: tuple
        @raise RuntimeError: If the C API call fails.
        """
        cdef double u, v
        _check(swmm_2d_triangle_get_init_velocity(self._engine, idx, &u, &v))
        return u, v

    def set_triangle_init_velocity(self, int idx, double u, double v) -> None:
        """Set the C{(u, v)} initial velocity of a triangle.

        Projected onto the explicit marcher's face normals as M{(h*u, h*v)}
        when the 2D surface initializes — at M{t = 0} only; hotstart and
        reinitialize still zero the face momentum. Persisted as sparse
        C{[2D_INITIAL_VELOCITY]} rows on save.

        @param idx: Triangle index (0-based).
        @type idx: int
        @param u: X-component of velocity in m/s; must be finite.
        @type u: float
        @param v: Y-component of velocity in m/s; must be finite.
        @type v: float
        @raise RuntimeError: If the C API rejects a non-finite value.
        """
        _check(swmm_2d_set_triangle_init_velocity(self._engine, idx, u, v))

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

    def add_triangle_coupling(self, int tri_idx, str node_name,
                              double cd, double area) -> None:
        """Append one node->cell coupling row for a triangle.

        Corresponds to the C{[2D_TRIANGLE_NODE_MAP]} repeated-row form. A
        triangle may carry several coupling rows (one per node), so this
        APPENDS rather than overwrites (contrast
        L{set_vertex_coupled_node}). The node name is stored verbatim and
        resolved against the current model; the C{.inp} writer emits one
        row per coupling.

        @param tri_idx: Triangle index (0-based).
        @type tri_idx: int
        @param node_name: Target 1D node id; must be non-empty.
        @type node_name: str
        @param cd: Discharge coefficient; must be > 0 (default 0.65).
        @type cd: float
        @param area: Effective exchange area in m^2; must be > 0.
        @type area: float
        @raise RuntimeError: If the C API rejects the row (bad triangle
            index, empty name, or non-positive cd/area).
        """
        cdef bytes b = (node_name or "").encode('utf-8')
        _check(swmm_2d_add_triangle_coupling(self._engine, tri_idx, b,
                                              cd, area))

    def clear_triangle_couplings(self) -> None:
        """Remove every authored node->cell coupling row (all triangles).

        Also clears the legacy per-triangle mirror (coupled node, CD and
        AREA back to defaults). Vertex couplings are untouched.

        @raise RuntimeError: If the C API call fails.
        """
        _check(swmm_2d_clear_triangle_couplings(self._engine))

    @property
    def triangle_coupling_rows(self) -> int:
        """Number of authored node->cell coupling rows.

        This counts C{[2D_TRIANGLE_NODE_MAP]} rows and is >= the number of
        distinct coupled triangles (a triangle may carry several rows).

        @return: Row count.
        @rtype: int
        @raise RuntimeError: If the C API call fails.
        """
        cdef int count = 0
        _check(swmm_2d_triangle_coupling_rows(self._engine, &count))
        return count

    def get_triangle_coupling_row(self, int row_idx):
        """Read one authored node->cell coupling row by row index.

        @param row_idx: Row index in C{[0, triangle_coupling_rows)}.
        @type row_idx: int
        @return: Tuple C{(tri_idx, node_idx, cd, area)} where C{node_idx}
            is C{-1} if the node name is unresolved.
        @rtype: tuple[int, int, float, float]
        @raise RuntimeError: If the C API call fails.
        """
        cdef int tri_idx = 0
        cdef int node_idx = 0
        cdef double cd = 0.0
        cdef double area = 0.0
        _check(swmm_2d_get_triangle_coupling_row(self._engine, row_idx,
                                                  &tri_idx, &node_idx,
                                                  &cd, &area))
        return (tri_idx, node_idx, cd, area)

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
    def solver_steps(self) -> int:
        """Number of explicit-marcher sub-steps in the last advance.

        Renamed from C{cvode_steps} with the D2 CVODE/ARKODE retirement.

        @return: Step count.
        @rtype: int
        @raise RuntimeError: If the C API call fails.
        """
        cdef long val
        _check(swmm_2d_get_solver_steps(self._engine, &val))
        return val

    @property
    def solver_last_step(self) -> float:
        """The explicit marcher's last sub-step size in seconds.

        Renamed from C{cvode_last_step} with the D2 CVODE/ARKODE
        retirement.

        @return: Step size.
        @rtype: float
        @raise RuntimeError: If the C API call fails.
        """
        cdef double val
        _check(swmm_2d_get_solver_last_step(self._engine, &val))
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

    # ------------------------------------------------------------------
    # Per-cell infiltration (plan §5.5, track I)
    # ------------------------------------------------------------------

    @property
    def infiltration(self):
        """``surface2d.infiltration`` — the L{Infiltration2DView} sub-view.

        Per-cell infiltration for the C{GROUNDWATER OFF} path: options,
        the tag-default mapping, per-cell overrides, and the held-rate /
        cumulative-depth readers.

        @return: The cached L{Infiltration2DView} for this mesh.
        @rtype: Infiltration2DView
        """
        if self._infiltration is None:
            self._infiltration = Infiltration2DView(<uintptr_t>self._engine)
        return self._infiltration


# ========================================================================
# Per-cell infiltration (plan §5.5.6, track I step I6)
# ========================================================================

Infil2DRow = namedtuple(
    "Infil2DRow", "method params dest",
    defaults=((0.0, 0.0, 0.0, 0.0, 0.0), SurfaceInfilDest.LOST),
)
Infil2DRow.__doc__ = """One infiltration specification.

``method`` is a L{SurfaceInfilMethod}, or ``None`` for "no infiltration
model" (the C{NONE} token). ``params`` is a tuple of up to five
POSITIONAL values in B{PROJECT UNITS} — the same numbers a user types
into a legacy C{[INFILTRATION]} row (in/hr and in on a US-C{FLOW_UNITS}
project, mm/hr and mm on SI):

    ==================  =========  ====  ============  ============  ====
    method              params[0]  [1]   [2]           [3]           [4]
    ==================  =========  ====  ============  ============  ====
    HORTON              f0         fmin  decay (1/hr)  dry_time (d)  Fmax
    MOD_HORTON          f0         fmin  decay (1/hr)  dry_time (d)  Fmax
    GREEN_AMPT          suction    Ks    IMD           --            --
    MOD_GREEN_AMPT      suction    Ks    IMD           --            --
    CURVE_NUMBER        CN         --    dry_time (d)  --            --
    CONSTANT            rate       --    --            --            --
    ==================  =========  ====  ============  ============  ====

``dest`` is a L{SurfaceInfilDest}; only C{LOST} is routed in this release.
"""

Infil2DCell = namedtuple("Infil2DCell", "row is_override")
Infil2DCell.__doc__ = """The infiltration specification in force at one cell.

Unpacks as ``row, is_override = surface2d.infiltration.cell(tri)``.
``is_override`` is C{True} when the row came from the per-cell
C{[2D_INFILTRATION]} layer rather than a tag / C{'*'} default.
"""


cdef void _infil_row_to_c(object row, SWMM_Infil2DRow* out) except *:
    """Fill a C{SWMM_Infil2DRow} from an L{Infil2DRow} (or a NONE row).

    @param row: The row to convert; C{None} or a row whose C{method} is
        C{None} produces the C{NONE} specification.
    @raise ValueError: If more than five parameters are supplied.
    """
    cdef int k
    out.has_method = 0
    out.method = 0
    for k in range(5):
        out.p[k] = 0.0
    out.dest = <int>SurfaceInfilDest.LOST

    if row is None or row.method is None:
        return

    params = tuple(row.params) if row.params is not None else ()
    if len(params) > 5:
        raise ValueError(
            f"at most 5 infiltration parameters, got {len(params)}")

    out.has_method = 1
    out.method = <int>int(SurfaceInfilMethod(row.method))
    for k in range(len(params)):
        out.p[k] = float(params[k])
    out.dest = <int>int(SurfaceInfilDest(row.dest))


cdef object _infil_row_from_c(SWMM_Infil2DRow* c):
    """Build an L{Infil2DRow} from a C{SWMM_Infil2DRow}."""
    cdef object params = (c.p[0], c.p[1], c.p[2], c.p[3], c.p[4])
    if not c.has_method:
        return Infil2DRow(None, params, SurfaceInfilDest.LOST)
    return Infil2DRow(SurfaceInfilMethod(c.method), params,
                      SurfaceInfilDest(c.dest))


class Infil2DDefaults(MutableMapping):
    """``surface2d.infiltration.defaults`` — tag → L{Infil2DRow} mapping.

    Mirrors C{[2D_INFILTRATION_DEFAULTS]}. The key C{"*"} is the mesh-wide
    fallback; every other key matches the C{TAG} column of
    C{[2D_TRIANGLES]}. Resolution order is
    C{per-cell override > tag row > '*' row > none}.

    .. code-block:: python

        infil = solver.surface2d.infiltration
        infil.defaults["*"] = Infil2DRow(None)                  # no default
        infil.defaults["LAWN"] = Infil2DRow(
            SurfaceInfilMethod.HORTON, (3.0, 0.5, 4.14, 7.0, 0.0))
        del infil.defaults["WOODS"]

    Assignment is only accepted before the solver initializes — see
    L{Infiltration2DView} for the staleness rule.
    """

    def __init__(self, engine_ptr):
        self._ptr = engine_ptr

    def __len__(self):
        cdef void* eng = <void*><uintptr_t>self._ptr
        cdef int n = 0
        _check(swmm_infil2d_defaults_count(eng, &n))
        return n

    def _tags(self):
        """Return the authored tags in file order (C{'*'} may be anywhere)."""
        cdef void* eng = <void*><uintptr_t>self._ptr
        cdef int n = 0
        cdef char buf[256]
        cdef int i
        _check(swmm_infil2d_defaults_count(eng, &n))
        out = []
        for i in range(n):
            _check(swmm_infil2d_get_default_tag(eng, i, buf, 256))
            out.append(buf.decode("utf-8"))
        return out

    def __iter__(self):
        # Materialised deliberately: a generator would put the C{char[256]}
        # scratch buffer in a closure, which Cython cannot do.
        return iter(self._tags())

    def __getitem__(self, key):
        cdef void* eng = <void*><uintptr_t>self._ptr
        cdef SWMM_Infil2DRow row
        cdef int i
        tags = self._tags()
        for i in range(len(tags)):
            if tags[i] != key:
                continue
            _check(swmm_infil2d_get_default(eng, i, &row))
            return _infil_row_from_c(&row)
        raise KeyError(key)

    def __setitem__(self, key, value):
        cdef void* eng = <void*><uintptr_t>self._ptr
        cdef bytes tag = str(key).encode("utf-8")
        cdef SWMM_Infil2DRow row
        _infil_row_to_c(value, &row)
        _check(swmm_infil2d_set_default(eng, tag, &row))

    def __delitem__(self, key):
        cdef void* eng = <void*><uintptr_t>self._ptr
        cdef bytes tag
        if key not in self:
            raise KeyError(key)
        tag = str(key).encode("utf-8")
        _check(swmm_infil2d_remove_default(eng, tag))

    def __repr__(self):
        try:
            return f"<Infil2DDefaults {sorted(self)}>"
        except Exception:
            return "<Infil2DDefaults (unavailable)>"


cdef class Infiltration2DView:
    """``surface2d.infiltration`` — per-cell infiltration on the 2D mesh.

    The C{GROUNDWATER OFF} loss model (plan §5.5): a held per-cell rate,
    recomputed on the C{INFIL_STEP} cadence and consumed by the explicit
    marcher. Wraps C{openswmm_infil2d.h}.

    .. code-block:: python

        infil = solver.surface2d.infiltration
        infil.infil_step = 300.0                       # seconds
        infil.defaults["LAWN"] = Infil2DRow(
            SurfaceInfilMethod.HORTON, (3.0, 0.5, 4.14, 7.0, 0.0))
        infil.set_cells([12, 13, 14], Infil2DRow(
            SurfaceInfilMethod.CURVE_NUMBER, (85.0, 0.0, 7.0)))

        row, is_override = infil.cell(12)
        f = infil.rate()          # m/s, per triangle
        F = infil.cumulative()    # m,   per triangle
        infil.total_volume        # m^3, the infil_out ledger row

    B{Units.} Row parameters are in B{project units} (see L{Infil2DRow});
    every readback channel is SI, like the rest of the 2D API.

    B{Staleness.} Parameters are baked into per-cell kernel state once,
    when the 2D surface initializes, and there is no per-cell re-init path.
    Every writer here therefore raises C{RuntimeError} (engine
    C{SWMM_ERR_LIFECYCLE}) once the solver has been initialized; edit in the
    opened state, then initialize.

    @ivar _engine: Internal pointer to the underlying C{SWMM_Engine} handle.
    """

    cdef void* _engine
    cdef object _ptr
    cdef object _defaults

    def __cinit__(self, uintptr_t engine_ptr):
        """Construct the view from a raw engine handle.

        @param engine_ptr: The raw engine handle (C{SWMM_Engine} cast to
            C{uintptr_t}).
        @type engine_ptr: int
        """
        self._engine = <void*>engine_ptr
        self._ptr = int(engine_ptr)
        self._defaults = None

    # -- options -------------------------------------------------------

    @property
    def infil_step(self) -> float:
        """Evaluation cadence in B{seconds} (C{[2D_INFILTRATION_OPTIONS]}).

        C{<= 0} means "use the project C{WET_STEP}", which the 2D surface
        resolves when it initializes. The value reported is the authored
        one, not the resolved one.

        @rtype: float
        @raise RuntimeError: If the C API call fails.
        """
        cdef SWMM_Infil2DOptions opts
        _check(swmm_infil2d_get_options(self._engine, &opts))
        return opts.infil_step

    @infil_step.setter
    def infil_step(self, double seconds) -> None:
        cdef SWMM_Infil2DOptions opts
        opts.infil_step = seconds
        _check(swmm_infil2d_set_options(self._engine, &opts))

    # -- tag defaults --------------------------------------------------

    @property
    def defaults(self):
        """The C{[2D_INFILTRATION_DEFAULTS]} tag → L{Infil2DRow} mapping.

        @rtype: Infil2DDefaults
        """
        if self._defaults is None:
            self._defaults = Infil2DDefaults(self._ptr)
        return self._defaults

    # -- per-cell overrides --------------------------------------------

    def cell(self, int tri):
        """Return the infiltration specification in force at one triangle.

        After the solver initializes this is the B{resolved} row, with
        C{is_override} reporting the resolved provenance. Before it
        initializes only the per-cell override layer is visible, so a cell
        carrying no override reports a C{NONE} row even when a tag or
        C{'*'} default would later apply.

        @param tri: Triangle index (0-based).
        @type tri: int
        @return: C{(row, is_override)}.
        @rtype: Infil2DCell
        @raise RuntimeError: If the C API call fails.
        """
        cdef SWMM_Infil2DRow row
        cdef int is_override = 0
        _check(swmm_infil2d_get_cell(self._engine, tri, &row, &is_override))
        return Infil2DCell(_infil_row_from_c(&row), bool(is_override))

    def set_cell(self, int tri, row) -> None:
        """Set (or clear) the per-cell override of one triangle.

        C{row=None} CLEARS the override so the cell falls back to its tag
        row / the C{'*'} row. That differs from an L{Infil2DRow} whose
        C{method} is C{None}, which stores an explicit C{NONE} override and
        so suppresses the defaults for that cell.

        @param tri: Triangle index (0-based).
        @type tri: int
        @param row: Row to store (parameters in B{project units}), or
            C{None} to clear the override.
        @type row: Infil2DRow or None
        @raise RuntimeError: If the C API rejects the assignment (including
            after the solver has been initialized).
        """
        cdef SWMM_Infil2DRow crow
        if row is None:
            _check(swmm_infil2d_set_cell(self._engine, tri, NULL))
            return
        _infil_row_to_c(row, &crow)
        _check(swmm_infil2d_set_cell(self._engine, tri, &crow))

    def set_cells(self, tris, row) -> None:
        """Assign one specification to many triangles in a single call.

        The select-many-cells-then-assign entry point: one validation pass,
        then one apply. B{All-or-nothing} — if any index is out of range
        nothing at all is written. An empty C{tris} is a no-op.

        @param tris: Iterable of 0-based triangle indices.
        @type tris: sequence[int]
        @param row: Row to store on every listed triangle (parameters in
            B{project units}), or C{None} to clear their overrides.
        @type row: Infil2DRow or None
        @raise RuntimeError: If the C API rejects the assignment.
        """
        cdef SWMM_Infil2DRow crow
        cdef np.ndarray[int, ndim=1] arr = np.ascontiguousarray(
            tris, dtype=np.intc).reshape(-1)
        cdef int n = <int>arr.shape[0]
        if n == 0:
            return
        if row is None:
            _check(swmm_infil2d_set_cells(self._engine, <int*>arr.data, n, NULL))
            return
        _infil_row_to_c(row, &crow)
        _check(swmm_infil2d_set_cells(self._engine, <int*>arr.data, n, &crow))

    # -- state readback (SI) -------------------------------------------

    def rate(self):
        """Return the held per-cell infiltration rate as a NumPy array.

        Units are B{m/s}, C{>= 0}, one entry per triangle. The rate is
        recomputed on the C{INFIL_STEP} cadence and held constant between
        updates. A mesh with no resolved model returns all zeros.

        @rtype: np.ndarray
        @raise RuntimeError: If the C API call fails.
        """
        cdef int nt = 0
        _check(swmm_2d_triangle_count(self._engine, &nt))
        cdef double[::1] out = np.zeros(max(nt, 1), dtype=np.float64)
        cdef double* p = &out[0]
        cdef void* eng = self._engine
        cdef int err
        with nogil:
            err = swmm_infil2d_get_rate_bulk(eng, p, nt if nt > 0 else 1)
        _check(err)
        return np.asarray(out)[:nt]

    def cumulative(self):
        """Return the cumulative infiltrated depth per cell as a NumPy array.

        Units are B{m}, one entry per triangle — the C{infil_cum} sidecar
        variable. A mesh with no resolved model returns all zeros.

        @rtype: np.ndarray
        @raise RuntimeError: If the C API call fails.
        """
        cdef int nt = 0
        _check(swmm_2d_triangle_count(self._engine, &nt))
        cdef double[::1] out = np.zeros(max(nt, 1), dtype=np.float64)
        cdef double* p = &out[0]
        cdef void* eng = self._engine
        cdef int err
        with nogil:
            err = swmm_infil2d_get_cum_bulk(eng, p, nt if nt > 0 else 1)
        _check(err)
        return np.asarray(out)[:nt]

    @property
    def total_volume(self) -> float:
        """Cumulative 2D infiltration loss in B{m³} (the C{infil_out} row).

        The whole-domain companion to L{cumulative}. Requires the 2D mass
        balance to be live (the same contract as
        C{Surface2D.get_mass_balance}).

        @rtype: float
        @raise RuntimeError: If the 2D mass balance is not active.
        """
        cdef double v = 0.0
        _check(swmm_infil2d_get_total_volume(self._engine, &v))
        return v

    def __repr__(self) -> str:
        try:
            return (f"<Infiltration2DView infil_step={self.infil_step} "
                    f"defaults={len(self.defaults)}>")
        except Exception:
            return "<Infiltration2DView (unavailable)>"
