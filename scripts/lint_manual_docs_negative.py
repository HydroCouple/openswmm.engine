#!/usr/bin/env python3
"""Prove that every lint check can fail.

A gate is not a gate until it has been seen to fail. For each case below the
runner copies docs/ (text files copied, images hard-linked, generated html/
skipped), applies one mutation, runs scripts/lint_manual_docs.py --docs-root on
the copy and asserts that it exits 1 with the expected message. An unmodified
copy is run first as the positive control. Everything lands in a reviewable
directory, never a temp dir:

    tests/output/docs_lint_negative_<YYYY-MM-DD>/<case>/docs   (the mutated copy)
    tests/output/docs_lint_negative_<YYYY-MM-DD>/<case>.log    (the lint output)
    tests/output/docs_lint_negative_<YYYY-MM-DD>/SUMMARY.txt

Exit 1 if the control fails, or any case does not fail the way it should.
"""
import datetime as dt
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DOCS = ROOT / "docs"
LINT = ROOT / "scripts" / "lint_manual_docs.py"
IMAGE_EXT = {".png", ".jpg", ".jpeg", ".gif", ".svg", ".emf", ".wmf"}

# (case, file relative to docs/, old text, new text, expected substring in the lint output)
CASES = [
    ("duplicate_page",
     "manuals/engine/sections/Chapter3-Files.md",
     "@page engine_manual_ch3_files", "@page engine_manual_ch4_reports",
     "duplicate @page engine_manual_ch4_reports"),
    ("unresolved_ref",
     "manuals/engine/engine_manual.md",
     "@subpage engine_manual_ch1_conceptual_model", "@subpage engine_manual_ch1_nonexistent",
     "unresolved @ref/@subpage 'engine_manual_ch1_nonexistent'"),
    ("ragged_table",
     "manuals/reference/hydraulics/sections/Chapter9-TwoDimensional.md",
     "| Closure | Prognostic state | Face law | Valid regime | Cost | Status |",
     "| Closure | Prognostic state | Face law | Valid regime | Cost | Status | Extra |",
     "table row has"),
    ("raw_dollar_math",
     "manuals/engine/sections/Chapter1-ConceptualModel.md",
     "This chapter discusses how SWMM models",
     "This chapter discusses how $x$ SWMM models",
     "raw $ math"),
    ("image_missing",
     "manuals/engine/engine_manual.md",
     "figures/figure1_1_urban_sewershed.jpg", "figures/nope.png",
     "image not found"),
    ("section_undocumented",
     "manuals/engine/sections/Chapter2-InputFileReference.md",
     "### Section: [TAGS]", "### Section-gone: [TAGS]",
     "[TAGS] is registered but undocumented"),
    ("options_missing_key",
     "manuals/engine/sections/Chapter2-InputFileReference.md",
     "| FV_CFL | number | 0.5 |", "| FV_CFLX | number | 0.5 |",
     "[OPTIONS] keyword FV_CFL is parsed but not documented"),
    ("options_bogus_key",
     "manuals/engine/sections/Chapter2-InputFileReference.md",
     "| FV_CFL | number | 0.5 |", "| FOO_BAR | number | 1 | Invented. |\n| FV_CFL | number | 0.5 |",
     "[OPTIONS] documents FOO_BAR, which the parser does not accept"),
    ("options_2d_missing_key",
     "manuals/engine/sections/Chapter2-InputFileReference.md",
     "| CELL_CLOSURE |", "| CELL_CLOSUREX |",
     "[2D_OPTIONS] keyword CELL_CLOSURE is parsed but not documented"),
    ("component_missing_section",
     "manuals/engine/sections/Chapter2-InputFileReference.md",
     "### Component section: [HEAT_FLUXES]", "### Component section-gone: [HEAT_FLUXES]",
     "[HEAT_FLUXES] is parsed by a component but has no"),
    ("component_stray_bracket",
     "manuals/reference/quality/sections/Chapter8-MultiSpeciesReactions.md",
     "## 8.1 Introduction", "## 8.1 Introduction\n\nSee also [HEAT_BOGUS].\n",
     "[HEAT_BOGUS] is not a section any component parses"),
    ("message_missing",
     "manuals/engine/sections/AppendixA-Messages.md",
     "ERROR 101:", "ERROR 1010:",
     "ERROR 101 is in ErrorCodes.hpp but not in Appendix A"),
    ("figure_uncited",
     "manuals/engine/sections/Chapter1-ConceptualModel.md",
     "figures/fig3-03-storm-drain-inlet.png", "figures/fig3-03-storm-drain-inlet-renamed.png",
     "never embedded by engine_manual_ch1_conceptual_model"),
    ("manifest_row_deleted",
     "figures/MANIFEST.tsv",
     "eng_fig3_03_storm_drain_inlet\t", "# eng_fig3_03_storm_drain_inlet\t",
     "has no MANIFEST.tsv row"),
    ("manifest_wrong_page",
     "figures/MANIFEST.tsv",
     "\tengine_manual_ch1_conceptual_model\t1-5\t", "\tengine_manual_ch4_reports\t1-5\t",
     "which is not in its page_id"),
    ("manifest_figure_no_drift",
     "figures/MANIFEST.tsv",
     "\tengine_manual_ch1_conceptual_model\t1-5\t", "\tengine_manual_ch1_conceptual_model\t1-99\t",
     "no 'Figure 1-99' caption within 5 lines"),
    ("manifest_bad_tier",
     "figures/MANIFEST.tsv",
     "eng_fig3_03_storm_drain_inlet\tlegacy\t", "eng_fig3_03_storm_drain_inlet\tantique\t",
     "tier 'antique' is not one of"),
    ("manifest_generated_without_generator",
     "figures/MANIFEST.tsv",
     "eng_fig3_03_storm_drain_inlet\tlegacy\t", "eng_fig3_03_storm_drain_inlet\tsynthetic\t",
     "generated rows must embed figures/png/eng_fig3_03_storm_drain_inlet.png"),
    ("manifest_orphan",
     "COPY", "manuals/engine/figures/fig3-03-storm-drain-inlet.png", "images/stray.png",
     "images/stray.png is on disk but has no MANIFEST.tsv row (orphan)"),
    ("manifest_ambiguous_basename",
     "COPY", "manuals/engine/figures/fig3-03-storm-drain-inlet.png",
     "images/fig3-03-storm-drain-inlet.png",
     "basename fig3-03-storm-drain-inlet.png exists in 2 IMAGE_PATH dirs"),
    ("manifest_unrenderable",
     "COPY", "manuals/engine/figures/fig3-03-storm-drain-inlet.png", "images/legacy.emf",
     ".emf is not renderable by browsers"),
    ("bad_status_word",
     "manuals/reference/hydraulics/sections/Chapter9-TwoDimensional.md",
     "\\status{Implemented} |", "\\status{Shipped} |",
     "\\status{Shipped} is not one of"),
    ("status_table_plain_text",
     "manuals/reference/hydraulics/sections/Chapter9-TwoDimensional.md",
     "| \\status{Implemented} |", "| Implemented |",
     "Status column must hold exactly one"),
    ("doxyfile_missing_alias",
     "Doxyfile",
     '"status{1}=', '"statusx{1}=',
     "has no status{1} alias"),
    ("css_colour_drift",
     "custom/css/manual.css",
     "#1baf7a", "#1baf7b",
     "lacks the Implemented colour #1baf7a"),
    ("mermaid_id_missing",
     "manuals/engine/sections/Chapter5-ProgrammaticApi.md",
     "<!-- workflow: engine_lifecycle -->\n", "",
     "lacks a `<!-- workflow: id -->` line"),
    ("mermaid_id_duplicate",
     "manuals/engine/sections/Chapter5-ProgrammaticApi.md",
     "<!-- workflow: plugin_resolution -->", "<!-- workflow: engine_lifecycle -->",
     "workflow id 'engine_lifecycle' already used"),
    ("mermaid_piped_label",
     "manuals/engine/sections/Chapter5-ProgrammaticApi.md",
     "G[Load library directly]", "G[Load library|directly]",
     "mermaid node label contains '|'"),
    ("mermaid_unclosed",
     "manuals/engine/sections/Chapter5-ProgrammaticApi.md",
     "    K --> L[Plugin receives host callbacks during the run]\n</pre>",
     "    K --> L[Plugin receives host callbacks during the run]\n",
     "mermaid block is not closed"),
    ("mermaid_command_char",
     "manuals/engine/sections/Chapter5-ProgrammaticApi.md",
     "G[Load library directly]", "G[Load @ref library directly]",
     "mermaid line contains @ or"),
    ("app_deck_missing",
     "manuals/application/application.md",
     "## Introduction", "## Introduction\n\nDeck: `docs/figures/decks/nope/nope.inp`\n",
     "cites docs/figures/decks/nope/nope.inp, which does not exist"),
    ("workflow_link_unknown_node",
     "manuals/reference/hydraulics/sections/Chapter8-FiniteVolume.md",
     '<span data-node="D">@ref hydraulics_ref_ch8_lts "8.5.6 Local time stepping"</span>',
     '<span data-node="ZZ">@ref hydraulics_ref_ch8_lts "8.5.6 Local time stepping"</span>',
     "workflow-links node 'ZZ' is not a node of workflow fv_substep"),
    ("workflow_link_wrong_id",
     "manuals/reference/hydraulics/sections/Chapter8-FiniteVolume.md",
     'data-workflow="fv_substep"', 'data-workflow="fv_substep_x"',
     "workflow-links names workflow 'fv_substep_x' but follows the block 'fv_substep'"),
    ("workflow_link_detached",
     "manuals/reference/hydraulics/sections/Chapter8-FiniteVolume.md",
     "*Figure 8-5 Substep workflow",
     '<div class="workflow-links" data-workflow="fv_substep">\n</div>\n\n*Figure 8-5 Substep workflow',
     "workflow-links block does not directly follow a mermaid block"),
    ("hotspot_wrong_fig",
     "manuals/engine/sections/Chapter1-ConceptualModel.md",
     '<div class="fig-hotspots" data-fig="eng_object_sketch">',
     '<div class="fig-hotspots" data-fig="quality_ch9_heat_budget">',
     "fig-hotspots block for quality_ch9_heat_budget, but the page embeds no such figure"),
    ("hotspot_bad_box",
     "manuals/engine/sections/Chapter1-ConceptualModel.md",
     'data-box="0.0900,0.0768,0.2400,0.1375"', 'data-box="0.2400,0.0768,0.0900,0.1375"',
     "is not inside the image"),
]


def copy_docs(dst):
    """Copy docs/ without the generated site; images are hard-linked (never mutated)."""

    def copy_fn(src, dest):
        if Path(src).suffix.lower() in IMAGE_EXT:
            try:
                os.link(src, dest)
                return dest
            except OSError:
                pass
        return shutil.copy2(src, dest)

    def ignore(src, names):
        # only the generated site at docs/html is skipped; docs/custom/html holds the header template
        skip = {".DS_Store", "__pycache__"} | ({"html"} if Path(src) == DOCS else set())
        return [n for n in names if n in skip]

    shutil.copytree(DOCS, dst, copy_function=copy_fn, ignore=ignore)


SRC_ROOT = ROOT / "src"


def run_lint(docs_root):
    p = subprocess.run([sys.executable, str(LINT), "--docs-root", str(docs_root),
                        "--src-root", str(SRC_ROOT)],
                       capture_output=True, text=True)
    return p.returncode, p.stdout + p.stderr


def main():
    # --src-root pins the sources the coverage checks read. The default is this
    # working tree; pass a checkout of HEAD (git archive HEAD src | tar -x) when a
    # peer session has uncommitted parser edits, so the control run is not red for
    # a section that is not yet in the branch.
    global SRC_ROOT
    argv = sys.argv[1:]
    if argv[:1] == ["--src-root"] and len(argv) > 1:
        SRC_ROOT = Path(argv[1]).resolve()
    out = ROOT / "tests" / "output" / f"docs_lint_negative_{dt.date.today().isoformat()}"
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    summary = []
    failures = 0

    control = out / "control" / "docs"
    copy_docs(control)
    rc, log = run_lint(control)
    (out / "control.log").write_text(log)
    ok = rc == 0
    summary.append(f"{'PASS' if ok else 'FAIL'}  control  (exit {rc}, expected 0)")
    failures += 0 if ok else 1

    for name, rel, old, new, expect in CASES:
        dst = out / name / "docs"
        copy_docs(dst)
        if rel == "COPY":
            # (name, "COPY", source, destination, expect): plant a second copy of
            # an image — the mutation for orphan / collision / format checks.
            src, dest = dst / old, dst / new
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(src, dest)
            what = f"copy {old} -> {new}"
        else:
            target = dst / rel
            text = target.read_text(errors="replace")
            if old not in text:
                summary.append(f"FAIL  {name}  mutation anchor not found in {rel}: {old[:50]!r}")
                failures += 1
                continue
            target.write_text(text.replace(old, new, 1))
            what = f"mutate {rel}: {old[:60]!r} -> {new[:60]!r}"
        rc, log = run_lint(dst)
        (out / f"{name}.log").write_text(f"$ lint --docs-root {dst}\n# {what}\n"
                                         f"# expect: {expect}\n\n{log}")
        ok = rc == 1 and expect in log
        summary.append(f"{'PASS' if ok else 'FAIL'}  {name}  (exit {rc}, "
                       f"{'found' if expect in log else 'MISSING'}: {expect})")
        failures += 0 if ok else 1

    text = "\n".join(summary) + f"\n\n{len(CASES)} cases, {failures} failures\n"
    (out / "SUMMARY.txt").write_text(text)
    print(text)
    print(f"artifacts: {out}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
