"""
GeoPackage Access
=================

:author: Caleb Buahin
:copyright: Copyright (c) HydroCouple 2026
:license: MIT

Type stubs for :mod:`openswmm.engine._geopackage`.

The :class:`GeoPackage` class provides read/write access to SWMM GeoPackage
databases for querying multi-run simulation results, importing observed data,
and performing spatial analysis.

This module is optional — it is only available when the package is built
with ``OPENSWMM_WITH_GEOPACKAGE=ON``.
"""

from types import TracebackType
from typing import List, Optional, Tuple, Type

import numpy as np
import numpy.typing as npt


class GeoPackage:
    """GeoPackage database access for SWMM models, results, and observed data.

    Supports the context manager protocol for automatic resource cleanup.

    Args:
        path: Path to the ``.gpkg`` file.

    Raises:
        RuntimeError: If the file cannot be opened.

    Example::

        from openswmm.engine import GeoPackage

        with GeoPackage("model.gpkg") as gpkg:
            sims = gpkg.simulation_ids()
            times, depths = gpkg.read_result_ts(sims[0], "NODE", "J1", "depth")
    """

    def __init__(self, path: str) -> None: ...
    def __enter__(self) -> "GeoPackage": ...
    def __exit__(
        self,
        exc_type: Optional[Type[BaseException]],
        exc_val: Optional[BaseException],
        exc_tb: Optional[TracebackType],
    ) -> bool: ...

    def close(self) -> None:
        """Close the database connection."""
        ...

    @property
    def last_error(self) -> str:
        """The last error message from the GeoPackage library."""
        ...

    # ------------------------------------------------------------------
    # Transactions
    # ------------------------------------------------------------------

    def begin(self) -> None:
        """Begin a transaction for bulk operations.

        Raises:
            RuntimeError: If the transaction cannot be started.
        """
        ...

    def commit(self) -> None:
        """Commit the current transaction.

        Raises:
            RuntimeError: If the commit fails.
        """
        ...

    def rollback(self) -> None:
        """Roll back the current transaction.

        Raises:
            RuntimeError: If the rollback fails.
        """
        ...

    # ------------------------------------------------------------------
    # Simulation metadata
    # ------------------------------------------------------------------

    def simulation_count(self) -> int:
        """Return the number of simulation runs in the file.

        Returns:
            Simulation count.
        """
        ...

    def simulation_ids(self) -> List[str]:
        """Return all simulation IDs as a list of strings.

        Returns:
            List of simulation identifier strings.
        """
        ...

    def object_counts(self, sim_id: str) -> dict:
        """Return model object counts for a simulation.

        Args:
            sim_id: Simulation identifier.

        Returns:
            Dict with keys: ``nodes``, ``links``, ``subcatchments``,
            ``gages``.
        """
        ...

    def variable_count(self) -> int:
        """Return the number of output variables defined.

        Returns:
            Variable count.
        """
        ...

    def topology_edge_count(self, sim_id: str) -> int:
        """Return the number of topology edges for a simulation.

        Args:
            sim_id: Simulation identifier.

        Returns:
            Topology edge count.
        """
        ...

    # ------------------------------------------------------------------
    # Result timeseries
    # ------------------------------------------------------------------

    def result_ts_count(
        self, sim_id: str, obj_type: str, obj_id: str, variable: str
    ) -> int:
        """Return the number of result timeseries records for a query.

        Args:
            sim_id: Simulation identifier.
            obj_type: ``"NODE"``, ``"LINK"``, ``"SUBCATCH"``, or ``"SYSTEM"``.
            obj_id: Object identifier (e.g. ``"J1"``).
            variable: Variable name (e.g. ``"depth"``, ``"flow"``).

        Returns:
            Record count.
        """
        ...

    def read_result_ts(
        self, sim_id: str, obj_type: str, obj_id: str, variable: str
    ) -> Tuple[npt.NDArray[np.float64], npt.NDArray[np.float64]]:
        """Read a result timeseries as numpy arrays.

        Args:
            sim_id: Simulation identifier.
            obj_type: ``"NODE"``, ``"LINK"``, ``"SUBCATCH"``, or ``"SYSTEM"``.
            obj_id: Object identifier.
            variable: Variable name.

        Returns:
            Tuple of (times, values) as numpy ``float64`` arrays.
        """
        ...

    # ------------------------------------------------------------------
    # Summary statistics
    # ------------------------------------------------------------------

    def read_summary(
        self, sim_id: str, obj_type: str, obj_id: str, variable: str
    ) -> float:
        """Read a single summary statistic value.

        Args:
            sim_id: Simulation identifier.
            obj_type: ``"NODE"``, ``"LINK"``, or ``"SUBCATCH"``.
            obj_id: Object identifier.
            variable: Variable name (e.g. ``"max_depth"``).

        Returns:
            Statistic value.

        Raises:
            KeyError: If the summary record is not found.
        """
        ...

    # ------------------------------------------------------------------
    # Observed data write
    # ------------------------------------------------------------------

    def create_observed_series(
        self,
        name: str,
        variable: str,
        obj_type: str = "",
        obj_id: str = "",
        source: str = "",
        units: str = "",
    ) -> int:
        """Create an observed data series.

        Args:
            name: Unique series name.
            variable: Variable being measured.
            obj_type: Model object type for linking, or ``""``
            obj_id: Model object ID for linking, or ``""``
            source: Data source description, or ``""``
            units: Measurement units, or ``""``

        Returns:
            Series ID (>= 0).

        Raises:
            RuntimeError: If the series cannot be created.
        """
        ...

    def write_observed_value(
        self, series_id: int, timestamp: str, value: float, flag: str = ""
    ) -> None:
        """Write a single observed data point.

        Args:
            series_id: Series ID from :meth:`create_observed_series`.
            timestamp: ISO 8601 timestamp string.
            value: Measured value.
            flag: Quality flag (e.g. ``"A"``, ``"P"``), or ``""``.

        Raises:
            RuntimeError: If the write fails.
        """
        ...

    def write_observed_values(
        self,
        series_id: int,
        timestamps: List[str],
        values: npt.ArrayLike,
        flags: Optional[List[str]] = None,
    ) -> None:
        """Bulk-write observed data points.

        For best performance wrap in a transaction::

            gpkg.begin()
            gpkg.write_observed_values(sid, timestamps, values)
            gpkg.commit()

        Args:
            series_id: Series ID.
            timestamps: List of ISO 8601 timestamp strings.
            values: List or numpy array of values.
            flags: Optional list of quality flag strings.

        Raises:
            RuntimeError: If the bulk write fails.
        """
        ...

    # ------------------------------------------------------------------
    # Observed data read
    # ------------------------------------------------------------------

    def observed_series_count(self) -> int:
        """Return the number of observed data series.

        Returns:
            Observed series count.
        """
        ...

    def observed_value_count(self, series_id: int) -> int:
        """Return the number of values in an observed series.

        Args:
            series_id: Series ID.

        Returns:
            Value count.
        """
        ...

    def read_observed_values(
        self, series_id: int
    ) -> Tuple[List[str], npt.NDArray[np.float64]]:
        """Read observed timeseries values.

        Args:
            series_id: Series ID.

        Returns:
            Tuple of (timestamps, values) where *timestamps* is a list of
            ISO 8601 strings and *values* is a numpy ``float64`` array.
        """
        ...

    # ------------------------------------------------------------------
    # Ad-hoc queries
    # ------------------------------------------------------------------

    def query_int(self, sql: str) -> int:
        """Execute a read-only SQL query and return the first integer result.

        Args:
            sql: SELECT query string.

        Returns:
            Integer result.
        """
        ...

    def query_double(self, sql: str) -> float:
        """Execute a read-only SQL query and return the first double result.

        Args:
            sql: SELECT query string.

        Returns:
            Double result.

        Raises:
            RuntimeError: If the query fails.
        """
        ...


# ------------------------------------------------------------------
# Module-level registration
# ------------------------------------------------------------------

def register(
    key: str = "",
    org: str = "",
    email: str = "",
    deploy: str = "",
) -> bool:
    """Register the GeoPackage plugin.

    Args:
        key: License key.
        org: Organisation name.
        email: Contact e-mail.
        deploy: Deployment identifier.

    Returns:
        ``True`` if registration succeeded.
    """
    ...


def is_registered() -> bool:
    """Check whether the GeoPackage plugin is registered.

    Returns:
        ``True`` if registered.
    """
    ...
