"""
Simulation statistics (Pythonic v1 surface)
===========================================

Type stubs for :mod:`openswmm.engine._statistics`.
"""

from typing import Any, List, NamedTuple

import numpy as np
from numpy.typing import NDArray

from ._enums import SummaryObject, SummaryTable
from ._solver import Solver


class SummaryColumn(NamedTuple):
    """One value column of a report summary table."""
    name: str
    units: str


class SummaryRow(NamedTuple):
    """The object one summary-table row describes (``object_index`` -1 for SYSTEM)."""
    object_type: SummaryObject
    object_index: int
    label: str


class ReportTable(NamedTuple):
    """One report summary table; ``values`` is (rows, columns), NaN where blank."""
    table: SummaryTable
    title: str
    columns: List[SummaryColumn]
    rows: List[SummaryRow]
    values: NDArray[Any]


class Statistics:
    def __init__(self, solver: Solver) -> None: ...

    # Node bulk
    @property
    def node_max_depth(self) -> NDArray[Any]:
        """Maximum depth reached at each node, in project units."""
        ...
    @property
    def node_max_overflow(self) -> NDArray[Any]:
        """Maximum overflow (flooding) rate at each node, in project units."""
        ...
    @property
    def node_vol_flooded(self) -> NDArray[Any]:
        """Cumulative flooded volume at each node, in project units."""
        ...
    @property
    def node_time_flooded(self) -> NDArray[Any]:
        """Cumulative flooded duration at each node, in seconds."""
        ...

    # Link bulk
    @property
    def link_max_flow(self) -> NDArray[Any]:
        """Maximum flow rate in each link, in project units."""
        ...
    @property
    def link_max_velocity(self) -> NDArray[Any]:
        """Maximum flow velocity in each link, in project units."""
        ...
    @property
    def link_max_filling(self) -> NDArray[Any]:
        """Maximum filling ratio (depth / full depth) in each link."""
        ...
    @property
    def link_vol_flow(self) -> NDArray[Any]:
        """Cumulative flow volume through each link, in project units."""
        ...
    @property
    def link_surcharge_time(self) -> NDArray[Any]:
        """Cumulative surcharge duration in each link, in seconds."""
        ...

    # Subcatchment bulk
    @property
    def subcatchment_runoff_vol(self) -> NDArray[Any]:
        """Cumulative runoff volume from each subcatchment, in project units."""
        ...
    @property
    def subcatchment_max_runoff(self) -> NDArray[Any]:
        """Peak runoff rate from each subcatchment, in project units."""
        ...
    @property
    def subcatchment_precip(self) -> NDArray[Any]:
        """Cumulative precipitation depth per subcatchment, in project depth
        units (in for US, mm for SI). The only statistic without a C ``_bulk``
        companion; gathered scalar-wise in a ``nogil`` loop.
        """
        ...

    # Scalar per-element getters (P2.6) — single-index reads without the
    # whole-network array allocation. All values in project units.
    def node_max_depth_at(self, idx: int) -> float: ...
    def node_max_overflow_at(self, idx: int) -> float: ...
    def node_vol_flooded_at(self, idx: int) -> float: ...
    def node_time_flooded_at(self, idx: int) -> float: ...
    def link_max_flow_at(self, idx: int) -> float: ...
    def link_max_velocity_at(self, idx: int) -> float: ...
    def link_max_filling_at(self, idx: int) -> float: ...
    def link_surcharge_time_at(self, idx: int) -> float: ...
    def link_vol_flow_at(self, idx: int) -> float: ...
    def subcatchment_max_runoff_at(self, idx: int) -> float: ...
    def subcatchment_runoff_vol_at(self, idx: int) -> float: ...

    # Report summary tables (the .rpt sections), readable once the run has
    # ended and until the solver closes. Values in each column's units.
    def summary_table(self, table: SummaryTable | int) -> ReportTable:
        """Read one report summary table (Node Depth, Outfall Loading, Pumping, ...)."""
        ...
    def summary_tables(self) -> List[ReportTable]:
        """Read every report summary table, in report order."""
        ...
    def summary_row_at(self, table: SummaryTable | int, row: int) -> SummaryRow:
        """Identify one row of a summary table."""
        ...
    def summary_value_at(self, table: SummaryTable | int, row: int, column: int) -> float:
        """Read one value of a summary table (NaN where the report prints nothing)."""
        ...
