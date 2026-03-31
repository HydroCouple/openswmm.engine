"""
Hot Start File Management
=========================

:author: Caleb Buahin
:copyright: Copyright (c) HydroCouple 2026
:license: MIT

The :class:`HotStart` class manages hot start files for saving and restoring
simulation state across runs.

Saving a hot start
------------------

.. code-block:: python

    with Solver("model.inp") as s:
        while s.step():
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
        while s.step():
            pass
        s.end()
        s.destroy()
"""

# cython: language_level=3

from ._common cimport *
from ._solver cimport Solver


cdef class HotStart:
    """Handle to a hot start file.

    Use :meth:`save` to write simulation state to disk and :meth:`open`
    to read it back. Supports the context manager protocol for automatic
    cleanup.
    """

    cdef SWMM_HotStart _handle

    def __init__(self):
        self._handle = NULL

    @staticmethod
    def save(Solver solver, str path):
        """Save the current simulation state to a hot start file.

        :param solver: An active :class:`Solver` in RUNNING or ENDED state.
        :param path: Output file path.
        :type path: str
        :raises EngineError: If the state cannot be saved.
        """
        cdef bytes b = path.encode('utf-8')
        _check(swmm_hotstart_save(solver._handle, b))

    @classmethod
    def open(cls, str path):
        """Open a hot start file for reading.

        :param path: Path to an existing hot start file.
        :type path: str
        :returns: A new :class:`HotStart` handle.
        :rtype: HotStart
        :raises IOError: If the file cannot be opened.
        """
        cdef bytes b = path.encode('utf-8')
        cdef SWMM_HotStart h = NULL
        _check(swmm_hotstart_open(b, &h))
        if h == NULL:
            raise IOError(f"Cannot open hot start file: {path}")
        cdef HotStart obj = cls.__new__(cls)
        obj._handle = h
        return obj

    def apply(self, Solver solver):
        """Apply this hot start state to an engine.

        The engine must be in ``INITIALIZED`` state (after
        :meth:`Solver.initialize` but before :meth:`Solver.start`).

        :param solver: Target :class:`Solver`.
        :raises EngineError: If the state cannot be applied.
        """
        _check(swmm_hotstart_apply(solver._handle, self._handle))

    def close(self):
        """Close the hot start file and free resources.

        Safe to call multiple times.
        """
        if self._handle != NULL:
            _check(swmm_hotstart_close(self._handle))
            self._handle = NULL

    def set_node_depth(self, str node_id, double depth):
        """Set the depth of a node in the hot start state.

        :param node_id: Node identifier.
        :param depth: Depth value.
        """
        cdef bytes b = node_id.encode('utf-8')
        _check(swmm_hotstart_set_node_depth(self._handle, b, depth))

    def set_node_head(self, str node_id, double head):
        """Set the hydraulic head of a node in the hot start state.

        :param node_id: Node identifier.
        :param head: Head value.
        """
        cdef bytes b = node_id.encode('utf-8')
        _check(swmm_hotstart_set_node_head(self._handle, b, head))

    def set_link_flow(self, str link_id, double flow):
        """Set the flow of a link in the hot start state.

        :param link_id: Link identifier.
        :param flow: Flow value.
        """
        cdef bytes b = link_id.encode('utf-8')
        _check(swmm_hotstart_set_link_flow(self._handle, b, flow))

    def set_link_depth(self, str link_id, double depth):
        """Set the depth of a link in the hot start state.

        :param link_id: Link identifier.
        :param depth: Depth value.
        """
        cdef bytes b = link_id.encode('utf-8')
        _check(swmm_hotstart_set_link_depth(self._handle, b, depth))

    def set_subcatch_runoff(self, str subcatch_id, double runoff):
        """Set the runoff of a subcatchment in the hot start state.

        :param subcatch_id: Subcatchment identifier.
        :param runoff: Runoff value.
        """
        cdef bytes b = subcatch_id.encode('utf-8')
        _check(swmm_hotstart_set_subcatch_runoff(self._handle, b, runoff))

    def get_sim_time(self) -> float:
        """Return the simulation time stored in the hot start file.

        :returns: Simulation time (decimal days).
        :rtype: float
        """
        cdef double v = 0.0
        _check(swmm_hotstart_get_sim_time(self._handle, &v))
        return v

    def get_crs(self) -> str:
        """Return the coordinate reference system string from the hot start file.

        :returns: CRS string.
        :rtype: str
        """
        cdef char buf[256]
        _check(swmm_hotstart_get_crs(self._handle, buf, 256))
        return buf.decode('utf-8')

    def node_count(self) -> int:
        """Return the number of nodes in the hot start file.

        :returns: Node count.
        :rtype: int
        """
        return swmm_hotstart_node_count(self._handle)

    def link_count(self) -> int:
        """Return the number of links in the hot start file.

        :returns: Link count.
        :rtype: int
        """
        return swmm_hotstart_link_count(self._handle)

    def warning_count(self) -> int:
        """Return the number of warnings generated when opening the hot start file.

        :returns: Warning count.
        :rtype: int
        """
        return swmm_hotstart_warning_count(self._handle)

    def get_warning(self, int index) -> str:
        """Return a warning message by index.

        :param index: Warning index.
        :returns: Warning message, or empty string if not available.
        :rtype: str
        """
        cdef const char* msg = swmm_hotstart_warning(self._handle, index)
        if msg == NULL:
            return ""
        return msg.decode('utf-8')

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.close()
        return False

    def __dealloc__(self):
        if self._handle != NULL:
            swmm_hotstart_close(self._handle)
            self._handle = NULL
