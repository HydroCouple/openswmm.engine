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
Initial quality (Pythonic v1 surface)
=====================================

:author: Caleb Buahin
:copyright: Copyright (c) 2026 Caleb Buahin
:license: Apache-2.0

``solver.initial_quality`` is the ``[INITIAL_QUALITY]`` table: the starting
concentration a named constituent holds in one node or one link at
``initialize()``. It is a **sequence of rows** indexed ``0..len-1`` in file
order. :meth:`InitialQuality.set` is an UPSERT keyed on
``(is_link, elem_index, constituent)`` — writing the same key twice edits the
row rather than duplicating it — and :meth:`InitialQuality.remove` deletes by
row index, after which every later row **shifts down by one**.

A constituent is either a ``[POLLUTANTS]`` pollutant name, or one of the two
reserved species: :attr:`InitialQuality.WATER_AGE` (``"__WATER_AGE__"``, value
in HOURS, signed) and :attr:`InitialQuality.TEMPERATURE`
(``"__TEMPERATURE__"``, value in degC). Pollutant values are concentrations in
that pollutant's own units and must be **non-negative**; the two reserved
species accept negative values.

Rows seed state at ``initialize()`` only, so mutation is confined to the
editable lifecycle states — ``BUILDING`` and ``OPENED``. Editing later raises
:class:`~openswmm.engine.LifecycleError`.

.. code-block:: python

    from openswmm.engine import Solver, InitialQuality

    with Solver("model.inp") as s:
        s.initial_quality.set("TSS", 25.0, node="J1")
        s.initial_quality.set("TSS", 12.0, link="C1")
        s.initial_quality.set(InitialQuality.WATER_AGE, -3.0, node="J1")
        s.initial_quality.set(InitialQuality.TEMPERATURE, 14.5, node="J1")
        for row in s.initial_quality:
            print(row.is_link, row.elem_index, row.constituent, row.value)
        s.initial_quality.remove(0)     # later rows shift down by one
"""

# cython: language_level=3

from collections.abc import Iterator
from typing import NamedTuple

from ._common cimport *
from ._enums import ErrorCode
from ._exceptions import BadHandleError


cdef inline SWMM_Engine _h(solver):
    return <SWMM_Engine><size_t>solver.handle


class InitialQualityEntry(NamedTuple):
    """One ``[INITIAL_QUALITY]`` row.

    :ivar is_link: ``True`` when ``elem_index`` addresses a link, ``False``
        when it addresses a node.
    :ivar elem_index: Zero-based node or link index, or ``-1`` when the row's
        element could not be resolved.
    :ivar constituent: A ``[POLLUTANTS]`` pollutant name, or the reserved
        ``"__WATER_AGE__"`` / ``"__TEMPERATURE__"`` species.
    :ivar value: Raw value — pollutant concentration in the pollutant's own
        units (non-negative), HOURS for ``"__WATER_AGE__"`` (signed), or degC
        for ``"__TEMPERATURE__"``.
    """

    is_link: bool
    elem_index: int
    constituent: str
    value: float


class InitialQuality:
    """``solver.initial_quality`` — the ``[INITIAL_QUALITY]`` row table.

    A sequence of :class:`InitialQualityEntry` rows in file order. Rows are
    flat snapshots, not live views: a row carries no identity beyond its
    ``(is_link, elem_index, constituent)`` key.
    """

    #: Reserved constituent name for water age. Its value is in HOURS and may
    #: be negative.
    WATER_AGE = "__WATER_AGE__"

    #: Reserved constituent name for temperature. Its value is in degC and may
    #: be negative.
    TEMPERATURE = "__TEMPERATURE__"

    def __init__(self, solver):
        self._solver = solver

    def __len__(self) -> int:
        """Number of ``[INITIAL_QUALITY]`` rows.

        :returns: The row count.
        :raises BadHandleError: The engine handle is invalid.
        """
        cdef int n = swmm_init_quality_count(_h(self._solver))
        if n < 0:
            raise BadHandleError(
                int(ErrorCode.BADHANDLE),
                "Invalid engine handle reading [INITIAL_QUALITY] count")
        return n

    def __iter__(self) -> Iterator[InitialQualityEntry]:
        cdef int n = len(self)
        for i in range(n):
            yield self[i]

    def __getitem__(self, int row_index) -> InitialQualityEntry:
        """Read one row by row index.

        :param row_index: Zero-based row position; negative indexes from the
            end, as for a list.
        :returns: An :class:`InitialQualityEntry` of
            ``(is_link, elem_index, constituent, value)``.
        :raises IndexError: *row_index* is out of range.
        :raises EngineError: The engine refused the read.
        """
        cdef int n = len(self)
        cdef int i = row_index + n if row_index < 0 else row_index
        if not (0 <= i < n):
            raise IndexError(row_index)
        cdef int is_link = 0
        cdef int elem_idx = -1
        cdef char buf[128]
        cdef double value = 0.0
        buf[0] = 0
        _check(swmm_init_quality_get(
            _h(self._solver), i, &is_link, &elem_idx, buf, 128, &value))
        return InitialQualityEntry(
            is_link != 0, elem_idx, buf.decode('utf-8'), value)

    def set(self, str constituent, double value, *, node=None,
            link=None) -> None:
        """Upsert the initial value of *constituent* in one node or one link.

        The row key is ``(is_link, elem_index, constituent)``: an existing row
        with that key is updated in place, otherwise a new row is appended.
        Exactly one of *node* or *link* must be given.

        Rows only seed state at ``initialize()``, so this is a
        ``BUILDING``/``OPENED``-only edit — the same contract as
        ``solver.pollutants[...].initial_concentration``.

        :param constituent: A ``[POLLUTANTS]`` pollutant name, or
            :attr:`WATER_AGE` / :attr:`TEMPERATURE`.
        :param value: Pollutant concentration in the pollutant's own units
            (must be **non-negative**); HOURS for :attr:`WATER_AGE` (signed);
            degC for :attr:`TEMPERATURE` (signed).
        :param node: ``int`` node index or ``str`` node id. Mutually
            exclusive with *link*.
        :param link: ``int`` link index or ``str`` link id. Mutually
            exclusive with *node*.
        :raises ValueError: Neither or both of *node* and *link* were given.
        :raises KeyError: No node/link with that id exists.
        :raises BadParamError: *constituent* names no pollutant or reserved
            species, the element index is bad, or a pollutant *value* is
            negative.
        :raises LifecycleError: The engine is past ``OPENED``.
        """
        cdef int is_link
        cdef int elem
        cdef bytes b = constituent.encode('utf-8')
        if (node is None) == (link is None):
            raise ValueError(
                "Pass exactly one of node= or link= to identify the element")
        if node is not None:
            is_link = 0
            elem = _resolve_index(
                _h(self._solver), node, swmm_node_index, swmm_node_count,
                "Node")
        else:
            is_link = 1
            elem = _resolve_index(
                _h(self._solver), link, swmm_link_index, swmm_link_count,
                "Link")
        _check(swmm_init_quality_set(
            _h(self._solver), is_link, elem, b, value))
        self._solver._bump_generation()

    def remove(self, int row_index) -> None:
        """Remove the row at *row_index*.

        Every later row **shifts down by one**, so a caller holding cached row
        indices must re-enumerate. ``BUILDING``/``OPENED`` only.

        :param row_index: Zero-based row position.
        :raises BadIndexError: *row_index* is out of range.
        :raises LifecycleError: The engine is past ``OPENED``.
        """
        _check(swmm_init_quality_remove(_h(self._solver), row_index))
        self._solver._bump_generation()

    def __repr__(self) -> str:
        try:
            return f"<InitialQuality n={len(self)}>"
        except Exception:
            return "<InitialQuality (engine closed)>"
