# Manual figures

Every image any manual embeds has one row in `MANIFEST.tsv`, and every
generated figure has a generator module here that can rebuild it. The audit
(`scripts/build_manual_figures.py check`) is part of the manual lint and runs
in CI; the generators run locally and their outputs are committed.

```
docs/figures/
  MANIFEST.tsv        one row per embedded image (column contract: scripts/build_manual_figures.py)
  status.py           feature -> Implemented | Experimental | Planned | Retired, with colours
  style.py            vendored house style (palette, chrome, DPI)
  src/<manual>/*.py   generators: REQUIRES = (...); def build(sink)
  decks/<name>/       small in-tree models for the simulated tier (+ provenance.md)
  png/  svg/          committed outputs (PNG is embedded; SVG is for the paper)
  cache/              git-ignored engine runs
```

## Tiers

| tier | means | generator | committed output |
|---|---|---|---|
| `synthetic` | numpy/matplotlib only; may `REQUIRES = ("openswmm",)` for geometry | required | `png/<fig_id>.png` + `svg/<fig_id>.svg` |
| `simulated` | runs an in-tree deck under `decks/` through the engine first | required | same |
| `external` | image whose generator or data cannot live in-tree (e.g. the Bellinge hero) | `-` | the image, with `provenance` filled |
| `legacy` | EPA or hand-drawn figure kept as-is | `-` | the image, in its manual's media directory |

## Writing a generator

```python
REQUIRES = ()                        # ("openswmm",) when the engine's Python package is needed

def build(sink):
    import style, status             # docs/figures is on sys.path during build
    fig = style.figure(8, 4.5)
    ...
    sink.save(fig, "hydraulics_ch8_hll_fan")          # one call per manifest row this module owns
```

`sink.run("negative_pressure/negative_pressure.inp", options={"FV_PRESSURE_CLOSURE": "TPA"})`
runs a deck through the engine CLI (cached under `cache/`) and returns the
`.inp/.rpt/.out/.h5` paths. A module may own several rows (the conceptual map
owns five); the build fails if it produces more or fewer than the manifest lists.

Naming: `fig_id` is `<manual>_<chapter>_<slug>` (`hydraulics_ch8_hll_fan`,
`eng_conceptual_map`, `workflow_ch3_negative_pressure_head_14p1`). Embed it
as `![Figure n-m caption](figures/png/<fig_id>.png)` followed by an italic
`*Figure n-m caption*` line — the audit expects the figure number within five
lines of the embed.

## Commands

```bash
python3 scripts/build_manual_figures.py check                      # stdlib only; CI runs this
python3 scripts/build_manual_figures.py list --manual hydraulics
python  scripts/build_manual_figures.py build --tier synthetic     # needs matplotlib
python  scripts/build_manual_figures.py build --tier simulated --cli install/Darwin/bin/openswmm
python3 scripts/build_manual_figures.py run-decks --cli <openswmm> # every simulated deck runs clean
```

Requirements for `build`: matplotlib ≥ 3.8 (which brings Pillow) and numpy;
h5py for figures that read 2D output; the `openswmm` package for
geometry-backed figures. The engine-run tier finds the CLI through `--cli`,
`$OPENSWMM_BIN`, `$OPENSWMM_BUILD_DIR`, or a `build*/` / `install/*/` tree.

## Determinism and bounds

Outputs are committed. Re-running a generator on one machine is diff-free
(metadata stripped, `svg.hashsalt` fixed, DejaVu Sans bundled with
matplotlib); byte identity across matplotlib versions is **not** required and
CI never compares pixels — it audits the manifest and proves the synthetic
generators execute. PNGs are exported at DPI 160 and must stay within
1600 px width and 400 KB; keep figure widths ≤ 10 in.

## Interactive diagrams: hotspots and workflow links

`docs/custom/js/manual-interactive.js` (shipped by the Doxyfile, loaded by
`custom/html/header.html`) wraps two kinds of diagram in a pan/zoom
viewport at page load — drag to pan, click the diagram then scroll (or hold
Ctrl/⌘) to zoom, pinch on touch, double-click to reset, toolbar buttons —
and makes their parts clickable. Both degrade to the static image or
diagram without JavaScript.

**Figure hotspots.** A generator registers clickable regions in data
coordinates:

```python
sink.hotspot("eng_process_flow", ax, x0, y0, x1, y1, "hydraulics_ref_ch8_finite_volume", "finite volume")
```

`save()` converts each box to fractions of the saved PNG (top-left
origin) and `build` writes, after the figure's caption on every page the
manifest row lists, a block the script reads:

```
<div class="fig-hotspots" data-fig="eng_process_flow">
<span class="hs" data-box="0.0210,0.1234,0.4567,0.2345">@ref hydraulics_ref_ch8_finite_volume "finite volume"</span>
</div>
```

The `@ref` is resolved by Doxygen (and validated by the lint), so a target
that disappears fails the build rather than the click. Targets for the
conceptual map, the process-flow chart and the object sketch come from
`refs.py` (feature id or box title → page id or anchor); `primitives.pill_row`
returns each pill's box through its `out=` list. `check` C13 asserts every
block names a figure the page embeds and holds only well-formed spans;
the blocks are hidden by `manual.css`. Never hand-edit a block — rebuild
the figure.

**Workflow links.** A Mermaid flowchart's nodes open a section when the
block is followed *directly* by

```
</pre>
<div class="workflow-links" data-workflow="fv_substep">
<span data-node="D">@ref hydraulics_ref_ch8_lts "8.5.6 Local time stepping"</span>
</div>
```

The script injects a Mermaid `click` directive per node before rendering
(the `<!-- workflow: id -->` comment does not survive Doxygen, so adjacency
is the pairing). The lint's `check_mermaid_workflows` requires the id to
match, every node to exist in the diagram, and a flowchart (Mermaid has no
`click` on state diagrams). Point nodes at explicit heading anchors
(`### 8.5.6 Local time stepping {#hydraulics_ref_ch8_lts}`), never at the
auto-generated `autotoc_md` ids.

## Why legacy files are prefixed with their manual

Doxygen copies every image into one output directory by basename. The three
EPA reference manuals shipped with the same `imageNN.png` names for different
figures, so each page could be served another manual's figure. The migration
of 2026-09-19 renamed every colliding file `<manual>-<name>` and the audit
(C12) now refuses duplicate basenames across `IMAGE_PATH` directories.
