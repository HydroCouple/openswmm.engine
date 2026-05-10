"""
Hot Start File Management
=========================

:author: Caleb Buahin
:copyright: Copyright (c) 2026 Caleb Buahin
:license: MIT

Type stubs for :mod:`openswmm.engine._hotstart`.

The :class:`HotStart` class manages hot start files for saving and restoring
simulation state across runs.
"""

from types import TracebackType
from typing import Optional, Type

from ._solver import Solver


class HotStart:
    """Handle to a hot start file.

    Use L{HotStart.save} to write simulation state to disk and L{HotStart.open}
    to read it back. Supports the context manager protocol for automatic
    cleanup.

    Example -- Saving a hot start::

        with Solver("model.inp") as s:
            while s.state == EngineState.RUNNING:
                if s.step() != 0:
                    break
                pass
            HotStart.save(s, "warmup.hs")

    Example -- Applying a hot start::

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

    @ivar _handle: Opaque pointer to the underlying C{SWMM_HotStart}
        handle. Managed internally; not part of the public API.
    """

    def __init__(self) -> None:
        """Construct an empty L{HotStart} handle.

        Use the L{HotStart.open} class method or L{HotStart.save} static
        method instead of constructing an instance directly.

        @return: A new, unattached L{HotStart} instance.
        @rtype: HotStart
        """
        ...

    # ====================================================================
    # File operations
    # ====================================================================

    @staticmethod
    def save(solver: Solver, path: str) -> None:
        """Save the current simulation state to a hot start file.

        Wraps C{swmm_hotstart_save}. Valid only when the solver is in
        C{RUNNING} or C{ENDED} state.

        @param solver: An active L{Solver} in C{RUNNING} or C{ENDED} state.
        @type solver: Solver
        @param path: Output file path.
        @type path: str
        @raise EngineError: If the state cannot be saved.
        """
        ...

    @classmethod
    def open(cls, path: str) -> "HotStart":
        """Open a hot start file for reading.

        Wraps C{swmm_hotstart_open}. The returned handle should be closed
        with L{HotStart.close} or by using a C{with} block.

        @param path: Path to an existing hot start file.
        @type path: str
        @return: A new L{HotStart} handle bound to the opened file.
        @rtype: HotStart
        @raise IOError: If the file cannot be opened.
        """
        ...

    def close(self) -> None:
        """Close the hot start file and free resources.

        Wraps C{swmm_hotstart_close}. Safe to call multiple times; once
        closed, the handle becomes inert.

        @return: C{None}.
        @rtype: NoneType
        """
        ...

    # ====================================================================
    # State application
    # ====================================================================

    def apply(self, solver: Solver) -> None:
        """Apply this hot start state to an engine.

        Wraps C{swmm_hotstart_apply}. The engine must be in C{INITIALIZED}
        state (after L{Solver.initialize} but before L{Solver.start}).

        @param solver: Target L{Solver} to which the hot start state is
            applied.
        @type solver: Solver
        @raise EngineError: If the state cannot be applied.
        """
        ...

    def set_node_depth(self, node_id: str, depth: float) -> None:
        """Set the depth of a node in the hot start state.

        Wraps C{swmm_hotstart_set_node_depth}.

        @param node_id: Node identifier.
        @type node_id: str
        @param depth: Depth value (project length units).
        @type depth: float
        @raise EngineError: If the underlying C call fails.
        """
        ...

    def set_node_head(self, node_id: str, head: float) -> None:
        """Set the hydraulic head of a node in the hot start state.

        Wraps C{swmm_hotstart_set_node_head}.

        @param node_id: Node identifier.
        @type node_id: str
        @param head: Hydraulic head value (project length units).
        @type head: float
        @raise EngineError: If the underlying C call fails.
        """
        ...

    def set_link_flow(self, link_id: str, flow: float) -> None:
        """Set the flow of a link in the hot start state.

        Wraps C{swmm_hotstart_set_link_flow}.

        @param link_id: Link identifier.
        @type link_id: str
        @param flow: Flow value (project flow units).
        @type flow: float
        @raise EngineError: If the underlying C call fails.
        """
        ...

    def set_link_depth(self, link_id: str, depth: float) -> None:
        """Set the depth of a link in the hot start state.

        Wraps C{swmm_hotstart_set_link_depth}.

        @param link_id: Link identifier.
        @type link_id: str
        @param depth: Depth value (project length units).
        @type depth: float
        @raise EngineError: If the underlying C call fails.
        """
        ...

    def set_subcatch_runoff(self, subcatch_id: str, runoff: float) -> None:
        """Set the runoff of a subcatchment in the hot start state.

        Wraps C{swmm_hotstart_set_subcatch_runoff}.

        @param subcatch_id: Subcatchment identifier.
        @type subcatch_id: str
        @param runoff: Runoff value (project flow units).
        @type runoff: float
        @raise EngineError: If the underlying C call fails.
        """
        ...

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
        ...

    def get_crs(self) -> str:
        """Return the coordinate reference system string from the hot start file.

        Wraps C{swmm_hotstart_get_crs}.

        @return: Coordinate reference system string (UTF-8 decoded). May
            be empty if no CRS was recorded.
        @rtype: str
        @raise EngineError: If the underlying C call fails.
        """
        ...

    def node_count(self) -> int:
        """Return the number of nodes in the hot start file.

        Wraps C{swmm_hotstart_node_count}.

        @return: Number of nodes.
        @rtype: int
        """
        ...

    def link_count(self) -> int:
        """Return the number of links in the hot start file.

        Wraps C{swmm_hotstart_link_count}.

        @return: Number of links.
        @rtype: int
        """
        ...

    def warning_count(self) -> int:
        """Return the number of warnings generated when opening the hot start file.

        Wraps C{swmm_hotstart_warning_count}.

        @return: Number of warnings.
        @rtype: int
        """
        ...

    def get_warning(self, index: int) -> str:
        """Return a warning message by index.

        Wraps C{swmm_hotstart_warning}.

        @param index: Zero-based warning index in C{[0, warning_count())}.
        @type index: int
        @return: Warning message, or empty string if not available.
        @rtype: str
        """
        ...

    def __enter__(self) -> "HotStart":
        """Enter the context manager.

        @return: This L{HotStart} instance.
        @rtype: HotStart
        """
        ...

    def __exit__(
        self,
        exc_type: Optional[Type[BaseException]],
        exc_val: Optional[BaseException],
        exc_tb: Optional[TracebackType],
    ) -> bool:
        """Exit the context manager and L{close} the file.

        @param exc_type: Exception type, if an exception was raised.
        @type exc_type: Optional[Type[BaseException]]
        @param exc_val: Exception value, if an exception was raised.
        @type exc_val: Optional[BaseException]
        @param exc_tb: Exception traceback, if an exception was raised.
        @type exc_tb: Optional[TracebackType]
        @return: C{False} so any in-flight exception propagates.
        @rtype: bool
        """
        ...
