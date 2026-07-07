#!/usr/bin/env python3
"""Audit drift between the OpenSWMM C engine public API and the Cython bindings.

Compares the C functions/enums exported from ``include/openswmm/engine/*.h``
against the ``cdef`` declarations in the Cython ``.pxd`` files and the
``IntEnum`` mirrors in ``_enums.py``.

Three reports are produced:

* **functions** -- ``SWMM_ENGINE_API`` C functions not declared in any ``.pxd``.
* **enums**     -- ``typedef enum SWMM_*`` types with no Python ``IntEnum``.
* **surfaced**  -- declared-in-pxd functions never referenced from a ``.pyx``
                   (a weaker signal -- may be wrapped via helper/bulk paths).

Run from anywhere::

    python python/scripts/api_drift_audit.py            # human summary
    python python/scripts/api_drift_audit.py --json      # machine readable
    python python/scripts/api_drift_audit.py --strict    # exit 1 if any gaps

This is the canonical "A1" tool referenced by the API-update plan; re-run it
after wrapping work to confirm the gap set has shrunk to the intentional set.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

# repo root = two levels up from this file (python/scripts/ -> python/ -> repo)
REPO = Path(__file__).resolve().parents[2]
HEADERS = REPO / "include" / "openswmm" / "engine"
ENGINE = REPO / "python" / "openswmm" / "engine"

# Internal/impl headers carry no public ABI -- exclude from the audit.
HEADER_EXCLUDE = {
    "openswmm_engine_impl.h",
    "openswmm_engine_internal.h",
    "openswmm_engine_export.h",
}

# C function: anything tagged SWMM_ENGINE_API, function name is the identifier
# immediately preceding the opening paren (declaration may span lines).
_FUNC_RE = re.compile(
    r"SWMM_ENGINE_API\b[^;{]*?\b(swmm_[A-Za-z0-9_]+)\s*\(",
    re.DOTALL,
)
_ENUM_RE = re.compile(r"typedef\s+enum\s+(SWMM_[A-Za-z0-9_]+)")
# pxd / pyx: any swmm_* token that is being called/declared (followed by paren).
_PXD_FUNC_RE = re.compile(r"\b(swmm_[A-Za-z0-9_]+)\s*\(")
_PY_ENUM_RE = re.compile(r"^class\s+([A-Za-z0-9_]+)\s*\(\s*IntEnum\s*\)", re.M)


def _read(p: Path) -> str:
    return p.read_text(encoding="utf-8", errors="replace")


def collect_c_functions() -> dict[str, str]:
    """Return {function_name: header_basename} for every public C function."""
    funcs: dict[str, str] = {}
    for h in sorted(HEADERS.glob("openswmm_*.h")):
        if h.name in HEADER_EXCLUDE:
            continue
        text = _read(h)
        for m in _FUNC_RE.finditer(text):
            funcs.setdefault(m.group(1), h.name)
    return funcs


def collect_c_enums() -> dict[str, str]:
    enums: dict[str, str] = {}
    for h in sorted(HEADERS.glob("openswmm_*.h")):
        if h.name in HEADER_EXCLUDE:
            continue
        for m in _ENUM_RE.finditer(_read(h)):
            enums.setdefault(m.group(1), h.name)
    return enums


def collect_pxd_functions() -> set[str]:
    """Functions declared in a .pxd OR via a `cdef extern` block inside a .pyx.

    Cython lets you put `cdef extern from "header.h"` declarations directly in a
    .pyx (the geopackage and 2D modules do this), so a function declared there
    is wrapped even though it never appears in a .pxd.
    """
    names: set[str] = set()
    for src in list(ENGINE.glob("*.pxd")) + list(ENGINE.glob("*.pyx")):
        for m in _PXD_FUNC_RE.finditer(_read(src)):
            names.add(m.group(1))
    return names


def collect_pyx_tokens() -> set[str]:
    blob = "\n".join(_read(p) for p in ENGINE.glob("*.pyx"))
    return set(_PXD_FUNC_RE.findall(blob)) | set(
        re.findall(r"\b(swmm_[A-Za-z0-9_]+)\b", blob)
    )


def collect_py_enums() -> set[str]:
    text = _read(ENGINE / "_enums.py")
    # normalise to the SWMM_<Name> form for comparison (strip the SWMM_ prefix
    # from the C side, compare on the bare CamelCase tail).
    return set(_PY_ENUM_RE.findall(text))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--json", action="store_true", help="emit JSON")
    ap.add_argument("--out", help="write JSON report to this path instead of stdout")
    ap.add_argument("--strict", action="store_true", help="exit 1 if gaps found")
    args = ap.parse_args()

    c_funcs = collect_c_functions()
    pxd = collect_pxd_functions()
    pyx = collect_pyx_tokens()
    c_enums = collect_c_enums()
    py_enums = collect_py_enums()

    missing_decl = {fn: hdr for fn, hdr in c_funcs.items() if fn not in pxd}
    declared_unsurfaced = sorted(
        fn for fn in (set(c_funcs) & pxd) if fn not in pyx
    )
    # enum compare: C "SWMM_ForcingType" -> tail "ForcingType"
    enum_tails = {name: name[len("SWMM_"):] for name in c_enums}
    missing_enums = {
        name: c_enums[name]
        for name, tail in enum_tails.items()
        if tail not in py_enums
    }

    report = {
        "totals": {
            "c_functions": len(c_funcs),
            "pxd_declared": len(pxd),
            "c_enums": len(c_enums),
            "py_enums": len(py_enums),
        },
        "functions_missing_from_pxd": dict(sorted(missing_decl.items())),
        "declared_but_unsurfaced_in_pyx": declared_unsurfaced,
        "enums_missing_from_python": dict(sorted(missing_enums.items())),
    }

    if args.out:
        lines = []
        by_hdr: dict[str, list[str]] = {}
        for fn, hdr in missing_decl.items():
            by_hdr.setdefault(hdr, []).append(fn)
        lines.append("== per-header missing-from-pxd counts ==")
        for hdr in sorted(by_hdr):
            lines.append(f"{len(by_hdr[hdr]):4d}  {hdr}")
        lines.append("")
        lines.append("== missing functions EXCLUDING 2d + callbacks ==")
        for hdr in sorted(by_hdr):
            if hdr in ("openswmm_2d.h", "openswmm_callbacks.h"):
                continue
            for fn in sorted(by_hdr[hdr]):
                lines.append(f"{hdr}: {fn}")
        lines.append("")
        lines.append(f"== enums missing from python ({len(missing_enums)}) ==")
        for name, hdr in sorted(missing_enums.items()):
            lines.append(f"{name}  ({hdr})")
        lines.append("")
        lines.append("== totals ==")
        lines.append(f"c_functions={len(c_funcs)} pxd={len(pxd)} "
                     f"missing={len(missing_decl)} "
                     f"c_enums={len(c_enums)} py_enums={len(py_enums)}")
        Path(args.out).write_text("\n".join(lines) + "\n", encoding="utf-8")
    elif args.json:
        print(json.dumps(report, indent=2))
    else:
        t = report["totals"]
        print(f"C functions: {t['c_functions']}  |  pxd-declared: {t['pxd_declared']}")
        print(f"C enums: {t['c_enums']}  |  python IntEnums: {t['py_enums']}")
        print()
        print(f"== Functions in headers but MISSING from .pxd ({len(missing_decl)}) ==")
        by_hdr: dict[str, list[str]] = {}
        for fn, hdr in sorted(missing_decl.items()):
            by_hdr.setdefault(hdr, []).append(fn)
        for hdr in sorted(by_hdr):
            print(f"  [{hdr}]")
            for fn in by_hdr[hdr]:
                print(f"      {fn}")
        print()
        print(f"== Enums in headers but MISSING from _enums.py ({len(missing_enums)}) ==")
        for name, hdr in sorted(missing_enums.items()):
            print(f"  {name}   ({hdr})")
        print()
        print(f"== Declared in .pxd but NOT referenced in any .pyx ({len(declared_unsurfaced)}) ==")
        print("   (weak signal -- may be reached via bulk/helper paths)")
        for fn in declared_unsurfaced:
            print(f"      {fn}")

    gaps = bool(missing_decl or missing_enums)
    if args.strict and gaps:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
