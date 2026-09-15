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
Process components (Pythonic v1 surface)
========================================

:author: Caleb Buahin
:copyright: Copyright (c) 2026 Caleb Buahin
:license: Apache-2.0

``solver.process_components`` is the ``[PROCESS_COMPONENTS]`` registration
table — the file-binding surface a component editor uses to locate the config
file its section is stored in. Each registration carries an id, the
``config="…"`` argument **as written** (which may be empty), and the effective
path the config was actually READ from at the last open.

Registering a component whose config file **does not exist yet** is legal and
deliberate: the "create component + config file" flow registers first and
writes the file afterwards, and the path is resolved at the next open. Until
that resolution happens, :attr:`ProcessComponent.resolved` is ``""``.
Registering an id that is already registered is refused.

Registration and removal are ``BUILDING``/``OPENED`` edits, and removal
**shifts every later index down**, so cached indexes must be re-read.

.. code-block:: python

    from openswmm.engine import Solver

    with Solver("model.inp") as s:
        pc = s.process_components.register("reactions", "model.rxn")
        print(pc.index, pc.id, pc.config, pc.resolved)  # resolved is "" here
        if "reactions" in s.process_components:
            print(s.process_components["reactions"].config)
        for comp in s.process_components:
            print(comp.id, comp.config or "(none)")
        s.process_components.remove("reactions")    # later indexes shift
"""

# cython: language_level=3

from collections.abc import Iterator
from typing import NamedTuple

from ._common cimport *
from ._enums import ErrorCode
from ._exceptions import BadHandleError, ElementNotFoundError


cdef inline SWMM_Engine _h(solver):
    return <SWMM_Engine><size_t>solver.handle


class ProcessComponent(NamedTuple):
    """One ``[PROCESS_COMPONENTS]`` registration.

    :ivar component_index: Zero-based registration index. **Not stable across
        removals** — removing a registration shifts every later index down.
    :ivar id: The component id.
    :ivar config: The ``config="…"`` argument exactly as written in the deck.
        ``""`` when the registration names no config file.
    :ivar resolved: The effective path the config was READ from at the last
        open. ``""`` until the component has been resolved — including right
        after :meth:`ProcessComponents.register`, and for a registration whose
        config file does not exist yet.
    """

    component_index: int
    id: str
    config: str
    resolved: str


class ProcessComponents:
    """``solver.process_components`` — the ``[PROCESS_COMPONENTS]`` table.

    A sequence of :class:`ProcessComponent` rows, addressable by index or by
    id. Rows are flat snapshots, not live views.
    """

    def __init__(self, solver):
        self._solver = solver

    def __len__(self) -> int:
        """Number of ``[PROCESS_COMPONENTS]`` registrations.

        :returns: The registration count.
        :raises BadHandleError: The engine handle is invalid.
        """
        cdef int n = swmm_process_component_count(_h(self._solver))
        if n < 0:
            raise BadHandleError(
                int(ErrorCode.BADHANDLE),
                "Invalid engine handle reading [PROCESS_COMPONENTS] count")
        return n

    def __iter__(self) -> Iterator[ProcessComponent]:
        cdef int n = len(self)
        for i in range(n):
            yield self._get(i)

    def __getitem__(self, key) -> ProcessComponent:
        """Read one registration by index or by id.

        :param key: ``int`` registration index (negatives count from the end)
            or ``str`` component id.
        :returns: A :class:`ProcessComponent` of
            ``(index, id, config, resolved)``.
        :raises IndexError: An ``int`` *key* is out of range.
        :raises ElementNotFoundError: A ``str`` *key* names no registration.
        :raises TypeError: *key* is neither ``int`` nor ``str``.
        :raises EngineError: The engine refused the read.
        """
        return self._get(self._index_of(key))

    def __contains__(self, component_id) -> bool:
        """Is a component with this id registered?

        :param component_id: The component id to look for.
        :returns: ``True`` when a registration with that id exists.
        """
        if not isinstance(component_id, str):
            return False
        cdef bytes b = (<str>component_id).encode('utf-8')
        return swmm_process_component_find(_h(self._solver), b) >= 0

    def get_index(self, str component_id) -> int:
        """Return the registration index of *component_id*.

        :param component_id: The component id.
        :returns: The zero-based registration index. Valid only until the next
            removal, which shifts later indexes down.
        :raises ElementNotFoundError: No registration carries that id.
        """
        cdef bytes b = component_id.encode('utf-8')
        cdef int i = swmm_process_component_find(_h(self._solver), b)
        if i < 0:
            raise ElementNotFoundError(
                component_id,
                f"Process component '{component_id}' not found")
        return i

    def register(self, str component_id, str config_path="") -> ProcessComponent:
        """Register a process component and return the new row.

        ``BUILDING``/``OPENED`` only. *config_path* **need not exist yet** —
        the "create component, then write its config file" flow registers
        first and writes afterwards; the path is read at the next open, so the
        returned row's :attr:`~ProcessComponent.resolved` is ``""`` until
        then. A duplicate id is refused rather than overwritten.

        :param component_id: The component id to register.
        :param config_path: The ``config="…"`` path to record; ``""`` (the
            default) registers the component with no config file.
        :returns: The newly registered :class:`ProcessComponent`.
        :raises BadParamError: *component_id* is already registered, or is
            otherwise not a usable id.
        :raises LifecycleError: The engine is past ``OPENED``.
        """
        cdef bytes b_id = component_id.encode('utf-8')
        cdef bytes b_path = config_path.encode('utf-8')
        _check(swmm_process_component_register(
            _h(self._solver), b_id, b_path))
        self._solver._bump_generation()
        return self._get(self.get_index(component_id))

    def remove(self, key) -> None:
        """Remove one registration, by index or by id.

        ``BUILDING``/``OPENED`` only. Every later registration **shifts down
        by one**, so a caller holding cached indexes must re-enumerate. The
        config file itself is untouched — only the registration goes away.

        :param key: ``int`` registration index (negatives count from the end)
            or ``str`` component id.
        :raises IndexError: An ``int`` *key* is out of range.
        :raises ElementNotFoundError: A ``str`` *key* names no registration.
        :raises TypeError: *key* is neither ``int`` nor ``str``.
        :raises LifecycleError: The engine is past ``OPENED``.
        """
        cdef int idx = self._index_of(key)
        _check(swmm_process_component_remove(_h(self._solver), idx))
        self._solver._bump_generation()

    # ---- Internals ---------------------------------------------------

    def _index_of(self, key) -> int:
        """Resolve *key* (index or id) to a validated registration index."""
        cdef int n
        cdef int i
        if isinstance(key, str):
            return self.get_index(key)
        # ``bool`` is a subclass of ``int`` — reject it explicitly so
        # ``components[True]`` doesn't silently mean index 1.
        if isinstance(key, bool):
            raise TypeError(
                "Process component key must be int or str, got bool")
        if isinstance(key, int) or hasattr(key, "__index__"):
            n = len(self)
            i = key.__index__()
            if i < 0:
                i += n
            if not (0 <= i < n):
                raise IndexError(key)
            return i
        raise TypeError(
            "Process component key must be int or str, got "
            f"{type(key).__name__}")

    def _get(self, int idx) -> ProcessComponent:
        """Read registration *idx* into a :class:`ProcessComponent`."""
        cdef char id_buf[128]
        cdef char config_buf[512]
        cdef char resolved_buf[512]
        id_buf[0] = 0
        config_buf[0] = 0
        resolved_buf[0] = 0
        _check(swmm_process_component_get(
            _h(self._solver), idx, id_buf, 128, config_buf, 512,
            resolved_buf, 512))
        return ProcessComponent(
            idx, id_buf.decode('utf-8'), config_buf.decode('utf-8'),
            resolved_buf.decode('utf-8'))

    def __repr__(self) -> str:
        try:
            return f"<ProcessComponents n={len(self)}>"
        except Exception:
            return "<ProcessComponents (engine closed)>"
