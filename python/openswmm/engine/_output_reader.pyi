"""
Binary Output File Reader
=========================

:author: Caleb Buahin
:copyright: Copyright (c) HydroCouple 2026
:license: MIT

Type stubs for :mod:`openswmm.engine._output_reader`.

The :class:`OutputReader` class reads SWMM binary output files (``.out``)
independently of the engine handle.
"""

from types import TracebackType
from typing import Optional, Type

import numpy as np
import numpy.typing as npt


class OutputReader:
    """Read a SWMM binary output file.

    Supports the context manager protocol for automatic resource cleanup.

    Args:
        path: Path to the ``.out`` file.

    Raises:
        IOError: If the file cannot be opened.

    Example::

        from openswmm.engine._output_reader import OutputReader

        with OutputReader("model.out") as out:
            print(f"Periods: {out.get_period_count()}")
            depths = out.get_node_result(0, 0)
    """

    def __init__(self, path: str) -> None: ...
    def __enter__(self) -> "OutputReader": ...
    def __exit__(
        self,
        exc_type: Optional[Type[BaseException]],
        exc_val: Optional[BaseException],
        exc_tb: Optional[TracebackType],
    ) -> bool: ...

    def close(self) -> None:
        """Close the output file and release resources."""
        ...

    # ------------------------------------------------------------------
    # Metadata
    # ------------------------------------------------------------------

    def get_version(self) -> int:
        """Return the SWMM version that produced the output file.

        Returns:
            Version integer.
        """
        ...

    def get_flow_units(self) -> int:
        """Return the flow units code.

        Returns:
            Flow units code (see :class:`~openswmm.engine.FlowUnits`).
        """
        ...

    def get_subcatch_count(self) -> int:
        """Return the number of subcatchments in the output file.

        Returns:
            Subcatchment count.
        """
        ...

    def get_node_count(self) -> int:
        """Return the number of nodes in the output file.

        Returns:
            Node count.
        """
        ...

    def get_link_count(self) -> int:
        """Return the number of links in the output file.

        Returns:
            Link count.
        """
        ...

    def get_pollut_count(self) -> int:
        """Return the number of pollutants in the output file.

        Returns:
            Pollutant count.
        """
        ...

    def get_period_count(self) -> int:
        """Return the number of reporting periods.

        Returns:
            Period count.
        """
        ...

    def get_start_date(self) -> float:
        """Return the simulation start date as a Julian date value.

        Returns:
            Julian date.
        """
        ...

    def get_report_step(self) -> int:
        """Return the reporting time step in seconds.

        Returns:
            Report step (seconds).
        """
        ...

    def get_error_code(self) -> int:
        """Return the error code stored in the output file.

        Returns:
            Error code.
        """
        ...

    # ------------------------------------------------------------------
    # Object IDs
    # ------------------------------------------------------------------

    def get_subcatch_id(self, index: int) -> str:
        """Return the ID string of a subcatchment by index.

        Args:
            index: Subcatchment index.

        Returns:
            Subcatchment identifier.
        """
        ...

    def get_node_id(self, index: int) -> str:
        """Return the ID string of a node by index.

        Args:
            index: Node index.

        Returns:
            Node identifier.
        """
        ...

    def get_link_id(self, index: int) -> str:
        """Return the ID string of a link by index.

        Args:
            index: Link index.

        Returns:
            Link identifier.
        """
        ...

    # ------------------------------------------------------------------
    # Per-period results
    # ------------------------------------------------------------------

    def get_subcatch_result(
        self, period: int, var: int
    ) -> npt.NDArray[np.float32]:
        """Return a subcatchment variable for all subcatchments at a period.

        Args:
            period: Reporting period index.
            var: Variable code (see :class:`~openswmm.engine.OutSubcatchVar`).

        Returns:
            Array of shape ``(n_subcatchments,)`` with dtype ``float32``.
        """
        ...

    def get_node_result(
        self, period: int, var: int
    ) -> npt.NDArray[np.float32]:
        """Return a node variable for all nodes at a period.

        Args:
            period: Reporting period index.
            var: Variable code (see :class:`~openswmm.engine.OutNodeVar`).

        Returns:
            Array of shape ``(n_nodes,)`` with dtype ``float32``.
        """
        ...

    def get_link_result(
        self, period: int, var: int
    ) -> npt.NDArray[np.float32]:
        """Return a link variable for all links at a period.

        Args:
            period: Reporting period index.
            var: Variable code (see :class:`~openswmm.engine.OutLinkVar`).

        Returns:
            Array of shape ``(n_links,)`` with dtype ``float32``.
        """
        ...

    def get_system_result(self, period: int, var: int) -> float:
        """Return a system-level variable at a period.

        Args:
            period: Reporting period index.
            var: Variable code (see :class:`~openswmm.engine.OutSystemVar`).

        Returns:
            Variable value.
        """
        ...

    # ------------------------------------------------------------------
    # Time series
    # ------------------------------------------------------------------

    def get_subcatch_series(
        self, subcatch_idx: int, var: int, start: int, end: int
    ) -> npt.NDArray[np.float32]:
        """Return a time series of a subcatchment variable.

        Args:
            subcatch_idx: Subcatchment index.
            var: Variable code.
            start: Start period index.
            end: End period index (inclusive).

        Returns:
            Array of shape ``(end - start + 1,)`` with dtype ``float32``.
        """
        ...

    def get_node_series(
        self, node_idx: int, var: int, start: int, end: int
    ) -> npt.NDArray[np.float32]:
        """Return a time series of a node variable.

        Args:
            node_idx: Node index.
            var: Variable code.
            start: Start period index.
            end: End period index (inclusive).

        Returns:
            Array of shape ``(end - start + 1,)`` with dtype ``float32``.
        """
        ...

    def get_link_series(
        self, link_idx: int, var: int, start: int, end: int
    ) -> npt.NDArray[np.float32]:
        """Return a time series of a link variable.

        Args:
            link_idx: Link index.
            var: Variable code.
            start: Start period index.
            end: End period index (inclusive).

        Returns:
            Array of shape ``(end - start + 1,)`` with dtype ``float32``.
        """
        ...

    def get_system_series(
        self, var: int, start: int, end: int
    ) -> npt.NDArray[np.float32]:
        """Return a time series of a system-level variable.

        Args:
            var: Variable code.
            start: Start period index.
            end: End period index (inclusive).

        Returns:
            Array of shape ``(end - start + 1,)`` with dtype ``float32``.
        """
        ...

    # ------------------------------------------------------------------
    # Per-object attribute
    # ------------------------------------------------------------------

    def get_subcatch_attribute(
        self, subcatch_idx: int, period: int
    ) -> npt.NDArray[np.float32]:
        """Return all attribute values for a subcatchment at a period.

        Args:
            subcatch_idx: Subcatchment index.
            period: Reporting period index.

        Returns:
            Array of attribute values with dtype ``float32``.
        """
        ...

    def get_node_attribute(
        self, node_idx: int, period: int
    ) -> npt.NDArray[np.float32]:
        """Return all attribute values for a node at a period.

        Args:
            node_idx: Node index.
            period: Reporting period index.

        Returns:
            Array of attribute values with dtype ``float32``.
        """
        ...

    def get_link_attribute(
        self, link_idx: int, period: int
    ) -> npt.NDArray[np.float32]:
        """Return all attribute values for a link at a period.

        Args:
            link_idx: Link index.
            period: Reporting period index.

        Returns:
            Array of attribute values with dtype ``float32``.
        """
        ...

    # ------------------------------------------------------------------
    # Time
    # ------------------------------------------------------------------

    def get_period_time(self, period: int) -> float:
        """Return the elapsed time for a reporting period.

        Args:
            period: Reporting period index.

        Returns:
            Elapsed time value.
        """
        ...
