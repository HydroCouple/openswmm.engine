#!/usr/bin/env python3
"""Reliable class-level MCP parity: which public binding classes are
referenced anywhere in the openswmm.mcp source tree.

The fuzzy C<->MCP matcher in build_matrix.py is advisory (it produced 358
false py-gaps). MCP tool modules import binding classes directly
(``from openswmm.engine import Links``), so a class-name grep over the MCP
source is an authoritative signal for whether a capability is surfaced.
"""
from __future__ import annotations

import csv
import json
import re
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
ENGINE_DIR = REPO / "python" / "openswmm" / "engine"
MCP_SRC = REPO.parent / "openswmm.mcp" / "src" / "openswmm_mcp"
OUT = HERE / "out"

CLASS_RE = re.compile(r"^\s*(?:cdef\s+)?class\s+(\w+)")


def public_classes() -> dict[str, str]:
    classes: dict[str, str] = {}
    for pyx in sorted(ENGINE_DIR.glob("_*.pyx")):
        for line in pyx.read_text(encoding="utf-8", errors="replace").splitlines():
            m = CLASS_RE.match(line)
            if m and not m.group(1).startswith("_"):
                classes.setdefault(m.group(1), pyx.stem)
    return classes


def main() -> int:
    classes = public_classes()
    blob = "\n".join(p.read_text(encoding="utf-8", errors="replace")
                     for p in MCP_SRC.rglob("*.py"))
    rows = []
    for cls, mod in sorted(classes.items()):
        ref = bool(re.search(rf"\b{re.escape(cls)}\b", blob))
        rows.append(dict(cls=cls, module=mod, referenced_in_mcp=ref))
    with (OUT / "mcp_class_coverage.csv").open("w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=["cls", "module", "referenced_in_mcp"])
        w.writeheader()
        w.writerows(rows)
    unref = [r["cls"] for r in rows if not r["referenced_in_mcp"]]
    summary = dict(n_classes=len(rows), n_unreferenced=len(unref),
                   unreferenced=unref)
    (OUT / "mcp_class_summary.json").write_text(json.dumps(summary, indent=2))
    print(f"public binding classes: {len(rows)}")
    print(f"referenced in MCP src:  {len(rows) - len(unref)}")
    print(f"NOT referenced in MCP:  {len(unref)}")
    for c in unref:
        print(f"  - {c}  ({dict((r['cls'], r['module']) for r in rows)[c]})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
