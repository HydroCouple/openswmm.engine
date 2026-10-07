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
  * Python -> MCP is *catalog-reachable*: the MCP server (v2) dispatches any
    member listed in ``openswmm.engine.catalog`` through its generic tools, so
    a C symbol is MCP-reachable when a catalogued member (other than the
    callback setters, which cannot cross MCP) lists it among its C symbols.
  * Python -> Gymnasium is *catalog-reachable* for the C symbols behind
    numeric element fields (observable and actuatable by catalog path), plus
    a name-precise scan of the adapter for anything it calls directly.

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
CATALOG_JSON = ENGINE_DIR / "catalog.json"
GYM_SRC_DEFAULT = (REPO_ROOT.parent / "openswmm.gymnasium" / "src"
                   / "openswmm_gymnasium")
MATRIX_MD = PARITY_DIR / "provenance_matrix.md"
GAPS_JSON = PARITY_DIR / "provenance_gaps.json"

# Share the strict tokenizer with the release gate: comments, strings and
# declarations do not establish reachability; .pxd helpers do.
import importlib.util
_spec = importlib.util.spec_from_file_location("_binding_audit", REPO_ROOT / "python/scripts/api_drift_audit.py")
_audit = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_audit)

_SWMM_TOKEN = re.compile(r"\b(swmm_[a-z0-9_]+)\b")
_SWMM_CALL = re.compile(r"\b(swmm_[a-z0-9_]+)\s*\(")
_CDEF_EXTERN = re.compile(r"cdef\s+extern\b")
_CLASS_RE = re.compile(r"^(?P<i>\s*)(?:cdef\s+)?class\s+(?P<name>\w+)")
_DEF_RE = re.compile(r"^(?P<i>\s*)(?:cdef|cpdef|def)\s+(?:[\w\.\[\], \*]+?\s+)??(?P<name>\w+)\s*\(")


# ---------------------------------------------------------------------------
# C side
# ---------------------------------------------------------------------------
def c_symbols() -> dict[str, str]:
    """Return {c_function: domain} for every SWMM_ENGINE_API export."""
    return {fn: Path(header).stem.removeprefix("openswmm_")
            for fn, header in _audit.collect_c_functions().items()}



# ---------------------------------------------------------------------------
# C -> Python (exact, via .pyx call graph)
# ---------------------------------------------------------------------------
def pyx_callgraph() -> dict[str, set[tuple[str, str, str]]]:
    """Return {c_symbol: {(module, class, method), ...}} from real call sites."""
    c_to_py: dict[str, set[tuple[str, str, str]]] = defaultdict(set)
    for pyx in sorted([*ENGINE_DIR.glob("*.pyx"), *ENGINE_DIR.glob("*.pxd")]):
        module = pyx.stem
        scopes: list[tuple[int, str, str]] = []   # (indent, kind, name)
        in_extern = False
        extern_indent = 0
        for raw in _audit.executable_cython(pyx.read_text(encoding="utf-8")).splitlines():
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
# Catalog reach (MCP and Gymnasium)
# ---------------------------------------------------------------------------
_NUMERIC = {"float", "int", "bool"}


def catalog_reach() -> tuple[set[str], set[str]]:
    """Return (C symbols the MCP reaches, C symbols behind numeric element fields).

    Empty sets when the catalog has not been generated yet.
    """
    if not CATALOG_JSON.is_file():
        return set(), set()
    cat = json.loads(CATALOG_JSON.read_text(encoding="utf-8"))
    targets = cat["targets"]

    def element_kind(target: str) -> bool:
        entry = targets.get(target, {})
        while "collection" not in entry:
            parent = entry.get("parent")
            if parent is None or parent not in targets:
                return False
            entry = targets[parent]
        return True

    skip: set[str] = set()
    mcp: set[str] = set()
    fields: set[str] = set()
    for name, t in targets.items():
        if name not in skip:
            mcp |= set(t.get("c", []))
    for funcs in cat.get("functions", {}).values():
        for f in funcs.values():
            mcp |= set(f.get("c", []))
    for m in cat["members"]:
        syms = set(m.get("c", []))
        if m["target"] in skip or (m["target"] == "solver" and m["name"].endswith("_callback")):
            continue  # Python callables cannot cross MCP
        mcp |= syms
        if m["form"] == "property" and m.get("type") in _NUMERIC and element_kind(m["target"]):
            fields |= syms
    return mcp, fields


# C functions with no meaning over MCP, and why. They are reported as
# ``mcp-na`` instead of ``mcp-review``.
MCP_NOT_APPLICABLE = {
    "swmm_set_progress_callback": "Python callables cannot cross MCP.",
    "swmm_set_step_begin_callback": "Python callables cannot cross MCP.",
    "swmm_set_step_end_callback": "Python callables cannot cross MCP.",
    "swmm_set_warning_callback": "Python callables cannot cross MCP.",
    "swmm_engine_run": "One-shot run; the MCP run tool steps the session instead.",
    "swmm_engine_run_with_callback": "One-shot run with a callback; see swmm_engine_run.",
    "swmm_error_message": "Error text; every MCP error already carries it.",
    "swmm_gpkg_register": "The GeoPackage plugin registers itself on import.",
    "swmm_gpkg_is_registered": "The GeoPackage plugin registers itself on import.",
    "swmm_transport_class_name": "Enum-to-name helper; describe('enum:TransportClass').",
    "swmm_transport_domain_name": "Enum-to-name helper; describe('enum:TransportDomain').",
    "swmm_xsect_shape_name": "Enum-to-name helper; describe('enum:XSectShape').",
}


# Explicit provenance marker a gymnasium module (or .pyx wrapper) can carry to
# pin exact coverage, e.g. ``# wraps: swmm_forcing_link_flow``.
_WRAPS_RE = re.compile(r"wraps:\s*((?:swmm_[a-z0-9_]+\s*,?\s*)+)", re.IGNORECASE)


def _py_refs(paths: list[Path]) -> tuple[set[str], set[str], set[str]]:
    """Return (attribute_names, bare_names, explicitly_wrapped_c_symbols)."""
    attrs: set[str] = set()
    names: set[str] = set()
    wrapped: set[str] = set()
    for p in paths:
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


def gym_refs(gym_src: Path) -> tuple[set[str], set[str], set[str]]:
    """Return the same triple for the gymnasium adapter surface.

    ``openswmm.gymnasium`` reaches the engine through exactly one module --
    ``_engine/solver_adapter.py``, a hand-written per-method shim. Everything
    else in the package takes a ``SolverAdapter``. Scanning the whole package
    would therefore report the adapter's *own* method names as engine
    coverage; scanning only the adapter is the honest signal.

    Any other module that imports ``openswmm`` directly is included too, so
    that a new bypass of the choke point shows up rather than hiding.
    """
    adapter = gym_src / "_engine" / "solver_adapter.py"
    paths = [adapter] if adapter.is_file() else []
    for p in sorted(gym_src.rglob("*.py")):
        if p == adapter:
            continue
        src = p.read_text(encoding="utf-8")
        if re.search(r"^\s*(from|import)\s+openswmm\b", src, re.MULTILINE):
            paths.append(p)
    return _py_refs(paths)


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
def build(gym_src: Path | None = None) -> list[dict]:
    csyms = c_symbols()
    c_to_py = pyx_callgraph()
    mcp_reach, field_reach = catalog_reach()
    if gym_src is not None and gym_src.is_dir():
        gym_attrs, gym_names, gym_wrapped = gym_refs(gym_src)
    else:
        gym_attrs, gym_names, gym_wrapped = set(), set(), set()
    overrides = load_overrides()

    # binding method-name universe (from the call graph) so the gymnasium
    # name scan is restricted to real binding methods.
    binding_methods = {m for tgts in c_to_py.values() for (_, _, m) in tgts}

    rows: list[dict] = []
    for fn, domain in sorted(csyms.items()):
        ov_status, ov_note = overrides.get(fn, ("", ""))
        py = sorted(c_to_py.get(fn, set()))
        py_methods = {m for (_, _, m) in py}
        py_classes = {c for (_, c, _) in py if c}

        mcp = "catalog" if fn in mcp_reach else "none"

        # Gymnasium reach. Advisory only: gymnasium is an RL surface, not an
        # API mirror, so "none" is the expected default and is never a gate.
        gym_explicit = fn in gym_wrapped
        gym_heuristic = bool(
            (py_methods & gym_attrs & binding_methods)
            or (py_classes & (gym_names | gym_attrs))
        )
        gym = ("exact" if gym_explicit else "field" if fn in field_reach
               else "heuristic" if gym_heuristic else "none")

        if fn in _audit.KNOWN_UNBOUND:
            status = "intentional"
        elif not py:
            status = "py-gap"                       # REAL, exact: no wrapper
        elif mcp == "catalog":
            status = "parity"
        elif fn in MCP_NOT_APPLICABLE:
            status = "mcp-na"
            ov_note = ov_note or MCP_NOT_APPLICABLE[fn]
        elif domain == "2d":
            status = "mcp-review-2d"                # advisory (2D build-cond.)
        else:
            status = "mcp-review"                   # advisory candidate
        rows.append(dict(function=fn, domain=domain,
                         py=[f"{mod}.{c + '.' if c else ''}{m}" for (mod, c, m) in py],
                         mcp=mcp, gym=gym, status=status, note=ov_note))
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
    buf.append("- MCP column — `catalog` means catalog-reachable: the MCP "
               "server's generic tools dispatch every member of "
               "`openswmm.engine.catalog`, and a catalogued member lists this "
               "C symbol. `mcp-na` rows have no meaning over MCP (the note "
               "says why); `mcp-review*` rows are wrapped in Python but reached "
               "by no catalogued member.\n")
    totals: dict[str, int] = defaultdict(int)
    mcp_tier: dict[str, int] = defaultdict(int)
    for r in rows:
        totals[r["status"]] += 1
        mcp_tier[r["mcp"]] += 1
    buf.append("## Summary\n")
    buf.append("| Status | Count |")
    buf.append("|---|---:|")
    for s in ["parity", "py-gap", "mcp-review", "mcp-review-2d", "mcp-na",
              "intentional", "internal"]:
        buf.append(f"| `{s}` | {totals.get(s, 0)} |")
    buf.append(f"| **Total** | **{len(rows)}** |\n")
    buf.append(f"MCP reach: `catalog` {mcp_tier.get('catalog', 0)}, "
               f"`none` {mcp_tier.get('none', 0)}.\n")
    gym_tier: dict[str, int] = defaultdict(int)
    for r in rows:
        gym_tier[r.get("gym", "none")] += 1
    reached = len(rows) - gym_tier.get("none", 0)
    buf.append(f"Gymnasium reach: **{reached}** of {len(rows)} C symbols are "
               f"reachable (`field` — behind a numeric element field, "
               f"{gym_tier.get('field', 0)}; `exact` {gym_tier.get('exact', 0)}; "
               f"`heuristic` {gym_tier.get('heuristic', 0)}). This column is "
               f"**advisory and never gated** — gymnasium is an RL surface, not "
               f"an API mirror, so most of the C API is legitimately out of "
               f"scope. It is tracked so that a capability the RL layer *should* "
               f"have adopted does not go unnoticed.\n")
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
        buf.append("| C function | Python (call-graph) | MCP | Gym | Status | Note |")
        buf.append("|---|---|---|---|---|---|")
        for r in sorted(by_domain[d], key=lambda x: x["function"]):
            pycell = ", ".join(f"`{p}`" for p in r["py"]) or "—"
            buf.append(f"| `{r['function']}` | {pycell} | {r['mcp']} | "
                       f"{r.get('gym', 'none')} | `{r['status']}` | {r['note']} |")
        buf.append("")
    return "\n".join(buf) + "\n"


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--gym-root", type=Path, default=GYM_SRC_DEFAULT,
                    help="openswmm_gymnasium package root (advisory column)")
    ap.add_argument("--check", action="store_true",
                    help="exit non-zero if any real py-gap exists")
    ap.add_argument("--fuzzy-pygaps", type=int, default=None,
                    help="py-gap count from build_matrix.py, for the comparison note")
    args = ap.parse_args(argv[1:])

    rows = build(args.gym_root)
    MATRIX_MD.write_text(render_markdown(rows, args.fuzzy_pygaps), encoding="utf-8")

    summary: dict[str, list[str]] = defaultdict(list)
    for r in rows:
        if r["status"] in ("py-gap", "mcp-review", "mcp-review-2d"):
            summary[r["status"]].append(r["function"])
    gym_reached = sum(1 for r in rows if r.get("gym", "none") != "none")
    GAPS_JSON.write_text(json.dumps(
        {"counts": {k: len(v) for k, v in summary.items()},
         "gym_reach": {"reached": gym_reached, "total": len(rows)},
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
