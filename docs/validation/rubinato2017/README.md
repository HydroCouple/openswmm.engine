# Rubinato 2017 review package

Prepared 21 October 2026; integrated into the manuals following review.

Latest follow-up (22 October): [momentum smoothing and full-table refinement report](momentum_report.md),
[three-step data](momentum_comparison.csv), and [complete plotted snapshot](momentum_data.json).
All 36 cases meet the final 1% timestep criterion (maximum 0.516%),
but 0/36 meet the illustrative experimental band. The reference and SWMMVis
manuals include the new comparison and convergence figures. The earlier assets
below remain the dated 21 October record.

- [Illustrated report](review.pdf) and [editable source](review.md).
- [All 36 steady measurements and model comparisons](steady_comparison.csv).
- [Experimental/simulated comparisons and animated results](../../manuals/reference/hydraulics/downloads/rubinato2017_results.html).
- [Printable visual supplement](../../manuals/reference/hydraulics/downloads/rubinato2017_visual_supplement.pdf).
- [Reusable PNG/SVG figures, viewer and plotted data](../../manuals/reference/hydraulics/downloads/rubinato2017_visuals.zip).
- [Computed summaries and binary identifiers](results_summary.json).
- [Hydraulic reference formulation](../../manuals/reference/hydraulics/sections/Chapter9-TwoDimensional.md).
- [Manual comparison page](../../manuals/reference/hydraulics/sections/Rubinato2017-Comparison.md).
- SWMMVis: `openswmm.gui/docs/validation/rubinato2017/example_draft.md` and
  five runnable models in `openswmm.gui/examples/rubinato2017/`.

The implementation's focused engine checks pass, and all 27 corrected
synthetic events pass the numerical checks. Experimental qualification is
incomplete: only 8/36 physical-shaft diagnostic rates fall within the
illustrative measurement band, five fail steady-window drift, and routing
step refinement changes representative rates materially. The original
junction reconstruction also has large continuity errors. No measured
transient series has been replayed. The report keeps these failures visible.

The hydraulic reference manual now includes the exchange formulation and a
comparison page with report/data downloads. SWMMVis T11 and the bundled
Welcome-page example use the same inputs. These source changes are ready
for the normal documentation build and release; experimental qualification
remains incomplete. The original PDF is retained as the dated review record.

The benchmark suite is `openswmm.engine.benchmarks/suites/coupling/`.
Its README lists build and reproduction commands. The dated review outputs
are retained locally at
`openswmm.engine.benchmarks/results/coupling/final-2026-10-21/`; focused
engine/GUI checks, source-page renders, refinement cases and example-output
checks are under `tests/verification/rubinato_final_2026-10-21/` in the
appropriate repository. These large run artifacts remain outside the
publication package.

The report generator accepts `--refinement` pointing at the retained
`routing_refinement/summary.json` to include the four routing-step checks.
Use `tests/benchmarks/rubinato2017/summarize.py`, then `render_review.py`.
Report figures are generated from data, not hand-edited.

The new visual supplement preserves the original PDF as the dated evidence
record. It adds all-row experimental/simulated parity and residual plots,
four routing-step comparisons, and actual saved synthetic T9 cell-depth
frames with native exchange samples. The offline viewer contains its data
and exports JSON. It never overlays invented experimental transient curves.

Rebuild the visual snapshot with
`tests/benchmarks/rubinato2017/build_visual_data.py --model-run [T9 output folder]`
(numpy/h5py). Rebuild the four figures with
`scripts/build_manual_figures.py build --only 'hydraulics_ch9_rubinato_*'`
(numpy/matplotlib), then run
`tests/benchmarks/rubinato2017/publish_visuals.py --gui-root [SWMMVis checkout]`
(reportlab/Pillow) to create the supplement and refresh both manual assets.
The snapshot records input/output and binary hashes. Replotting needs no
native engine and does not imply that new simulations have been performed.
The generator's `visuals/` staging folder stays local; the curated files under
the manual's `downloads/` directory are the versioned publication assets.

Reproduce the follow-up with `tests/benchmarks/rubinato2017/momentum_study.md`.
Its publisher is `publish_momentum.py --gui-root [SWMMVis checkout]`.
