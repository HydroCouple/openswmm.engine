#!/usr/bin/env python3
"""Build the C ↔ Python ↔ MCP parity matrix.

Joins three TSVs produced by the sibling extractors:

  - c_funcs.tsv     (extract_c_funcs.py)
  - py_methods.tsv  (extract_py_methods.py)
  - mcp_tools.tsv   (extract_mcp_tools.py)

Emits `parity_matrix.md` (next to the TSVs) and `gaps.json` (CI-consumable
summary). Each row in the matrix is one C function with best-effort
Python/MCP matches and a status:

  - parity        : C ↔ Python ↔ MCP all match (or MCP intentionally aggregates)
  - py-gap        : C present, no Python method
  - mcp-gap       : Python present, no MCP tool
  - mcp-gap-2d    : C is in 2d domain, gap is build-conditional
  - intentional   : explicitly skipped (override via overrides.tsv)
  - internal      : helper / not API (override via overrides.tsv)
  - unknown       : seed status pre-classification (user reviews)

Heuristics for auto-matching:

  - Python: strip `swmm_` then `<domain>_` prefix from C function name to get
    a verb-object; look up any Python method whose `qualname.split('.')[-1]`
    equals or fuzzy-matches that verb-object within the same domain.
  - MCP: search MCP tool names containing either the verb-object or a known
    domain alias (e.g. `node` → `query.get_node_info`). MCP matches are
    advisory hints, not authoritative.

`--check` mode exits non-zero if any row has status `unknown` (used by CI).

Overrides: a manually-curated `overrides.tsv` can pin specific rows to
`intentional` or `internal` with a reason string. Format:

    c_function<TAB>status<TAB>note
"""
from __future__ import annotations

import argparse
import csv
import json
import re
import sys
from collections import defaultdict
from dataclasses import dataclass, field
from pathlib import Path

PARITY_DIR = Path(__file__).resolve().parents[1]
C_TSV = PARITY_DIR / "c_funcs.tsv"
PY_TSV = PARITY_DIR / "py_methods.tsv"
MCP_TSV = PARITY_DIR / "mcp_tools.tsv"
OVERRIDES_TSV = PARITY_DIR / "overrides.tsv"
MATRIX_MD = PARITY_DIR / "parity_matrix.md"
GAPS_JSON = PARITY_DIR / "gaps.json"

# Domain aliases: map C-side domain (header stem after openswmm_) to the
# Python stub stem (without leading underscore) when they differ.
DOMAIN_ALIASES: dict[str, str] = {
    # 1:1 mappings cover the common case; this dict only encodes mismatches.
    "engine": "solver",
    "output": "output_reader",
}

# Mapping from C domain to likely MCP namespace(s) for hinting.
MCP_DOMAIN_HINTS: dict[str, list[str]] = {
    "engine": ["lifecycle"],
    "model": ["building", "editing"],
    "nodes": ["nodes", "query", "editing", "building"],
    "links": ["links", "query", "editing", "building"],
    "subcatchments": ["subcatchments", "query", "editing", "building"],
    "gages": ["query", "editing", "building"],
    "spatial": ["spatial_quality"],
    "quality": ["spatial_quality"],
    "pollutants": ["query", "building"],
    "hotstart": ["hotstart"],
    "tables": ["tables", "building"],
    "controls": ["controls", "forcing"],
    "forcing": ["forcing"],
    "inflows": ["inflows", "forcing", "building"],
    "infrastructure": ["infrastructure", "spatial_quality"],
    "output": ["analysis"],
    "statistics": ["analysis"],
    "edit": ["editing"],
    "massbalance": ["analysis"],
    "2d": [],
    "geopackage": ["geopackage"],
    "callbacks": ["lifecycle"],
}


@dataclass
class CFunc:
    domain: str
    function: str
    header: str
    signature: str
    verb_obj: str = ""  # post-strip of swmm_ and domain prefix


@dataclass
class PyMethod:
    domain: str
    qualname: str
    module: str
    kind: str
    signature: str


@dataclass
class McpEntry:
    namespace: str
    name: str
    module: str
    kind: str
    signature: str


@dataclass
class Row:
    c: CFunc
    py: list[PyMethod] = field(default_factory=list)
    mcp: list[McpEntry] = field(default_factory=list)
    status: str = "unknown"
    note: str = ""


def read_tsv(path: Path) -> list[dict[str, str]]:
    if not path.is_file():
        print(f"error: missing input TSV: {path}", file=sys.stderr)
        sys.exit(2)
    with path.open(encoding="utf-8") as f:
        reader = csv.DictReader(f, delimiter="\t")
        return [r for r in reader if r and not next(iter(r.values()), "").startswith("#")]


def compute_verb_obj(c_func: str, domain: str) -> str:
    """Strip `swmm_` and the domain stem from a C function name.

    e.g. `swmm_node_get_depth` with domain `nodes` -> `get_depth`.
         `swmm_2d_get_depth` with domain `2d`      -> `get_depth`.
         `swmm_gage_delete` with domain `edit`     -> `gage_delete`.
    """
    name = c_func
    if name.startswith("swmm_"):
        name = name[len("swmm_"):]
    # Try common domain prefixes (singular and plural).
    candidates = {domain, domain.rstrip("s"), domain + "s"}
    # 2d: only strip the bare `2d_` prefix; Python attaches `triangle/vertex/edge`
    # to the verb (e.g. C `2d_triangle_get_area` -> Py `get_triangle_area`).
    if domain == "2d":
        candidates.update({"2d"})
    # subcatch / subcatchments / subcatchment
    if domain == "subcatchments":
        candidates.update({"subcatch", "subcatchment"})
    # engine domain has functions named engine_*
    if domain == "engine":
        candidates.update({"engine"})
    # geopackage uses `gpkg_` shorthand in C
    if domain == "geopackage":
        candidates.update({"gpkg"})
    # statistics uses `stat_` prefix in C (`swmm_stat_link_max_filling`)
    if domain == "statistics":
        candidates.update({"stat"})
    # massbalance uses `swmm_get_<noun>_continuity_error` etc. — no shared prefix
    # to strip; verb_obj will be the full tail and we rely on alt_verb_objs.
    # output uses `output_` (already covered since domain == "output")
    # try longest-prefix-first match
    for prefix in sorted(candidates, key=len, reverse=True):
        if not prefix:
            continue
        if name.startswith(prefix + "_"):
            return name[len(prefix) + 1:]
        if name == prefix:
            return ""
    return name


# Cross-cutting domains where C functions are named `<object>_<verb>` (e.g.
# `swmm_gage_delete` in `openswmm_edit.h`); for these, also try stripping
# common object prefixes so the verb_obj reduces to a true verb-object.
CROSS_DOMAIN_OBJECT_PREFIXES: dict[str, set[str]] = {
    "edit": {"node", "link", "subcatch", "subcatchment", "gage", "table",
             "transect", "timeseries", "curve", "pollutant", "landuse"},
    "statistics": {"node", "link", "subcatch", "subcatchment"},
    "controls": {"link"},
    "forcing": {"node", "link", "subcatch", "gage"},
}


def alt_verb_objs(verb_obj: str, domain: str) -> set[str]:
    """Generate alternative leaf-name candidates for fuzzy matching.

    1. The verb_obj itself.
    2. Reversed underscore parts (handles `gage_delete` <-> `delete_gage`).
    3. Move `get_`/`set_` token to the front (handles C `triangle_get_area`
       <-> Py `get_triangle_area`).
    4. Strip trailing `_bulk` (Py omits the suffix for NumPy-only variants).
    5. Strip leading `get_` / `set_` (Py uses properties for some accessors).
    6. For cross-cutting domains, also strip a leading object prefix
       (`gage_delete` -> `delete`) and then re-attach it after.
    """
    out: set[str] = {verb_obj}
    if not verb_obj:
        return out

    def _add_token_variants(token: str) -> None:
        out.add(token)
        if token.endswith("_bulk"):
            out.add(token[: -len("_bulk")])

    _add_token_variants(verb_obj)

    parts = verb_obj.split("_")
    if len(parts) >= 2:
        _add_token_variants("_".join(parts[::-1]))
        _add_token_variants("_".join(parts[1:] + parts[:1]))
        _add_token_variants("_".join(parts[-1:] + parts[:-1]))

    # If verb_obj contains a `get`/`set`/`add`/`clear` token at any position,
    # promote it to the front: `triangle_get_area` -> `get_triangle_area`,
    # `title_add_line` -> `add_title_line`.
    for accessor in ("get", "set", "add", "clear", "remove"):
        if accessor in parts:
            idx = parts.index(accessor)
            if idx != 0:
                promoted = [accessor] + parts[:idx] + parts[idx + 1:]
                _add_token_variants("_".join(promoted))
        # Strip leading get_/set_ for Python-property style (`abs_tolerance`).
        if verb_obj.startswith(accessor + "_"):
            _add_token_variants(verb_obj[len(accessor) + 1:])

    # Try singular/plural variants of the first token (Python uses `option`
    # singular while C uses `options` plural; same for some other prefixes).
    if parts:
        first = parts[0]
        if first.endswith("s") and len(first) > 1:
            singular = first.rstrip("s")
            _add_token_variants("_".join([singular] + parts[1:]))
            # also with each accessor variant
            for accessor in ("get", "set", "add"):
                if accessor in parts[1:]:
                    idx = parts.index(accessor)
                    promoted = [accessor, singular] + parts[1:idx] + parts[idx + 1:]
                    _add_token_variants("_".join(promoted))

    obj_prefixes = CROSS_DOMAIN_OBJECT_PREFIXES.get(domain, set())
    if obj_prefixes and parts:
        head = parts[0]
        if head in obj_prefixes and len(parts) >= 2:
            tail = "_".join(parts[1:])
            _add_token_variants(tail)
            _add_token_variants(f"{tail}_{head}")
            # `<obj>_analyze_impact` -> `analyze_<obj>_impact` (Python `ModelEditor.analyze_node_impact`)
            if len(parts) >= 3 and parts[1] == "analyze":
                _add_token_variants(f"analyze_{head}_{'_'.join(parts[2:])}")

    # `<noun>_id` / `<noun>_index` (e.g. `node_id`) -> Py `get_id` / `get_index`
    if len(parts) == 1 and parts[0] in ("id", "index"):
        _add_token_variants(f"get_{parts[0]}")
    return out


def domain_to_py(domain: str) -> str:
    return DOMAIN_ALIASES.get(domain, domain)


def index_python(py_rows: list[dict[str, str]]) -> dict[str, list[PyMethod]]:
    """Index Python methods by (py_domain, leaf method name)."""
    out: dict[str, list[PyMethod]] = defaultdict(list)
    for r in py_rows:
        leaf = r["qualname"].split(".")[-1]
        key = f"{r['domain']}::{leaf}"
        out[key].append(PyMethod(**r))
    return out


def index_mcp(mcp_rows: list[dict[str, str]]) -> dict[str, list[McpEntry]]:
    """Index MCP entries by both namespace and by name substrings."""
    by_ns: dict[str, list[McpEntry]] = defaultdict(list)
    for r in mcp_rows:
        by_ns[r["namespace"]].append(McpEntry(**r))
    return by_ns


def match_python(cf: CFunc, py_idx: dict[str, list[PyMethod]]) -> list[PyMethod]:
    py_domain = domain_to_py(cf.domain)
    if not cf.verb_obj:
        return []
    candidates: set[str] = set()
    for variant in alt_verb_objs(cf.verb_obj, cf.domain):
        if variant:
            candidates.add(f"{py_domain}::{variant}")
    found: list[PyMethod] = []
    seen: set[tuple[str, str]] = set()
    for key in candidates:
        for m in py_idx.get(key, []):
            sig_key = (m.module, m.qualname)
            if sig_key in seen:
                continue
            seen.add(sig_key)
            found.append(m)
    return found


def match_mcp(cf: CFunc, mcp_by_ns: dict[str, list[McpEntry]]) -> list[McpEntry]:
    namespaces = MCP_DOMAIN_HINTS.get(cf.domain, [])
    if not namespaces or not cf.verb_obj:
        return []
    needles = {n for n in alt_verb_objs(cf.verb_obj, cf.domain) if n}
    found: list[McpEntry] = []
    seen: set[tuple[str, str]] = set()
    for ns in namespaces:
        for entry in mcp_by_ns.get(ns, []):
            name = entry.name
            for needle in needles:
                if (
                    name == needle
                    or name.endswith("_" + needle)
                    or name.startswith(needle + "_")
                    or name == needle.replace("_", "")
                ):
                    key = (entry.namespace, entry.name)
                    if key not in seen:
                        seen.add(key)
                        found.append(entry)
                    break
    return found


def load_overrides() -> dict[str, tuple[str, str]]:
    if not OVERRIDES_TSV.is_file():
        return {}
    out: dict[str, tuple[str, str]] = {}
    with OVERRIDES_TSV.open(encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            parts = line.split("\t")
            if len(parts) < 2:
                continue
            fn = parts[0].strip()
            status = parts[1].strip()
            note = parts[2].strip() if len(parts) > 2 else ""
            out[fn] = (status, note)
    return out


def classify(row: Row, overrides: dict[str, tuple[str, str]]) -> None:
    if row.c.function in overrides:
        row.status, row.note = overrides[row.c.function]
        return
    if not row.py:
        row.status = "py-gap"
        return
    if not row.mcp:
        row.status = "mcp-gap-2d" if row.c.domain == "2d" else "mcp-gap"
        return
    row.status = "parity"


def render_markdown(rows: list[Row]) -> str:
    buf: list[str] = []
    buf.append("# C ↔ Python ↔ MCP Parity Matrix\n")
    buf.append("Generated by `docs/parity/tools/build_matrix.py`. Do not edit "
               "by hand — adjust `overrides.tsv` for intentional/internal "
               "annotations and re-run.\n")
    # Summary
    totals: dict[str, int] = defaultdict(int)
    for r in rows:
        totals[r.status] += 1
    buf.append("## Summary\n")
    buf.append("| Status | Count |")
    buf.append("|---|---:|")
    for status in ["parity", "py-gap", "mcp-gap", "mcp-gap-2d",
                   "intentional", "internal", "unknown"]:
        buf.append(f"| `{status}` | {totals.get(status, 0)} |")
    buf.append(f"| **Total** | **{len(rows)}** |\n")

    # Per-domain breakdown
    by_domain: dict[str, list[Row]] = defaultdict(list)
    for r in rows:
        by_domain[r.c.domain].append(r)

    buf.append("## Per-domain counts\n")
    buf.append("| Domain | C funcs | parity | py-gap | mcp-gap | other |")
    buf.append("|---|---:|---:|---:|---:|---:|")
    for domain in sorted(by_domain):
        drows = by_domain[domain]
        c_total = len(drows)
        parity_count = sum(1 for r in drows if r.status == "parity")
        py_gap = sum(1 for r in drows if r.status == "py-gap")
        mcp_gap = sum(1 for r in drows if r.status in ("mcp-gap", "mcp-gap-2d"))
        other = c_total - parity_count - py_gap - mcp_gap
        buf.append(f"| `{domain}` | {c_total} | {parity_count} | {py_gap} | "
                   f"{mcp_gap} | {other} |")
    buf.append("")

    # Full table per domain
    for domain in sorted(by_domain):
        buf.append(f"## Domain: `{domain}`\n")
        buf.append("| C function | Python | MCP | Status | Note |")
        buf.append("|---|---|---|---|---|")
        for r in sorted(by_domain[domain], key=lambda x: x.c.function):
            py_cell = ", ".join(f"`{p.module}.{p.qualname}`" for p in r.py) or "—"
            mcp_cell = ", ".join(f"`{m.namespace}.{m.name}`" for m in r.mcp) or "—"
            buf.append(f"| `{r.c.function}` | {py_cell} | {mcp_cell} | "
                       f"`{r.status}` | {r.note} |")
        buf.append("")

    return "\n".join(buf) + "\n"


def write_gaps_json(rows: list[Row], path: Path) -> None:
    summary: dict[str, list[str]] = defaultdict(list)
    for r in rows:
        if r.status in ("py-gap", "mcp-gap", "mcp-gap-2d", "unknown"):
            summary[r.status].append(r.c.function)
    counts = {k: len(v) for k, v in summary.items()}
    payload = {"counts": counts, "gaps": dict(summary)}
    path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n",
                    encoding="utf-8")


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true",
                        help="exit non-zero if any row is unclassified")
    args = parser.parse_args(argv[1:])

    c_rows = read_tsv(C_TSV)
    py_rows = read_tsv(PY_TSV)
    mcp_rows = read_tsv(MCP_TSV)
    overrides = load_overrides()

    py_idx = index_python(py_rows)
    mcp_by_ns = index_mcp(mcp_rows)

    rows: list[Row] = []
    for c in c_rows:
        cf = CFunc(domain=c["domain"], function=c["function"],
                   header=c["header"], signature=c["signature"])
        cf.verb_obj = compute_verb_obj(cf.function, cf.domain)
        row = Row(c=cf)
        row.py = match_python(cf, py_idx)
        row.mcp = match_mcp(cf, mcp_by_ns)
        classify(row, overrides)
        rows.append(row)

    MATRIX_MD.write_text(render_markdown(rows), encoding="utf-8")
    write_gaps_json(rows, GAPS_JSON)

    totals: dict[str, int] = defaultdict(int)
    for r in rows:
        totals[r.status] += 1
    print(f"Wrote {MATRIX_MD.name} and {GAPS_JSON.name}.", file=sys.stderr)
    print(f"Status counts: {dict(totals)}", file=sys.stderr)

    if args.check and totals.get("unknown", 0) > 0:
        print(f"error: {totals['unknown']} unclassified rows", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
