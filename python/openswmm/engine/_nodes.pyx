"""
Node access (Pythonic v1 surface)
=================================

:author: Caleb Buahin
:copyright: Copyright (c) 2026 Caleb Buahin
:license: MIT

The :class:`Nodes` collection and :class:`Node` wrapper are the entry
point for reading and writing node state in the new bindings. The
collection acts like an ordered mapping over the engine's node array;
items returned by indexing or iteration are :class:`Node` wrappers
exposing a property-style API.

.. code-block:: python

    from openswmm.engine import Solver

    with Solver("model.inp") as s:
        # Indexing — by integer or by string id, both accepted.
        j1 = s.nodes["J1"]
        same = s.nodes[0]
        assert j1.id == same.id

        # Iteration.
        for node in s.nodes:
            if node.type == NodeType.OUTFALL:
                print(node.id, node.invert_elev)

        # Per-object property access.
        print(j1.depth, j1.head, j1.lateral_inflow)
        j1.lateral_inflow = 0.5

        # Bulk vectorised access — same memory model as before.
        depths = s.nodes.depths               # np.ndarray, dtype float64
        s.nodes.depths = depths * 1.1         # vectorised write

        # Sub-views per type.
        s.nodes["OUT1"].outfall.type = OutfallType.FIXED
        s.nodes["S1"].storage.functional = (0.0, 0.0, 100.0)
        # ``s.nodes["J1"].outfall`` raises AttributeError (J1 is a junction).

        # Statistics sub-view.
        print(s.nodes["J1"].stats.max_depth)
"""

# cython: language_level=3

import numpy as np
cimport numpy as np

from ._common cimport *
from ._enums import NodeType, OutfallType
from ._exceptions import ElementNotFoundError, StaleObjectError


# =============================================================================
# Helpers
# =============================================================================

cdef inline SWMM_Engine _h(solver):
    return <SWMM_Engine><size_t>solver.handle


cdef inline int _resolve_node(solver, object key) except -1:
    return _resolve_index(_h(solver), key, swmm_node_index, swmm_node_count, "Node")


cdef inline void _check_fresh(node) except *:
    """Raise StaleObjectError when a wrapper's captured generation no longer
    matches the solver's. Called from every Node property/method that
    addresses the engine by index."""
    if node._gen != node._solver.generation:
        raise StaleObjectError(
            f"Node wrapper (id={node._captured_id!r}, index={node._index}) is stale; "
            "look it up again from solver.nodes."
        )


# =============================================================================
# Sub-views (StorageView, OutfallView, DividerView, NodeStatsView)
# =============================================================================

cdef class NodeStatsView:
    """Per-node summary statistics view. Available on every :class:`Node`."""
    cdef object _node

    def __init__(self, node):
        self._node = node

    @property
    def max_depth(self) -> float:
        _check_fresh(self._node)
        cdef double v = 0.0
        _check(swmm_node_get_stat_max_depth(_h(self._node._solver), self._node._index, &v))
        return v

    @property
    def max_overflow(self) -> float:
        _check_fresh(self._node)
        cdef double v = 0.0
        _check(swmm_node_get_stat_max_overflow(_h(self._node._solver), self._node._index, &v))
        return v

    @property
    def vol_flooded(self) -> float:
        _check_fresh(self._node)
        cdef double v = 0.0
        _check(swmm_node_get_stat_vol_flooded(_h(self._node._solver), self._node._index, &v))
        return v

    @property
    def time_flooded(self) -> float:
        """Cumulative seconds the node has been flooded."""
        _check_fresh(self._node)
        cdef double v = 0.0
        _check(swmm_node_get_stat_time_flooded(_h(self._node._solver), self._node._index, &v))
        return v

    def __repr__(self) -> str:
        return f"<NodeStatsView for {self._node!r}>"


cdef class StorageView:
    """``node.storage`` — storage-node-only properties. Accessing on a
    non-storage node raises :class:`AttributeError`."""
    cdef object _node

    def __init__(self, node):
        self._node = node

    @property
    def curve(self) -> int:
        """Index of the storage curve (or -1 if functional)."""
        _check_fresh(self._node)
        cdef int v = 0
        _check(swmm_node_get_storage_curve(_h(self._node._solver), self._node._index, &v))
        return v

    @curve.setter
    def curve(self, int curve_idx) -> None:
        _check_fresh(self._node)
        _check(swmm_node_set_storage_curve(_h(self._node._solver), self._node._index, curve_idx))

    @property
    def functional(self) -> tuple:
        """``(a, b, c)`` coefficients of the functional storage relation."""
        _check_fresh(self._node)
        cdef double a = 0.0, b = 0.0, c = 0.0
        _check(swmm_node_get_storage_functional(_h(self._node._solver), self._node._index, &a, &b, &c))
        return (a, b, c)

    @functional.setter
    def functional(self, value) -> None:
        _check_fresh(self._node)
        a, b, c = value
        _check(swmm_node_set_storage_functional(
            _h(self._node._solver), self._node._index, a, b, c))

    @property
    def seep_rate(self) -> float:
        _check_fresh(self._node)
        cdef double v = 0.0
        _check(swmm_node_get_storage_seep_rate(_h(self._node._solver), self._node._index, &v))
        return v

    @seep_rate.setter
    def seep_rate(self, double rate) -> None:
        _check_fresh(self._node)
        _check(swmm_node_set_storage_seep_rate(_h(self._node._solver), self._node._index, rate))

    @property
    def exfil_params(self) -> tuple:
        """``(suction, ksat, imd)`` Green-Ampt exfiltration parameters."""
        _check_fresh(self._node)
        cdef double s = 0.0, k = 0.0, i = 0.0
        _check(swmm_node_get_exfil_params(_h(self._node._solver), self._node._index, &s, &k, &i))
        return (s, k, i)

    @exfil_params.setter
    def exfil_params(self, value) -> None:
        _check_fresh(self._node)
        suction, ksat, imd = value
        _check(swmm_node_set_exfil_params(
            _h(self._node._solver), self._node._index, suction, ksat, imd))

    def __repr__(self) -> str:
        return f"<StorageView for {self._node!r}>"


cdef class OutfallView:
    """``node.outfall`` — outfall-only properties."""
    cdef object _node

    def __init__(self, node):
        self._node = node

    @property
    def type(self):
        _check_fresh(self._node)
        cdef int v = 0
        _check(swmm_node_get_outfall_type(_h(self._node._solver), self._node._index, &v))
        return OutfallType(v)

    @type.setter
    def type(self, value) -> None:
        _check_fresh(self._node)
        _check(swmm_node_set_outfall_type(
            _h(self._node._solver), self._node._index, int(value)))

    def set_stage(self, double stage) -> None:
        """Configure a FIXED outfall with the given stage."""
        _check_fresh(self._node)
        _check(swmm_node_set_outfall_stage(
            _h(self._node._solver), self._node._index, stage))

    def set_tidal_curve(self, int curve_idx) -> None:
        """Configure a TIDAL outfall with the given tidal curve."""
        _check_fresh(self._node)
        _check(swmm_node_set_outfall_tidal(
            _h(self._node._solver), self._node._index, curve_idx))

    def set_timeseries(self, int ts_idx) -> None:
        """Configure a TIMESERIES outfall with the given time series."""
        _check_fresh(self._node)
        _check(swmm_node_set_outfall_timeseries(
            _h(self._node._solver), self._node._index, ts_idx))

    def get_tidal_curve(self) -> int:
        """Return the tidal-curve index of a C{TIDAL} outfall.

        @return: Index of the tidal curve assigned to this outfall.
        @rtype: int
        """
        _check_fresh(self._node)
        cdef int curve_idx = 0
        _check(swmm_node_get_outfall_tidal(
            _h(self._node._solver), self._node._index, &curve_idx))
        return curve_idx

    def get_timeseries(self) -> int:
        """Return the stage-time-series index of a C{TIMESERIES} outfall.

        @return: Index of the stage time series assigned to this outfall.
        @rtype: int
        """
        _check_fresh(self._node)
        cdef int ts_idx = 0
        _check(swmm_node_get_outfall_timeseries(
            _h(self._node._solver), self._node._index, &ts_idx))
        return ts_idx

    @property
    def param(self) -> float:
        _check_fresh(self._node)
        cdef double v = 0.0
        _check(swmm_node_get_outfall_param(_h(self._node._solver), self._node._index, &v))
        return v

    @property
    def flap_gate(self) -> bool:
        _check_fresh(self._node)
        cdef int v = 0
        _check(swmm_node_get_outfall_flap_gate(_h(self._node._solver), self._node._index, &v))
        return v != 0

    @flap_gate.setter
    def flap_gate(self, bint value) -> None:
        _check_fresh(self._node)
        _check(swmm_node_set_outfall_flap_gate(
            _h(self._node._solver), self._node._index, 1 if value else 0))

    @property
    def route_to(self) -> int:
        """Index of the subcatchment that receives outfall discharge,
        or ``-1`` when none configured."""
        _check_fresh(self._node)
        cdef int v = 0
        _check(swmm_node_get_outfall_route_to(_h(self._node._solver), self._node._index, &v))
        return v

    @route_to.setter
    def route_to(self, int subcatch_idx) -> None:
        _check_fresh(self._node)
        _check(swmm_node_set_outfall_route_to(
            _h(self._node._solver), self._node._index, subcatch_idx))

    def __repr__(self) -> str:
        return f"<OutfallView for {self._node!r}>"


cdef class DividerView:
    """``node.divider`` — divider-only properties."""
    cdef object _node

    def __init__(self, node):
        self._node = node

    @property
    def type(self) -> int:
        _check_fresh(self._node)
        cdef int v = 0
        _check(swmm_node_get_divider_type(_h(self._node._solver), self._node._index, &v))
        return v

    @type.setter
    def type(self, int value) -> None:
        _check_fresh(self._node)
        _check(swmm_node_set_divider_type(_h(self._node._solver), self._node._index, value))

    def __repr__(self) -> str:
        return f"<DividerView for {self._node!r}>"


# =============================================================================
# Node wrapper
# =============================================================================

cdef class Node:
    """A single node, addressed by index relative to the parent
    :class:`Nodes` collection.

    The wrapper is cheap — it holds a reference to the :class:`Solver`,
    the integer index, the captured generation counter, and the id at
    creation time (for nicer error messages). It carries no state of
    its own; every property/method round-trips through the C API.

    Equality compares ``(solver, index)``; hashing is consistent with
    that. After a structural mutation (rename, delete, type-convert,
    or add) wrappers minted before the mutation become **stale** and
    raise :class:`StaleObjectError` on access — re-look up the node from
    the collection.
    """

    cdef readonly object _solver
    cdef readonly int _index
    cdef readonly long long _gen
    cdef readonly str _captured_id
    cdef object _stats
    cdef object _storage
    cdef object _outfall
    cdef object _divider

    def __init__(self, solver, int index):
        self._solver = solver
        self._index = index
        self._gen = solver.generation
        cdef const char* raw = swmm_node_id(_h(solver), index)
        self._captured_id = raw.decode('utf-8') if raw != NULL else ""
        self._stats = None
        self._storage = None
        self._outfall = None
        self._divider = None

    # ---- Identity ---------------------------------------------------

    @property
    def id(self) -> str:
        _check_fresh(self)
        cdef const char* raw = swmm_node_id(_h(self._solver), self._index)
        return raw.decode('utf-8') if raw != NULL else ""

    @property
    def tag(self) -> str:
        """The node's free-form tag string (from the INP C{[TAGS]} section).

        Empty string when the node has no tag. Assigning C{None} or C{""}
        clears it. The tag is keyed by index and persists across L{rename}.

        @rtype: str
        """
        _check_fresh(self)
        cdef char buf[256]
        _check(swmm_node_get_tag(_h(self._solver), self._index, buf, 256))
        return buf.decode('utf-8')

    @tag.setter
    def tag(self, value) -> None:
        _check_fresh(self)
        cdef bytes b = (value or "").encode('utf-8')
        _check(swmm_node_set_tag(_h(self._solver), self._index, b))

    @property
    def index(self) -> int:
        _check_fresh(self)
        return self._index

    @property
    def type(self):
        _check_fresh(self)
        cdef int v = 0
        _check(swmm_node_get_type(_h(self._solver), self._index, &v))
        return NodeType(v)

    @property
    def solver(self):
        """The parent :class:`Solver`. Useful for cross-domain access."""
        return self._solver

    # ---- Geometry ---------------------------------------------------

    @property
    def invert_elev(self) -> float:
        _check_fresh(self)
        cdef double v = 0.0
        _check(swmm_node_get_invert_elev(_h(self._solver), self._index, &v))
        return v

    @invert_elev.setter
    def invert_elev(self, double value) -> None:
        _check_fresh(self)
        _check(swmm_node_set_invert_elev(_h(self._solver), self._index, value))

    @property
    def max_depth(self) -> float:
        _check_fresh(self)
        cdef double v = 0.0
        _check(swmm_node_get_max_depth(_h(self._solver), self._index, &v))
        return v

    @max_depth.setter
    def max_depth(self, double value) -> None:
        _check_fresh(self)
        _check(swmm_node_set_max_depth(_h(self._solver), self._index, value))

    @property
    def surcharge_depth(self) -> float:
        _check_fresh(self)
        cdef double v = 0.0
        _check(swmm_node_get_surcharge_depth(_h(self._solver), self._index, &v))
        return v

    @surcharge_depth.setter
    def surcharge_depth(self, double value) -> None:
        _check_fresh(self)
        _check(swmm_node_set_surcharge_depth(_h(self._solver), self._index, value))

    @property
    def ponded_area(self) -> float:
        _check_fresh(self)
        cdef double v = 0.0
        _check(swmm_node_get_ponded_area(_h(self._solver), self._index, &v))
        return v

    @ponded_area.setter
    def ponded_area(self, double value) -> None:
        _check_fresh(self)
        _check(swmm_node_set_pond_area(_h(self._solver), self._index, value))

    @property
    def initial_depth(self) -> float:
        _check_fresh(self)
        cdef double v = 0.0
        _check(swmm_node_get_initial_depth(_h(self._solver), self._index, &v))
        return v

    @initial_depth.setter
    def initial_depth(self, double value) -> None:
        _check_fresh(self)
        _check(swmm_node_set_initial_depth(_h(self._solver), self._index, value))

    @property
    def crown_elev(self) -> float:
        _check_fresh(self)
        cdef double v = 0.0
        _check(swmm_node_get_crown_elev(_h(self._solver), self._index, &v))
        return v

    @property
    def full_volume(self) -> float:
        _check_fresh(self)
        cdef double v = 0.0
        _check(swmm_node_get_full_volume(_h(self._solver), self._index, &v))
        return v

    @property
    def degree(self) -> int:
        _check_fresh(self)
        cdef int v = 0
        _check(swmm_node_get_degree(_h(self._solver), self._index, &v))
        return v

    # ---- Hydraulic state -------------------------------------------

    @property
    def depth(self) -> float:
        _check_fresh(self)
        cdef double v = 0.0
        _check(swmm_node_get_depth(_h(self._solver), self._index, &v))
        return v

    @depth.setter
    def depth(self, double value) -> None:
        _check_fresh(self)
        _check(swmm_node_set_depth(_h(self._solver), self._index, value))

    @property
    def head(self) -> float:
        _check_fresh(self)
        cdef double v = 0.0
        _check(swmm_node_get_head(_h(self._solver), self._index, &v))
        return v

    @property
    def volume(self) -> float:
        _check_fresh(self)
        cdef double v = 0.0
        _check(swmm_node_get_volume(_h(self._solver), self._index, &v))
        return v

    @property
    def lateral_inflow(self) -> float:
        _check_fresh(self)
        cdef double v = 0.0
        _check(swmm_node_get_lateral_inflow(_h(self._solver), self._index, &v))
        return v

    @lateral_inflow.setter
    def lateral_inflow(self, double value) -> None:
        _check_fresh(self)
        _check(swmm_node_set_lateral_inflow(_h(self._solver), self._index, value))

    @property
    def overflow(self) -> float:
        _check_fresh(self)
        cdef double v = 0.0
        _check(swmm_node_get_overflow(_h(self._solver), self._index, &v))
        return v

    @property
    def inflow(self) -> float:
        _check_fresh(self)
        cdef double v = 0.0
        _check(swmm_node_get_inflow(_h(self._solver), self._index, &v))
        return v

    @property
    def losses(self) -> float:
        _check_fresh(self)
        cdef double v = 0.0
        _check(swmm_node_get_losses(_h(self._solver), self._index, &v))
        return v

    @property
    def outflow(self) -> float:
        _check_fresh(self)
        cdef double v = 0.0
        _check(swmm_node_get_outflow(_h(self._solver), self._index, &v))
        return v

    def set_head_boundary(self, double head) -> None:
        """Apply a one-shot head boundary value for this step."""
        _check_fresh(self)
        _check(swmm_node_set_head_boundary(_h(self._solver), self._index, head))

    # ---- Quality ---------------------------------------------------

    def quality(self, pollutant) -> float:
        """Return the concentration of ``pollutant`` (index or id)."""
        _check_fresh(self)
        cdef int p_idx = _resolve_pollutant(self._solver, pollutant)
        cdef double v = 0.0
        _check(swmm_node_get_quality(_h(self._solver), self._index, p_idx, &v))
        return v

    def set_quality_mass_flux(self, pollutant, double mass_rate) -> None:
        """Inject a mass flux (model units / time) for ``pollutant``."""
        _check_fresh(self)
        cdef int p_idx = _resolve_pollutant(self._solver, pollutant)
        _check(swmm_node_set_quality_mass_flux(
            _h(self._solver), self._index, p_idx, mass_rate))

    # ---- Derived ---------------------------------------------------

    def depth_from_volume(self, double volume) -> float:
        """Storage-curve lookup: depth corresponding to ``volume``."""
        _check_fresh(self)
        cdef double v = 0.0
        _check(swmm_node_get_depth_from_volume(
            _h(self._solver), self._index, volume, &v))
        return v

    # ---- Sub-views -------------------------------------------------

    @property
    def stats(self) -> NodeStatsView:
        if self._stats is None:
            self._stats = NodeStatsView(self)
        return self._stats

    @property
    def storage(self) -> StorageView:
        if self.type != NodeType.STORAGE:
            raise AttributeError(
                f"node {self.id!r} is a {self.type.name}, not STORAGE; "
                "the .storage sub-view is only valid for storage nodes"
            )
        if self._storage is None:
            self._storage = StorageView(self)
        return self._storage

    @property
    def outfall(self) -> OutfallView:
        if self.type != NodeType.OUTFALL:
            raise AttributeError(
                f"node {self.id!r} is a {self.type.name}, not OUTFALL; "
                "the .outfall sub-view is only valid for outfall nodes"
            )
        if self._outfall is None:
            self._outfall = OutfallView(self)
        return self._outfall

    @property
    def divider(self) -> DividerView:
        if self.type != NodeType.DIVIDER:
            raise AttributeError(
                f"node {self.id!r} is a {self.type.name}, not DIVIDER; "
                "the .divider sub-view is only valid for divider nodes"
            )
        if self._divider is None:
            self._divider = DividerView(self)
        return self._divider

    # ---- Equality / repr ------------------------------------------

    def __eq__(self, other):
        if not isinstance(other, Node):
            return NotImplemented
        return (self._solver is other._solver
                and self._index == other._index)

    def __hash__(self):
        return hash((id(self._solver), self._index))

    def __repr__(self) -> str:
        try:
            return f"<Node id={self._captured_id!r} index={self._index}>"
        except Exception:
            return f"<Node index={self._index} (stale or closed)>"


# Helper: resolve pollutant identifier via the pollutants C API.
cdef int _resolve_pollutant(solver, key) except -1:
    return _resolve_index(
        _h(solver), key, swmm_pollutant_index, swmm_pollutant_count, "Pollutant")


# =============================================================================
# Nodes collection
# =============================================================================

cdef class Nodes:
    """Indexable, iterable collection of :class:`Node` wrappers.

    Constructed lazily via ``solver.nodes`` — users rarely need to
    instantiate ``Nodes`` directly.
    """

    cdef object _solver

    def __init__(self, solver):
        self._solver = solver

    # ---- Sized / Container / Iterable ------------------------------

    def __len__(self) -> int:
        return swmm_node_count(_h(self._solver))

    def __iter__(self):
        cdef int n = swmm_node_count(_h(self._solver))
        for i in range(n):
            yield Node(self._solver, i)

    def __getitem__(self, key) -> Node:
        cdef int i = _resolve_node(self._solver, key)
        return Node(self._solver, i)

    def __contains__(self, key) -> bool:
        try:
            _resolve_node(self._solver, key)
            return True
        except (KeyError, IndexError, TypeError):
            return False

    # ---- Identity lookups -----------------------------------------

    def get_index(self, str node_id) -> int:
        """Return the integer index of a node by id, or raise
        :exc:`KeyError`."""
        cdef bytes b = node_id.encode('utf-8')
        cdef int i = swmm_node_index(_h(self._solver), b)
        if i < 0:
            raise ElementNotFoundError(node_id)
        return i

    def get_id(self, int idx) -> str:
        """Return the string id of the node at integer index ``idx``."""
        if not (0 <= idx < len(self)):
            raise IndexError(idx)
        cdef const char* raw = swmm_node_id(_h(self._solver), idx)
        return raw.decode('utf-8') if raw != NULL else ""

    # ---- Editing (structural — bumps generation) ------------------

    def add(self, str node_id, node_type) -> Node:
        """Append a new node and return a wrapper for it."""
        cdef bytes b = node_id.encode('utf-8')
        _check(swmm_node_add(_h(self._solver), b, int(node_type)))
        self._solver._bump_generation()
        cdef int new_idx = swmm_node_index(_h(self._solver), b)
        return Node(self._solver, new_idx)

    def pop_last(self, str node_id) -> None:
        """Remove the most recently added node (must match ``node_id``)."""
        cdef bytes b = node_id.encode('utf-8')
        _check(swmm_node_pop_last(_h(self._solver), b))
        self._solver._bump_generation()

    def rename(self, key, str new_id) -> None:
        """Rename the node addressed by ``key``."""
        cdef int i = _resolve_node(self._solver, key)
        cdef bytes b = new_id.encode('utf-8')
        _check(swmm_node_rename(_h(self._solver), i, b))
        self._solver._bump_generation()

    # ---- Bulk numpy properties ------------------------------------

    @property
    def depths(self):
        """All node depths as a 1-D ``float64`` array, length ``len(self)``.

        Returned array shares an internal scratch buffer the engine
        reuses on the next call — copy it (``.copy()``) if you need to
        hold the values across a step.
        """
        cdef SWMM_Engine h = _h(self._solver)
        cdef int n = swmm_node_count(h)
        cdef np.ndarray[double, ndim=1] buf = np.empty(n, dtype=np.float64)
        cdef double* p = <double*>buf.data
        cdef int err
        with nogil:
            err = swmm_node_get_depths_bulk(h, p, n)
        _check(err)
        return buf

    @depths.setter
    def depths(self, values) -> None:
        cdef SWMM_Engine h = _h(self._solver)
        cdef int n = swmm_node_count(h)
        cdef np.ndarray[double, ndim=1] arr = np.ascontiguousarray(values, dtype=np.float64)
        if arr.shape[0] != n:
            raise ValueError(
                f"depths array length {arr.shape[0]} != node count {n}")
        cdef const double* p = <const double*>arr.data
        cdef int err
        with nogil:
            err = swmm_node_set_depths_bulk(h, p, n)
        _check(err)

    @property
    def heads(self):
        cdef SWMM_Engine h = _h(self._solver)
        cdef int n = swmm_node_count(h)
        cdef np.ndarray[double, ndim=1] buf = np.empty(n, dtype=np.float64)
        cdef int err
        with nogil:
            err = swmm_node_get_heads_bulk(h, <double*>buf.data, n)
        _check(err)
        return buf

    @property
    def inflows(self):
        cdef SWMM_Engine h = _h(self._solver)
        cdef int n = swmm_node_count(h)
        cdef np.ndarray[double, ndim=1] buf = np.empty(n, dtype=np.float64)
        cdef int err
        with nogil:
            err = swmm_node_get_inflows_bulk(h, <double*>buf.data, n)
        _check(err)
        return buf

    @property
    def overflows(self):
        cdef SWMM_Engine h = _h(self._solver)
        cdef int n = swmm_node_count(h)
        cdef np.ndarray[double, ndim=1] buf = np.empty(n, dtype=np.float64)
        cdef int err
        with nogil:
            err = swmm_node_get_overflows_bulk(h, <double*>buf.data, n)
        _check(err)
        return buf

    @property
    def volumes(self):
        cdef SWMM_Engine h = _h(self._solver)
        cdef int n = swmm_node_count(h)
        cdef np.ndarray[double, ndim=1] buf = np.empty(n, dtype=np.float64)
        cdef int err
        with nogil:
            err = swmm_node_get_volumes_bulk(h, <double*>buf.data, n)
        _check(err)
        return buf

    @property
    def outflows(self):
        cdef SWMM_Engine h = _h(self._solver)
        cdef int n = swmm_node_count(h)
        cdef np.ndarray[double, ndim=1] buf = np.empty(n, dtype=np.float64)
        cdef int err
        with nogil:
            err = swmm_node_get_outflows_bulk(h, <double*>buf.data, n)
        _check(err)
        return buf

    @property
    def losses(self):
        cdef SWMM_Engine h = _h(self._solver)
        cdef int n = swmm_node_count(h)
        cdef np.ndarray[double, ndim=1] buf = np.empty(n, dtype=np.float64)
        cdef int err
        with nogil:
            err = swmm_node_get_losses_bulk(h, <double*>buf.data, n)
        _check(err)
        return buf

    @property
    def lateral_inflows(self):
        cdef SWMM_Engine h = _h(self._solver)
        cdef int n = swmm_node_count(h)
        cdef np.ndarray[double, ndim=1] buf = np.empty(n, dtype=np.float64)
        cdef int err
        with nogil:
            err = swmm_node_get_lateral_inflows_bulk(h, <double*>buf.data, n)
        _check(err)
        return buf

    def set_lateral_inflows(self, values) -> None:
        """Vectorised setter for lateral inflows (write-only; no
        symmetric read property because the engine *also* updates this
        field internally, so a property would be misleading)."""
        cdef SWMM_Engine h = _h(self._solver)
        cdef int n = swmm_node_count(h)
        cdef np.ndarray[double, ndim=1] arr = np.ascontiguousarray(values, dtype=np.float64)
        if arr.shape[0] != n:
            raise ValueError(
                f"lateral_inflows array length {arr.shape[0]} != node count {n}")
        cdef const double* p = <const double*>arr.data
        cdef int err
        with nogil:
            err = swmm_node_set_lat_inflows_bulk(h, p, n)
        _check(err)

    def qualities(self, pollutant):
        """Per-node concentration of ``pollutant`` as a numpy array."""
        cdef SWMM_Engine h = _h(self._solver)
        cdef int n = swmm_node_count(h)
        cdef int p_idx = _resolve_pollutant(self._solver, pollutant)
        cdef np.ndarray[double, ndim=1] buf = np.empty(n, dtype=np.float64)
        cdef int err
        with nogil:
            err = swmm_node_get_quality_bulk(h, p_idx, <double*>buf.data, n)
        _check(err)
        return buf

    @property
    def ids(self):
        """All node ids as a ``numpy.ndarray`` of dtype ``object``."""
        return np.asarray(self._ids_list(), dtype=object)

    def _ids_list(self, int stride=64):
        cdef SWMM_Engine h = _h(self._solver)
        cdef int n = swmm_node_count(h)
        cdef np.ndarray[char, ndim=1, mode="c"] buf = np.zeros(
            n * stride, dtype=np.int8)
        cdef int err
        with nogil:
            err = swmm_node_get_ids_bulk(h, <char*>buf.data, stride, n)
        _check(err)
        raw = bytes(buf)
        out = []
        for i in range(n):
            slot = raw[i * stride:(i + 1) * stride]
            nul = slot.find(b"\x00")
            if nul >= 0:
                slot = slot[:nul]
            out.append(slot.decode("utf-8"))
        return out

    # ---- Repr -----------------------------------------------------

    def __repr__(self) -> str:
        try:
            n = len(self)
            return f"<Nodes n={n}>"
        except Exception:
            return "<Nodes (engine closed)>"
