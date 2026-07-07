"""
Hot start files (Pythonic v1 surface)
=====================================

:author: Caleb Buahin
:copyright: Copyright (c) 2026 Caleb Buahin
:license: MIT

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
from datetime import datetime
from typing import List, NamedTuple, Optional

from ._common cimport *
from ._solver cimport Solver
from ._dates import datetime_to_oadate, oadate_to_datetime


cdef class HotStart:
    """Handle to a hot start file. See module docstring."""

    cdef SWMM_HotStart _handle

    def __init__(self):
        self._handle = NULL

    # ------------------------------------------------------------------
    # Lifecycle classmethods
    # ------------------------------------------------------------------

    @staticmethod
    def save_from(Solver solver, path) -> None:
        """Save ``solver`` state to ``path``. Raises on failure."""
        cdef bytes b = os.fspath(path).encode('utf-8')
        cdef SWMM_Engine h = solver._handle
        cdef const char* p = b
        cdef int rc
        with nogil:
            rc = swmm_hotstart_save(h, p)
        _check(rc)

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
        _check(rc)
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
        cdef SWMM_Engine h = solver._handle
        cdef SWMM_HotStart hs = self._handle
        cdef int rc
        with nogil:
            rc = swmm_hotstart_apply(h, hs)
        _check(rc)

    # ------------------------------------------------------------------
    # Metadata
    # ------------------------------------------------------------------

    @property
    def sim_datetime(self) -> datetime:
        """Moment at which the saved state was captured."""
        cdef double v = 0.0
        _check(swmm_hotstart_get_sim_time(self._handle, &v))
        return oadate_to_datetime(v)

    @property
    def crs(self) -> Optional[str]:
        """CRS string captured in the file, or ``None`` if absent."""
        cdef char buf[256]
        cdef int rc = swmm_hotstart_get_crs(self._handle, buf, 256)
        if rc != 0:
            return None
        s = buf.decode('utf-8')
        return s or None

    @property
    def node_count(self) -> int:
        return swmm_hotstart_node_count(self._handle)

    @property
    def link_count(self) -> int:
        return swmm_hotstart_link_count(self._handle)

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

    def __repr__(self) -> str:
        try:
            return (f"<HotStart sim={self.sim_datetime.isoformat()} "
                    f"nodes={self.node_count} links={self.link_count}>")
        except Exception:
            return "<HotStart (closed)>"


# =============================================================================
# Save schedule — solver.save_schedule
# =============================================================================

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
        cdef char buf[512]
        _check(swmm_hotstart_saves_get_path(h, idx, buf, 512))
        cdef double when = 0.0
        _check(swmm_hotstart_saves_get_datetime(h, idx, &when))
        return SaveScheduleEntry(
            when=oadate_to_datetime(when), path=buf.decode('utf-8'))

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
        # re-adding (same approach as solver.events).
        n = len(self)
        if idx < 0:
            idx += n
        idx = max(0, min(idx, n))
        existing = [self[i] for i in range(n)]
        existing.insert(idx, value)
        self.clear()
        for ent in existing:
            self.append(ent)

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
