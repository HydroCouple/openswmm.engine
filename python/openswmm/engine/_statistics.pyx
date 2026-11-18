# SPDX-License-Identifier: Apache-2.0
#
# Copyright 2026 Caleb Buahin
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""
Simulation statistics (Pythonic v1 surface)
===========================================

:author: Caleb Buahin
:copyright: Copyright (c) 2026 Caleb Buahin
:license: Apache-2.0

This module exposes the **bulk** views of the per-object cumulative
statistics. Single-object access lives on the wrapper classes
(:attr:`Node.stats`, :attr:`Link.stats`, :attr:`Subcatchment.stats`),
so this view is the right tool when you want a numpy array of every
node's flooded volume in one C call.

.. code-block:: python

    with Solver("model.inp") as s:
        for _ in s.steps():
            pass
        stats = s.statistics
        vol_flooded   = stats.node_vol_flooded         # np.ndarray(n_nodes)
        max_flow      = stats.link_max_flow            # np.ndarray(n_links)
        runoff_vol    = stats.subcatchment_runoff_vol  # np.ndarray(n_subcatch)
"""

# cython: language_level=3

from typing import List, NamedTuple

import numpy as np
cimport numpy as np

from ._common cimport *
from ._enums import SummaryObject, SummaryTable


cdef inline SWMM_Engine _h(solver):
    return <SWMM_Engine><size_t>solver.handle


class SummaryColumn(NamedTuple):
    """One value column of a report summary table."""
    name: str
    """The report's heading, e.g. ``"Maximum Total Inflow"``."""
    units: str
    """The report's unit word for this run (``"CMS"``, ``"%"``, ``"days"``), or ``""``."""


class SummaryRow(NamedTuple):
    """The object one summary-table row describes."""
    object_type: SummaryObject
    """Node, link, subcatchment (a LID unit's) or ``SYSTEM`` for a total row."""
    object_index: int
    """Node, link or subcatchment index; ``-1`` for a ``SYSTEM`` row."""
    label: str
    """The report's text column: node or link type, the LID control name, or ``""``."""


class ReportTable(NamedTuple):
    """One report summary table, as :meth:`Statistics.summary_table` reads it."""
    table: SummaryTable
    title: str
    """The report's title, e.g. ``"Outfall Loading Summary"``."""
    columns: List[SummaryColumn]
    rows: List[SummaryRow]
    values: np.ndarray
    """``float64`` array of shape ``(len(rows), len(columns))``; NaN where the report prints
    nothing."""


class Statistics:
    """Bulk per-object cumulative statistics. Reach via ``solver.statistics``.

    Each property returns a fresh ``float64`` numpy array of length
    matching the domain count.
    """

    def __init__(self, solver):
        self._solver = solver

    # ------------------------------------------------------------------
    # Node bulk
    # ------------------------------------------------------------------

    @property
    def node_max_depth(self):
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int n = swmm_node_count(h)
        cdef np.ndarray[double, ndim=1] buf = np.empty(n, dtype=np.float64)
        cdef int err = 0
        if n > 0:  # the C bulk calls refuse a zero count
            with self._solver._operation(<size_t>h):
                with nogil:
                    err = swmm_stat_node_max_depth_bulk(h, <double*>buf.data, n)
        _check(err)
        return buf

    @property
    def node_max_overflow(self):
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int n = swmm_node_count(h)
        cdef np.ndarray[double, ndim=1] buf = np.empty(n, dtype=np.float64)
        cdef int err = 0
        if n > 0:  # the C bulk calls refuse a zero count
            with self._solver._operation(<size_t>h):
                with nogil:
                    err = swmm_stat_node_max_overflow_bulk(h, <double*>buf.data, n)
        _check(err)
        return buf

    @property
    def node_vol_flooded(self):
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int n = swmm_node_count(h)
        cdef np.ndarray[double, ndim=1] buf = np.empty(n, dtype=np.float64)
        cdef int err = 0
        if n > 0:  # the C bulk calls refuse a zero count
            with self._solver._operation(<size_t>h):
                with nogil:
                    err = swmm_stat_node_vol_flooded_bulk(h, <double*>buf.data, n)
        _check(err)
        return buf

    @property
    def node_time_flooded(self):
        """Cumulative flooded duration per node (seconds)."""
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int n = swmm_node_count(h)
        cdef np.ndarray[double, ndim=1] buf = np.empty(n, dtype=np.float64)
        cdef int err = 0
        if n > 0:  # the C bulk calls refuse a zero count
            with self._solver._operation(<size_t>h):
                with nogil:
                    err = swmm_stat_node_time_flooded_bulk(h, <double*>buf.data, n)
        _check(err)
        return buf

    # ------------------------------------------------------------------
    # Link bulk
    # ------------------------------------------------------------------

    @property
    def link_max_flow(self):
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int n = swmm_link_count(h)
        cdef np.ndarray[double, ndim=1] buf = np.empty(n, dtype=np.float64)
        cdef int err = 0
        if n > 0:  # the C bulk calls refuse a zero count
            with self._solver._operation(<size_t>h):
                with nogil:
                    err = swmm_stat_link_max_flow_bulk(h, <double*>buf.data, n)
        _check(err)
        return buf

    @property
    def link_max_velocity(self):
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int n = swmm_link_count(h)
        cdef np.ndarray[double, ndim=1] buf = np.empty(n, dtype=np.float64)
        cdef int err = 0
        if n > 0:  # the C bulk calls refuse a zero count
            with self._solver._operation(<size_t>h):
                with nogil:
                    err = swmm_stat_link_max_velocity_bulk(h, <double*>buf.data, n)
        _check(err)
        return buf

    @property
    def link_max_filling(self):
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int n = swmm_link_count(h)
        cdef np.ndarray[double, ndim=1] buf = np.empty(n, dtype=np.float64)
        cdef int err = 0
        if n > 0:  # the C bulk calls refuse a zero count
            with self._solver._operation(<size_t>h):
                with nogil:
                    err = swmm_stat_link_max_filling_bulk(h, <double*>buf.data, n)
        _check(err)
        return buf

    @property
    def link_vol_flow(self):
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int n = swmm_link_count(h)
        cdef np.ndarray[double, ndim=1] buf = np.empty(n, dtype=np.float64)
        cdef int err = 0
        if n > 0:  # the C bulk calls refuse a zero count
            with self._solver._operation(<size_t>h):
                with nogil:
                    err = swmm_stat_link_vol_flow_bulk(h, <double*>buf.data, n)
        _check(err)
        return buf

    @property
    def link_surcharge_time(self):
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int n = swmm_link_count(h)
        cdef np.ndarray[double, ndim=1] buf = np.empty(n, dtype=np.float64)
        cdef int err = 0
        if n > 0:  # the C bulk calls refuse a zero count
            with self._solver._operation(<size_t>h):
                with nogil:
                    err = swmm_stat_link_surcharge_time_bulk(h, <double*>buf.data, n)
        _check(err)
        return buf

    # ------------------------------------------------------------------
    # Subcatchment bulk
    # ------------------------------------------------------------------

    @property
    def subcatchment_runoff_vol(self):
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int n = swmm_subcatch_count(h)
        cdef np.ndarray[double, ndim=1] buf = np.empty(n, dtype=np.float64)
        cdef int err = 0
        if n > 0:  # the C bulk calls refuse a zero count
            with self._solver._operation(<size_t>h):
                with nogil:
                    err = swmm_stat_subcatch_runoff_vol_bulk(h, <double*>buf.data, n)
        _check(err)
        return buf

    @property
    def subcatchment_max_runoff(self):
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int n = swmm_subcatch_count(h)
        cdef np.ndarray[double, ndim=1] buf = np.empty(n, dtype=np.float64)
        cdef int err = 0
        if n > 0:  # the C bulk calls refuse a zero count
            with self._solver._operation(<size_t>h):
                with nogil:
                    err = swmm_stat_subcatch_max_runoff_bulk(h, <double*>buf.data, n)
        _check(err)
        return buf

    @property
    def subcatchment_precip(self):
        """Cumulative precipitation depth at each subcatchment.

        This is the only statistic with no C ``_bulk`` companion, so the
        per-subcatchment scalar getter is gathered into an array in a
        ``nogil`` loop. Values are in project depth units (inches for US
        models, millimetres for SI), consistent with the unit refactor.

        @return: One precipitation depth per subcatchment, indexed by
            subcatchment order.
        @rtype: numpy.ndarray
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int n = swmm_subcatch_count(h)
        cdef np.ndarray[double, ndim=1] buf = np.empty(n, dtype=np.float64)
        cdef double* ptr = <double*>buf.data
        cdef int i
        cdef int err = 0
        # The scalar getter is not declared ``nogil`` (only the C ``_bulk``
        # variants are); gather with the GIL held — the loop is over
        # subcatchments only, so the overhead is negligible.
        for i in range(n):
            err = swmm_stat_subcatch_precip(h, i, &ptr[i])
            if err != 0:
                break
        _check(err)
        return buf

    # ------------------------------------------------------------------
    # Scalar per-element getters (P2.6)
    #
    # The bulk array properties above allocate an N-length array; these
    # read a single element without that allocation, for hot single-index
    # queries. ``_at`` suffix avoids the name clash with the bulk arrays.
    # All values are in project units (see Solver.flow_units / unit_system).
    # ------------------------------------------------------------------

    def node_max_depth_at(self, int idx) -> float:
        """Peak depth at one node (project length units).

        @rtype: float
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_stat_node_max_depth(h, idx, &v))
        return v

    def node_max_overflow_at(self, int idx) -> float:
        """Peak overflow (flooding) rate at one node (project flow units).

        @rtype: float
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_stat_node_max_overflow(h, idx, &v))
        return v

    def node_vol_flooded_at(self, int idx) -> float:
        """Total flooded volume at one node (project volume units).

        @rtype: float
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_stat_node_vol_flooded(h, idx, &v))
        return v

    def node_time_flooded_at(self, int idx) -> float:
        """Total time flooded at one node (seconds).

        @rtype: float
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_stat_node_time_flooded(h, idx, &v))
        return v

    def link_max_flow_at(self, int idx) -> float:
        """Peak flow in one link (project flow units).

        @rtype: float
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_stat_link_max_flow(h, idx, &v))
        return v

    def link_max_velocity_at(self, int idx) -> float:
        """Peak velocity in one link (project length/time units).

        @rtype: float
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_stat_link_max_velocity(h, idx, &v))
        return v

    def link_max_filling_at(self, int idx) -> float:
        """Peak fractional filling C{[0, 1]} of one link.

        @rtype: float
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_stat_link_max_filling(h, idx, &v))
        return v

    def link_surcharge_time_at(self, int idx) -> float:
        """Total surcharge time of one link (seconds).

        @rtype: float
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_stat_link_surcharge_time(h, idx, &v))
        return v

    def link_vol_flow_at(self, int idx) -> float:
        """Total flow volume through one link (project volume units).

        @rtype: float
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_stat_link_vol_flow(h, idx, &v))
        return v

    def subcatchment_max_runoff_at(self, int idx) -> float:
        """Peak runoff rate from one subcatchment (project flow units).

        @rtype: float
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_stat_subcatch_max_runoff(h, idx, &v))
        return v

    def subcatchment_runoff_vol_at(self, int idx) -> float:
        """Total runoff volume from one subcatchment (project volume units).

        @rtype: float
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_stat_subcatch_runoff_vol(h, idx, &v))
        return v

    # ------------------------------------------------------------------
    # Report summary tables
    #
    # The tables the .rpt prints (Node Depth, Outfall Loading, Pumping,
    # LID Performance, ...), computed by the code that writes the report,
    # so the values match it unrounded. Readable once the run has ENDED
    # and until the solver closes.
    # ------------------------------------------------------------------

    def summary_table(self, table) -> ReportTable:
        """Read one report summary table in a single pass.

        Values are in each column's units; times of maxima are days from the
        report start. Rows are in report order.

        @param table: A :class:`SummaryTable` value.
        @raises LifecycleError: Before :meth:`Solver.end`, or after close.
        @raises BadIndexError: For an unknown table.
        @rtype: ReportTable
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int t = int(table)
        cdef _TextBufs tb
        cdef int ncol = 0
        cdef int nrow = 0
        cdef int needed = 0
        cdef int c
        cdef np.ndarray types
        cdef np.ndarray indices
        cdef np.ndarray values
        cdef char* label_ptr
        _tb_open(&tb, 1)
        try:
            while True:
                _check(swmm_summary_table_title(h, t, tb.p[0], tb.size))
                if not _tb_retry(&tb):
                    break
            title = _tb_str(&tb, 0)
        finally:
            _tb_close(&tb)
        _check(swmm_summary_column_count(h, t, &ncol))
        _check(swmm_summary_row_count(h, t, &nrow))
        columns = []
        _tb_open(&tb, 2)
        try:
            for c in range(ncol):
                while True:
                    _check(swmm_summary_column(h, t, c, tb.p[0], tb.size, tb.p[1], tb.size))
                    if not _tb_retry(&tb):
                        break
                columns.append(SummaryColumn(_tb_str(&tb, 0), _tb_str(&tb, 1)))
        finally:
            _tb_close(&tb)
        values = np.full((nrow, ncol), np.nan, dtype=np.float64)
        rows = []
        if nrow > 0:
            types = np.empty(nrow, dtype=np.intc)
            indices = np.empty(nrow, dtype=np.intc)
            _check(swmm_summary_rows_read(h, t, <int*>types.data, <int*>indices.data, nrow))
            if ncol > 0:
                _check(swmm_summary_values_read(h, t, <double*>values.data, nrow * ncol))
            # Size query: by contract it sets ``needed`` and returns BADPARAM.
            swmm_summary_labels_read(h, t, NULL, 0, &needed)
            label_buf = bytearray(max(needed, 1))
            label_ptr = label_buf
            _check(swmm_summary_labels_read(h, t, label_ptr, len(label_buf), NULL))
            labels = bytes(label_buf).split(b"\0", 1)[0].decode("utf-8", "replace").split("\n")
            for r in range(nrow):
                rows.append(SummaryRow(SummaryObject(int(types[r])), int(indices[r]),
                                       labels[r] if r < len(labels) else ""))
        return ReportTable(SummaryTable(t), title, columns, rows, values)

    def summary_tables(self) -> List[ReportTable]:
        """Read every report summary table, in report order (tables without rows included).

        @rtype: list[ReportTable]
        """
        return [self.summary_table(t) for t in SummaryTable]

    def summary_row_at(self, table, int row) -> SummaryRow:
        """Identify one row of a summary table.

        @param table: A :class:`SummaryTable` value.
        @param row: Zero-based row.
        @rtype: SummaryRow
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int t = int(table)
        cdef int object_type = 0
        cdef int object_index = -1
        cdef _TextBufs tb
        _tb_open(&tb, 1)
        try:
            while True:
                _check(swmm_summary_row(h, t, row, &object_type, &object_index, tb.p[0],
                                        tb.size))
                if not _tb_retry(&tb):
                    break
            label = _tb_str(&tb, 0)
        finally:
            _tb_close(&tb)
        return SummaryRow(SummaryObject(object_type), object_index, label)

    def summary_value_at(self, table, int row, int column) -> float:
        """Read one value of a summary table (NaN where the report prints nothing).

        Each call rebuilds the table; read whole tables with :meth:`summary_table`.

        @param table: A :class:`SummaryTable` value.
        @param row: Zero-based row.
        @param column: Zero-based value column.
        @rtype: float
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_summary_value(h, int(table), row, column, &v))
        return v

    def __repr__(self) -> str:
        return f"<Statistics for {self._solver!r}>"
