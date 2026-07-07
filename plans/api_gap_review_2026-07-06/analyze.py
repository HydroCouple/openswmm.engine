#!/usr/bin/env python3
"""Comprehensive Python-bindings gap analysis for openswmm.engine.

Four dimensions:
  1. C API -> Python coverage (true exposure, not just extern-declared).
  2. .pyi stub completeness + docstring coverage vs .pyx implementations.
  3. Test coverage of the binding surface.
  4. MCP tool parity (reuses the vetted plans/parity matcher).

Writes reviewable CSV/JSON artefacts next to this script (data/ + out/).
Pure text analysis; does not import the compiled extension.
"""
from __future__ import annotations

import ast
import csv
import json
import re
import sys
from collections import defaultdict
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]                      # openswmm.engine
ENGINE_DIR = REPO / "python" / "openswmm" / "engine"
HEADER_DIR = REPO / "include" / "openswmm" / "engine"
TESTS_DIR = REPO / "python" / "tests"
PARITY_DIR = REPO / "plans" / "parity"
DATA = HERE / "data"
OUT = HERE / "out"
OUT.mkdir(parents=True, exist_ok=True)

SWMM_TOK = re.compile(r"\b(swmm_[a-z0-9_]+)\b")


def read_tsv(path: Path) -> list[dict[str, str]]:
    with path.open(encoding="utf-8") as f:
        return [r for r in csv.DictReader(f, delimiter="\t")
                if r and not next(iter(r.values()), "").startswith("#")]


# ---------------------------------------------------------------------------
# Dimension 1: C API -> Python exposure
# ---------------------------------------------------------------------------
def load_overrides() -> dict[str, tuple[str, str]]:
    out: dict[str, tuple[str, str]] = {}
    p = PARITY_DIR / "overrides.tsv"
    for line in p.read_text(encoding="utf-8").splitlines():
        if not line or line.startswith("#"):
            continue
        parts = line.split("\t")
        if len(parts) >= 2:
            out[parts[0].strip()] = (parts[1].strip(),
                                     parts[2].strip() if len(parts) > 2 else "")
    return out


def refs_in(glob: str, strip_comments: bool = True) -> set[str]:
    """Collect swmm_* identifiers. Strips ``#`` line comments so a function
    mentioned only in a comment (e.g. swmm_get_current_time) is not counted
    as a real reference."""
    refs: set[str] = set()
    for p in sorted(ENGINE_DIR.glob(glob)):
        for line in p.read_text(encoding="utf-8", errors="replace").splitlines():
            if strip_comments:
                line = line.split("#", 1)[0]
            for m in SWMM_TOK.finditer(line):
                refs.add(m.group(1))
    return refs


def dim1_c_coverage() -> dict:
    c_rows = read_tsv(DATA / "c_funcs.tsv")
    c_funcs = {r["function"]: r["domain"] for r in c_rows}
    overrides = load_overrides()

    refs_pxd = refs_in("*.pxd")
    refs_pyx = refs_in("*.pyx")
    called = refs_pyx                       # actually invoked from Python layer
    declared = refs_pxd | refs_pyx          # visible to Cython at all

    rows = []
    for fn, domain in sorted(c_funcs.items()):
        ov_status, ov_note = overrides.get(fn, ("", ""))
        is_called = fn in called
        is_declared = fn in declared
        intentional = ov_status in ("intentional", "internal")
        forced_parity = ov_status == "parity"
        if is_called or forced_parity:
            status = "exposed"
        elif intentional:
            status = "intentional"
        elif is_declared:
            status = "extern-only"          # declared to Cython, never called
        else:
            status = "unbound"              # not referenced anywhere
        rows.append(dict(function=fn, domain=domain, called=is_called,
                         declared=is_declared, override=ov_status,
                         status=status, note=ov_note))

    with (OUT / "c_coverage.csv").open("w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        w.writeheader()
        w.writerows(rows)

    counts = defaultdict(int)
    for r in rows:
        counts[r["status"]] += 1
    by_domain = defaultdict(lambda: defaultdict(int))
    for r in rows:
        by_domain[r["domain"]][r["status"]] += 1

    gaps = [r for r in rows if r["status"] in ("extern-only", "unbound")]
    return dict(total=len(rows), counts=dict(counts),
                by_domain={k: dict(v) for k, v in by_domain.items()},
                gaps=gaps,
                refs_pxd=len(refs_pxd), refs_pyx=len(refs_pyx))


# ---------------------------------------------------------------------------
# Dimension 2: .pyx public API -> .pyi stubs + docstrings
# ---------------------------------------------------------------------------
DEF_RE = re.compile(r"^(?P<indent>\s*)(?:cpdef|def)\s+"
                    r"(?:[\w\.\[\], \*]+?\s+)??(?P<name>\w+)\s*\(")
CLASS_RE = re.compile(r"^(?P<indent>\s*)(?:cdef\s+)?class\s+(?P<name>\w+)")
PROP_RE = re.compile(r"^\s*@property\b")
SETTER_RE = re.compile(r"^\s*@(\w+)\.setter\b")


def parse_pyx_public(path: Path):
    """Yield dicts: {class, name, kind, has_doc} for Python-visible defs.

    kind in {function, method, property, setter}. Dunder names kept only for
    docstring pass, excluded from stub-diff by caller.
    """
    lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    results = []
    class_stack = []   # (indent, name)
    pending_prop = None  # 'property' | 'setter'
    i = 0
    while i < len(lines):
        line = lines[i]
        if not line.strip() or line.lstrip().startswith("#"):
            i += 1
            continue
        cm = CLASS_RE.match(line)
        if cm:
            indent = len(cm.group("indent"))
            while class_stack and class_stack[-1][0] >= indent:
                class_stack.pop()
            class_stack.append((indent, cm.group("name")))
            pending_prop = None
            i += 1
            continue
        if PROP_RE.match(line):
            pending_prop = "property"
            i += 1
            continue
        sm = SETTER_RE.match(line)
        if sm:
            pending_prop = "setter"
            i += 1
            continue
        dm = DEF_RE.match(line)
        if dm:
            indent = len(dm.group("indent"))
            name = dm.group("name")
            while class_stack and class_stack[-1][0] >= indent:
                class_stack.pop()
            cls = class_stack[-1][1] if class_stack and indent > class_stack[-1][0] else ""
            # advance to end of signature (line ending with ':' at def level)
            j = i
            while j < len(lines) and not re.search(r":\s*(#.*)?$", lines[j]):
                j += 1
            # docstring = first non-blank line after signature is a string
            has_doc = False
            k = j + 1
            while k < len(lines) and not lines[k].strip():
                k += 1
            if k < len(lines):
                s = lines[k].lstrip()
                if s.startswith(('"""', "'''", 'r"""', "r'''", '"', "'")):
                    has_doc = True
            kind = pending_prop or ("method" if cls else "function")
            results.append(dict(cls=cls, name=name, kind=kind, has_doc=has_doc,
                                line=i + 1))
            pending_prop = None
            i = j + 1
            continue
        pending_prop = None
        i += 1
    return results


def _stub_member_names(body, cls: str, out: set[tuple[str, str]]) -> None:
    """Collect def names AND annotated/plain attribute names.

    Stubs often express properties as class-level annotations
    (``temp_source: int``) rather than ``@property def``; those must count
    as present or every such property reads as a false stub-gap.
    """
    for m in body:
        if isinstance(m, (ast.FunctionDef, ast.AsyncFunctionDef)):
            out.add((cls, m.name))
        elif isinstance(m, ast.AnnAssign) and isinstance(m.target, ast.Name):
            out.add((cls, m.target.id))
        elif isinstance(m, ast.Assign):
            for t in m.targets:
                if isinstance(t, ast.Name):
                    out.add((cls, t.id))


def parse_pyi_names(path: Path) -> set[tuple[str, str]]:
    out: set[tuple[str, str]] = set()
    try:
        tree = ast.parse(path.read_text(encoding="utf-8"))
    except SyntaxError:
        return out
    _stub_member_names(tree.body, "", out)
    for node in tree.body:
        if isinstance(node, ast.ClassDef):
            _stub_member_names(node.body, node.name, out)
    return out


def dim2_stubs_docstrings() -> dict:
    stub_gaps = []      # public callable in .pyx, absent from .pyi
    stale_stubs = []    # in .pyi but not .pyx
    doc_gaps = []       # public def/cpdef lacking docstring
    modules = []
    per_module = {}

    for pyx in sorted(ENGINE_DIR.glob("_*.pyx")):
        mod = pyx.stem
        pyi = ENGINE_DIR / f"{mod}.pyi"
        pub = parse_pyx_public(pyx)
        pyi_names = parse_pyi_names(pyi) if pyi.exists() else set()
        has_stub = pyi.exists()
        modules.append(mod)

        # consistent public filter for BOTH sides: no underscore/dunder names,
        # no underscore-prefixed (private) classes.
        def is_public(cls: str, name: str) -> bool:
            return not name.startswith("_") and not cls.startswith("_")

        pyx_public = {(r["cls"], r["name"]) for r in pub
                      if is_public(r["cls"], r["name"])}
        pyi_public = {(c, n) for (c, n) in pyi_names if is_public(c, n)}

        # missing: public in .pyx, absent from .pyi (compare vs FULL pyi so a
        # public name documented under any form still counts as covered).
        missing = sorted(pyx_public - pyi_names) if has_stub else sorted(pyx_public)
        # stale: public in .pyi, not found anywhere in .pyx public surface.
        pyx_all_names = {n for _, n in pyx_public}
        stale = [(c, n) for (c, n) in (pyi_public - pyx_public)
                 if n not in pyx_all_names]

        # docstring gaps: only real def/cpdef methods+functions (not properties/setters,
        # not dunders), public
        for r in pub:
            if r["kind"] in ("property", "setter"):
                continue
            if r["name"].startswith("_"):
                continue
            if not r["has_doc"]:
                doc_gaps.append(dict(module=mod, cls=r["cls"], name=r["name"],
                                     line=r["line"]))
        for (c, n) in missing:
            stub_gaps.append(dict(module=mod, cls=c, name=n, has_stub=has_stub))
        for (c, n) in stale:
            stale_stubs.append(dict(module=mod, cls=c, name=n))

        per_module[mod] = dict(has_stub=has_stub,
                               n_public=len(pyx_public),
                               n_missing=len(missing),
                               n_doc_gaps=sum(1 for r in pub
                                              if r["kind"] not in ("property", "setter")
                                              and not r["name"].startswith("_")
                                              and not r["has_doc"]))

    for name, rows in (("stub_gaps.csv", stub_gaps),
                       ("stale_stubs.csv", stale_stubs),
                       ("docstring_gaps.csv", doc_gaps)):
        if rows:
            with (OUT / name).open("w", newline="", encoding="utf-8") as f:
                w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
                w.writeheader()
                w.writerows(rows)

    return dict(n_modules=len(modules), per_module=per_module,
                n_stub_gaps=len(stub_gaps), stub_gaps=stub_gaps,
                n_stale_stubs=len(stale_stubs), stale_stubs=stale_stubs,
                n_doc_gaps=len(doc_gaps), doc_gaps=doc_gaps)


# ---------------------------------------------------------------------------
# Dimension 3: test coverage of binding classes/methods
# ---------------------------------------------------------------------------
def dim3_tests() -> dict:
    # gather all test source text
    test_text = []
    for p in sorted(TESTS_DIR.rglob("*.py")):
        test_text.append(p.read_text(encoding="utf-8", errors="replace"))
    blob = "\n".join(test_text)

    # binding classes = cdef class / class in .pyx (public, non underscore)
    classes = {}
    methods = defaultdict(set)
    for pyx in sorted(ENGINE_DIR.glob("_*.pyx")):
        for r in parse_pyx_public(pyx):
            if r["cls"] and not r["cls"].startswith("_"):
                classes.setdefault(r["cls"], pyx.stem)
                if (not r["name"].startswith("_")
                        and r["kind"] in ("method", "property", "setter")):
                    methods[r["cls"]].add(r["name"])

    class_rows = []
    for cls, mod in sorted(classes.items()):
        cls_ref = bool(re.search(rf"\b{re.escape(cls)}\b", blob))
        meths = sorted(methods.get(cls, ()))
        ref_meths = [m for m in meths
                     if re.search(rf"\.{re.escape(m)}\b", blob)]
        class_rows.append(dict(cls=cls, module=mod, class_referenced=cls_ref,
                               n_methods=len(meths),
                               n_methods_referenced=len(ref_meths),
                               untested_methods=";".join(
                                   m for m in meths if m not in ref_meths)))

    with (OUT / "test_coverage.csv").open("w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=list(class_rows[0].keys()))
        w.writeheader()
        w.writerows(class_rows)

    untested_classes = [r["cls"] for r in class_rows if not r["class_referenced"]]
    total_m = sum(r["n_methods"] for r in class_rows)
    ref_m = sum(r["n_methods_referenced"] for r in class_rows)
    return dict(n_classes=len(class_rows),
                n_untested_classes=len(untested_classes),
                untested_classes=untested_classes,
                total_methods=total_m, referenced_methods=ref_m,
                class_rows=class_rows,
                n_test_files=len(test_text))


# ---------------------------------------------------------------------------
# Dimension 4: MCP parity (reuse vetted matcher)
# ---------------------------------------------------------------------------
def dim4_mcp() -> dict:
    sys.path.insert(0, str(PARITY_DIR / "tools"))
    import build_matrix as bm
    bm.C_TSV = DATA / "c_funcs.tsv"
    bm.PY_TSV = DATA / "py_methods.tsv"
    bm.MCP_TSV = DATA / "mcp_tools.tsv"
    bm.OVERRIDES_TSV = PARITY_DIR / "overrides.tsv"
    bm.MATRIX_MD = OUT / "parity_matrix.md"
    bm.GAPS_JSON = OUT / "gaps.json"

    c_rows = bm.read_tsv(bm.C_TSV)
    py_rows = bm.read_tsv(bm.PY_TSV)
    mcp_rows = bm.read_tsv(bm.MCP_TSV)
    overrides = bm.load_overrides()
    py_idx = bm.index_python(py_rows)
    mcp_by_ns = bm.index_mcp(mcp_rows)

    # true-exposure set from dim1 (called in .pyx) to reconcile false py-gaps
    called = refs_in("*.pyx")

    rows = []
    for c in c_rows:
        cf = bm.CFunc(domain=c["domain"], function=c["function"],
                      header=c["header"], signature=c["signature"])
        cf.verb_obj = bm.compute_verb_obj(cf.function, cf.domain)
        row = bm.Row(c=cf)
        row.py = bm.match_python(cf, py_idx)
        row.mcp = bm.match_mcp(cf, mcp_by_ns)
        bm.classify(row, overrides)
        rows.append(row)

    bm.MATRIX_MD.write_text(bm.render_markdown(rows), encoding="utf-8")
    bm.write_gaps_json(rows, bm.GAPS_JSON)

    counts = defaultdict(int)
    for r in rows:
        counts[r.status] += 1

    # mcp-gaps: python exists but no mcp tool
    mcp_gaps = [dict(function=r.c.function, domain=r.c.domain,
                     py=";".join(f"{p.module}.{p.qualname}" for p in r.py))
                for r in rows if r.status in ("mcp-gap", "mcp-gap-2d")]

    # matrix py-gaps, split by whether actually called in .pyx (false negative)
    matrix_pygaps = [r for r in rows if r.status == "py-gap"]
    real_pygaps = [r.c.function for r in matrix_pygaps if r.c.function not in called]
    false_pygaps = [r.c.function for r in matrix_pygaps if r.c.function in called]

    with (OUT / "mcp_gaps.csv").open("w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=["function", "domain", "py"])
        w.writeheader()
        w.writerows(mcp_gaps)

    return dict(counts=dict(counts),
                n_mcp_gaps=len(mcp_gaps), mcp_gaps=mcp_gaps,
                n_matrix_pygaps=len(matrix_pygaps),
                n_real_pygaps=len(real_pygaps), real_pygaps=real_pygaps,
                n_false_pygaps=len(false_pygaps), false_pygaps=false_pygaps)


def main() -> int:
    result = dict(
        dim1=dim1_c_coverage(),
        dim2=dim2_stubs_docstrings(),
        dim3=dim3_tests(),
        dim4=dim4_mcp(),
    )
    (OUT / "summary.json").write_text(json.dumps(result, indent=2, default=str),
                                      encoding="utf-8")

    d1, d2, d3, d4 = result["dim1"], result["dim2"], result["dim3"], result["dim4"]
    print("=== DIM 1: C API -> Python exposure ===")
    print(f"  C functions: {d1['total']}  | .pxd refs {d1['refs_pxd']} "
          f"| .pyx refs {d1['refs_pyx']}")
    print(f"  status: {d1['counts']}")
    print(f"  gaps (extern-only + unbound): {len(d1['gaps'])}")
    for g in d1["gaps"]:
        print(f"    - {g['function']} [{g['domain']}] {g['status']}")
    print("\n=== DIM 2: stubs + docstrings ===")
    print(f"  modules: {d2['n_modules']}  stub-gaps: {d2['n_stub_gaps']}  "
          f"stale-stubs: {d2['n_stale_stubs']}  doc-gaps: {d2['n_doc_gaps']}")
    print("\n=== DIM 3: tests ===")
    print(f"  binding classes: {d3['n_classes']}  untested classes: "
          f"{d3['n_untested_classes']} {d3['untested_classes']}")
    print(f"  methods referenced: {d3['referenced_methods']}/{d3['total_methods']}")
    print("\n=== DIM 4: MCP parity ===")
    print(f"  matrix counts: {d4['counts']}")
    print(f"  mcp-gaps: {d4['n_mcp_gaps']}  matrix py-gaps: {d4['n_matrix_pygaps']} "
          f"(real {d4['n_real_pygaps']}, false {d4['n_false_pygaps']})")
    print(f"  real py-gaps: {d4['real_pygaps']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
