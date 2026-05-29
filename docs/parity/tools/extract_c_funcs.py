#!/usr/bin/env python3
"""Extract exported C API functions from openswmm_*.h headers.

Reads every `include/openswmm/engine/openswmm_*.h` (excluding `*_export.h`) and
emits a TSV with one row per exported function:

    domain<TAB>function<TAB>header<TAB>signature

`domain` is the header stem after stripping the `openswmm_` prefix (e.g.
`nodes`, `links`, `2d`). `function` is the C symbol name. `signature` is the
full collapsed declaration line(s) ending in `;`.

Detection rule: any line beginning with `SWMM_ENGINE_API` (the export macro
defined in `openswmm_engine_export.h`). Multi-line declarations are joined.

Output goes to stdout; redirect to `c_funcs.tsv`.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]
HEADER_DIR = REPO_ROOT / "include" / "openswmm" / "engine"

EXPORT_PREFIX = "SWMM_ENGINE_API"
FUNC_RE = re.compile(r"\b(swmm_[a-z0-9_]+)\s*\(")


def collapse_decl(lines: list[str], start: int) -> tuple[str, int]:
    """Join multi-line declaration starting at `lines[start]` until ';' seen.

    Returns (collapsed_signature, index_of_terminating_line).
    """
    buf: list[str] = []
    i = start
    while i < len(lines):
        line = lines[i].rstrip()
        buf.append(line)
        if ";" in line:
            break
        i += 1
    signature = re.sub(r"\s+", " ", " ".join(buf)).strip()
    return signature, i


def extract_from_header(path: Path) -> list[tuple[str, str]]:
    """Return list of (function_name, signature) from one header."""
    out: list[tuple[str, str]] = []
    text = path.read_text(encoding="utf-8", errors="replace")
    lines = text.splitlines()
    i = 0
    while i < len(lines):
        line = lines[i].lstrip()
        if line.startswith(EXPORT_PREFIX):
            sig, end = collapse_decl(lines, i)
            m = FUNC_RE.search(sig)
            if m:
                out.append((m.group(1), sig))
            i = end + 1
        else:
            i += 1
    return out


def domain_for(header: Path) -> str:
    stem = header.stem
    if stem.startswith("openswmm_"):
        stem = stem[len("openswmm_"):]
    return stem


def main(argv: list[str]) -> int:
    if not HEADER_DIR.is_dir():
        print(f"error: header dir not found: {HEADER_DIR}", file=sys.stderr)
        return 2

    headers = sorted(
        p for p in HEADER_DIR.glob("openswmm_*.h")
        if not p.name.endswith("_export.h")
    )
    if not headers:
        print(f"error: no openswmm_*.h headers found under {HEADER_DIR}",
              file=sys.stderr)
        return 2

    rows: list[tuple[str, str, str, str]] = []
    seen: set[tuple[str, str]] = set()
    for header in headers:
        domain = domain_for(header)
        for fn_name, sig in extract_from_header(header):
            key = (domain, fn_name)
            if key in seen:
                continue
            seen.add(key)
            rows.append((domain, fn_name, header.name, sig))

    rows.sort()
    print("domain\tfunction\theader\tsignature")
    for row in rows:
        print("\t".join(row))

    print(f"# {len(rows)} functions across {len(headers)} headers",
          file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
