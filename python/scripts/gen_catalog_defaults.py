"""Generate ``catalog_defaults.json``: engine defaults, validation behavior and enum members per
catalog property, by running the engine.

The API catalog (``gen_catalog.py``) describes every property's type, unit kind and access but
carries no default value, range or enum member list, which a generated property editor needs.
This script builds a bare model through ``ModelBuilder`` (one object of every node and link
type, one subcatchment, one gage; nothing else set), adds a pollutant and a land use on the
solver, reads every read-write property's value, then probes the setter with ``-1`` and ``0``
on a fresh copy to learn which values the engine refuses and with what message. Enum members
come from the engine's enum classes.

Output (next to ``catalog.json``)::

    {"schema": 1, "catalog_revision": N, "engine_version": "...", "model_units": "CMS",
     "properties": {"<path>": {"default": value, "enum": "EnumName" | null, "probe": "Class.attr",
                               "rejects": {"-1": "message" | null, "0": ...}}},
     "enums": {"EnumName": {"MEMBER": value, ...}}}

Defaults are in the engine's user units for the model's unit system (CMS). Properties whose
target has no live object in the bare model (reaction terms, MSX species, the 2D surface)
carry ``"default": null`` and ``"missing": "<reason>"`` so the gap stays visible.

    python python/scripts/gen_catalog_defaults.py          # rewrite catalog_defaults.json
    python python/scripts/gen_catalog_defaults.py --check  # fail if stale
"""
from __future__ import annotations

import argparse
import enum
import importlib.metadata
import json
import math
import sys
from pathlib import Path
from typing import Any

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "python"))

import openswmm.engine as engine  # noqa: E402
from openswmm.engine import catalog  # noqa: E402

OUT_JSON = REPO / "python" / "openswmm" / "engine" / "catalog_defaults.json"

NODE_TYPES = ("JUNCTION", "OUTFALL", "STORAGE", "DIVIDER")
LINK_TYPES = ("CONDUIT", "PUMP", "ORIFICE", "WEIR", "OUTLET")
#: Element key per element target in the bare model; collections without a builder call are
#: filled through ``<collection>.add(key)`` on the solver.
KEYS = {"node": "N_JUNCTION", "node.storage": "N_STORAGE", "node.outfall": "N_OUTFALL",
        "node.divider": "N_DIVIDER", "link": "L_CONDUIT", "link.pump": "L_PUMP",
        "link.orifice": "L_ORIFICE", "link.weir": "L_WEIR", "link.outlet": "L_OUTLET",
        "subcatchment": "S1", "gage": "G1", "pollutant": "P1", "landuse": "LU1"}
TEXT_TYPES = ("str", "Optional[str]", "datetime", "XSection")


def enum_classes() -> dict[str, type[enum.Enum]]:
    found: dict[str, type[enum.Enum]] = {}
    for module in (engine, getattr(engine, "_enums", None)):
        if module is None:
            continue
        for name in dir(module):
            obj = getattr(module, name)
            if isinstance(obj, type) and issubclass(obj, enum.Enum) and obj is not enum.Enum:
                found[name] = obj
    return found


def build_model(classes: dict[str, type[enum.Enum]]) -> Any:
    """A bare model: every object kind present, nothing set beyond topology."""
    b = engine.ModelBuilder()
    b.set_option("FLOW_UNITS", "CMS")
    b.add_gage("G1")
    nodes = {kind: b.add_node("N_" + kind, classes["NodeType"][kind]) for kind in NODE_TYPES}
    end = b.add_node("N_END", classes["NodeType"]["JUNCTION"])
    for kind, upstream in zip(LINK_TYPES, ("JUNCTION", "STORAGE", "DIVIDER", "JUNCTION", "STORAGE")):
        link = b.add_link("L_" + kind, classes["LinkType"][kind])
        b.set_link_nodes(link, nodes[upstream], end)
    b.add_subcatch("S1")
    return b


def open_solver(classes: dict[str, type[enum.Enum]]) -> Any:
    solver = build_model(classes).to_solver()
    solver.pollutants.add(KEYS["pollutant"])
    collection_of(solver, "landuse").add(KEYS["landuse"])
    return solver


def collection_of(solver: Any, target: str) -> Any:
    """The collection object that holds *target*'s elements (the part before ``[]``)."""
    path = catalog.targets()[target]["path"]
    obj = solver
    for part in filter(None, path.split(".")):
        if part.endswith("[]"):
            return getattr(obj, part[:-2])
        obj = getattr(obj, part)
    raise KeyError(f"{target!r} is not an element target")


def to_jsonable(value: Any) -> Any:
    if isinstance(value, enum.Enum):
        return value.name
    if isinstance(value, float):
        return None if math.isnan(value) else value
    if isinstance(value, (list, tuple)):
        return [to_jsonable(v) for v in value]
    if isinstance(value, (int, str, bool)) or value is None:
        return value
    try:
        return value.isoformat()
    except AttributeError:
        return repr(value)


def live_object(solver: Any, builder: Any, target: str) -> Any:
    if target == "builder":
        return builder
    return catalog.resolve(solver, target, KEYS.get(target))


def probe_setter(classes: dict[str, type[enum.Enum]], member: dict[str, Any]) -> dict[str, str | None]:
    """Try ``-1`` and ``0`` on a fresh solver; the message the engine gives is the finding."""
    rejects: dict[str, str | None] = {}
    for probe in (-1, 0):
        solver = open_solver(classes)
        try:
            setattr(live_object(solver, None, member["target"]), member["name"], probe)
            rejects[str(probe)] = None
        except Exception as ex:  # noqa: BLE001 - the refusal is the data
            rejects[str(probe)] = f"{type(ex).__name__}: {ex}"[:160]
        finally:
            close_quietly(solver)
    return rejects


def close_quietly(solver: Any) -> None:
    for name in ("close", "destroy"):
        try:
            getattr(solver, name)()
        except Exception:  # noqa: BLE001
            pass


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--check", action="store_true", help="fail if catalog_defaults.json is stale")
    ap.add_argument("--no-probe", action="store_true", help="skip the -1 / 0 setter probes")
    args = ap.parse_args(argv)

    classes = enum_classes()
    cat = catalog.load()
    rw = [m for m in cat["members"] if m["form"] == "property" and m.get("access") == "rw"]
    builder = engine.ModelBuilder()
    solver = open_solver(classes)
    properties: dict[str, dict[str, Any]] = {}
    for m in rw:
        entry: dict[str, Any] = {"default": None, "enum": None, "probe": m.get("python")}
        type_name = m.get("type") or ""
        if type_name in classes:
            entry["enum"] = type_name
        try:
            obj = live_object(solver, builder, m["target"])
            key = KEYS.get(m["target"])
            if key is not None and getattr(obj, "id", key) != key:
                raise RuntimeError(f"resolved {getattr(obj, 'id')!r} for key {key!r}")
            entry["default"] = to_jsonable(getattr(obj, m["name"]))
        except Exception as ex:  # noqa: BLE001 - the gap itself is the finding
            entry["missing"] = f"{type(ex).__name__}: {ex}"[:160]
        properties[m["path"]] = entry
    close_quietly(solver)

    if not args.no_probe:
        for m in rw:
            entry = properties[m["path"]]
            type_name = m.get("type") or ""
            if "missing" in entry or entry["enum"] or m["target"] == "builder" \
                    or type_name in TEXT_TYPES or type_name.startswith(("List", "Tuple", "NDArray")):
                continue
            entry["rejects"] = probe_setter(classes, m)

    out = {
        "schema": 1,
        "catalog_revision": cat.get("revision"),
        "engine_version": importlib.metadata.version("openswmm"),
        "model_units": "CMS",
        "properties": properties,
        "enums": {name: {member.name: to_jsonable(member.value) for member in cls}
                  for name, cls in sorted(classes.items())},
    }
    text = json.dumps(out, indent=1, ensure_ascii=False) + "\n"
    missing = [p for p, e in properties.items() if "missing" in e]
    print(f"properties={len(properties)} with defaults={len(properties) - len(missing)} "
          f"missing={len(missing)} enums={len(out['enums'])}")
    if args.check:
        current = OUT_JSON.read_text(encoding="utf-8") if OUT_JSON.exists() else ""
        if current != text:
            print(f"ERROR: {OUT_JSON.relative_to(REPO)} is stale; run python/scripts/gen_catalog_defaults.py",
                  file=sys.stderr)
            return 1
        return 0
    OUT_JSON.write_text(text, encoding="utf-8")
    print(f"wrote {OUT_JSON.relative_to(REPO)} ({len(text) // 1024} KiB)")
    if missing:
        print("no live object for: " + ", ".join(missing))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
