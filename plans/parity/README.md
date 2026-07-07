# C ↔ Python ↔ MCP Parity Tooling

This directory holds the gap-tracking infrastructure for the openswmm engine
API surface:

- `parity_matrix.md` — the rendered matrix, one row per exported C function,
  with its Python and MCP counterparts (or a `py-gap` / `mcp-gap` marker).
- `gaps.json` — machine-readable summary; consumed by CI.
- `c_funcs.tsv`, `py_methods.tsv`, `mcp_tools.tsv` — raw extraction artefacts.
- `overrides.tsv` — manually-curated annotations for cases the auto-matcher
  cannot resolve (renames, intentional non-exposures, internal helpers).
- `tools/` — the three extractors and the matrix builder.

## Regenerating the matrix

```
python3 docs/parity/tools/extract_c_funcs.py    > docs/parity/c_funcs.tsv
python3 docs/parity/tools/extract_py_methods.py > docs/parity/py_methods.tsv
python3 docs/parity/tools/extract_mcp_tools.py  > docs/parity/mcp_tools.tsv
python3 docs/parity/tools/build_matrix.py
```

The matrix is built from the working tree, so it reflects current
uncommitted state of both `openswmm.engine` and the sibling `openswmm.mcp`
repo. To run against a pristine HEAD, stash WIP first.

## Inputs

### `extract_c_funcs.py`

Reads `include/openswmm/engine/openswmm_*.h` (excluding `*_export.h`).
Detection rule: any declaration prefixed by `SWMM_ENGINE_API` (the export
macro). Multi-line declarations are joined. The `domain` column derives from
the header stem after the `openswmm_` prefix.

### `extract_py_methods.py`

AST-parses `python/openswmm/engine/_*.pyi`. Emits one row per top-level
function or class method (including properties / class methods / static
methods). The `domain` column is the stub stem with the leading underscore
removed.

### `extract_mcp_tools.py`

AST-parses `../openswmm.mcp/src/openswmm_mcp/{tools,resources,prompts}/*.py`.
Detection rule: any function decorated with `@<ns>_mcp.tool` or
`@<ns>_mcp.resource(...)`. The path is overridable via `--mcp-root`.

### `build_matrix.py`

Joins the three TSVs. Auto-matches with a chain of heuristics:

1. Strip `swmm_` and the domain prefix from the C name to get a verb-object.
2. Generate alternative leaf-name variants (reverse halves, promote
   `get_`/`set_` to the front, drop `_bulk` suffix, drop `get_`/`set_`
   prefix for property-style accessors, swap cross-domain object prefix).
3. Look up the variants in the Python index, keyed by `<py_domain>::<leaf>`.
4. Hint a likely MCP namespace via `MCP_DOMAIN_HINTS`; substring-match tool
   names within that namespace.

The matcher is intentionally fuzzy on the MCP side — MCP tools often
aggregate several C functions into one workflow tool, so the MCP column is
treated as advisory unless tightened via `overrides.tsv`.

## Status enum

| Status | Meaning |
|---|---|
| `parity` | C function has a Python entry point and at least one MCP tool. |
| `py-gap` | C function has no Python wrapper. Drives Phase 1 work. |
| `mcp-gap` | Python exists but no MCP tool. Drives Phase 2–3 work. |
| `mcp-gap-2d` | Same as `mcp-gap` but the C function is in the `2d` domain, which is build-conditional on `OPENSWMM_BUILD_2D=ON`. MCP registration must skip it on non-2D builds. |
| `intentional` | C function is deliberately not exposed in Python (e.g. memory-management helpers superseded by GC, error introspection superseded by `EngineError`). Note column gives the reason. |
| `internal` | Build/dev helper rather than a public API entry point. |
| `unknown` | Seed status; should never appear in committed matrices. CI fails on any. |

## `overrides.tsv` format

Tab-separated, three columns:

```
c_function<TAB>status<TAB>note
```

Lines beginning with `#` are comments. The `note` becomes the matrix row's
note column.

Pin a row to `parity` when the auto-matcher missed a real Python wrapping
under a different name (e.g. C `swmm_2d_triangle_count` → Python property
`Surface2D.n_triangles`). Include a `via:` note pointing to the actual
Python attribute so future readers don't re-flag it.

## CI integration

```
python3 docs/parity/tools/build_matrix.py --check
```

Exits non-zero if any row carries the `unknown` status, which would mean
new C functions landed without classification. The CI policy is **not** to
fail on `py-gap` / `mcp-gap` counts — those are tracked over time as
remediation work proceeds.

## Provenance builder (`build_matrix_provenance.py`)

`build_matrix.py` matches C→Python by *guessing* name variants, which on the
2026-07-06 tree produced **358 false `py-gap` rows** (every one of those C
functions is in fact wrapped). `build_matrix_provenance.py` replaces the guess
with the **actual call graph**:

- **C→Python is exact.** Each `.pyx` method body is parsed for the `swmm_*`
  symbols it calls; inverting gives the exact wrapper per C symbol. `py-gap`
  therefore means a genuinely unwrapped function (0 on the current tree, vs 358
  fuzzy). Use `--check` in CI — it gates on this reliable signal.
- **Python→MCP is name-precise, with an exact opt-in.** A C symbol is
  `parity` when one of its (call-graph-derived) wrapper method/class names is
  referenced in an MCP tool, or — authoritatively — when an MCP tool carries a
  `wraps:` marker naming the symbol. Rows without either are `mcp-review`
  (advisory *candidate*, not a confirmed gap, because MCP legitimately
  aggregates many C ops into one dispatching tool).

### The `wraps:` convention

To pin exact MCP provenance (and clear a false `mcp-review`), add a comment or
docstring line to the MCP tool naming the C symbols it dispatches to, all on one
line:

```
# wraps: swmm_forcing_link_flow swmm_forcing_node_lat_inflow swmm_forcing_node_quality
```

The builder collects these across `tools/`, `resources/`, `prompts/` and
promotes matching rows to `parity` with MCP evidence `exact`. See
`forcing.set_forcing` / `set_climate_forcing` for worked examples.

Outputs `provenance_matrix.md` + `provenance_gaps.json` (additive — it does not
touch the fuzzy `parity_matrix.md`).

## Updating the plan

The aggregate counts in `parity_matrix.md` should be cross-referenced
against the gap-closure plan at
`~/.claude/plans/the-c-api-for-declarative-falcon.md`. When a phase lands,
the corresponding rows transition from `py-gap` / `mcp-gap` to `parity`,
and the plan's effort tables can be marked off.
