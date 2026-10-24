#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
Public Python surface of ``openswmm.engine``, read from the ``.pyi`` stubs.

Writes ``{module: {"functions": [...], "classes": {Class: {"bases": [...],
"members": {name: kind}}}}}`` as JSON, where ``kind`` is ``method``,
``property`` (annotated attribute) or ``dunder``. Other language bindings
(the WebAssembly package) use this file as the member checklist for mirroring
the Python API.

``--check`` also verifies that every stub member is implemented: each
``.pyi`` class member must exist in the matching ``.pyx`` (``def``/``cpdef``,
``property x:``, ``cdef public``) or ``.py`` class, directly or through a base
class. Text only: no compiled extension is imported.

Usage::

    python3 scripts/python_surface.py                 # JSON to stdout
    python3 scripts/python_surface.py -o surface.json
    python3 scripts/python_surface.py --check
"""

from __future__ import annotations

import argparse
import ast
import json
import re
import sys
from pathlib import Path

ENGINE = Path(__file__).resolve().parents[1] / "openswmm" / "engine"

# Dunder methods that are part of the public protocol (len(), x[i], in, iteration,
# context management). Other dunders (__init__, __repr__, ...) are not mirrored.
PUBLIC_DUNDERS = {
    "__len__", "__getitem__", "__setitem__", "__delitem__", "__contains__",
    "__iter__", "__enter__", "__exit__", "__call__",
}  # fmt: skip


def _keep(name: str) -> bool:
    return not name.startswith("_") or name in PUBLIC_DUNDERS


def stub_surface(pyi: Path) -> dict:
    """Classes and module-level functions declared in one stub file."""
    tree = ast.parse(pyi.read_text(encoding="utf-8"), filename=str(pyi))
    functions, classes = [], {}
    for node in tree.body:
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)) and _keep(node.name):
            functions.append(node.name)
        elif isinstance(node, ast.ClassDef):
            members: dict[str, str] = {}
            for item in node.body:
                if isinstance(item, (ast.FunctionDef, ast.AsyncFunctionDef)) and _keep(item.name):
                    is_prop = any(
                        isinstance(d, ast.Name) and d.id == "property"
                        or isinstance(d, ast.Attribute) and d.attr in ("setter", "getter")
                        for d in item.decorator_list
                    )
                    kind = "dunder" if item.name.startswith("__") else (
                        "property" if is_prop else "method")
                    members.setdefault(item.name, kind)
                elif isinstance(item, ast.AnnAssign) and isinstance(item.target, ast.Name):
                    if _keep(item.target.id):
                        members[item.target.id] = "property"
            bases = [ast.unparse(b) for b in node.bases]
            classes[node.name] = {"bases": bases, "members": dict(sorted(members.items()))}
    return {"functions": sorted(set(functions)), "classes": classes}


_CLASS_RE = re.compile(r"^(?:cdef\s+)?class\s+(\w+)\s*(?:\(([^)]*)\))?\s*:", re.M)
_MEMBER_RES = [
    re.compile(r"^\s+(?:cpdef|def)\s+(?:[\w.\[\], *]+\s+)?(\w+)\s*\(", re.M),  # def / cpdef
    re.compile(r"^\s+property\s+(\w+)\s*:", re.M),  # Cython property blocks
    re.compile(r"^\s+cdef\s+(?:public|readonly)\s+[\w.\[\], *]+?\s+(\w+)\s*$", re.M),
    re.compile(r"^\s+(\w+)\s*(?::[^=\n]+)?=", re.M),  # class-level assignments
    re.compile(r"^ {4}(\w+)\s*:\s*[^=\n]+$", re.M),  # NamedTuple / dataclass fields
    re.compile(r"\bself\.(\w+)\s*(?::[^=\n]+)?=(?!=)", re.M),  # attributes set in methods
]


def impl_classes(src: Path) -> dict[str, tuple[list[str], set[str]]]:
    """Class name -> (bases, member names) from a .pyx or .py implementation."""
    text = src.read_text(encoding="utf-8")
    if src.suffix == ".py":
        tree = ast.parse(text, filename=str(src))
        out = {}
        for node in ast.walk(tree):
            if isinstance(node, ast.ClassDef):
                names = set()
                for item in node.body:
                    if isinstance(item, (ast.FunctionDef, ast.AsyncFunctionDef)):
                        names.add(item.name)
                    elif isinstance(item, ast.AnnAssign) and isinstance(item.target, ast.Name):
                        names.add(item.target.id)
                    elif isinstance(item, ast.Assign):
                        names |= {t.id for t in item.targets if isinstance(t, ast.Name)}
                out[node.name] = ([ast.unparse(b) for b in node.bases], names)
        return out
    out = {}
    starts = list(_CLASS_RE.finditer(text))
    for i, m in enumerate(starts):
        body = text[m.end(): starts[i + 1].start() if i + 1 < len(starts) else len(text)]
        # The class body ends at the next top-level (column-0) statement.
        end = re.search(r"^\S", body, re.M)
        body = body[: end.start()] if end else body
        names = set()
        for rx in _MEMBER_RES:
            names |= set(rx.findall(body))
        bases = [b.strip() for b in (m.group(2) or "").split(",") if b.strip()]
        out[m.group(1)] = (bases, names)
    return out


def collect(engine: Path = ENGINE) -> dict:
    return {pyi.stem: stub_surface(pyi) for pyi in sorted(engine.glob("*.pyi"))
            if pyi.stem != "__init__"}


def check(engine: Path = ENGINE) -> list[str]:
    """Stub members with no implementation, as 'module.Class.member' strings."""
    surface = collect(engine)
    impls: dict[str, tuple[list[str], set[str]]] = {}
    for src in sorted(engine.glob("*.pyx")) + sorted(engine.glob("*.py")):
        for name, val in impl_classes(src).items():
            impls.setdefault(name, val)

    def implemented(cls: str, member: str, seen: frozenset = frozenset()) -> bool:
        if cls in seen or cls not in impls:
            return False
        bases, names = impls[cls]
        if member in names:
            return True
        return any(implemented(b.split(".")[-1].split("[")[0], member, seen | {cls})
                   for b in bases)

    missing = []
    for module, info in surface.items():
        for cls, spec in info["classes"].items():
            if cls not in impls:
                # Protocol/NamedTuple/TypedDict-style stub classes with no runtime class body.
                continue
            for member in spec["members"]:
                if not implemented(cls, member):
                    missing.append(f"{module}.{cls}.{member}")
    return missing


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--engine-dir", type=Path, default=ENGINE)
    ap.add_argument("-o", "--output", type=Path)
    ap.add_argument("--check", action="store_true")
    a = ap.parse_args()
    if a.check:
        missing = check(a.engine_dir)
        for m in missing:
            print(f"python_surface: stub member not implemented: {m}", file=sys.stderr)
        n = sum(len(c["members"]) for i in collect(a.engine_dir).values()
                for c in i["classes"].values())
        print(f"python_surface: {n} stub members, {len(missing)} unimplemented")
        return 1 if missing else 0
    text = json.dumps(collect(a.engine_dir), indent=1, sort_keys=True) + "\n"
    if a.output:
        a.output.write_text(text, encoding="utf-8")
    else:
        sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
