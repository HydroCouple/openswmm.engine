"""
Hot Start File Management
=========================

:author: Caleb Buahin
:copyright: Copyright (c) HydroCouple 2026
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

    Use :meth:`save` to write simulation state to disk and :meth:`open`
    to read it back. Supports the context manager protocol for automatic
    cleanup.

    Example -- Saving a hot start::

        with Solver("model.inp") as s:
            while s.step():
                pass
            HotStart.save(s, "warmup.hs")

    Example -- Applying a hot start::

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

    def __init__(self) -> None: ...

    @staticmethod
    def save(solver: Solver, path: str) -> None:
        """Save the current simulation state to a hot start file.

        Args:
            solver: An active :class:`Solver` in RUNNING or ENDED state.
            path: Output file path.

        Raises:
            EngineError: If the state cannot be saved.
        """
        ...

    @classmethod
    def open(cls, path: str) -> "HotStart":
        """Open a hot start file for reading.

        Args:
            path: Path to an existing hot start file.

        Returns:
            A new :class:`HotStart` handle.

        Raises:
            IOError: If the file cannot be opened.
        """
        ...

    def apply(self, solver: Solver) -> None:
        """Apply this hot start state to an engine.

        The engine must be in ``INITIALIZED`` state (after
        :meth:`Solver.initialize` but before :meth:`Solver.start`).

        Args:
            solver: Target :class:`Solver`.

        Raises:
            EngineError: If the state cannot be applied.
        """
        ...

    def close(self) -> None:
        """Close the hot start file and free resources.

        Safe to call multiple times.
        """
        ...

    def __enter__(self) -> "HotStart": ...
    def __exit__(
        self,
        exc_type: Optional[Type[BaseException]],
        exc_val: Optional[BaseException],
        exc_tb: Optional[TracebackType],
    ) -> bool: ...
