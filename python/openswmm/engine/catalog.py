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
Machine-readable map of the Python engine API.

``catalog.json`` is generated from the stubs, Cython sources and C headers by
``python/scripts/gen_catalog.py``; CI fails when it is stale. It lists every
*target* reachable from a :class:`Solver` -- services such as ``forcing`` and
``surface2d.groundwater``, element kinds such as ``node`` and ``link`` and
their sub-views such as ``node.stats`` -- and every property and method on
them, with type, access, units, lifecycle phases and wrapped C symbols.

.. code-block:: python

    from openswmm.engine import catalog

    catalog.lookup("node.stats.max_depth")["units"]      # 'length'
    obj = catalog.resolve(solver, "node.stats", "J1")    # the live NodeStatsView
"""

from __future__ import annotations

import json
from functools import lru_cache
from pathlib import Path
from typing import Any

__all__ = ["load", "targets", "members", "lookup", "resolve"]

_PATH = Path(__file__).with_name("catalog.json")


@lru_cache(maxsize=1)
def load() -> dict[str, Any]:
    """Return the whole catalog (cached)."""
    data: dict[str, Any] = json.loads(_PATH.read_text(encoding="utf-8"))
    return data


@lru_cache(maxsize=1)
def _by_path() -> dict[str, dict[str, Any]]:
    return {m["path"]: m for m in load()["members"]}


def targets() -> dict[str, dict[str, Any]]:
    """``{target name: entry}``; element targets carry ``collection`` and ``key``."""
    entries: dict[str, dict[str, Any]] = load()["targets"]
    return entries


def members(target: str | None = None) -> list[dict[str, Any]]:
    """Members of one target, or of all targets."""
    return [m for m in load()["members"] if target is None or m["target"] == target]


def lookup(path: str) -> dict[str, Any]:
    """Return the member entry for a dotted *path* such as ``"node.depth"``."""
    try:
        return _by_path()[path]
    except KeyError:
        raise KeyError(f"{path!r} is not in the engine catalog") from None


def resolve(solver: Any, target: str, key: int | str | None = None) -> Any:
    """Return the live object behind *target* on *solver*.

    Element targets (``node``, ``link.pump``, ``species`` ...) need the element
    *key* (id or index). Standalone roots (``builder``, ``output``, ...) are not
    reachable from a solver and raise ``KeyError``.
    """
    entry = targets()[target]
    if "construct" in entry:
        raise KeyError(f"{target!r} is a standalone root; construct it directly")
    obj = solver
    for part in filter(None, entry["path"].split(".")):
        if part.endswith("[]"):
            if key is None:
                raise ValueError(f"{target!r} is an element target; pass its id or index")
            obj = getattr(obj, part[:-2])[key]
        else:
            obj = getattr(obj, part)
    return obj
