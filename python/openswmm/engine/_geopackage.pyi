"""
GeoPackage Access
=================

:author: Caleb Buahin
:copyright: Copyright (c) 2026 Caleb Buahin
:license: MIT

Type stubs for :mod:`openswmm.engine._geopackage`.

The :class:`GeoPackage` class provides read/write access to SWMM GeoPackage
databases for querying multi-run simulation results, importing observed data,
and performing spatial analysis.

This module is optional - it is only available when the package is built
with ``OPENSWMM_WITH_GEOPACKAGE=ON``.
"""

from types import TracebackType
from typing import List, Optional, Tuple, Type

import numpy as np
import numpy.typing as npt


class GeoPackage:
    """GeoPackage database access for SWMM models, results, and observed data.

    Wraps the C{swmm_gpkg_*} entry points of the OpenSWMM GeoPackage
    plugin. Supports the context-manager protocol for automatic resource
    cleanup.

    Example::

        from openswmm.engine import GeoPackage

        with GeoPackage("model.gpkg") as gpkg:
            sims = gpkg.simulation_ids()
            times, depths = gpkg.read_result_ts(
                sims[0], "NODE", "J1", "depth")

    @ivar last_error: The most recent error message reported by the
        underlying GeoPackage library.
    """

    def __init__(self, path: str) -> None:
        """Open a GeoPackage file.

        @param path: Path to the C{.gpkg} file.
        @type path: str
        @raise RuntimeError: If the file cannot be opened.
        """
        ...

    def __enter__(self) -> "GeoPackage":
        """Enter the context-manager scope.

        @return: This L{GeoPackage} instance.
        @rtype: GeoPackage
        """
        ...

    def __exit__(
        self,
        exc_type: Optional[Type[BaseException]],
        exc_val: Optional[BaseException],
        exc_tb: Optional[TracebackType],
    ) -> bool:
        """Close the database when leaving the context-manager scope.

        @param exc_type: Exception type if one was raised, otherwise
            C{None}.
        @type exc_type: Optional[Type[BaseException]]
        @param exc_val: Exception instance if one was raised, otherwise
            C{None}.
        @type exc_val: Optional[BaseException]
        @param exc_tb: Traceback if one was raised, otherwise C{None}.
        @type exc_tb: Optional[TracebackType]
        @return: Always C{False} (do not suppress exceptions).
        @rtype: bool
        """
        ...

    # ====================================================================
    # File lifecycle (open/close)
    # ====================================================================

    def close(self) -> None:
        """Close the database connection.

        Calling C{close} more than once is harmless.
        """
        ...

    @property
    def last_error(self) -> str:
        """The last error message from the GeoPackage library.

        @return: Error message string, or C{""} if no error has been
            reported.
        @rtype: str
        """
        ...

    # ====================================================================
    # File lifecycle - transactions
    # ====================================================================

    def begin(self) -> None:
        """Begin a transaction for bulk operations.

        @raise RuntimeError: If the transaction cannot be started.
        """
        ...

    def commit(self) -> None:
        """Commit the current transaction.

        @raise RuntimeError: If the commit fails.
        """
        ...

    def rollback(self) -> None:
        """Roll back the current transaction.

        @raise RuntimeError: If the rollback fails.
        """
        ...

    # ====================================================================
    # Read operations - simulation metadata
    # ====================================================================

    def simulation_count(self) -> int:
        """Return the number of simulation runs in the file.

        @return: Simulation count.
        @rtype: int
        """
        ...

    def simulation_ids(self) -> List[str]:
        """Return all simulation IDs as a list of strings.

        @return: List of simulation identifier strings.
        @rtype: List[str]
        """
        ...

    def object_counts(self, sim_id: str) -> dict:
        """Return model object counts for a simulation.

        @param sim_id: Simulation identifier.
        @type sim_id: str
        @return: Dict with keys C{"nodes"}, C{"links"},
            C{"subcatchments"}, and C{"gages"}.
        @rtype: dict
        """
        ...

    def variable_count(self) -> int:
        """Return the number of output variables defined.

        @return: Variable count.
        @rtype: int
        """
        ...

    def topology_edge_count(self, sim_id: str) -> int:
        """Return the number of topology edges for a simulation.

        @param sim_id: Simulation identifier.
        @type sim_id: str
        @return: Topology edge count.
        @rtype: int
        """
        ...

    # ====================================================================
    # Read operations - result timeseries
    # ====================================================================

    def result_ts_count(
        self, sim_id: str, obj_type: str, obj_id: str, variable: str
    ) -> int:
        """Return the number of result timeseries records for a query.

        @param sim_id: Simulation identifier.
        @type sim_id: str
        @param obj_type: One of C{"NODE"}, C{"LINK"}, C{"SUBCATCH"},
            or C{"SYSTEM"}.
        @type obj_type: str
        @param obj_id: Object identifier (e.g. C{"J1"}).
        @type obj_id: str
        @param variable: Variable name (e.g. C{"depth"}, C{"flow"}).
        @type variable: str
        @return: Record count.
        @rtype: int
        """
        ...

    def read_result_ts(
        self, sim_id: str, obj_type: str, obj_id: str, variable: str
    ) -> Tuple[npt.NDArray[np.float64], npt.NDArray[np.float64]]:
        """Read a result timeseries as NumPy arrays.

        @param sim_id: Simulation identifier.
        @type sim_id: str
        @param obj_type: One of C{"NODE"}, C{"LINK"}, C{"SUBCATCH"},
            or C{"SYSTEM"}.
        @type obj_type: str
        @param obj_id: Object identifier.
        @type obj_id: str
        @param variable: Variable name.
        @type variable: str
        @return: Tuple C{(times, values)} as NumPy C{float64} arrays.
        @rtype: Tuple[np.ndarray, np.ndarray]
        @see: L{result_ts_count}
        """
        ...

    # ====================================================================
    # Read operations - summary statistics
    # ====================================================================

    def read_summary(
        self, sim_id: str, obj_type: str, obj_id: str, variable: str
    ) -> float:
        """Read a single summary statistic value.

        @param sim_id: Simulation identifier.
        @type sim_id: str
        @param obj_type: One of C{"NODE"}, C{"LINK"}, or C{"SUBCATCH"}.
        @type obj_type: str
        @param obj_id: Object identifier.
        @type obj_id: str
        @param variable: Variable name (e.g. C{"max_depth"}).
        @type variable: str
        @return: Statistic value.
        @rtype: float
        @raise KeyError: If the summary record is not found.
        """
        ...

    # ====================================================================
    # Write operations - observed data
    # ====================================================================

    def create_observed_series(
        self,
        name: str,
        variable: str,
        obj_type: str = "",
        obj_id: str = "",
        source: str = "",
        units: str = "",
    ) -> int:
        """Create an observed-data series.

        @param name: Unique series name.
        @type name: str
        @param variable: Variable being measured.
        @type variable: str
        @param obj_type: Model object type for linking, or C{""} for
            none.
        @type obj_type: str
        @param obj_id: Model object ID for linking, or C{""} for none.
        @type obj_id: str
        @param source: Data source description, or C{""} for none.
        @type source: str
        @param units: Measurement units, or C{""} for none.
        @type units: str
        @return: Series ID (M{>= 0}).
        @rtype: int
        @raise RuntimeError: If the series cannot be created.
        """
        ...

    def write_observed_value(
        self, series_id: int, timestamp: str, value: float, flag: str = ""
    ) -> None:
        """Write a single observed data point.

        @param series_id: Series ID returned by
            L{create_observed_series}.
        @type series_id: int
        @param timestamp: ISO 8601 timestamp string.
        @type timestamp: str
        @param value: Measured value.
        @type value: float
        @param flag: Quality flag (e.g. C{"A"}, C{"P"}), or C{""} for
            none.
        @type flag: str
        @raise RuntimeError: If the write fails.
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

        For best performance wrap the call in a transaction::

            gpkg.begin()
            gpkg.write_observed_values(sid, timestamps, values)
            gpkg.commit()

        @param series_id: Series ID.
        @type series_id: int
        @param timestamps: List of ISO 8601 timestamp strings.
        @type timestamps: List[str]
        @param values: List or NumPy array of values.
        @type values: npt.ArrayLike
        @param flags: Optional list of quality-flag strings, one per
            timestamp.
        @type flags: Optional[List[str]]
        @raise RuntimeError: If the bulk write fails.
        @raise MemoryError: If the C string-pointer arrays cannot be
            allocated.
        """
        ...

    # ====================================================================
    # Read operations - observed data
    # ====================================================================

    def observed_series_count(self) -> int:
        """Return the number of observed-data series.

        @return: Observed-series count.
        @rtype: int
        """
        ...

    def observed_value_count(self, series_id: int) -> int:
        """Return the number of values in an observed series.

        @param series_id: Series ID.
        @type series_id: int
        @return: Value count.
        @rtype: int
        """
        ...

    def read_observed_values(
        self, series_id: int
    ) -> Tuple[List[str], npt.NDArray[np.float64]]:
        """Read observed-timeseries values.

        @param series_id: Series ID.
        @type series_id: int
        @return: Tuple C{(timestamps, values)} where C{timestamps} is a
            list of ISO 8601 strings and C{values} is a NumPy
            C{float64} array.
        @rtype: Tuple[List[str], np.ndarray]
        """
        ...

    # ====================================================================
    # Layer queries (ad-hoc SQL)
    # ====================================================================

    def query_int(self, sql: str) -> int:
        """Execute a read-only SQL query and return the first integer result.

        @param sql: C{SELECT} query string.
        @type sql: str
        @return: Integer result of the query.
        @rtype: int
        """
        ...

    def query_double(self, sql: str) -> float:
        """Execute a read-only SQL query and return the first double result.

        @param sql: C{SELECT} query string.
        @type sql: str
        @return: Double-precision result of the query.
        @rtype: float
        @raise RuntimeError: If the query fails.
        """
        ...


# ====================================================================
# Schema queries - module-level registration
# ====================================================================

def register(
    key: str = "",
    org: str = "",
    email: str = "",
    deploy: str = "",
) -> bool:
    """Register the GeoPackage plugin.

    @param key: License key, or C{""} if not required.
    @type key: str
    @param org: Organisation name, or C{""}.
    @type org: str
    @param email: Contact e-mail, or C{""}.
    @type email: str
    @param deploy: Deployment identifier, or C{""}.
    @type deploy: str
    @return: C{True} if registration succeeded.
    @rtype: bool
    """
    ...


def is_registered() -> bool:
    """Check whether the GeoPackage plugin is registered.

    @return: C{True} if registered.
    @rtype: bool
    """
    ...
