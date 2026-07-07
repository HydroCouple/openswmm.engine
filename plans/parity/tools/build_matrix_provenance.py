#!/usr/bin/env python3
"""Call-graph *provenance* parity matrix — the low-false-positive successor
to the fuzzy ``build_matrix.py``.

Motivation
----------
The fuzzy matcher (``build_matrix.py``) matches C functions to Python methods
by guessing name variants (verb-object reversal, ``get_``/``set_`` promotion,
etc.). On the 2026-07-06 tree it produced **358 "py-gap" rows that are all
false** — every one of those C functions *is* wrapped, the matcher just
couldn't guess the Python name. That makes its py-gap / mcp-gap counts
unusable as a drift signal.

This builder derives the mapping from the **actual call graph** instead of
name similarity, so there are essentially no false positives:

  * C -> Python is *exact*: a Cython wrapper must call the C symbol by name,
    so we parse each ``.pyx`` method body for ``swmm_*`` calls and attribute
    them to the enclosing ``def``. Inverting gives, per C symbol, the exact
    wrapping method(s).
  * Python -> MCP is *name-precise*: MCP tool bodies call binding methods by
    attribute (``session.nodes.get_depth(...)``). We collect attribute names
    used in each MCP tool and mark a C symbol MCP-covered when one of its
    (call-graph-derived) wrapping methods is referenced by name — restricted
    to names that are actually binding methods, which removes the fuzzy
    matcher's guesswork.

Outputs ``provenance_matrix.md`` and ``provenance_gaps.json`` next to the
fuzzy artefacts (additive — it does not touch ``parity_matrix.md``).

``--check`` exits non-zero if any C symbol has **no** Python wrapper and is
not an intentional non-exposure (the real, reliable drift guard).
"""
from __future__ import annotations

import argparse
import ast
import json
import re
import sys
from collections import defaultdict
from pathlib import Path

PARITY_DIR = Path(__file__).resolve().parents[1]
REPO_ROOT = PARITY_DIR.parents[1]
ENGINE_DIR = REPO_ROOT / "python" / "openswmm" / "engine"
HEADER_DIR = REPO_ROOT / "include" / "openswmm" / "engine"
OVERRIDES_TSV = PARITY_DIR / "overrides.tsv"
MCP_SRC_DEFAULT = REPO_ROOT.parent / "openswmm.mcp" / "src" / "openswmm_mcp"
MATRIX_MD = PARITY_DIR / "provenance_matrix.md"
GAPS_JSON = PARITY_DIR / "provenance_gaps.json"

_SWMM_TOKEN = re.compile(r"\b(swmm_[a-z0-9_]+)\b")
_SWMM_CALL = re.compile(r"\b(swmm_[a-z0-9_]+)\s*\(")
_CDEF_EXTERN = re.compile(r"cdef\s+extern\b")
_CLASS_RE = re.compile(r"^(?P<i>\s*)(?:cdef\s+)?class\s+(?P<name>\w+)")
_DEF_RE = re.compile(r"^(?P<i>\s*)(?:cpdef|def)\s+(?:[\w\.\[\], \*]+?\s+)??(?P<name>\w+)\s*\(")


# ---------------------------------------------------------------------------
# C side
# ---------------------------------------------------------------------------
def c_symbols() -> dict[str, str]:
    """Return {c_function: domain} for every SWMM_ENGINE_API export."""
    out: dict[str, str] = {}
    for h in sorted(HEADER_DIR.glob("openswmm_*.h")):
        if h.name.endswith("_export.h"):
            continue
        domain = h.stem[len("openswmm_"):]
        lines = h.read_text(encoding="utf-8").splitlines()
        i = 0
        while i < len(lines):
            if lines[i].lstrip().startswith("SWMM_ENGINE_API"):
                buf = [lines[i]]
                while ";" not in lines[i] and i + 1 < len(lines):
                    i += 1
                    buf.append(lines[i])
                m = _SWMM_CALL.search(re.sub(r"\s+", " ", " ".join(buf)))
                if m:
                    out.setdefault(m.group(1), domain)
            i += 1
    return out


# ---------------------------------------------------------------------------
# C -> Python (exact, via .pyx call graph)
# ---------------------------------------------------------------------------
def pyx_callgraph() -> dict[str, set[tuple[str, str, str]]]:
    """Return {c_symbol: {(module, class, method), ...}} from real call sites."""
    c_to_py: dict[str, set[tuple[str, str, str]]] = defaultdict(set)
    for pyx in sorted(ENGINE_DIR.glob("*.pyx")):
        module = pyx.stem
        scopes: list[tuple[int, str, str]] = []   # (indent, kind, name)
        in_extern = False
        extern_indent = 0
        for raw in pyx.read_text(encoding="utf-8").splitlines():
            code = raw.split("#", 1)[0]
            if not code.strip():
                continue
            indent = len(code) - len(code.lstrip())
            if _CDEF_EXTERN.match(code.strip()):
                in_extern = True
                extern_indent = indent
                continue
            if in_extern:
                if indent <= extern_indent:
                    in_extern = False
                else:
                    continue
            cm = _CLASS_RE.match(code)
            if cm:
                ci = len(cm.group("i"))
                while scopes and scopes[-1][0] >= ci:
                    scopes.pop()
                scopes.append((ci, "class", cm.group("name")))
                continue
            dm = _DEF_RE.match(code)
            if dm:
                di = len(dm.group("i"))
                while scopes and scopes[-1][0] >= di:
                    scopes.pop()
                scopes.append((di, "def", dm.group("name")))
                continue
            calls = _SWMM_TOKEN.findall(code)
            if not calls:
                continue
            # attribute to the innermost def scope; enclosing class = nearest
            # class scope beneath it.
            cur_def = next((s for s in reversed(scopes) if s[1] == "def"), None)
            cur_cls = next((s for s in reversed(scopes) if s[1] == "class"), None)
            method = cur_def[2] if cur_def else "<module>"
            cls = cur_cls[2] if cur_cls else ""
            for sym in calls:
                c_to_py[sym].add((module, cls, method))
    return c_to_py


# ---------------------------------------------------------------------------
# Python -> MCP (name-precise, via MCP tool ASTs)
# ---------------------------------------------------------------------------
# Explicit provenance marker an MCP tool (or .pyx wrapper) can carry to pin
# exact coverage, e.g. ``# wraps: swmm_forcing_link_flow, swmm_forcing_link_setting``.
_WRAPS_RE = re.compile(r"wraps:\s*((?:swmm_[a-z0-9_]+\s*,?\s*)+)", re.IGNORECASE)


def mcp_refs(mcp_src: Path) -> tuple[set[str], set[str], set[str]]:
    """Return (attribute_names, class_names, explicitly_wrapped_c_symbols)."""
    attrs: set[str] = set()
    names: set[str] = set()
    wrapped: set[str] = set()
    for sub in ("tools", "resources", "prompts"):
        d = mcp_src / sub
        if not d.is_dir():
            continue
        for p in sorted(d.glob("*.py")):
            if p.name == "__init__.py":
                continue
            src = p.read_text(encoding="utf-8")
            for m in _WRAPS_RE.finditer(src):
                wrapped.update(re.findall(r"swmm_[a-z0-9_]+", m.group(1)))
            try:
                tree = ast.parse(src)
            except SyntaxError:
                continue
            for node in ast.walk(tree):
                if isinstance(node, ast.Attribute):
                    attrs.add(node.attr)
                elif isinstance(node, ast.Name):
                    names.add(node.id)
    return attrs, names, wrapped


# ---------------------------------------------------------------------------
# Overrides (reused verbatim from the fuzzy builder's format)
# ---------------------------------------------------------------------------
def load_overrides() -> dict[str, tuple[str, str]]:
    out: dict[str, tuple[str, str]] = {}
    if not OVERRIDES_TSV.is_file():
        return out
    for line in OVERRIDES_TSV.read_text(encoding="utf-8").splitlines():
        if not line or line.startswith("#"):
            continue
        parts = line.split("\t")
        if len(parts) >= 2:
            out[parts[0].strip()] = (parts[1].strip(),
                                     parts[2].strip() if len(parts) > 2 else "")
    return out


# ---------------------------------------------------------------------------
# Build
# ---------------------------------------------------------------------------
def build(mcp_src: Path) -> list[dict]:
    csyms = c_symbols()
    c_to_py = pyx_callgraph()
    mcp_attrs, mcp_names, mcp_wrapped = mcp_refs(mcp_src)
    overrides = load_overrides()

    # binding method-name universe (from the call graph) so MCP matching is
    # restricted to real binding methods.
    binding_methods = {m for tgts in c_to_py.values() for (_, _, m) in tgts}

    rows: list[dict] = []
    for fn, domain in sorted(csyms.items()):
        ov_status, ov_note = overrides.get(fn, ("", ""))
        py = sorted(c_to_py.get(fn, set()))
        py_methods = {m for (_, _, m) in py}
        py_classes = {c for (_, c, _) in py if c}

        # exact coverage if a `wraps:` annotation names this symbol; else a
        # name-precise heuristic (wrapping method or class referenced in MCP).
        explicit = fn in mcp_wrapped
        heuristic = bool(
            (py_methods & mcp_attrs & binding_methods)
            or (py_classes & (mcp_names | mcp_attrs))
        )
        mcp = "exact" if explicit else ("heuristic" if heuristic else "none")

        if ov_status in ("intentional", "internal"):
            status = ov_status
        elif not py:
            status = "py-gap"                       # REAL, exact: no wrapper
        elif mcp in ("exact", "heuristic"):
            status = "parity"
        elif domain == "2d":
            status = "mcp-review-2d"                # advisory (2D build-cond.)
        else:
            status = "mcp-review"                   # advisory candidate
        rows.append(dict(function=fn, domain=domain,
                         py=[f"{mod}.{c + '.' if c else ''}{m}" for (mod, c, m) in py],
                         mcp=mcp, status=status, note=ov_note))
    return rows


def render_markdown(rows: list[dict], fuzzy_pygaps: int | None) -> str:
    buf: list[str] = []
    buf.append("# C ↔ Python ↔ MCP Parity Matrix — provenance (call-graph) build\n")
    buf.append("Generated by `plans/parity/tools/build_matrix_provenance.py`. "
               "Unlike `parity_matrix.md` (fuzzy name matching), the Python "
               "column here is the **exact** wrapper derived from `.pyx` call "
               "sites, so `py-gap` means a genuinely unwrapped C function.\n")
    buf.append("**Reliability tiers:**\n")
    buf.append("- `py-gap` / `parity` (C↔Python) — **exact**: a wrapper either "
               "calls the C symbol or it doesn't. Trust these; `--check` gates on "
               "`py-gap`.\n")
    buf.append("- `mcp-review*` — **advisory**: the exact wrapping method/class "
               "isn't referenced by name in any MCP tool, but MCP legitimately "
               "aggregates many C ops into one dispatching tool (e.g. "
               "`forcing_set_forcing`, `lifecycle_events_count`), so a review "
               "candidate is **not** a confirmed gap. Pin exact MCP coverage by "
               "adding a `wraps: swmm_x, swmm_y` marker to the MCP tool "
               "(comment or docstring); the builder reads it and promotes the "
               "row to `parity`.\n")
    totals: dict[str, int] = defaultdict(int)
    mcp_tier: dict[str, int] = defaultdict(int)
    for r in rows:
        totals[r["status"]] += 1
        mcp_tier[r["mcp"]] += 1
    buf.append("## Summary\n")
    buf.append("| Status | Count |")
    buf.append("|---|---:|")
    for s in ["parity", "py-gap", "mcp-review", "mcp-review-2d",
              "intentional", "internal"]:
        buf.append(f"| `{s}` | {totals.get(s, 0)} |")
    buf.append(f"| **Total** | **{len(rows)}** |\n")
    buf.append(f"MCP coverage evidence: `exact` (via `wraps:`) "
               f"{mcp_tier.get('exact', 0)}, `heuristic` (name reference) "
               f"{mcp_tier.get('heuristic', 0)}, `none` {mcp_tier.get('none', 0)}.\n")
    if fuzzy_pygaps is not None:
        buf.append(f"> Fuzzy `build_matrix.py` reported **{fuzzy_pygaps}** py-gaps "
                   f"on the same tree; provenance reports **{totals.get('py-gap', 0)}** "
                   f"real py-gaps (false-positive reduction: "
                   f"{fuzzy_pygaps - totals.get('py-gap', 0)}).\n")

    by_domain: dict[str, list[dict]] = defaultdict(list)
    for r in rows:
        by_domain[r["domain"]].append(r)
    buf.append("## Per-domain counts\n")
    buf.append("| Domain | C funcs | parity | py-gap | mcp-gap | other |")
    buf.append("|---|---:|---:|---:|---:|---:|")
    for d in sorted(by_domain):
        dr = by_domain[d]
        par = sum(1 for r in dr if r["status"] == "parity")
        pg = sum(1 for r in dr if r["status"] == "py-gap")
        mg = sum(1 for r in dr if r["status"] in ("mcp-review", "mcp-review-2d"))
        buf.append(f"| `{d}` | {len(dr)} | {par} | {pg} | {mg} | "
                   f"{len(dr) - par - pg - mg} |")
    buf.append("")
    for d in sorted(by_domain):
        buf.append(f"## Domain: `{d}`\n")
        buf.append("| C function | Python (call-graph) | MCP | Status | Note |")
        buf.append("|---|---|---|---|---|")
        for r in sorted(by_domain[d], key=lambda x: x["function"]):
            pycell = ", ".join(f"`{p}`" for p in r["py"]) or "—"
            buf.append(f"| `{r['function']}` | {pycell} | {r['mcp']} | "
                       f"`{r['status']}` | {r['note']} |")
        buf.append("")
    return "\n".join(buf) + "\n"


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--mcp-root", type=Path, default=MCP_SRC_DEFAULT)
    ap.add_argument("--check", action="store_true",
                    help="exit non-zero if any real py-gap exists")
    ap.add_argument("--fuzzy-pygaps", type=int, default=None,
                    help="py-gap count from build_matrix.py, for the comparison note")
    args = ap.parse_args(argv[1:])

    rows = build(args.mcp_root)
    MATRIX_MD.write_text(render_markdown(rows, args.fuzzy_pygaps), encoding="utf-8")

    summary: dict[str, list[str]] = defaultdict(list)
    for r in rows:
        if r["status"] in ("py-gap", "mcp-review", "mcp-review-2d"):
            summary[r["status"]].append(r["function"])
    GAPS_JSON.write_text(json.dumps(
        {"counts": {k: len(v) for k, v in summary.items()},
         "gaps": dict(summary)}, indent=2, sort_keys=True) + "\n", encoding="utf-8")

    totals: dict[str, int] = defaultdict(int)
    for r in rows:
        totals[r["status"]] += 1
    print(f"Wrote {MATRIX_MD.name} and {GAPS_JSON.name}.", file=sys.stderr)
    print(f"Status counts: {dict(totals)}", file=sys.stderr)

    real_pygaps = [r["function"] for r in rows if r["status"] == "py-gap"]
    if args.check and real_pygaps:
        print(f"error: {len(real_pygaps)} real py-gap(s): {real_pygaps}",
              file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
