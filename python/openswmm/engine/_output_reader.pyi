"""
Binary Output File Reader
=========================

:author: Caleb Buahin
:copyright: Copyright (c) 2026 Caleb Buahin
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
    Operates on the binary C{.out} file produced by a completed simulation
    and does not require an active engine handle.

    Example::

        from openswmm.engine._output_reader import OutputReader

        with OutputReader("model.out") as out:
            print(f"Periods: {out.get_period_count()}")
            depths = out.get_node_result(0, 0)

    @ivar _handle: Opaque pointer to the underlying C{SWMM_Output}
        handle. Managed internally; not part of the public API.
    """

    def __init__(self, path: str) -> None:
        """Open a SWMM binary output file.

        Wraps C{swmm_output_open}.

        @param path: Path to the C{.out} file.
        @type path: str
        @raise IOError: If the file cannot be opened.
        """
        ...

    # ====================================================================
    # File lifecycle (open / close)
    # ====================================================================

    def __enter__(self) -> "OutputReader":
        """Enter the context manager.

        @return: This L{OutputReader} instance.
        @rtype: OutputReader
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

    def close(self) -> None:
        """Close the output file and release resources.

        Wraps C{swmm_output_close}. Safe to call multiple times.

        @return: C{None}.
        @rtype: NoneType
        """
        ...

    # ====================================================================
    # Metadata queries
    # ====================================================================

    def get_version(self) -> int:
        """Return the SWMM version that produced the output file.

        Wraps C{swmm_output_get_version}.

        @return: Version integer.
        @rtype: int
        """
        ...

    def get_flow_units(self) -> int:
        """Return the flow units code.

        Wraps C{swmm_output_get_flow_units}.

        @return: Flow units code (see L{openswmm.engine.FlowUnits}).
        @rtype: int
        @see: L{openswmm.engine.FlowUnits}
        """
        ...

    def get_subcatch_count(self) -> int:
        """Return the number of subcatchments in the output file.

        Wraps C{swmm_output_get_subcatch_count}.

        @return: Number of subcatchments.
        @rtype: int
        """
        ...

    def get_node_count(self) -> int:
        """Return the number of nodes in the output file.

        Wraps C{swmm_output_get_node_count}.

        @return: Number of nodes.
        @rtype: int
        """
        ...

    def get_link_count(self) -> int:
        """Return the number of links in the output file.

        Wraps C{swmm_output_get_link_count}.

        @return: Number of links.
        @rtype: int
        """
        ...

    def get_pollut_count(self) -> int:
        """Return the number of pollutants in the output file.

        Wraps C{swmm_output_get_pollut_count}.

        @return: Number of pollutants.
        @rtype: int
        """
        ...

    def get_subcatch_id(self, index: int) -> str:
        """Return the ID string of a subcatchment by index.

        Wraps C{swmm_output_get_subcatch_id}.

        @param index: Zero-based subcatchment index.
        @type index: int
        @return: Subcatchment identifier (UTF-8 decoded), or empty string
            if not available.
        @rtype: str
        """
        ...

    def get_node_id(self, index: int) -> str:
        """Return the ID string of a node by index.

        Wraps C{swmm_output_get_node_id}.

        @param index: Zero-based node index.
        @type index: int
        @return: Node identifier (UTF-8 decoded), or empty string if not
            available.
        @rtype: str
        """
        ...

    def get_link_id(self, index: int) -> str:
        """Return the ID string of a link by index.

        Wraps C{swmm_output_get_link_id}.

        @param index: Zero-based link index.
        @type index: int
        @return: Link identifier (UTF-8 decoded), or empty string if not
            available.
        @rtype: str
        """
        ...

    # ====================================================================
    # Time / period queries
    # ====================================================================

    def get_period_count(self) -> int:
        """Return the number of reporting periods.

        Wraps C{swmm_output_get_period_count}.

        @return: Number of reporting periods.
        @rtype: int
        """
        ...

    def get_start_date(self) -> float:
        """Return the simulation start date as a Julian date value.

        Wraps C{swmm_output_get_start_date}.

        @return: Julian date.
        @rtype: float
        @raise RuntimeError: If the underlying read fails.
        """
        ...

    def get_report_step(self) -> int:
        """Return the reporting time step in seconds.

        Wraps C{swmm_output_get_report_step}.

        @return: Reporting time step (seconds).
        @rtype: int
        """
        ...

    def get_error_code(self) -> int:
        """Return the error code stored in the output file.

        Wraps C{swmm_output_get_error_code}.

        @return: Error code (C{0} on success).
        @rtype: int
        """
        ...

    def get_period_time(self, period: int) -> float:
        """Return the elapsed time for a reporting period.

        Wraps C{swmm_output_get_period_time}.

        @param period: Zero-based reporting period index.
        @type period: int
        @return: Elapsed time value (project time units).
        @rtype: float
        @raise RuntimeError: If the underlying read fails.
        """
        ...

    # ====================================================================
    # Time series extraction (per element type): subcatchments
    # ====================================================================

    def get_subcatch_result(
        self, period: int, var: int
    ) -> npt.NDArray[np.float32]:
        """Return a subcatchment variable for all subcatchments at a period.

        Wraps C{swmm_output_get_subcatch_result}.

        @param period: Zero-based reporting period index.
        @type period: int
        @param var: Variable code (see L{openswmm.engine.OutSubcatchVar}).
        @type var: int
        @return: Array of shape C{(n_subcatchments,)} with dtype C{float32}.
        @rtype: np.ndarray
        @raise RuntimeError: If the underlying read fails.
        @see: L{openswmm.engine.OutSubcatchVar}
        """
        ...

    def get_subcatch_series(
        self, subcatch_idx: int, var: int, start: int, end: int
    ) -> npt.NDArray[np.float32]:
        """Return a time series of a subcatchment variable.

        Wraps C{swmm_output_get_subcatch_series}.

        @param subcatch_idx: Zero-based subcatchment index.
        @type subcatch_idx: int
        @param var: Variable code (see L{openswmm.engine.OutSubcatchVar}).
        @type var: int
        @param start: Start period index.
        @type start: int
        @param end: End period index (inclusive).
        @type end: int
        @return: Array of shape C{(end - start + 1,)} with dtype C{float32}.
        @rtype: np.ndarray
        @raise RuntimeError: If the underlying read fails.
        """
        ...

    def get_subcatch_attribute(
        self, subcatch_idx: int, period: int
    ) -> npt.NDArray[np.float32]:
        """Return all attribute values for a subcatchment at a period.

        Wraps C{swmm_output_get_subcatch_attribute}.

        @param subcatch_idx: Zero-based subcatchment index.
        @type subcatch_idx: int
        @param period: Zero-based reporting period index.
        @type period: int
        @return: Array of attribute values with dtype C{float32}.
        @rtype: np.ndarray
        @raise RuntimeError: If the underlying read fails.
        """
        ...

    # ====================================================================
    # Time series extraction (per element type): nodes
    # ====================================================================

    def get_node_result(
        self, period: int, var: int
    ) -> npt.NDArray[np.float32]:
        """Return a node variable for all nodes at a period.

        Wraps C{swmm_output_get_node_result}.

        @param period: Zero-based reporting period index.
        @type period: int
        @param var: Variable code (see L{openswmm.engine.OutNodeVar}).
        @type var: int
        @return: Array of shape C{(n_nodes,)} with dtype C{float32}.
        @rtype: np.ndarray
        @raise RuntimeError: If the underlying read fails.
        @see: L{openswmm.engine.OutNodeVar}
        """
        ...

    def get_node_series(
        self, node_idx: int, var: int, start: int, end: int
    ) -> npt.NDArray[np.float32]:
        """Return a time series of a node variable.

        Wraps C{swmm_output_get_node_series}.

        @param node_idx: Zero-based node index.
        @type node_idx: int
        @param var: Variable code (see L{openswmm.engine.OutNodeVar}).
        @type var: int
        @param start: Start period index.
        @type start: int
        @param end: End period index (inclusive).
        @type end: int
        @return: Array of shape C{(end - start + 1,)} with dtype C{float32}.
        @rtype: np.ndarray
        @raise RuntimeError: If the underlying read fails.
        """
        ...

    def get_node_attribute(
        self, node_idx: int, period: int
    ) -> npt.NDArray[np.float32]:
        """Return all attribute values for a node at a period.

        Wraps C{swmm_output_get_node_attribute}.

        @param node_idx: Zero-based node index.
        @type node_idx: int
        @param period: Zero-based reporting period index.
        @type period: int
        @return: Array of attribute values with dtype C{float32}.
        @rtype: np.ndarray
        @raise RuntimeError: If the underlying read fails.
        """
        ...

    # ====================================================================
    # Time series extraction (per element type): links
    # ====================================================================

    def get_link_result(
        self, period: int, var: int
    ) -> npt.NDArray[np.float32]:
        """Return a link variable for all links at a period.

        Wraps C{swmm_output_get_link_result}.

        @param period: Zero-based reporting period index.
        @type period: int
        @param var: Variable code (see L{openswmm.engine.OutLinkVar}).
        @type var: int
        @return: Array of shape C{(n_links,)} with dtype C{float32}.
        @rtype: np.ndarray
        @raise RuntimeError: If the underlying read fails.
        @see: L{openswmm.engine.OutLinkVar}
        """
        ...

    def get_link_series(
        self, link_idx: int, var: int, start: int, end: int
    ) -> npt.NDArray[np.float32]:
        """Return a time series of a link variable.

        Wraps C{swmm_output_get_link_series}.

        @param link_idx: Zero-based link index.
        @type link_idx: int
        @param var: Variable code (see L{openswmm.engine.OutLinkVar}).
        @type var: int
        @param start: Start period index.
        @type start: int
        @param end: End period index (inclusive).
        @type end: int
        @return: Array of shape C{(end - start + 1,)} with dtype C{float32}.
        @rtype: np.ndarray
        @raise RuntimeError: If the underlying read fails.
        """
        ...

    def get_link_attribute(
        self, link_idx: int, period: int
    ) -> npt.NDArray[np.float32]:
        """Return all attribute values for a link at a period.

        Wraps C{swmm_output_get_link_attribute}.

        @param link_idx: Zero-based link index.
        @type link_idx: int
        @param period: Zero-based reporting period index.
        @type period: int
        @return: Array of attribute values with dtype C{float32}.
        @rtype: np.ndarray
        @raise RuntimeError: If the underlying read fails.
        """
        ...

    # ====================================================================
    # Time series extraction (per element type): system
    # ====================================================================

    def get_system_result(self, period: int, var: int) -> float:
        """Return a system-level variable at a period.

        Wraps C{swmm_output_get_system_result}.

        @param period: Zero-based reporting period index.
        @type period: int
        @param var: Variable code (see L{openswmm.engine.OutSystemVar}).
        @type var: int
        @return: Variable value.
        @rtype: float
        @raise RuntimeError: If the underlying read fails.
        @see: L{openswmm.engine.OutSystemVar}
        """
        ...

    def get_system_series(
        self, var: int, start: int, end: int
    ) -> npt.NDArray[np.float32]:
        """Return a time series of a system-level variable.

        Wraps C{swmm_output_get_system_series}.

        @param var: Variable code (see L{openswmm.engine.OutSystemVar}).
        @type var: int
        @param start: Start period index.
        @type start: int
        @param end: End period index (inclusive).
        @type end: int
        @return: Array of shape C{(end - start + 1,)} with dtype C{float32}.
        @rtype: np.ndarray
        @raise RuntimeError: If the underlying read fails.
        """
        ...
