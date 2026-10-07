from pathlib import Path
import json,statistics,collections,csv,math
p=Path(__file__).resolve().parent;med=statistics.median;rows=json.loads((p/'accuracy_results.json').read_text());diagnosis=json.loads((p/'diagnosis.json').read_text())
s='''# Phase 7: second-order topography reconstruction and long-wave accuracy

## Delivered

The CPU full-SWE second-order path now reconstructs water depth alongside free surface and velocity. The face bed is their difference. Previously it extrapolated free surface/velocity while keeping the bed piecewise constant, which left an artificial bed step even on a smooth, fully wet slope. An independent regression demonstrates the consequence: the old path lowers the advected depth and fails to return the physical mass flux of equal reconstructed flow states.

The new reconstruction preserves the existing dry-cell/dry-sill fallback and positivity budget. Its pressure correction includes the within-cell bed contribution, retaining the lake-at-rest balance. The previous piecewise-constant-bed kernel entry point remains available. First-order, local-inertial, diffusive-wave and transport fallback algorithms are unchanged. No input option or public API changed. Two additional depth-gradient blocks use 16 bytes per cell only for second order; they share the existing private gradient buffers, with no change to the solver object's layout.

The code and two regressions are applied to the working tree, **not committed**. The pre-existing quiescent-reactivation hunk and unrelated staged/unstaged work remain intact. This phase improves accuracy, with a measured CPU cost; it is not another speed optimization.

## Independent diagnosis

A fresh solver/source snapshot is identified in `baseline_commit.txt`, `baseline.diff` and `baseline_manifest.json`. The analytical formulas were checked against the [SWASHES compilation, section 4.2.2](https://arxiv.org/pdf/1110.0288). Both bowls are frictionless on a 4 m square with h0 = 0.1 m; the planar displacement is 0.5 m. Engine gravity is used consistently. The numerical initial depth is sampled at cell centroids and the represented bed comes from vertex elevations, so measured errors include spatial representation error.

Twenty fresh three-period diagnostic runs compare first order, second order, RK2 with spatial reconstruction disabled, four-times-smaller CFL, and much smaller dry/activation thresholds. Each uses 64 divisions, triangles or quads, one tier and one thread. Histories sample 64 times per period and track depth error, volume, energy, integrated momentum, centroid, mean squared radius, velocity error and wet-area classification.

Reducing CFL from 0.4 to 0.1 and lowering dry/activation thresholds from 1e-7/1e-6 m to 1e-10/1e-9 m barely changes the baseline error. Disabling spatial reconstruction while retaining RK2 brings the solution near first-order accuracy. This identifies spatial treatment as the dominant cause in these tests; it does not prove that temporal or shoreline errors are absent.

| Bowl / shape | First-order depth L1 | Second order | RK2, first-order space | Second order, CFL 0.1 | Second order, lower thresholds |
|---|---:|---:|---:|---:|---:|
'''
for case in ['radial','planar']:
 for quad in [0,1]:
  g={r['label']:r for r in diagnosis if r['case']==case and r['quad']==quad}
  s+=f"| {case} / {'quad'if quad else 'triangle'} | "+' | '.join(f"{g[k]['relative_l1']:.2%}"for k in ['first','second','rk2_first_space','cfl01','low_threshold'])+' |\n'
s+='''
## Discrete change and safeguards

For each active wet cell, the existing limited gradient pass now also forms a limited depth gradient. At a face, `h_face = max(0, h_cell + grad(h) · arm)` and `z_face = eta_face - h_face`. Hydrostatic reconstruction uses the maximum of the two reconstructed face beds. The pressure reference remains `h_ref = max(0, eta_face - z_cell)`, with each side's correction `0.5*g*(h_star² - h_ref²)*normal`.

That correction can be split into the ordinary hydrostatic face correction plus `0.5*g*(h_face² - h_ref²)*normal`, the within-cell bed contribution. Keeping the cell-bed reference prevents a reconstructed bed from introducing spurious acceleration into a constant-surface lake. For equal face depth/velocity on a smooth slope, the flux now uses that depth instead of a depth reduced by the difference between cell bed levels. On the tested flat-bed problems, the new path reduces to the previous flux exactly.

The two new tests check physical mass flux on a smooth slope with constant depth/velocity (triangles/quads, one/four threads), and the planar bowl's rotation phase after three periods. Both tests fail on the baseline and pass with the change. Existing dry-sill, positivity, source-budget and coupling tests are retained. The emerged-lake regression now covers both FLAT and VFR storage closures at both orders.

## Accuracy confirmation

56 baseline/candidate executions cover both bowls, 32/64/128 divisions, both cell shapes, one/three periods, plus ten periods at 64 divisions. All use second order, one tier, one CPU thread and identical 64-samples-per-period publication times. Every run is finite, retains nonnegative depths and conserves water. The moving-bowl refinement matrix uses FLAT storage; VFR is qualified here by the stationary emerged-lake regression. Triangular meshes contain twice as many cells as quadrilateral meshes at the same division count.

Relative depth L1 is the integrated absolute depth error divided by integrated exact depth at the final time. The full histories and maximum/time-mean errors are retained in `accuracy_results.json`; the choice is not based only on a visually favorable snapshot.

| Bowl | Divisions | Shape | Periods | Before depth L1 | Phase 7 depth L1 |
|---|---:|---|---:|---:|---:|
'''
groups=collections.defaultdict(dict)
for r in rows:groups[(r['case'],r['nx'],r['quad'],r['duration'])][r['variant']]=r
for k,g in sorted(groups.items()):
 s+=f"| {k[0]} | {k[1]} | {'quad'if k[2]else 'triangle'} | {k[3]} | {g['baseline']['relative_l1']:.2%} | {g['selected']['relative_l1']:.2%} |\n"
s+=f"\nMaximum absolute relative water-volume error is {max(abs(r['volume_relative_error'])for r in rows):.3g}. The 32-division, three-period radial quad case slightly regresses (21.64% to 21.90%); it remains in the table. Time-mean depth error improves in all {len(groups)} confirmation configurations, but that does not imply improvement of every observable.\n"
assert all(g['selected']['mean_relative_l1']<g['baseline']['mean_relative_l1']for g in groups.values())
s+='''
## Phase, energy and shoreline behavior

At 64 divisions after three periods, the planar centroid phase lag decreases from 29.9° to 5.8° on triangles and from 33.2° to 5.2° on quads. At ten periods, those lags decrease from 91.4°/111.5° to 14.8°/12.9°. These are centroid-angle errors at complete periods, not a general phase estimator for every field.

![Planar centroid, error and energy histories](comparison.png)

Mechanical energy is integrated as `area * [0.5*h*(u²+v²) + g*(0.5*h² + h*z)]`, with the bed datum at the bowl bottom. The plotted ratio is total energy relative to its initial value; it is not the fraction of oscillation amplitude retained. Numerical energy loss remains substantial. On the 64-division planar three-period case, triangles improve from 80.2% to 84.4% retained energy, while quads worsen from 83.1% to 79.6%. Thus the quad depth improvement is driven mainly by better phase, despite stronger damping. Several radial energy measures also regress.

Shoreline classification uses a 1e-6 m wetness threshold at cell centers and records the area where numerical/exact wetness disagrees. This is a cell-based proxy, not a geometrically resolved shoreline-distance error. Velocity L2 is weighted by exact water depth; the radial centroid angle is meaningless near the origin and is not used to assess radial phase.

The change is a targeted consistency and phase improvement. It does not solve long-duration damping or establish formal second-order convergence on arbitrary meshes. The 64-division planar ten-period errors remain approximately 44%/59%, so these runs must not be described as highly accurate.

## Correctness and build qualification

- **166 regression tests passed, one unavailable Kokkos backend test skipped**, across 15 suites. Both new regressions fail the baseline as expected.
- **72 compatibility executions:** all first-order histories/fields match exactly across the six analytical families, mesh shapes and one/four tiers. Second-order Ritter/Stoker flat-bed histories/fields also match exactly. Wet/dry lakes remain stationary to roundoff; second-order lake histories differ at roundoff and are checked by an absolute stationary-depth bound, not claimed bitwise identical.
- **Eight thread-reproducibility executions:** second-order radial/planar three-period histories and final depth fields match bitwise across one/four threads on both cell shapes.
- **56 accuracy executions**, plus 20 attribution runs and a 24-run initial screen. All final scientific values are finite. An earlier separate-vector prototype and the final packed-gradient implementation produce identical non-timing metrics in all 56 confirmation runs.

Component executables are isolated from the working build. For the full-engine suites, the initial reused-object attempt exposed incompatible frozen source/object layouts from unrelated 1D work. Those results were discarded. All 176 engine objects and the 15-source GeoPackage archive were rebuilt against one frozen support snapshot, and every test executable was rebuilt against matching headers. The final baseline/candidate libraries share these support objects and differ only in the solver object. Loaded library paths are verified for each suite. The support snapshot is the phase-5 source context, whose non-solver 2D code matches this phase's fresh snapshot; ongoing unrelated 1D changes are not validated by this review. No production 1D file was edited.

## CPU cost

Five randomized adjacent pairs per workload plus discarded warmups give 50 timed executions, 60 total. Diagnostic history calculations are removed from these executables. Runs use one CPU thread; no other review build/test ran concurrently. Host load is recorded and unrelated host activity can affect elapsed time. Advancement time excludes initialization; child CPU time includes it.

| Case | Divisions | Shape | Order | Periods / seconds | Before wall s | Phase 7 wall s | CPU cost change | Substep change |
|---|---:|---|---:|---:|---:|---:|---:|---:|
'''
perf=json.loads((p/'performance_results.json').read_text());pg=collections.defaultdict(dict)
for r in perf:pg[(r['case'],r['nx'],r['quad'],r['order'],r['duration'])].setdefault(r['variant'],[]).append(r)
for k,g in pg.items():
 b=g['baseline'];f=g['selected'];bw=med(r['seconds']for r in b);fw=med(r['seconds']for r in f);bc=med(r['cpu_seconds']for r in b);fc=med(r['cpu_seconds']for r in f);bs=med(r['steps']for r in b);fs=med(r['steps']for r in f)
 s+=f"| {k[0]} | {k[1]} | {'quad'if k[2]else 'triangle'} | {k[3]} | {k[4]} | {bw:.4f} | {fw:.4f} | {fc/bc-1:+.1%} | {fs/bs-1:+.1%} |\n"
s+='''
The second-order bowl cases cost about 11–16% more CPU. The flat-bed second-order control also pays for the extra gradient variable even though its trajectory is unchanged. The unchanged first-order algorithm shows a small timing regression on this host; no first-order speedup is claimed. Two extra scalar gradient blocks cost 16 bytes per mesh cell when second order is active (about 15.3 MiB per million cells). There are no new per-face heap allocations.

## Next numerical phase

Focus on the remaining dissipation: quantify oscillatory energy above equilibrium, fit radial width amplitude/phase, and separate wet/dry fallback, limiter and Riemann-flux contributions. Any attempt to reduce damping must retain lake-at-rest balance, positive depth, conservation, stable dry-sill behavior and the improved phase, and include distorted/mixed-mesh convergence tests. An idle-host repeat of CPU controls and eventual GPU qualification remain separate follow-ups.

## Reproduction and artifacts

`prepare.py`, `prototype.py`, `finalize_candidate.py` and `pack_gradients.py` record the isolated source evolution. `build.py`/`build_variant.py` build component executables. `build_engine.py`, followed by `rebuild_support.py` and `rebuild_geo.py`, constructs the final matched engine pair and suites. `diagnose.py`, `screen.py`, `confirm_accuracy.py`, `compatibility.py`, `check_regressions.py`, `prepare_performance.py` and `performance.py` record the run protocols. Source/build manifests and raw JSON/CSV evidence remain local review artifacts. `selected.patch` contains only this phase's production/test edits. `final_verification.json` records source hashes, counts and uncommitted status. `comparison.png` and `comparison.svg` visualize the same recorded histories.
'''
(p/'REPORT.md').write_text(s)
