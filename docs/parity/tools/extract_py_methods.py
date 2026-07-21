#!/usr/bin/env python3
"""Extract Python API methods from `.pyi` stubs in openswmm.engine.

Walks every `python/openswmm/engine/_*.pyi` stub, AST-parses it, and emits one
row per top-level function or class method:

    domain<TAB>qualname<TAB>module<TAB>kind<TAB>signature

`domain` is the stub stem after stripping the leading underscore (e.g. `_2d`
becomes `2d`, matching the C-side `domain` produced by `extract_c_funcs.py`).
`qualname` is `ClassName.method` for class methods or just `func` for module-
level functions. `kind` is one of `method`, `staticmethod`, `classmethod`,
`property`, `function`. `signature` is the rendered argument list.

Output goes to stdout; redirect to `py_methods.tsv`.
"""
from __future__ import annotations

import ast
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]
STUB_DIR = REPO_ROOT / "python" / "openswmm" / "engine"


def render_signature(node: ast.FunctionDef | ast.AsyncFunctionDef) -> str:
    """Render a function signature back to source-like text."""
    try:
        return f"({ast.unparse(node.args)}) -> {ast.unparse(node.returns) if node.returns else 'None'}"
    except Exception:
        return "(...)"


def classify(node: ast.FunctionDef | ast.AsyncFunctionDef) -> str:
    names = {
        d.id if isinstance(d, ast.Name) else
        d.attr if isinstance(d, ast.Attribute) else ""
        for d in node.decorator_list
    }
    if "staticmethod" in names:
        return "staticmethod"
    if "classmethod" in names:
        return "classmethod"
    if "property" in names:
        return "property"
    if any(n.endswith(".setter") or n.endswith(".getter") or n.endswith(".deleter")
           for n in names):
        return "property"
    return "method"


def extract_from_stub(path: Path) -> list[tuple[str, str, str]]:
    """Return list of (qualname, kind, signature) from one .pyi stub."""
    out: list[tuple[str, str, str]] = []
    try:
        tree = ast.parse(path.read_text(encoding="utf-8"))
    except SyntaxError as exc:
        print(f"warning: failed to parse {path.name}: {exc}", file=sys.stderr)
        return out

    for node in tree.body:
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
            sig = render_signature(node)
            out.append((node.name, "function", sig))
        elif isinstance(node, ast.ClassDef):
            cls = node.name
            for member in node.body:
                if isinstance(member, (ast.FunctionDef, ast.AsyncFunctionDef)):
                    kind = classify(member)
                    sig = render_signature(member)
                    out.append((f"{cls}.{member.name}", kind, sig))
    return out


def domain_for(stub: Path) -> str:
    stem = stub.stem
    return stem.lstrip("_") or stub.stem


def main(argv: list[str]) -> int:
    if not STUB_DIR.is_dir():
        print(f"error: stub dir not found: {STUB_DIR}", file=sys.stderr)
        return 2

    stubs = sorted(p for p in STUB_DIR.glob("_*.pyi") if p.name != "__init__.pyi")
    if not stubs:
        print(f"error: no _*.pyi stubs under {STUB_DIR}", file=sys.stderr)
        return 2

    rows: list[tuple[str, str, str, str, str]] = []
    seen: set[tuple[str, str]] = set()
    for stub in stubs:
        domain = domain_for(stub)
        module = stub.stem
        for qualname, kind, sig in extract_from_stub(stub):
            key = (domain, qualname)
            if key in seen:
                continue
            seen.add(key)
            rows.append((domain, qualname, module, kind, sig))

    rows.sort()
    print("domain\tqualname\tmodule\tkind\tsignature")
    for row in rows:
        print("\t".join(row))

    print(f"# {len(rows)} Python methods across {len(stubs)} stubs",
          file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
