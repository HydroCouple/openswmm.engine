"""
Engine Lifecycle
================

:author: Caleb Buahin
:copyright: Copyright (c) HydroCouple 2026
:license: MIT

Type stubs for :mod:`openswmm.engine._solver`.

The :class:`Solver` class manages the full SWMM engine lifecycle:
create -> open -> initialize -> start -> step -> end -> report -> close -> destroy.
"""

from types import TracebackType
from typing import Optional, Type


class EngineError(Exception):
    """Raised when a C API call returns a non-zero error code.

    Attributes:
        code: The SWMM error code.
        message: Human-readable error description.
    """

    def __init__(self, code: int, message: str = "") -> None: ...


class Solver:
    """SWMM engine lifecycle manager.

    Manages the complete SWMM simulation lifecycle from file parsing through
    timestep execution to report generation. Supports both manual lifecycle
    control and the context manager protocol.

    Args:
        inp: Path to the SWMM input file (``.inp``).
        rpt: Path for the report file (``.rpt``). Empty string to skip.
        out: Path for the binary output file (``.out``). Empty string to skip.

    Note:
        The engine handle is created lazily on the first call to :meth:`open`.
        Multiple independent :class:`Solver` instances may coexist.

    Example::

        with Solver("model.inp", "model.rpt", "model.out") as s:
            while s.step():
                print(f"Elapsed: {s.elapsed:.4f} days")
    """

    def __init__(self, inp: str = "", rpt: str = "", out: str = "") -> None: ...

    def create(self) -> None:
        """Create the engine instance.

        Raises:
            MemoryError: If allocation fails.
        """
        ...

    def open(self) -> None:
        """Open and parse the input file; load plugins.

        Transitions the engine to ``OPENED`` state.

        Raises:
            EngineError: If the input file cannot be parsed.
        """
        ...

    def initialize(self) -> None:
        """Initialize the simulation (allocate arrays, set initial conditions).

        Transitions the engine to ``INITIALIZED`` state.
        """
        ...

    def start(self, save_results: bool = True) -> None:
        """Start the simulation.

        Args:
            save_results: If ``True``, write binary output to the ``.out`` file.
        """
        ...

    def step(self) -> bool:
        """Advance the simulation by one explicit timestep.

        Returns:
            ``True`` if the simulation should continue;
            ``False`` when the simulation is complete.
        """
        ...

    def end(self) -> None:
        """End the simulation; join IO thread; finalize plugins."""
        ...

    def report(self) -> None:
        """Write the summary report."""
        ...

    def close(self) -> None:
        """Close all files and free simulation state."""
        ...

    def destroy(self) -> None:
        """Destroy the engine handle and free all memory."""
        ...

    @property
    def elapsed(self) -> float:
        """Elapsed simulation time in decimal days after the last :meth:`step`."""
        ...

    @property
    def state(self) -> int:
        """Current engine lifecycle state code.

        See :class:`~openswmm.engine.EngineState` for code meanings.
        """
        ...

    @property
    def handle(self) -> int:
        """Raw C engine handle as an integer (for advanced interop)."""
        ...

    def get_start_time(self) -> float:
        """Return the simulation start time (decimal days)."""
        ...

    def get_end_time(self) -> float:
        """Return the simulation end time (decimal days)."""
        ...

    def get_current_time(self) -> float:
        """Return the current simulation time (decimal days)."""
        ...

    def get_routing_step(self) -> float:
        """Return the routing timestep (seconds)."""
        ...

    def model_write(self, path: str) -> None:
        """Write the current model to a SWMM ``.inp`` file.

        Args:
            path: Output file path.
        """
        ...

    def __enter__(self) -> "Solver": ...
    def __exit__(
        self,
        exc_type: Optional[Type[BaseException]],
        exc_val: Optional[BaseException],
        exc_tb: Optional[TracebackType],
    ) -> bool: ...
