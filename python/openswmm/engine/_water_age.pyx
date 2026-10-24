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
Water age (Pythonic v1 surface)
===============================

:author: Caleb Buahin
:copyright: Copyright (c) 2026 Caleb Buahin
:license: Apache-2.0

``solver.water_age`` is the editable ``[WATER_AGE_SOURCES]`` table: a GLOBAL
age per source pathway, plus per-NODE overrides for the ``DWF`` and
``EXTERNAL_INFLOW`` pathways. Every value is in **HOURS**, the config file's
own unit, and **negative values are legal** — a negative source age *extracts*
age-volume rather than adding it, clamped so the resulting age never goes
below zero. Only ``DWF`` and ``EXTERNAL_INFLOW`` accept NODE scope; asking any
other pathway for a node override is refused with
:class:`~openswmm.engine.BadParamError`, exactly as the deck parser refuses it
(the A1a scope rule).

**Edits are live.** Setters mutate engine state and the water-age loaders
re-read the table every step, so a mid-run edit takes effect on the next
routing step. :meth:`WaterAge.save` persists the current table as a
``model.age`` component file.

.. code-block:: python

    from openswmm.engine import Solver, WaterAgeSource

    with Solver("model.inp") as s:
        if s.water_age.enabled:
            s.water_age.globals[WaterAgeSource.RAINFALL] = 0.0
            s.water_age.globals[WaterAgeSource.DWF] = 6.0
            # Negative is legal: this DWF stream extracts age-volume.
            s.water_age.node_overrides.set(WaterAgeSource.DWF, "J1", -2.0)
            for row in s.water_age.node_overrides:
                print(row.source.name, row.node_index, row.hours)
            s.water_age.save("model.age")
"""

# cython: language_level=3

from collections.abc import Iterator
from typing import NamedTuple

from ._common cimport *
from ._enums import WaterAgeSource


cdef inline SWMM_Engine _h(solver):
    return <SWMM_Engine><size_t>solver.handle


cdef inline int _resolve_node(solver, key) except -1:
    return _resolve_index(
        _h(solver), key, swmm_node_index, swmm_node_count, "Node")


def _source(key):
    """Coerce *key* (member, int code, or member name) to :class:`WaterAgeSource`.

    :param key: A :class:`WaterAgeSource` member, its integer code, or its
        member name (case-insensitive).
    :returns: The matching :class:`WaterAgeSource` member.
    :raises KeyError: *key* names no member; the message lists the valid ones.
    """
    if isinstance(key, WaterAgeSource):
        return key
    try:
        if isinstance(key, str):
            return WaterAgeSource[key.upper()]
        return WaterAgeSource(key)
    except (KeyError, ValueError):
        pass
    valid = ", ".join(m.name for m in WaterAgeSource)
    raise KeyError(
        f"{key!r} is not a valid WaterAgeSource; valid members: {valid}")


class WaterAgeOverride(NamedTuple):
    """One ``[WATER_AGE_SOURCES]`` NODE-scope override row.

    :ivar source: The :class:`WaterAgeSource` the row overrides. Only
        ``DWF`` and ``EXTERNAL_INFLOW`` can appear here (the A1a scope rule).
    :ivar node_index: Zero-based index of the node the override applies to.
    :ivar hours: Age this source's water enters that node at, in HOURS.
        Signed — a negative value extracts age-volume, clamped so the
        resulting age never goes below zero.
    """

    source: WaterAgeSource
    node_index: int
    hours: float


# =============================================================================
# [WATER_AGE_SOURCES] — GLOBAL ages
# =============================================================================

class _WaterAgeGlobals:
    """``solver.water_age.globals`` — mapping keyed by :class:`WaterAgeSource`.

    Values are inlet ages in HOURS, one per source pathway. The table is a
    fixed enum extent, not a parsed list, so every pathway is readable even on
    a model that configured none of them.
    """

    def __init__(self, solver):
        self._solver = solver

    def __len__(self) -> int:
        """Number of source pathways the table carries (7).

        :returns: ``len(WaterAgeSource)``.
        """
        return len(WaterAgeSource)

    def __iter__(self) -> Iterator[WaterAgeSource]:
        return iter(WaterAgeSource)

    def __getitem__(self, key) -> float:
        """Read one pathway's GLOBAL source age.

        :param key: A :class:`WaterAgeSource` member, code, or name.
        :returns: The global source age in HOURS. Signed — see
            :meth:`__setitem__`.
        :raises KeyError: *key* is not a valid :class:`WaterAgeSource`.
        :raises EngineError: The engine refused the read.
        """
        cdef double hours = 0.0
        source = _source(key)
        _check(swmm_water_age_get_global_source(
            _h(self._solver), int(source), &hours))
        return hours

    def __setitem__(self, key, double hours) -> None:
        """Write one pathway's GLOBAL source age and mark the table present.

        The edit is live: the water-age loaders re-read the table, so a
        mid-run write takes effect on the next routing step.

        :param key: A :class:`WaterAgeSource` member, code, or name.
        :param hours: Inlet age in HOURS. **Negative values are legal** — a
            negative source age extracts age-volume instead of adding it, and
            the engine clamps the result so age never goes below zero.
        :raises KeyError: *key* is not a valid :class:`WaterAgeSource`.
        :raises EngineError: The engine refused the write.
        """
        source = _source(key)
        _check(swmm_water_age_set_global_source(
            _h(self._solver), int(source), hours))

    def __contains__(self, key) -> bool:
        """Is *key* a source pathway this table carries?

        :param key: A :class:`WaterAgeSource` member, code, or name.
        :returns: ``True`` when *key* names a valid pathway.
        """
        try:
            _source(key)
            return True
        except (KeyError, TypeError):
            return False

    def __repr__(self) -> str:
        try:
            return f"<WaterAgeGlobals n={len(self)}>"
        except Exception:
            return "<WaterAgeGlobals (engine closed)>"


# =============================================================================
# [WATER_AGE_SOURCES] — NODE-scope overrides
# =============================================================================

class _WaterAgeOverrides:
    """``solver.water_age.node_overrides`` — the NODE-scope override rows.

    A sequence of :class:`WaterAgeOverride` rows indexed by row position. Row
    order is stable across edits within a session.

    Only :attr:`WaterAgeSource.DWF` and
    :attr:`WaterAgeSource.EXTERNAL_INFLOW` may be overridden per node — the
    A1a scope rule, refused rather than silently ignored, the same answer the
    deck gets.
    """

    def __init__(self, solver):
        self._solver = solver

    def __len__(self) -> int:
        """Number of NODE-scope override rows.

        :returns: The row count.
        :raises EngineError: The engine refused the read.
        """
        cdef int count = 0
        _check(swmm_water_age_override_count(_h(self._solver), &count))
        return count

    def __iter__(self) -> Iterator[WaterAgeOverride]:
        cdef int n = len(self)
        for i in range(n):
            yield self[i]

    def __getitem__(self, int row_index) -> WaterAgeOverride:
        """Read one NODE override by row index.

        :param row_index: Zero-based row position; negative indexes from the
            end, as for a list.
        :returns: A :class:`WaterAgeOverride` of
            ``(source, node_index, hours)`` with *hours* in HOURS, signed.
        :raises IndexError: *row_index* is out of range.
        :raises EngineError: The engine refused the read.
        """
        cdef int n = len(self)
        cdef int i = row_index + n if row_index < 0 else row_index
        if not (0 <= i < n):
            raise IndexError(row_index)
        cdef int source = 0
        cdef int node_index = -1
        cdef double hours = 0.0
        _check(swmm_water_age_get_override(
            _h(self._solver), i, &source, &node_index, &hours))
        return WaterAgeOverride(WaterAgeSource(source), node_index, hours)

    def set(self, source, node, double hours) -> None:
        """Add or update the NODE override for ``(source, node)``.

        An existing ``(source, node)`` pair is **updated**, not duplicated:
        one row per pair, whether the caller reaches it by writing once or
        twice. The edit is live — it takes effect on the next routing step.

        :param source: A :class:`WaterAgeSource` member, code, or name. Only
            :attr:`WaterAgeSource.DWF` and
            :attr:`WaterAgeSource.EXTERNAL_INFLOW` take NODE scope.
        :param node: ``int`` node index or ``str`` node id.
        :param hours: Inlet age in HOURS. **Negative values are legal** — a
            negative source age extracts age-volume, clamped so the resulting
            age never goes below zero.
        :raises KeyError: *source* is not a valid :class:`WaterAgeSource`, or
            no node with that id exists.
        :raises BadParamError: *source* is neither ``DWF`` nor
            ``EXTERNAL_INFLOW`` — the A1a scope rule, the same refusal the
            parser gives.
        :raises EngineError: *node* is out of range, or the engine otherwise
            refused the write.
        """
        kind = _source(source)
        cdef int ni = _resolve_node(self._solver, node)
        _check(swmm_water_age_set_override(
            _h(self._solver), int(kind), ni, hours))
        self._solver._bump_generation()

    def remove(self, source, node) -> None:
        """Remove the NODE override for ``(source, node)``.

        Keyed on the ``(source, node)`` pair, **not** on a row index — the row
        table is addressed positionally for reading only. Later rows may shift
        after a removal, so a caller iterating by index must re-read the
        length.

        :param source: A :class:`WaterAgeSource` member, code, or name.
        :param node: ``int`` node index or ``str`` node id.
        :raises KeyError: *source* is not a valid :class:`WaterAgeSource`, or
            no node with that id exists.
        :raises BadIndexError: No override exists for that ``(source, node)``
            pair.
        :raises EngineError: The engine otherwise refused the removal.
        """
        kind = _source(source)
        cdef int ni = _resolve_node(self._solver, node)
        _check(swmm_water_age_remove_override(
            _h(self._solver), int(kind), ni))
        self._solver._bump_generation()

    def __repr__(self) -> str:
        try:
            return f"<WaterAgeOverrides n={len(self)}>"
        except Exception:
            return "<WaterAgeOverrides (engine closed)>"


# =============================================================================
# Water-age view
# =============================================================================

class WaterAge:
    """``solver.water_age`` — the ``[WATER_AGE_SOURCES]`` configuration.

    Every value is in HOURS and signed: a negative source age extracts
    age-volume, clamped so age never goes below zero. Edits are live — the
    loaders re-read the table every step, so a mid-run edit takes effect on
    the next routing step.
    """

    def __init__(self, solver):
        self._solver = solver
        self._globals = None
        self._node_overrides = None

    @property
    def enabled(self) -> bool:
        """Is ``[OPTIONS] WATER_AGE`` on? Read-only.

        :returns: ``True`` when water-age tracking is enabled for the model.
        :raises EngineError: The engine refused the read.
        """
        cdef int enabled = 0
        _check(swmm_water_age_get_enabled(_h(self._solver), &enabled))
        return enabled != 0

    @property
    def globals(self) -> _WaterAgeGlobals:
        """GLOBAL source ages in HOURS, a mapping keyed by
        :class:`WaterAgeSource`."""
        if self._globals is None:
            self._globals = _WaterAgeGlobals(self._solver)
        return self._globals

    @property
    def node_overrides(self) -> _WaterAgeOverrides:
        """NODE-scope override rows, indexed by row position. Only ``DWF``
        and ``EXTERNAL_INFLOW`` are in scope."""
        if self._node_overrides is None:
            self._node_overrides = _WaterAgeOverrides(self._solver)
        return self._node_overrides

    def save(self, path) -> None:
        """Write the current table as a ``[WATER_AGE_SOURCES]`` component file.

        The output is the ``model.age`` format the waterage component parses,
        so a saved file reloads into the same table.

        :param path: Destination file path (``str`` or ``os.PathLike``).
        :raises EngineError: The file could not be written.
        """
        cdef bytes b = str(path).encode('utf-8')
        _check(swmm_water_age_save(_h(self._solver), b))

    def __repr__(self) -> str:
        try:
            return (f"<WaterAge enabled={self.enabled} "
                    f"overrides={len(self.node_overrides)}>")
        except Exception:
            return "<WaterAge (engine closed)>"
