"""
Engine Lifecycle
================

:author: Caleb Buahin
:copyright: Copyright (c) 2026 Caleb Buahin
:license: MIT

Type stubs for :mod:`openswmm.engine._solver`.

The :class:`Solver` class manages the full SWMM engine lifecycle:
create -> open -> initialize -> start -> step -> end -> report -> close -> destroy.
"""

from datetime import datetime
from types import TracebackType
from typing import Callable, Optional, Type


def run(
    inp: str,
    rpt: str = "",
    out: str = "",
    plugin_lib: Optional[str] = None,
) -> int:
    """Run a SWMM simulation from start to finish in a single call.

    Convenience wrapper that performs the full lifecycle (open through
    destroy) without exposing intermediate state.

    @param inp: Path to the SWMM input file (C{.inp}).
    @type inp: str
    @param rpt: Path for the report file (C{.rpt}). Empty string to skip.
    @type rpt: str
    @param out: Path for the binary output file (C{.out}). Empty string to
        skip.
    @type out: str
    @param plugin_lib: Optional path to a plugin shared library.
    @type plugin_lib: str or None
    @return: Error code from the C API (C{0} on success, non-zero on
        failure).
    @rtype: int
    """
    ...


def run_with_callback(
    inp: str,
    rpt: str = "",
    out: str = "",
    callback: Optional[Callable[[float], None]] = None,
    plugin_lib: Optional[str] = None,
) -> int:
    """Run a SWMM simulation with a progress callback.

    Convenience wrapper around L{run} that periodically invokes C{callback}
    with the simulation's elapsed-time fraction. When C{callback} is C{None},
    behaves identically to L{run}.

    @param inp: Path to the SWMM input file (C{.inp}).
    @type inp: str
    @param rpt: Path for the report file (C{.rpt}). Empty string to skip.
    @type rpt: str
    @param out: Path for the binary output file (C{.out}). Empty string to
        skip.
    @type out: str
    @param callback: A callable receiving ``(progress: float)`` where
        progress is a fraction in [0, 1]. ``None`` disables the callback.
    @type callback: callable or None
    @param plugin_lib: Optional path to a plugin shared library.
    @type plugin_lib: str or None
    @return: Error code from the C API (C{0} on success, non-zero on
        failure).
    @rtype: int
    """
    ...


class EngineError(Exception):
    """Raised when a C API call returns a non-zero error code.

    @ivar code: The SWMM error code returned by the C API.
    @ivar message: Human-readable error description.

    @param code: The SWMM error code.
    @type code: int
    @param message: Human-readable error description. When empty, a default
        message is derived from the SWMM error code via the C API.
    @type message: str
    """

    def __init__(self, code: int, message: str = "") -> None: ...


class Solver:
    """SWMM engine lifecycle manager.

    Manages the complete SWMM simulation lifecycle from file parsing through
    timestep execution to report generation. Supports both manual lifecycle
    control and the context manager protocol.

    @param inp: Path to the SWMM input file (C{.inp}).
    @type inp: str
    @param rpt: Path for the report file (C{.rpt}). Empty string to skip.
    @type rpt: str
    @param out: Path for the binary output file (C{.out}). Empty string to skip.
    @type out: str

    @note: The engine handle is created lazily on the first call to L{open}.
        Multiple independent L{Solver} instances may coexist.

    Example::

        from openswmm.engine import EngineState

        with Solver("model.inp", "model.rpt", "model.out") as s:
            while s.state == EngineState.RUNNING:
                rc = s.step()
                if rc != 0:
                    break
                print(f"Elapsed: {s.elapsed:.4f} days")
    """

    def __init__(self, inp: str = "", rpt: str = "", out: str = "") -> None: ...

    # =========================================================================
    # Lifecycle
    # =========================================================================

    def create(self) -> None:
        """Create the engine instance.

        Allocates the underlying C engine handle. This is normally called
        automatically by L{open}; explicit calls are only needed when working
        with the engine handle directly.

        @return: None
        @rtype: None
        @raise MemoryError: If allocation fails.
        """
        ...

    def open(self, plugin_lib: Optional[str] = None) -> int:
        """Open and parse the input file; load plugins.

        Transitions the engine to C{OPENED} state.

        @param plugin_lib: Optional path to a plugin shared library.
        @type plugin_lib: str or None
        @return: Error code from the C API (C{0} on success, non-zero on
            failure). Lifecycle status is queryable via L{state}.
        @rtype: int
        """
        ...

    def initialize(self) -> int:
        """Initialize the simulation.

        Allocates simulation arrays and applies initial conditions.
        Transitions the engine to C{INITIALIZED} state.

        @return: Error code from the C API (C{0} on success, non-zero on
            failure).
        @rtype: int
        """
        ...

    def start(self, save_results: bool = True) -> int:
        """Start the simulation.

        Transitions the engine to C{RUNNING} state and prepares the binary
        output file when C{save_results} is C{True}.

        @param save_results: If C{True}, write binary output to the C{.out}
            file. When C{False}, no binary output is produced.
        @type save_results: bool
        @return: Error code from the C API (C{0} on success, non-zero on
            failure).
        @rtype: int
        """
        ...

    def step(self) -> int:
        """Advance the simulation by one explicit timestep.

        Caller polls L{state} to detect completion: when the simulation is
        finished the engine transitions from C{RUNNING} to C{ENDED}, so the
        idiomatic loop is::

            from openswmm.engine import EngineState
            while solver.state == EngineState.RUNNING:
                rc = solver.step()
                if rc != 0:
                    break

        @return: Error code from the C API (C{0} on success, non-zero on
            failure).
        @rtype: int
        """
        ...

    def stride(self, n_steps: int) -> int:
        """Advance the simulation by C{n_steps} timesteps in one call.

        Updates L{elapsed} as a side effect.

        @param n_steps: Number of timesteps to advance.
        @type n_steps: int
        @return: Error code from the C API (C{0} on success, non-zero on
            failure).
        @rtype: int
        """
        ...

    def end(self) -> int:
        """End the simulation; join the IO thread; finalize plugins.

        Transitions the engine to C{ENDED} state.

        @return: Error code from the C API (C{0} on success, non-zero on
            failure).
        @rtype: int
        """
        ...

    def report(self) -> int:
        """Write the summary report to the C{.rpt} file.

        @return: Error code from the C API (C{0} on success, non-zero on
            failure).
        @rtype: int
        """
        ...

    def close(self) -> int:
        """Close all files and free simulation state.

        Transitions the engine to C{CLOSED} state. Safe to call when the
        handle is already C{NULL}.

        @return: Error code from the C API (C{0} on success or when the
            handle is C{NULL}, non-zero on failure).
        @rtype: int
        """
        ...

    def destroy(self) -> None:
        """Destroy the engine handle and free all memory.

        After this call the L{Solver} no longer holds a valid C handle.
        Safe to call when the handle is already C{NULL}.

        @return: None
        @rtype: None
        """
        ...

    # =========================================================================
    # Timing properties
    # =========================================================================

    @property
    def elapsed(self) -> float:
        """Elapsed simulation time in decimal days after the last L{step}.

        @return: Elapsed simulation time (decimal days).
        @rtype: float
        """
        ...

    # =========================================================================
    # State / handle
    # =========================================================================

    @property
    def state(self) -> int:
        """Current engine lifecycle state code.

        @return: Lifecycle state code.
        @rtype: int
        @see: L{openswmm.engine.EngineState} for code meanings.
        """
        ...

    @property
    def handle(self) -> int:
        """Raw C engine handle as an integer (for advanced interop).

        @return: The underlying C engine pointer cast to an integer.
        @rtype: int
        """
        ...

    # =========================================================================
    # Timing accessors
    # =========================================================================

    def get_start_time(self) -> float:
        """Return the simulation start time.

        @return: Simulation start time (decimal days).
        @rtype: float
        @raise EngineError: On C API failure.
        """
        ...

    def get_end_time(self) -> float:
        """Return the simulation end time.

        @return: Simulation end time (decimal days).
        @rtype: float
        @raise EngineError: On C API failure.
        """
        ...

    def get_current_time(self) -> float:
        """Return the current simulation time.

        @return: Current simulation time (decimal days).
        @rtype: float
        @raise EngineError: On C API failure.
        """
        ...

    def get_routing_step(self) -> float:
        """Return the routing timestep.

        @return: Routing timestep (seconds).
        @rtype: float
        @raise EngineError: On C API failure.
        """
        ...

    # =========================================================================
    # Typed time-control properties (datetime)
    # =========================================================================

    start_datetime: datetime
    """Simulation start date/time.

    @raise EngineError: On C API failure.
    """

    end_datetime: datetime
    """Simulation end date/time.

    @raise EngineError: On C API failure.
    """

    report_start_datetime: datetime
    """Report start date/time.

    @raise EngineError: On C API failure.
    """

    # =========================================================================
    # Model write
    # =========================================================================

    def model_write(self, path: str) -> None:
        """Write the current model to a SWMM C{.inp} file.

        @param path: Output file path.
        @type path: str
        @return: None
        @rtype: None
        @raise EngineError: On C API failure.
        """
        ...

    # =========================================================================
    # Context manager
    # =========================================================================

    def __enter__(self) -> "Solver": ...
    def __exit__(
        self,
        exc_type: Optional[Type[BaseException]],
        exc_val: Optional[BaseException],
        exc_tb: Optional[TracebackType],
    ) -> bool: ...
