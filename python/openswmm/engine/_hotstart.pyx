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
Hot start files (Pythonic v1 surface)
=====================================

:author: Caleb Buahin
:copyright: Copyright (c) 2026 Caleb Buahin
:license: Apache-2.0

Hot start files capture the hydraulic + quality state of a simulation
at a moment in time and let a follow-up run resume from there.

.. code-block:: python

    from pathlib import Path
    from openswmm.engine import Solver, HotStart

    # ---- save ----
    with Solver("warmup.inp") as s:
        for _ in s.steps():
            pass
        HotStart.save_from(s, Path("warmup.hs"))

    # ---- apply ----
    with HotStart.open("warmup.hs") as hs:
        print(hs.sim_datetime, hs.crs, hs.warnings)
        hs.nodes["J1"].depth = 1.2          # optional pre-apply edits
        with Solver("storm.inp") as s:
            hs.apply(s)                      # in OPENED/INITIALIZED state
            for _ in s.steps():
                pass

The runtime save-schedule (the ``[SAVE HOTSTART]`` block on the engine)
is exposed via :attr:`Solver.save_schedule`.
"""

# cython: language_level=3

import os
from collections.abc import MutableSequence
from datetime import datetime, timedelta, timezone
from typing import List, NamedTuple, Optional

from ._common cimport *
from ._solver cimport Solver
from ._dates import datetime_to_oadate, oadate_to_datetime

cdef extern from "openswmm/engine/openswmm_hotstart.h":
    enum:
        SWMM_ERR_HOTSTART
    const char* swmm_hotstart_last_io_error()

cdef void _check_io(int rc) except *:
    cdef const char* detail
    if rc == SWMM_ERR_HOTSTART:
        detail = swmm_hotstart_last_io_error()
        if detail != NULL and detail[0] != 0:
            from ._exceptions import raise_for_code
            raise_for_code(rc, detail.decode('utf-8'))
    _check(rc)


# SWMM_HOTSTART_* content bits, by name.
_CONTENTS = ((0x01, "SPECIES"), (0x02, "GROUNDWATER"), (0x04, "GW_TRANSPORT"),
             (0x08, "SNOW"), (0x10, "LID"), (0x20, "SURFACE"),
             (0x40, "ENGINE_CHECKPOINT"), (0x80, "LEGACY"))


class HotStartNodeRecord(NamedTuple):
    """One node record stored in a hot-start file, in internal units.

    ``depth`` and ``head`` in ft, ``volume`` in ft3, ``age`` in seconds
    (negative when water age is not tracked). ``head`` and ``volume`` are NaN
    when the file does not store them (a legacy file) or when
    :meth:`HotStart.set_node_state` marked them to be derived at restart.
    ``concentrations`` holds one value per :attr:`HotStart.species`.
    """
    id: str
    depth: float
    head: float
    volume: float
    age: float
    concentrations: tuple


class HotStartLinkRecord(NamedTuple):
    """One link record stored in a hot-start file, in internal units.

    ``flow`` in ft3/s (signed in link orientation), ``depth`` in ft,
    ``volume`` in ft3 (NaN when not stored or marked to be derived by
    :meth:`HotStart.set_link_state`), ``age`` in seconds (negative when not
    tracked). ``concentrations`` holds one value per :attr:`HotStart.species`.
    """
    id: str
    flow: float
    depth: float
    volume: float
    age: float
    concentrations: tuple


class HotStartSubcatchmentRecord(NamedTuple):
    """One subcatchment record stored in a hot-start file, in internal units.

    ``runoff`` and ``gw_flow`` in ft3/s, ``gw_upper_moisture`` as a fraction,
    ``gw_lower_depth`` in ft. A value the file does not hold is NaN.
    """
    id: str
    runoff: float
    gw_flow: float
    gw_upper_moisture: float
    gw_lower_depth: float


cdef class HotStart:
    """Handle to a hot start file. See module docstring."""

    cdef SWMM_HotStart _handle

    def __init__(self):
        self._handle = NULL

    cdef SWMM_HotStart _live(self) except NULL:
        if self._handle == NULL:
            from ._exceptions import BadHandleError
            from ._enums import ErrorCode
            raise BadHandleError(ErrorCode.BADHANDLE, "HotStart is closed")
        return self._handle

    # ------------------------------------------------------------------
    # Lifecycle classmethods
    # ------------------------------------------------------------------

    @staticmethod
    def save_from(Solver solver, path) -> None:
        """Save ``solver`` state to ``path``. Raises on failure."""
        cdef bytes b = os.fspath(path).encode('utf-8')
        cdef SWMM_Engine h = <SWMM_Engine><size_t>solver.handle
        cdef const char* p = b
        cdef int rc
        with solver._operation(<size_t>h):
            with nogil:
                rc = swmm_hotstart_save(h, p)
            _check_io(rc)

    @classmethod
    def open(cls, path) -> "HotStart":
        """Open an existing hot start file."""
        cdef HotStart obj = HotStart()
        cdef bytes b = os.fspath(path).encode('utf-8')
        cdef SWMM_HotStart hs = NULL
        cdef const char* p = b
        cdef int rc
        with nogil:
            rc = swmm_hotstart_open(p, &hs)
        _check_io(rc)
        obj._handle = hs
        return obj

    @classmethod
    def open_legacy(cls, Solver model, path) -> "HotStart":
        """Open a legacy EPA SWMM 5 hot-start file for inspection.

        The legacy format stores values in object order without names, so the
        file is read against ``model`` (a Solver that is OPENED or later): its
        subcatchment, node, link and pollutant counts must match the model's,
        and records take the model's names. Node head and volume and link
        volume are not stored (NaN). The handle is read-only: the setters and
        :meth:`apply` refuse it, and :meth:`close` writes nothing. The model is
        not changed. Raises :class:`HotStartError` when the file cannot be
        read, is not a legacy file or was written for another model.
        """
        cdef HotStart obj = HotStart()
        cdef bytes b = os.fspath(path).encode('utf-8')
        cdef SWMM_Engine h = <SWMM_Engine><size_t>model.handle
        cdef SWMM_HotStart hs = NULL
        cdef const char* p = b
        cdef int rc
        with model._operation(<size_t>h):
            with nogil:
                rc = swmm_hotstart_open_legacy(h, p, &hs)
            _check_io(rc)
        obj._handle = hs
        return obj

    def close(self) -> None:
        """Close the hot-start file and release its handle."""
        if self._handle != NULL:
            swmm_hotstart_close(self._handle)
            self._handle = NULL

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.close()
        return False

    def __dealloc__(self):
        if self._handle != NULL:
            swmm_hotstart_close(self._handle)
            self._handle = NULL

    # ------------------------------------------------------------------
    # Apply
    # ------------------------------------------------------------------

    def apply(self, Solver solver) -> None:
        """Apply this hot start to ``solver``. The solver must be
        INITIALIZED (post-:meth:`Solver.initialize`, pre-:meth:`Solver.start`)."""
        cdef SWMM_Engine h = <SWMM_Engine><size_t>solver.handle
        cdef SWMM_HotStart hs = self._handle
        cdef int rc
        with solver._operation(<size_t>h):
            rc = swmm_hotstart_apply(h, hs)
        _check(rc)

    # ------------------------------------------------------------------
    # Metadata
    # ------------------------------------------------------------------

    @property
    def sim_datetime(self) -> datetime:
        """Moment at which the saved state was captured: the saving run's
        start date plus the elapsed simulation time."""
        cdef double elapsed = 0.0
        _check(swmm_hotstart_get_sim_time(self._handle, &elapsed))
        return self.start_datetime + timedelta(seconds=elapsed)

    @property
    def start_datetime(self) -> datetime:
        """Start date and time of the run that saved the state."""
        cdef double v = 0.0
        _check(swmm_hotstart_get_start_date(self._handle, &v))
        return oadate_to_datetime(v)

    @property
    def crs(self) -> Optional[str]:
        """CRS string captured in the file, or ``None`` if absent."""
        cdef _TextBufs t
        cdef int rc
        _tb_open(&t, 1)
        try:
            while True:
                rc = swmm_hotstart_get_crs(self._handle, t.p[0], t.size)
                if rc != 0:
                    return None
                if not _tb_retry(&t):
                    break
            s = _tb_str(&t, 0)
            return s or None
        finally:
            _tb_close(&t)

    @property
    def node_count(self) -> int:
        return swmm_hotstart_node_count(self._handle)

    @property
    def link_count(self) -> int:
        return swmm_hotstart_link_count(self._handle)

    @property
    def subcatchment_count(self) -> int:
        return swmm_hotstart_subcatch_count(self._handle)

    @property
    def contents(self) -> tuple:
        """State blocks the file holds, drawn from ``"SPECIES"``,
        ``"GROUNDWATER"``, ``"GW_TRANSPORT"``, ``"SNOW"``, ``"LID"``,
        ``"SURFACE"``, ``"ENGINE_CHECKPOINT"`` (a full checkpoint, which a
        restart restores instead of the records) and ``"LEGACY"`` (opened with
        :meth:`open_legacy`)."""
        cdef int flags = 0
        _check(swmm_hotstart_get_contents(self._handle, &flags))
        return tuple(name for bit, name in _CONTENTS if flags & bit)

    @property
    def version(self) -> int:
        """File format version: the native record version, or 1-4 for a
        legacy SWMM 5 file."""
        cdef int v = 0
        _check(swmm_hotstart_get_version(self._handle, &v))
        return v

    @property
    def saved_at(self) -> Optional[datetime]:
        """When a native file was saved (UTC), or ``None`` for a legacy file."""
        cdef long long seconds = 0
        _check(swmm_hotstart_get_saved_at(self._handle, &seconds))
        return datetime.fromtimestamp(seconds, tz=timezone.utc) if seconds else None

    @property
    def end_datetime(self) -> Optional[datetime]:
        """End date and time of the run that saved the state, or ``None`` for
        a legacy file."""
        cdef double v = 0.0
        _check(swmm_hotstart_get_end_date(self._handle, &v))
        return oadate_to_datetime(v) if v else None

    @property
    def species(self) -> List[str]:
        """Names of the species (pollutants or reaction species, as saved)
        whose node and link concentrations the file holds."""
        cdef SWMM_HotStart hs = self._live()
        cdef int n = swmm_hotstart_species_count(hs)
        cdef int i
        cdef _TextBufs t
        out = []
        _tb_open(&t, 1)
        try:
            for i in range(n):
                while True:
                    _check(swmm_hotstart_get_species(hs, i, t.p[0], t.size))
                    if not _tb_retry(&t):
                        break
                out.append(_tb_str(&t, 0))
        finally:
            _tb_close(&t)
        return out

    def node_records(self) -> List[HotStartNodeRecord]:
        """Every stored node record, in file order (see
        :class:`HotStartNodeRecord`)."""
        cdef SWMM_HotStart hs = self._live()
        cdef int n = swmm_hotstart_node_count(hs)
        cdef int ns = swmm_hotstart_species_count(hs)
        cdef int i, k
        cdef double depth = 0.0, head = 0.0, volume = 0.0, age = 0.0, c = 0.0
        cdef _TextBufs t
        out = []
        _tb_open(&t, 1)
        try:
            for i in range(n):
                while True:
                    _check(swmm_hotstart_get_node(hs, i, t.p[0], t.size,
                                                  &depth, &head, &volume, &age))
                    if not _tb_retry(&t):
                        break
                conc = []
                for k in range(ns):
                    _check(swmm_hotstart_get_node_species(hs, i, k, &c))
                    conc.append(c)
                out.append(HotStartNodeRecord(_tb_str(&t, 0), depth, head, volume, age,
                                              tuple(conc)))
        finally:
            _tb_close(&t)
        return out

    def link_records(self) -> List[HotStartLinkRecord]:
        """Every stored link record, in file order (see
        :class:`HotStartLinkRecord`)."""
        cdef SWMM_HotStart hs = self._live()
        cdef int n = swmm_hotstart_link_count(hs)
        cdef int ns = swmm_hotstart_species_count(hs)
        cdef int i, k
        cdef double flow = 0.0, depth = 0.0, volume = 0.0, age = 0.0, c = 0.0
        cdef _TextBufs t
        out = []
        _tb_open(&t, 1)
        try:
            for i in range(n):
                while True:
                    _check(swmm_hotstart_get_link(hs, i, t.p[0], t.size,
                                                  &flow, &depth, &volume, &age))
                    if not _tb_retry(&t):
                        break
                conc = []
                for k in range(ns):
                    _check(swmm_hotstart_get_link_species(hs, i, k, &c))
                    conc.append(c)
                out.append(HotStartLinkRecord(_tb_str(&t, 0), flow, depth, volume, age,
                                              tuple(conc)))
        finally:
            _tb_close(&t)
        return out

    def subcatchment_records(self) -> List[HotStartSubcatchmentRecord]:
        """Every stored subcatchment record, in file order (see
        :class:`HotStartSubcatchmentRecord`)."""
        cdef SWMM_HotStart hs = self._live()
        cdef int n = swmm_hotstart_subcatch_count(hs)
        cdef int i
        cdef double runoff = 0.0, gw_flow = 0.0, upper = 0.0, lower = 0.0
        cdef _TextBufs t
        out = []
        _tb_open(&t, 1)
        try:
            for i in range(n):
                while True:
                    _check(swmm_hotstart_get_subcatch(hs, i, t.p[0], t.size,
                                                      &runoff, &gw_flow, &upper, &lower))
                    if not _tb_retry(&t):
                        break
                out.append(HotStartSubcatchmentRecord(_tb_str(&t, 0), runoff, gw_flow,
                                                      upper, lower))
        finally:
            _tb_close(&t)
        return out

    @property
    def warnings(self) -> List[str]:
        """All warnings emitted during open/apply, in order."""
        cdef int n = swmm_hotstart_warning_count(self._handle)
        cdef const char* raw
        out = []
        for i in range(n):
            raw = swmm_hotstart_warning(self._handle, i)
            out.append(raw.decode('utf-8') if raw != NULL else "")
        return out

    # ------------------------------------------------------------------
    # Direct state edits — by id (the C API is id-keyed, not index-keyed)
    # ------------------------------------------------------------------

    def set_node_depth(self, str node_id, double depth) -> None:
        """Seed the initial water depth at *node_id* in the hot-start state."""
        cdef bytes b = node_id.encode('utf-8')
        _check(swmm_hotstart_set_node_depth(self._handle, b, depth))

    def set_node_head(self, str node_id, double head) -> None:
        """Seed the initial hydraulic head at *node_id* in the hot-start state."""
        cdef bytes b = node_id.encode('utf-8')
        _check(swmm_hotstart_set_node_head(self._handle, b, head))

    def set_link_flow(self, str link_id, double flow) -> None:
        """Seed the initial flow at *link_id* in the hot-start state."""
        cdef bytes b = link_id.encode('utf-8')
        _check(swmm_hotstart_set_link_flow(self._handle, b, flow))

    def set_link_depth(self, str link_id, double depth) -> None:
        """Seed the initial depth at *link_id* in the hot-start state."""
        cdef bytes b = link_id.encode('utf-8')
        _check(swmm_hotstart_set_link_depth(self._handle, b, depth))

    def set_subcatchment_runoff(self, str sub_id, double runoff) -> None:
        """Seed the initial runoff at *sub_id* in the hot-start state."""
        cdef bytes b = sub_id.encode('utf-8')
        _check(swmm_hotstart_set_subcatch_runoff(self._handle, b, runoff))

    def set_node_state(self, str node_id, double depth) -> None:
        """Set *node_id*'s depth (ft) so that a restart derives a consistent
        head (invert + depth) and volume from the restarting model. The edit
        is written to the file on :meth:`close`. Raises
        :class:`BadParamError` for an unknown ID or a negative or non-finite
        depth, and :class:`HotStartError` for a legacy file or one holding a
        full engine checkpoint (a restart would not read the records)."""
        cdef bytes b = node_id.encode('utf-8')
        _check(swmm_hotstart_set_node_state(self._handle, b, depth))

    def set_link_state(self, str link_id, double flow, double depth) -> None:
        """Set *link_id*'s flow (ft3/s, signed in link orientation) and depth
        (ft) so that a restart derives a consistent volume. The edit is
        written to the file on :meth:`close`. Raises as
        :meth:`set_node_state`; the flow must be finite."""
        cdef bytes b = link_id.encode('utf-8')
        _check(swmm_hotstart_set_link_state(self._handle, b, flow, depth))

    def __repr__(self) -> str:
        try:
            return (f"<HotStart sim={self.sim_datetime.isoformat()} "
                    f"nodes={self.node_count} links={self.link_count}>")
        except Exception:
            return "<HotStart (closed)>"


# =============================================================================
# Save schedule — solver.save_schedule
# =============================================================================

# SWMM_HOTSTART_FORMAT_AUTO / _LEGACY / _NATIVE, by value.
_SAVE_FORMATS = ("AUTO", "LEGACY", "NATIVE")
# SWMM_HOTSTART_OMITS_* bits, by name.
_LEGACY_OMITS = ((0x1, "WATER_AGE"), (0x2, "REACTION_SPECIES"),
                 (0x4, "AQUIFER"), (0x8, "NODE_LIDS"))


class SaveScheduleEntry(NamedTuple):
    """One row in the engine's ``[SAVE HOTSTART]`` schedule."""
    when: datetime
    path: str


class SaveSchedule(MutableSequence):
    """``solver.save_schedule`` — schedule of hot-start save events.

    .. code-block:: python

        from datetime import datetime
        solver.save_schedule.append(
            SaveScheduleEntry(when=datetime(2024,1,1,12,0), path="midday.hs"))
        for entry in solver.save_schedule:
            print(entry.when, entry.path)
        del solver.save_schedule[0]
        solver.save_schedule.clear()
    """

    def __init__(self, solver):
        self._solver = solver

    def _h(self):
        # Returns the int handle; cdef-friendly C cast happens at call sites.
        return self._solver.handle

    def __len__(self) -> int:
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int n = 0
        _check(swmm_hotstart_saves_count(h, &n))
        return n

    def __getitem__(self, idx):
        if isinstance(idx, slice):
            return [self[i] for i in range(*idx.indices(len(self)))]
        if not isinstance(idx, int):
            raise TypeError(
                "SaveSchedule index must be int or slice, got "
                + type(idx).__name__)
        n = len(self)
        if idx < 0:
            idx += n
        if not 0 <= idx < n:
            raise IndexError(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        path = _read_engine_index_string(h, idx, swmm_hotstart_saves_get_path)
        cdef double when = 0.0
        _check(swmm_hotstart_saves_get_datetime(h, idx, &when))
        return SaveScheduleEntry(
            when=oadate_to_datetime(when), path=path)

    def __setitem__(self, idx, value):
        if not isinstance(idx, int):
            raise TypeError("SaveSchedule index must be int")
        n = len(self)
        if idx < 0:
            idx += n
        if not 0 <= idx < n:
            raise IndexError(idx)
        when, path = self._unpack(value)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef bytes b = path.encode('utf-8')
        _check(swmm_hotstart_saves_set_path(h, idx, b))
        _check(swmm_hotstart_saves_set_datetime(h, idx, datetime_to_oadate(when)))

    def __delitem__(self, idx):
        if not isinstance(idx, int):
            raise TypeError("SaveSchedule index must be int")
        n = len(self)
        if idx < 0:
            idx += n
        if not 0 <= idx < n:
            raise IndexError(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_hotstart_saves_remove(h, idx))

    def insert(self, idx, value):
        """Insert *value* at *idx* (emulated via clear + re-add; the C save API is append-only)."""
        # The C API only supports append; emulate insert by clearing and
        # re-adding (same approach as solver.events). Formats ride along.
        n = len(self)
        if idx < 0:
            idx += n
        idx = max(0, min(idx, n))
        existing = [(self[i], self.get_format(i)) for i in range(n)]
        existing.insert(idx, (value, "AUTO"))
        self.clear()
        for i, (ent, fmt) in enumerate(existing):
            self.append(ent)
            if fmt != "AUTO":
                self.set_format(i, fmt)

    def get_format(self, idx) -> str:
        """File format of entry *idx*: ``"AUTO"``, ``"LEGACY"`` or ``"NATIVE"``.

        AUTO (the default, and every row without a format keyword) writes the
        legacy SWMM5 ``.hsf``, or the native file when the model has node
        LIDs. NATIVE also keeps the state :meth:`legacy_omits` names; legacy
        SWMM 5 cannot read it.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int fmt = 0
        _check(swmm_hotstart_saves_get_format(h, self._index(idx), &fmt))
        return _SAVE_FORMATS[fmt]

    def set_format(self, idx, fmt: str) -> None:
        """Set the file format of entry *idx* (``"AUTO"``, ``"LEGACY"`` or ``"NATIVE"``)."""
        key = str(fmt).upper()
        if key not in _SAVE_FORMATS:
            raise ValueError(
                "hot-start save format must be one of " + ", ".join(_SAVE_FORMATS)
                + "; got " + repr(fmt))
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_hotstart_saves_set_format(h, self._index(idx),
                                              _SAVE_FORMATS.index(key)))

    def legacy_omits(self) -> tuple:
        """Model state a legacy SWMM5 file has no place for and NATIVE keeps.

        A tuple drawn from ``"WATER_AGE"``, ``"REACTION_SPECIES"``,
        ``"AQUIFER"`` and ``"NODE_LIDS"``; empty when nothing in the model
        needs NATIVE. ``[POLLUTANTS]`` concentrations are in both formats.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int flags = 0
        _check(swmm_hotstart_legacy_omits(h, &flags))
        return tuple(name for bit, name in _LEGACY_OMITS if flags & bit)

    def _index(self, idx) -> int:
        if not isinstance(idx, int):
            raise TypeError("SaveSchedule index must be int")
        n = len(self)
        if idx < 0:
            idx += n
        if not 0 <= idx < n:
            raise IndexError(idx)
        return idx

    def append(self, value):
        """Append a hot-start save-schedule entry *value*."""
        when, path = self._unpack(value)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef bytes b = path.encode('utf-8')
        _check(swmm_hotstart_saves_add(h, b, datetime_to_oadate(when)))

    def clear(self):
        """Remove all entries from the hot-start save schedule."""
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_hotstart_saves_clear(h))

    @staticmethod
    def _unpack(value):
        if isinstance(value, SaveScheduleEntry):
            return value.when, os.fspath(value.path)
        if isinstance(value, dict):
            return value["when"], os.fspath(value["path"])
        try:
            when, path = value
            return when, os.fspath(path)
        except (TypeError, ValueError) as exc:
            raise TypeError(
                "SaveSchedule entry must be SaveScheduleEntry, "
                "(when, path) tuple, or dict with 'when'/'path' keys"
            ) from exc

    def __repr__(self) -> str:
        try:
            return f"<SaveSchedule n={len(self)}>"
        except Exception:
            return "<SaveSchedule (engine closed)>"
