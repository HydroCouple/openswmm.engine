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
Binary output file reader (Pythonic v1 surface)
===============================================

:author: Caleb Buahin
:copyright: Copyright (c) 2026 Caleb Buahin
:license: Apache-2.0

The :class:`OutputReader` reads a SWMM binary ``.out`` file
independently of any running engine.

.. code-block:: python

    from pathlib import Path
    from openswmm.engine import OutputReader, OutNodeVar

    with OutputReader(Path("model.out")) as out:
        print(out.start_datetime, out.report_step, out.period_count)

        # Enum-typed variable selection, str or int object selector.
        depths = out.node_series("J1", OutNodeVar.DEPTH)

        # All-attribute dict for a single object at a single period.
        attrs = out.node_attributes("J1", period=10)
        print(attrs[OutNodeVar.DEPTH], attrs[OutNodeVar.HEAD])

        # period_times is a datetime64[s] array ready for matplotlib.
        ax.plot(out.period_times, depths)
"""

# cython: language_level=3

import os
from datetime import datetime, timedelta
from typing import Dict, List

import numpy as np
cimport numpy as np

from ._common cimport *
from ._dates import oadate_to_datetime
from ._enums import (
    FlowUnits,
    OutLinkVar,
    OutNodeVar,
    OutSubcatchVar,
    OutSystemVar,
)


cdef class OutputReader:
    """Read a SWMM binary ``.out`` file.

    Supports the context-manager protocol. Operates on a file produced
    by any past run; does not require an active :class:`Solver`.

    :param path: ``str``, :class:`pathlib.Path`, or any
        :class:`os.PathLike`.
    :raises FileError: If the file cannot be opened.
    """

    cdef SWMM_Output _handle
    cdef object _node_ids       # cached list[str]
    cdef object _link_ids
    cdef object _subcatch_ids
    cdef object _pollutant_ids
    cdef object _period_times   # cached np.ndarray[datetime64[s]]

    def __init__(self, path):
        cdef bytes b = os.fspath(path).encode('utf-8')
        self._handle = swmm_output_open(b)
        if self._handle == NULL:
            from ._exceptions import FileError
            from ._enums import ErrorCode
            raise FileError(ErrorCode.OUTFILE.value,
                            f"cannot open output file: {os.fspath(path)}")
        self._node_ids = None
        self._link_ids = None
        self._subcatch_ids = None
        self._pollutant_ids = None
        self._period_times = None

    # ------------------------------------------------------------------
    # Lifecycle
    # ------------------------------------------------------------------

    def close(self) -> None:
        """Close the binary output file and release its handle."""
        if self._handle != NULL:
            swmm_output_close(self._handle)
            self._handle = NULL

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.close()
        return False

    def __dealloc__(self):
        if self._handle != NULL:
            swmm_output_close(self._handle)
            self._handle = NULL

    # ------------------------------------------------------------------
    # Metadata properties
    # ------------------------------------------------------------------

    @property
    def version(self) -> int:
        return swmm_output_get_version(self._handle)

    @property
    def flow_units(self):
        return FlowUnits(swmm_output_get_flow_units(self._handle))

    @property
    def period_count(self) -> int:
        return swmm_output_get_period_count(self._handle)

    @property
    def report_step(self) -> timedelta:
        return timedelta(seconds=swmm_output_get_report_step(self._handle))

    @property
    def start_datetime(self) -> datetime:
        cdef double v = 0.0
        cdef int rc = swmm_output_get_start_date(self._handle, &v)
        if rc != 0:
            from ._exceptions import raise_for_code
            raise_for_code(rc)
        return oadate_to_datetime(v)

    @property
    def pollutant_count(self) -> int:
        return swmm_output_get_pollut_count(self._handle)

    @property
    def node_count(self) -> int:
        return swmm_output_get_node_count(self._handle)

    @property
    def link_count(self) -> int:
        return swmm_output_get_link_count(self._handle)

    @property
    def subcatchment_count(self) -> int:
        return swmm_output_get_subcatch_count(self._handle)

    @property
    def error_code(self) -> int:
        """SWMM error code stored in the file footer (0 = clean run)."""
        return swmm_output_get_error_code(self._handle)

    # ------------------------------------------------------------------
    # Object id arrays
    # ------------------------------------------------------------------

    @property
    def node_ids(self) -> List[str]:
        if self._node_ids is None:
            self._node_ids = self._read_node_ids()
        return list(self._node_ids)

    @property
    def link_ids(self) -> List[str]:
        if self._link_ids is None:
            self._link_ids = self._read_link_ids()
        return list(self._link_ids)

    @property
    def subcatchment_ids(self) -> List[str]:
        if self._subcatch_ids is None:
            self._subcatch_ids = self._read_subcatch_ids()
        return list(self._subcatch_ids)

    @property
    def pollutant_ids(self) -> List[str]:
        """Species (pollutant) column names, in column order.

        Reading these names is the only way to identify what a species column
        holds: the per-column unit field is a three-value concentration enum,
        so the water-age column (``__WATER_AGE__``, reported in HOURS) reuses
        a concentration code. Key on the name, not on the unit code.
        """
        if self._pollutant_ids is None:
            self._pollutant_ids = self._read_pollutant_ids()
        return list(self._pollutant_ids)

    cdef list _read_node_ids(self):
        cdef int n = swmm_output_get_node_count(self._handle)
        cdef const char* raw
        out = []
        for i in range(n):
            raw = swmm_output_get_node_id(self._handle, i)
            out.append(raw.decode('utf-8') if raw != NULL else "")
        return out

    cdef list _read_link_ids(self):
        cdef int n = swmm_output_get_link_count(self._handle)
        cdef const char* raw
        out = []
        for i in range(n):
            raw = swmm_output_get_link_id(self._handle, i)
            out.append(raw.decode('utf-8') if raw != NULL else "")
        return out

    cdef list _read_subcatch_ids(self):
        cdef int n = swmm_output_get_subcatch_count(self._handle)
        cdef const char* raw
        out = []
        for i in range(n):
            raw = swmm_output_get_subcatch_id(self._handle, i)
            out.append(raw.decode('utf-8') if raw != NULL else "")
        return out

    cdef list _read_pollutant_ids(self):
        cdef int n = swmm_output_get_pollut_count(self._handle)
        cdef const char* raw
        out = []
        for i in range(n):
            raw = swmm_output_get_pollut_id(self._handle, i)
            out.append(raw.decode('utf-8') if raw != NULL else "")
        return out

    # ------------------------------------------------------------------
    # Time axis
    # ------------------------------------------------------------------

    @property
    def period_times(self) -> np.ndarray:
        """All reporting-period times as a ``datetime64[s]`` numpy array.

        Computed lazily and cached so the second access is free.
        """
        if self._period_times is not None:
            return self._period_times
        cdef int n = swmm_output_get_period_count(self._handle)
        cdef double t = 0.0
        cdef int rc
        py_dts = np.empty(n, dtype='datetime64[s]')
        for i in range(n):
            rc = swmm_output_get_period_time(self._handle, i, &t)
            if rc != 0:
                from ._exceptions import raise_for_code
                raise_for_code(rc)
            py_dts[i] = np.datetime64(oadate_to_datetime(t).replace(microsecond=0))
        self._period_times = py_dts
        return py_dts

    # ------------------------------------------------------------------
    # Index resolution from string ids (no engine handle available)
    # ------------------------------------------------------------------

    cdef int _resolve(self, key, str kind) except -1:
        if isinstance(key, str):
            if kind == "node":
                ids = self.node_ids
            elif kind == "link":
                ids = self.link_ids
            else:
                ids = self.subcatchment_ids
            try:
                return ids.index(key)
            except ValueError:
                raise KeyError(key)
        if isinstance(key, bool):
            raise TypeError(f"{kind} key must be int or str, got bool")
        if isinstance(key, int):
            if kind == "node":
                count = swmm_output_get_node_count(self._handle)
            elif kind == "link":
                count = swmm_output_get_link_count(self._handle)
            else:
                count = swmm_output_get_subcatch_count(self._handle)
            if not 0 <= key < count:
                raise IndexError(f"{kind} index {key} out of range [0, {count})")
            return key
        if hasattr(key, "__index__"):
            return self._resolve(int(key.__index__()), kind)
        raise TypeError(
            f"{kind} key must be int or str, got {type(key).__name__}")

    # ------------------------------------------------------------------
    # Per-period results — variable is enum, returns all-object array
    # ------------------------------------------------------------------

    def node_result(self, int period, var) -> np.ndarray:
        """All nodes' value of ``var`` at ``period``."""
        cdef int v = int(var)
        cdef int n = swmm_output_get_node_count(self._handle)
        cdef np.ndarray[float, ndim=1] buf = np.empty(n, dtype=np.float32)
        cdef int rc = swmm_output_get_node_result(
            self._handle, period, v, <float*>buf.data)
        if rc != 0:
            from ._exceptions import raise_for_code
            raise_for_code(rc)
        return buf

    def link_result(self, int period, var) -> np.ndarray:
        """Return an array of *var* values for every link at output *period*."""
        cdef int v = int(var)
        cdef int n = swmm_output_get_link_count(self._handle)
        cdef np.ndarray[float, ndim=1] buf = np.empty(n, dtype=np.float32)
        cdef int rc = swmm_output_get_link_result(
            self._handle, period, v, <float*>buf.data)
        if rc != 0:
            from ._exceptions import raise_for_code
            raise_for_code(rc)
        return buf

    def subcatchment_result(self, int period, var) -> np.ndarray:
        """Return an array of *var* values for every subcatchment at output *period*."""
        cdef int v = int(var)
        cdef int n = swmm_output_get_subcatch_count(self._handle)
        cdef np.ndarray[float, ndim=1] buf = np.empty(n, dtype=np.float32)
        cdef int rc = swmm_output_get_subcatch_result(
            self._handle, period, v, <float*>buf.data)
        if rc != 0:
            from ._exceptions import raise_for_code
            raise_for_code(rc)
        return buf

    def system_result(self, int period, var) -> float:
        """Return the system-wide *var* value at output *period*."""
        cdef int v = int(var)
        cdef float value = 0.0
        cdef int rc = swmm_output_get_system_result(
            self._handle, period, v, &value)
        if rc != 0:
            from ._exceptions import raise_for_code
            raise_for_code(rc)
        return float(value)

    # ------------------------------------------------------------------
    # Time series per object — int | str, enum
    # ------------------------------------------------------------------

    def _series_range(self, int period_count, start, end):
        if start is None:
            start = 0
        if end is None:
            end = period_count - 1
        if not 0 <= start <= end <= period_count - 1:
            raise IndexError(
                f"period range [{start}, {end}] out of [0, {period_count - 1}]")
        return int(start), int(end)

    def node_series(self, node, var, *, start=None, end=None) -> np.ndarray:
        """Return the time series of *var* for *node* over the reporting window."""
        cdef int idx = self._resolve(node, "node")
        cdef int v = int(var)
        cdef int period_count = swmm_output_get_period_count(self._handle)
        cdef int s, e
        s, e = self._series_range(period_count, start, end)
        cdef int n = e - s + 1
        cdef np.ndarray[float, ndim=1] buf = np.empty(n, dtype=np.float32)
        cdef SWMM_Output h = self._handle
        cdef float* p = <float*>buf.data
        cdef int rc
        with nogil:
            rc = swmm_output_get_node_series(h, idx, v, s, e, p)
        if rc != 0:
            from ._exceptions import raise_for_code
            raise_for_code(rc)
        return buf

    def link_series(self, link, var, *, start=None, end=None) -> np.ndarray:
        """Return the time series of *var* for *link* over the reporting window."""
        cdef int idx = self._resolve(link, "link")
        cdef int v = int(var)
        cdef int period_count = swmm_output_get_period_count(self._handle)
        cdef int s, e
        s, e = self._series_range(period_count, start, end)
        cdef int n = e - s + 1
        cdef np.ndarray[float, ndim=1] buf = np.empty(n, dtype=np.float32)
        cdef SWMM_Output h = self._handle
        cdef float* p = <float*>buf.data
        cdef int rc
        with nogil:
            rc = swmm_output_get_link_series(h, idx, v, s, e, p)
        if rc != 0:
            from ._exceptions import raise_for_code
            raise_for_code(rc)
        return buf

    def subcatchment_series(self, sub, var, *, start=None, end=None) -> np.ndarray:
        """Return the time series of *var* for *sub* over the reporting window."""
        cdef int idx = self._resolve(sub, "subcatchment")
        cdef int v = int(var)
        cdef int period_count = swmm_output_get_period_count(self._handle)
        cdef int s, e
        s, e = self._series_range(period_count, start, end)
        cdef int n = e - s + 1
        cdef np.ndarray[float, ndim=1] buf = np.empty(n, dtype=np.float32)
        cdef SWMM_Output h = self._handle
        cdef float* p = <float*>buf.data
        cdef int rc
        with nogil:
            rc = swmm_output_get_subcatch_series(h, idx, v, s, e, p)
        if rc != 0:
            from ._exceptions import raise_for_code
            raise_for_code(rc)
        return buf

    def system_series(self, var, *, start=None, end=None) -> np.ndarray:
        """Return the system-wide time series of *var* over the reporting window."""
        cdef int v = int(var)
        cdef int period_count = swmm_output_get_period_count(self._handle)
        cdef int s, e
        s, e = self._series_range(period_count, start, end)
        cdef int n = e - s + 1
        cdef np.ndarray[float, ndim=1] buf = np.empty(n, dtype=np.float32)
        cdef SWMM_Output h = self._handle
        cdef float* p = <float*>buf.data
        cdef int rc
        with nogil:
            rc = swmm_output_get_system_series(h, v, s, e, p)
        if rc != 0:
            from ._exceptions import raise_for_code
            raise_for_code(rc)
        return buf

    # ------------------------------------------------------------------
    # All-attribute dict at one period
    # ------------------------------------------------------------------

    def node_attributes(self, node, int period) -> Dict[OutNodeVar, float]:
        """Return all output variables for *node* at output *period* as a dict."""
        cdef int idx = self._resolve(node, "node")
        cdef int n_attrs = 0
        cdef int budget = max(32, int(OutNodeVar.POLLUT_BASE) + self.pollutant_count)
        cdef np.ndarray[float, ndim=1] buf = np.empty(budget, dtype=np.float32)
        cdef int rc = swmm_output_get_node_attribute(
            self._handle, idx, period, <float*>buf.data, &n_attrs)
        if rc != 0:
            from ._exceptions import raise_for_code
            raise_for_code(rc)
        return self._attrs_to_dict(OutNodeVar, buf[:n_attrs])

    def link_attributes(self, link, int period) -> Dict[OutLinkVar, float]:
        """Return all output variables for *link* at output *period* as a dict."""
        cdef int idx = self._resolve(link, "link")
        cdef int n_attrs = 0
        cdef int budget = max(32, int(OutLinkVar.POLLUT_BASE) + self.pollutant_count)
        cdef np.ndarray[float, ndim=1] buf = np.empty(budget, dtype=np.float32)
        cdef int rc = swmm_output_get_link_attribute(
            self._handle, idx, period, <float*>buf.data, &n_attrs)
        if rc != 0:
            from ._exceptions import raise_for_code
            raise_for_code(rc)
        return self._attrs_to_dict(OutLinkVar, buf[:n_attrs])

    def subcatchment_attributes(self, sub, int period) -> Dict[OutSubcatchVar, float]:
        """Return all output variables for *sub* at output *period* as a dict."""
        cdef int idx = self._resolve(sub, "subcatchment")
        cdef int n_attrs = 0
        cdef int budget = max(32, int(OutSubcatchVar.POLLUT_BASE) + self.pollutant_count)
        cdef np.ndarray[float, ndim=1] buf = np.empty(budget, dtype=np.float32)
        cdef int rc = swmm_output_get_subcatch_attribute(
            self._handle, idx, period, <float*>buf.data, &n_attrs)
        if rc != 0:
            from ._exceptions import raise_for_code
            raise_for_code(rc)
        return self._attrs_to_dict(OutSubcatchVar, buf[:n_attrs])

    @staticmethod
    def _attrs_to_dict(enum_cls, values):
        """Build ``{enum_member: float}`` for base attributes; pollutant
        slots (index >= ``POLLUT_BASE``) become keyed by integer index
        because the enum doesn't enumerate them individually."""
        out = {}
        try:
            base = int(enum_cls.POLLUT_BASE)
        except AttributeError:
            base = len(values)
        for i, v in enumerate(values):
            if i < base:
                try:
                    out[enum_cls(i)] = float(v)
                except ValueError:
                    out[i] = float(v)
            else:
                out[i] = float(v)
        return out

    # ------------------------------------------------------------------
    # Per-node summary stats — re-derived from the .out file
    # ------------------------------------------------------------------

    def node_stats(self, node):
        """Cumulative flooding/overflow stats for ``node``.

        The values are aggregated over the file's report-step samples; for
        runs with a much finer routing step the engine-side stats
        (:attr:`Node.stats`) are more precise.
        """
        cdef int idx = self._resolve(node, "node")
        return _OutputNodeStats(self, idx)

    def __repr__(self) -> str:
        try:
            return (f"<OutputReader periods={self.period_count} "
                    f"nodes={self.node_count} links={self.link_count}>")
        except Exception:
            return "<OutputReader (closed)>"


cdef class _OutputNodeStats:
    """View returned by :meth:`OutputReader.node_stats`."""
    cdef object _reader
    cdef int _index

    def __init__(self, reader, int index):
        self._reader = reader
        self._index = index

    @property
    def max_depth(self) -> float:
        cdef OutputReader r = <OutputReader>self._reader
        cdef double v = 0.0
        cdef int rc = swmm_output_get_node_stat_max_depth(
            r._handle, self._index, &v)
        if rc != 0:
            from ._exceptions import raise_for_code
            raise_for_code(rc)
        return v

    @property
    def max_overflow(self) -> float:
        cdef OutputReader r = <OutputReader>self._reader
        cdef double v = 0.0
        cdef int rc = swmm_output_get_node_stat_max_overflow(
            r._handle, self._index, &v)
        if rc != 0:
            from ._exceptions import raise_for_code
            raise_for_code(rc)
        return v

    @property
    def vol_flooded(self) -> float:
        cdef OutputReader r = <OutputReader>self._reader
        cdef double v = 0.0
        cdef int rc = swmm_output_get_node_stat_vol_flooded(
            r._handle, self._index, &v)
        if rc != 0:
            from ._exceptions import raise_for_code
            raise_for_code(rc)
        return v

    @property
    def time_flooded(self) -> float:
        """Cumulative flooded duration in seconds."""
        cdef OutputReader r = <OutputReader>self._reader
        cdef double v = 0.0
        cdef int rc = swmm_output_get_node_stat_time_flooded(
            r._handle, self._index, &v)
        if rc != 0:
            from ._exceptions import raise_for_code
            raise_for_code(rc)
        return v

    def __repr__(self) -> str:
        return f"<_OutputNodeStats index={self._index}>"
