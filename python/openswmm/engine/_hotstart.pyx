"""
Hot Start File Management
=========================

:author: Caleb Buahin
:copyright: Copyright (c) 2026 Caleb Buahin
:license: MIT

The :class:`HotStart` class manages hot start files for saving and restoring
simulation state across runs.

Saving a hot start
------------------

.. code-block:: python

    with Solver("model.inp") as s:
        while s.state == EngineState.RUNNING:
            if s.step() != 0:
                break
            pass
        HotStart.save(s, "warmup.hs")

Applying a hot start
--------------------

.. code-block:: python

    with HotStart.open("warmup.hs") as hs:
        s = Solver("model.inp")
        s.open()
        s.initialize()
        hs.apply(s)
        s.start()
        while s.state == EngineState.RUNNING:
            if s.step() != 0:
                break
            pass
        s.end()
        s.destroy()
"""

# cython: language_level=3

from ._common cimport *
from ._solver cimport Solver


cdef class HotStart:
    """Handle to a hot start file.

    Use L{HotStart.save} to write simulation state to disk and L{HotStart.open}
    to read it back. Supports the context manager protocol for automatic
    cleanup.

    @ivar _handle: Opaque pointer to the underlying C{SWMM_HotStart}
        handle. Managed internally; not part of the public API.
    """

    cdef SWMM_HotStart _handle

    def __init__(self):
        """Construct an empty L{HotStart} handle.

        Use the L{HotStart.open} class method or L{HotStart.save} static
        method instead of constructing an instance directly.

        @return: A new, unattached L{HotStart} instance.
        @rtype: HotStart
        """
        self._handle = NULL

    # ====================================================================
    # File operations
    # ====================================================================

    @staticmethod
    def save(Solver solver, str path):
        """Save the current simulation state to a hot start file.

        Wraps C{swmm_hotstart_save}. Valid only when the solver is in
        C{RUNNING} or C{ENDED} state.

        @param solver: An active L{Solver} in C{RUNNING} or C{ENDED} state.
        @type solver: Solver
        @param path: Output file path.
        @type path: str
        @raise EngineError: If the state cannot be saved.
        """
        cdef bytes b = path.encode('utf-8')
        _check(swmm_hotstart_save(solver._handle, b))

    @classmethod
    def open(cls, str path):
        """Open a hot start file for reading.

        Wraps C{swmm_hotstart_open}. The returned handle should be closed
        with L{HotStart.close} or by using a C{with} block.

        @param path: Path to an existing hot start file.
        @type path: str
        @return: A new L{HotStart} handle bound to the opened file.
        @rtype: HotStart
        @raise IOError: If the file cannot be opened.
        """
        cdef bytes b = path.encode('utf-8')
        cdef SWMM_HotStart h = NULL
        _check(swmm_hotstart_open(b, &h))
        if h == NULL:
            raise IOError(f"Cannot open hot start file: {path}")
        cdef HotStart obj = cls.__new__(cls)
        obj._handle = h
        return obj

    def close(self):
        """Close the hot start file and free resources.

        Wraps C{swmm_hotstart_close}. Safe to call multiple times; once
        closed, the handle becomes inert.

        @return: C{None}.
        @rtype: NoneType
        """
        if self._handle != NULL:
            _check(swmm_hotstart_close(self._handle))
            self._handle = NULL

    # ====================================================================
    # State application
    # ====================================================================

    def apply(self, Solver solver):
        """Apply this hot start state to an engine.

        Wraps C{swmm_hotstart_apply}. The engine must be in C{INITIALIZED}
        state (after L{Solver.initialize} but before L{Solver.start}).

        @param solver: Target L{Solver} to which the hot start state is
            applied.
        @type solver: Solver
        @raise EngineError: If the state cannot be applied.
        """
        _check(swmm_hotstart_apply(solver._handle, self._handle))

    def set_node_depth(self, str node_id, double depth):
        """Set the depth of a node in the hot start state.

        Wraps C{swmm_hotstart_set_node_depth}.

        @param node_id: Node identifier.
        @type node_id: str
        @param depth: Depth value (project length units).
        @type depth: float
        @raise EngineError: If the underlying C call fails.
        """
        cdef bytes b = node_id.encode('utf-8')
        _check(swmm_hotstart_set_node_depth(self._handle, b, depth))

    def set_node_head(self, str node_id, double head):
        """Set the hydraulic head of a node in the hot start state.

        Wraps C{swmm_hotstart_set_node_head}.

        @param node_id: Node identifier.
        @type node_id: str
        @param head: Hydraulic head value (project length units).
        @type head: float
        @raise EngineError: If the underlying C call fails.
        """
        cdef bytes b = node_id.encode('utf-8')
        _check(swmm_hotstart_set_node_head(self._handle, b, head))

    def set_link_flow(self, str link_id, double flow):
        """Set the flow of a link in the hot start state.

        Wraps C{swmm_hotstart_set_link_flow}.

        @param link_id: Link identifier.
        @type link_id: str
        @param flow: Flow value (project flow units).
        @type flow: float
        @raise EngineError: If the underlying C call fails.
        """
        cdef bytes b = link_id.encode('utf-8')
        _check(swmm_hotstart_set_link_flow(self._handle, b, flow))

    def set_link_depth(self, str link_id, double depth):
        """Set the depth of a link in the hot start state.

        Wraps C{swmm_hotstart_set_link_depth}.

        @param link_id: Link identifier.
        @type link_id: str
        @param depth: Depth value (project length units).
        @type depth: float
        @raise EngineError: If the underlying C call fails.
        """
        cdef bytes b = link_id.encode('utf-8')
        _check(swmm_hotstart_set_link_depth(self._handle, b, depth))

    def set_subcatch_runoff(self, str subcatch_id, double runoff):
        """Set the runoff of a subcatchment in the hot start state.

        Wraps C{swmm_hotstart_set_subcatch_runoff}.

        @param subcatch_id: Subcatchment identifier.
        @type subcatch_id: str
        @param runoff: Runoff value (project flow units).
        @type runoff: float
        @raise EngineError: If the underlying C call fails.
        """
        cdef bytes b = subcatch_id.encode('utf-8')
        _check(swmm_hotstart_set_subcatch_runoff(self._handle, b, runoff))

    # ====================================================================
    # Status queries
    # ====================================================================

    def get_sim_time(self) -> float:
        """Return the simulation time stored in the hot start file.

        Wraps C{swmm_hotstart_get_sim_time}.

        @return: Simulation time (decimal days).
        @rtype: float
        @raise EngineError: If the underlying C call fails.
        """
        cdef double v = 0.0
        _check(swmm_hotstart_get_sim_time(self._handle, &v))
        return v

    def get_crs(self) -> str:
        """Return the coordinate reference system string from the hot start file.

        Wraps C{swmm_hotstart_get_crs}.

        @return: Coordinate reference system string (UTF-8 decoded). May
            be empty if no CRS was recorded.
        @rtype: str
        @raise EngineError: If the underlying C call fails.
        """
        cdef char buf[256]
        _check(swmm_hotstart_get_crs(self._handle, buf, 256))
        return buf.decode('utf-8')

    def node_count(self) -> int:
        """Return the number of nodes in the hot start file.

        Wraps C{swmm_hotstart_node_count}.

        @return: Number of nodes.
        @rtype: int
        """
        return swmm_hotstart_node_count(self._handle)

    def link_count(self) -> int:
        """Return the number of links in the hot start file.

        Wraps C{swmm_hotstart_link_count}.

        @return: Number of links.
        @rtype: int
        """
        return swmm_hotstart_link_count(self._handle)

    def warning_count(self) -> int:
        """Return the number of warnings generated when opening the hot start file.

        Wraps C{swmm_hotstart_warning_count}.

        @return: Number of warnings.
        @rtype: int
        """
        return swmm_hotstart_warning_count(self._handle)

    def get_warning(self, int index) -> str:
        """Return a warning message by index.

        Wraps C{swmm_hotstart_warning}.

        @param index: Zero-based warning index in C{[0, warning_count())}.
        @type index: int
        @return: Warning message, or empty string if not available.
        @rtype: str
        """
        cdef const char* msg = swmm_hotstart_warning(self._handle, index)
        if msg == NULL:
            return ""
        return msg.decode('utf-8')

    def __enter__(self):
        """Enter the context manager.

        @return: This L{HotStart} instance.
        @rtype: HotStart
        """
        return self

    def __exit__(self, *args):
        """Exit the context manager and L{close} the file.

        @param args: Standard C{(exc_type, exc_val, exc_tb)} tuple.
        @type args: tuple
        @return: C{False} so any in-flight exception propagates.
        @rtype: bool
        """
        self.close()
        return False

    def __dealloc__(self):
        """Free the underlying C handle on garbage collection.

        Internal Cython destructor; do not call directly.
        """
        if self._handle != NULL:
            swmm_hotstart_close(self._handle)
            self._handle = NULL
