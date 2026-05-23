#!/usr/bin/env python3
"""Extract MCP tools and resources from openswmm.mcp.

Walks every `src/openswmm_mcp/tools/*.py` and `src/openswmm_mcp/resources/*.py`
in the sibling openswmm.mcp repo, AST-parses each module, and emits one row
per function decorated with `@<namespace>_mcp.tool` or
`@<namespace>_mcp.resource(...)`. Output TSV:

    namespace<TAB>name<TAB>module<TAB>kind<TAB>signature

`namespace` is the prefix in `<ns>_mcp` (e.g. `query`, `lifecycle`, `spatial_quality`).
`kind` is `tool` or `resource`. `signature` is the rendered argument list.

Output goes to stdout; redirect to `mcp_tools.tsv`.

Path discovery: defaults to `../../openswmm.mcp/src/openswmm_mcp/` relative
to this script's grand-parent repo. Override via `--mcp-root <path>`.
"""
from __future__ import annotations

import argparse
import ast
import sys
from pathlib import Path

DEFAULT_MCP_ROOT = (
    Path(__file__).resolve().parents[3].parent
    / "openswmm.mcp" / "src" / "openswmm_mcp"
)


def render_signature(node: ast.FunctionDef | ast.AsyncFunctionDef) -> str:
    try:
        ret = ast.unparse(node.returns) if node.returns else "None"
        return f"({ast.unparse(node.args)}) -> {ret}"
    except Exception:
        return "(...)"


def decorator_info(node: ast.FunctionDef | ast.AsyncFunctionDef) -> tuple[str, str] | None:
    """Return (namespace, kind) for a decorated MCP tool/resource, else None.

    Matches:
      @<ns>_mcp.tool                 -> (ns, "tool")
      @<ns>_mcp.tool(...)            -> (ns, "tool")
      @<ns>_mcp.resource(...)        -> (ns, "resource")
      @<ns>_mcp.resource             -> (ns, "resource")
    """
    for d in node.decorator_list:
        target = d.func if isinstance(d, ast.Call) else d
        if not isinstance(target, ast.Attribute):
            continue
        attr = target.attr
        if attr not in ("tool", "resource"):
            continue
        owner = target.value
        if not isinstance(owner, ast.Name):
            continue
        if not owner.id.endswith("_mcp"):
            continue
        ns = owner.id[: -len("_mcp")]
        return ns, attr
    return None


def extract_from_module(path: Path) -> list[tuple[str, str, str, str]]:
    out: list[tuple[str, str, str, str]] = []
    try:
        tree = ast.parse(path.read_text(encoding="utf-8"))
    except SyntaxError as exc:
        print(f"warning: failed to parse {path}: {exc}", file=sys.stderr)
        return out

    for node in ast.walk(tree):
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
            info = decorator_info(node)
            if info is None:
                continue
            ns, kind = info
            sig = render_signature(node)
            out.append((ns, node.name, kind, sig))
    return out


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mcp-root", type=Path, default=DEFAULT_MCP_ROOT,
                        help="path to openswmm_mcp package root")
    args = parser.parse_args(argv[1:])

    if not args.mcp_root.is_dir():
        print(f"error: mcp root not found: {args.mcp_root}", file=sys.stderr)
        return 2

    candidate_dirs = ["tools", "resources", "prompts"]
    py_files: list[Path] = []
    for sub in candidate_dirs:
        d = args.mcp_root / sub
        if d.is_dir():
            py_files.extend(sorted(p for p in d.glob("*.py")
                                   if p.name != "__init__.py"))

    if not py_files:
        print(f"error: no tool/resource modules under {args.mcp_root}",
              file=sys.stderr)
        return 2

    rows: list[tuple[str, str, str, str, str]] = []
    seen: set[tuple[str, str]] = set()
    for path in py_files:
        module = path.stem
        for ns, name, kind, sig in extract_from_module(path):
            key = (ns, name)
            if key in seen:
                continue
            seen.add(key)
            rows.append((ns, name, module, kind, sig))

    rows.sort()
    print("namespace\tname\tmodule\tkind\tsignature")
    for row in rows:
        print("\t".join(row))

    n_tools = sum(1 for r in rows if r[3] == "tool")
    n_resources = sum(1 for r in rows if r[3] == "resource")
    print(f"# {len(rows)} entries ({n_tools} tools, {n_resources} resources) "
          f"across {len(py_files)} modules", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
