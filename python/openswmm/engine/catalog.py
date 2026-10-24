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

Paths are a compatibility contract. Paths through a shared view resolve to the
view's target (``surface2d.groundwater.cells`` is ``groundwater2d.cells``).
``revision()`` increases whenever a path is renamed or removed; renamed paths
keep working through :func:`lookup`, :func:`resolve` and :func:`members` with a
``DeprecationWarning``, and removed ones fail with the reason. Consumers state
the revision they were written against with :func:`require_revision`.
"""

from __future__ import annotations

import json
import warnings
from functools import lru_cache
from pathlib import Path
from typing import Any

__all__ = [
    "load", "targets", "members", "lookup", "resolve", "unit_label", "UNIT_KINDS",
    "revision", "canonical", "require_revision",
]

_PATH = Path(__file__).with_name("catalog.json")


@lru_cache(maxsize=1)
def load() -> dict[str, Any]:
    """Return the whole catalog (cached)."""
    data: dict[str, Any] = json.loads(_PATH.read_text(encoding="utf-8"))
    return data


@lru_cache(maxsize=1)
def _by_path() -> dict[str, dict[str, Any]]:
    return {m["path"]: m for m in load()["members"]}


def revision() -> int:
    """Catalog revision; it increases whenever a path is renamed or removed."""
    return int(load().get("revision", 1))


def require_revision(minimum: int, consumer: str = "this package") -> None:
    """Raise ``RuntimeError`` when the installed catalog is older than *minimum*."""
    if revision() < minimum:
        raise RuntimeError(
            f"{consumer} needs openswmm engine catalog revision {minimum} or newer; the "
            f"installed openswmm has revision {revision()}. Upgrade or rebuild openswmm."
        )


def renamed_path(path: str, renamed: dict[str, str]) -> str | None:
    """*path* with its longest renamed prefix replaced, or ``None`` if none applies."""
    best = max((old for old in renamed if path == old or path.startswith(old + ".")),
               key=len, default=None)
    return None if best is None else renamed[best] + path[len(best):]


def views_of(catalog: dict[str, Any]) -> dict[str, str]:
    """Object-valued properties that expose another target: ``{path: view_target}``."""
    return {m["path"]: m["view_target"] for m in catalog["members"] if "view_target" in m}


def current_path(path: str, catalog: dict[str, Any],
                 views: dict[str, str] | None = None) -> tuple[str, bool]:
    """Spell *path* against *catalog*: ``(current path, whether a rename applied)``.

    Renamed prefixes are replaced first; then a prefix that is a shared view
    (``surface2d.groundwater`` exposes ``groundwater2d``) is replaced by its
    target, so ``surface2d.groundwater.cells`` reads ``groundwater2d.cells``.
    """
    new = renamed_path(path, catalog.get("renamed", {}))
    was_renamed = new is not None
    path = new if was_renamed else path
    views = views_of(catalog) if views is None else views
    for _ in range(8):  # views can chain (coupling.groundwater -> coupling.surface)
        parts = path.split(".")
        for i in range(len(parts), 0, -1):
            target = views.get(".".join(parts[:i]))
            if target is not None:
                rest = ".".join(parts[i:])
                path = (rest if target == "solver" else f"{target}.{rest}") if rest else target
                break
        else:
            break
    return path, was_renamed


@lru_cache(maxsize=1)
def _views() -> dict[str, str]:
    return views_of(load())


_WARNED: set[str] = set()


def canonical(path: str) -> str:
    """The current spelling of a target or member *path*.

    Shared views resolve silently; renamed paths warn once each with a
    ``DeprecationWarning``.
    """
    current, was_renamed = current_path(path, load(), _views())
    if was_renamed and path not in _WARNED:
        _WARNED.add(path)
        warnings.warn(f"engine catalog path {path!r} is now {current!r}",
                      DeprecationWarning, stacklevel=3)
    return current


def _removed_reason(path: str) -> str | None:
    for prefix, reason in load().get("removed", {}).items():
        if path == prefix or path.startswith(prefix + "."):
            return str(reason)
    return None


def targets() -> dict[str, dict[str, Any]]:
    """``{target name: entry}``; element targets carry ``collection`` and ``key``."""
    entries: dict[str, dict[str, Any]] = load()["targets"]
    return entries


def members(target: str | None = None) -> list[dict[str, Any]]:
    """Members of one target, or of all targets."""
    if target is not None and target not in targets():
        target = canonical(target)
    return [m for m in load()["members"] if target is None or m["target"] == target]


def lookup(path: str) -> dict[str, Any]:
    """Return the member entry for a dotted *path* such as ``"node.depth"``."""
    entry = _by_path().get(path) or _by_path().get(canonical(path))
    if entry is not None:
        return entry
    reason = _removed_reason(path)
    suffix = f" (removed: {reason})" if reason else ""
    raise KeyError(f"{path!r} is not in the engine catalog{suffix}")


def resolve(solver: Any, target: str, key: int | str | None = None) -> Any:
    """Return the live object behind *target* on *solver*.

    Element targets (``node``, ``link.pump``, ``species`` ...) need the element
    *key* (id or index). Standalone roots (``builder``, ``output``, ...) are not
    reachable from a solver and raise ``KeyError``.
    """
    if target not in targets():
        target = canonical(target)
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


# Unit kinds (a member's ``units``) that are not literal units, and their labels.
_FIXED = {
    "dimensionless": "dimensionless",
    "fraction": "fraction",
    "percent": "%",
    "count": "count",
    "manning_n": "Manning's n",
    "concentration": "pollutant concentration units",
    "user_defined": "units implied by the model's expressions",
    "shape_dependent": "depends on the cross-section shape (see xsect.shape)",
    "unverified": "stored as given; the engine applies no unit conversion",
    "datetime": "DateTime (decimal days)",
}
_BY_SYSTEM = {  # kind: (US label, SI label)
    "length": ("ft", "m"),
    "area": ("ft2", "m2"),
    "land_area": ("ac", "ha"),
    "volume": ("ft3", "m3"),
    "velocity": ("ft/s", "m/s"),
    "rain_rate": ("in/hr", "mm/hr"),
    "rain_depth": ("in", "mm"),
    "evap_rate": ("in/day", "mm/day"),
    "temperature": ("degF", "degC"),
    "user_temperature": ("degF", "degC"),
    "wind_speed": ("mph", "km/hr"),
    "length2/s": ("ft2/s", "m2/s"),
    "weir_coeff": ("ft^0.5/s", "m^0.5/s"),
    "section_factor": ("ft^8/3", "m^8/3"),
}
#: Every symbolic unit kind; any other ``units`` value is a literal unit ("m3/s", "s").
UNIT_KINDS = frozenset(_FIXED) | frozenset(_BY_SYSTEM) | {"flow"}


def unit_label(
    kind: str | None, unit_system: str | None = None, flow_units: Any = None
) -> str | None:
    """Human-readable units for a catalog unit kind in a model's unit system.

    ``flow`` takes the model's flow units, as a token (``"CFS"``, ``"CMS"``
    ...) or as ``Solver.flow_units`` itself; other system-dependent kinds take
    ``"US"`` or ``"SI"`` and come back unchanged without one; literal units
    pass through.

    .. code-block:: python

        catalog.unit_label(catalog.lookup("link.length")["units"], "SI")   # 'm'
    """
    if kind is None or kind in _FIXED:
        return _FIXED.get(kind) if kind else None
    if kind == "flow":
        if flow_units is None or flow_units == "":
            return kind
        # A FlowUnits member (CFS is 0, so falsy) labels by its name.
        return str(getattr(flow_units, "name", flow_units))
    if kind in _BY_SYSTEM and unit_system in ("US", "SI"):
        return _BY_SYSTEM[kind][unit_system == "SI"]
    return kind
