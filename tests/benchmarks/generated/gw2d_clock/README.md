# Groundwater clock qualification fixtures

Phase 0 measures the shared surface/GW ladder. Phase 1 adds opt-in independent
stepping to the first-order CPU marcher. The reference runs below use `GW_CLOCK SHARED`,
the engine default.

`storm_mixed.inp` is a closed two-triangle pan with SIGMA and CLOSED_FORM columns,
a synthetic ten-minute 120 mm/hr rain pulse, and a one-hour recession horizon.
The near-saturated starting state exercises infiltration headroom exhaustion. The
Phase 1 runner also adds a 0.2 L/s groundwater well to the bulk cell as a separate
control for measurable physical Dunne return. `stiff_column.inp` has one very conductive, near-saturated
SIGMA column next to an ordinary CLOSED_FORM column on a one-metre mesh. Both
are numerical stress fixtures, not calibrated soils. They are starting points
for later accuracy gates, not evidence those gates have passed.

Run the Phase 0 comparison from the repository root:

```sh
python3 tests/benchmarks/scripts/gw_clock_baseline.py \
  /absolute/path/to/baseline/openswmm \
  /absolute/path/to/candidate/openswmm \
  tests/output/gw2d_clock_<date>/qualification \
  --bellinge /absolute/path/to/BellingeSWMM_v021_gw_quality.inp
```

Use an interpreter with `numpy` and `h5py`. Retain the baseline engine library,
not only the CLI; its loader path must point to the retained library. The runner
requires different baseline/candidate library hashes and a new output directory.
It also runs the existing `soa_sigma`, `soa_gw_species_heat`, and `mixed_water_t1`,
`t4`, `t8` fixtures. Bellinge is explicitly a **300-second startup slice** with
one thread. It does not establish full-event performance or a storm distribution.
Original model/dependency hashes and deliberate edits are recorded; dependencies
are linked read-only as inputs and all generated outputs stay in the run directory.

For the **complete two-day Bellinge event**, pass `--full-bellinge` to the same
command and use a fresh output directory. This runs only Bellinge. It keeps the
authored dates (172,680 simulated seconds), eight threads, rain, mesh, report start,
15-minute output interval and field selections. Only the CPU backend and local
output path are prescribed. The profiled build runs first, then the baseline and
unprofiled candidate. Each completed side writes its own `measurement.json`.
`RESULTS.md` reports the physical profile as soon as it exists and marks parity
as verified only after the two control runs and all comparisons pass. Regenerate
it with `python3 tests/benchmarks/scripts/gw_clock_full_report.py OUTPUT_DIR`.

For each fixture the runner compares the baseline, candidate with profiling off,
and candidate with profiling on. `.out` bytes and every HDF5 dataset's dtype,
shape and bytes must match. Report contents must match except for the three
wall-clock footer lines; reports are compared as bytes because some report
units use non-UTF-8 characters. HDF5 container metadata is not a byte-identity
gate. Elapsed times are diagnostic, not speedup acceptance; use repeated isolated
runs for performance conclusions.

`OPENSWMM_PERF=1` enables `[GW-CLOCK]`, `[GW-CLOCK-TIERS]`, and
`[GW-CLOCK-CLOSURE]` diagnostics on stderr. `OPENSWMM_GW_CLOCK_CELLS=1` additionally
emits each covered cell's firing count/rate. Profiling off allocates no per-cell
diagnostic storage and emits no clock records. No report, public API, physics,
checkpoint format, or hotstart format changes are needed for these diagnostics.

- `firings` counts completed kernel cell updates on the retained solver object;
  source-knot cuts each count separately. `macro`, `tail`, `direct`, and `rk2`
  identify the calling path. RK2 records are retained-stage observations, not
  accepted whole-step counts or a measure of all attempted work.
- `batches` counts nonempty tier calls cumulatively across re-tiering. The legacy
  `tierFirings()` counter still resets at each re-tier and is left unchanged.
- `overfire` bins `dt_stab / actual_interval` into `<1`, `[1,2)`, `[2,8)`,
  `[8,32)`, `[32,128)`, `[128,512)`, `[512,2048)`, and `>=2048`.
- `zone_ratio` bins `dt_g / dt_u` once per covered cell per stability refresh
  into `<1/16`, `[1/16,1/4)`, `[1/4,1)`, `[1,4)`, `[4,16)`, and `>=16`.
  Sentinel bounds (`>=1e29`) are counted separately, not called infinite speedup.
- Diagnostics are not serialized. A fresh solver begins a new observation;
  restoring a checkpoint into an existing solver does not rewind observations.
  Existing `ProcessPerf` timers separately include attempted trial work. Do not
  equate those timers with retained-path firing counts on rollback/RK2 workloads.

The runner verifies all firing-origin, closure, histogram and per-cell totals,
including covered-cell times rebuild counts. A complete result is `results.json`
with all eight fixtures (seven without Bellinge). An interrupted/failed comparison
does not qualify omitted fixtures, even when earlier rows passed.


The standalone A1.3 prototype uses the real six-cell groundwater, Darcy, node
and tracer kernels. Build the `gw2d_clock_gate` target, then run
`ctest --test-dir BUILD -R '^gw2d_clock_gate$' -V`. The gate retains its partial
checkpoint and event trace under `tests/output/gw2d_clock_gate/`.
The trace identifies each owner, its actual interval, and the latest external
producer time eligible for that gather (-1 means lateral packets only).

For Phase 1 window comparisons run `python3 tests/benchmarks/scripts/gw_clock_phase1.py CLI NEW_OUTPUT_DIR`.
The runner compares float64 HDF5 fields at common report times. For the original adaptive
inputs the 10 s wet step caps the requested 30 s routing step at 10 s. Use `--fixed-windows`
to exercise measured 1, 10 and 30 s hydraulic steps. This sets wet and dry steps to 30 s,
sets the mesh infiltration interval explicitly to 10 s and disables variable hydraulic steps.
The runner verifies executed step counts and the coupled report's minimum, mean and maximum.
It checks a dry recession with a head boundary and deep loss, then compares head and cumulative
Dunne return on a storm with a live node bed and conduit. The authored surface maximum and CFL
number are both halved in the refinement run. A second run uses a 60-second surface ceiling.
The runner retains the numeric tables and returns failure for a missed acceptance bound.
It also checks the one-hour dry recession against shared-clock head and runs a ten-minute storm
followed by 24 hours without rain, with deep loss and climate-driven evapotranspiration.

### Production targeted policy and A2 qualification

`INDEPENDENT` now includes directional receipt budgets, native head-rate limits and
absolute capacity/check timing. `GW_BUDGET_FRACTION` and `GW_HEAD_TOLERANCE` default to
0.5 and 0.05. `SHARED` remains the default. The production binary does not use the
`GW_STUDY_*` switches below.

```sh
python3 tests/benchmarks/scripts/gw_clock_phase1.py "$PRODUCTION_CLI" "$OUT/windows" --fixed-windows
python3 tests/benchmarks/scripts/gw_clock_qualification.py "$PRODUCTION_CLI" "$OUT/producer" --suite producer
python3 tests/benchmarks/scripts/gw_clock_qualification.py "$PRODUCTION_CLI" "$OUT/envelope" --suite envelope
```

The first command retains the original cross-window allowances. The producer suite
compares each routing window to its own 0.125 s producer-step reference at actual
surface ceilings of 1, 0.5 and 0.25 s, retaining executed counts and mean step lengths.
Unlike the earlier diagnostic experiment, these production runs change the authored
surface ceiling; the independent base remains 1 s. Undefined relative errors against
zero return are JSON null; absolute max/RMS errors are still tested. A 1e-12 absolute
comparison allowance covers floating-point equality, without changing the accuracy limits.
The envelope suite runs 1, 10, 60, 600 and 3600 s groundwater ceilings and all nine
budget/tolerance pairs from the plan on the sustained positive-overflow storm. It
reports both default accuracy and monotonic convergence; a failed gate exits nonzero.

### Broader Phase 2 accuracy and stale-head qualification

```sh
python3 tests/benchmarks/scripts/gw_clock_accuracy.py "$PRODUCTION_CLI" "$OUT/accuracy"
python3 tests/benchmarks/scripts/gw_clock_accuracy.py "$PRODUCTION_CLI" "$OUT/accuracy_half" --surface-max-step .5
python3 tests/benchmarks/scripts/gw_clock_node_accuracy.py "$PRODUCTION_CLI" "$OUT/node"
python3 tests/benchmarks/scripts/gw_clock_node_accuracy.py "$PRODUCTION_CLI" "$OUT/node_fine" --gw-base-step .125
```

The accuracy runner uses the authored SIGMA, mixed-closure and species/heat fixtures,
FLOAT64 output, the plan's one-second independent reference, two finer reference
ceilings, and all nine beta/head-tolerance pairs. It records errors by closure and
species, physical soil temperatures, canonical water/species/energy residuals,
budget counters and return depths. `--surface-max-step` is an explicit producer
refinement control; the groundwater base defaults to one second. All input and result
files are retained. The one-second reference can itself have appreciable error.

`node_saturation.inp` has a pipe feeding a charged storage node whose bed fills an
aquifer cell to the kernel's native saturation guard. The node runner verifies
actual 1/10/30 s hydraulic windows and compares the complete per-window cumulative
transfer differences, event totals, saturation guard and early budget updates.
An accurate event total does not imply accurate per-window transfers.

Both runners exit nonzero for failed accuracy gates. The corrected October 31
head sweeps pass. Native head rates now include internal column recharge, and
the producer check cap covers occupied base rungs and the first coupled check.
An authored finer groundwater ceiling also refines delivery to its selected
ceiling period, avoiding an entire coarse producer packet in one fine substep.
Physical stiffness alone retains internal subcycling and the base producer
cadence. The independent layered temperature view is refreshed after the table
commit; physical energy remains in the thermal owner.

The one-second node-window gate still fails with the default one-second base.
After the thin-column storage correction, the explicit .125 s base has 0.035%
per-window L2 node error against legacy SHARED, but its last small packet differs
by about 7.14% and fails the strict individual-window gate. A separately retained
SHARED diagnostic with the same storage law matches every packet exactly at all
three windows. The user-approved A6 amendment now uses that matched-physics
reference for node accuracy, retaining the failed legacy check as compatibility
evidence. All 18 accuracy gates pass at explicit base .125 s; the numeric limits
remain 2%. The one-second node base is still unqualified.

Select the matched reference explicitly with
`gw_clock_node_accuracy.py ENGINE OUTPUT --gw-base-step .125 --reference-engine REFERENCE_ENGINE`.
The runner also executes production SHARED and saves its metrics and failed gates
under `legacy_compatibility`. Without the reference option it retains the original
legacy comparison and failure exit status. The reference's build/patch provenance
and exact production-INDEPENDENT equivalence must be retained. Approved saved-run
reanalysis is under `tests/output/gw2d_clock_2026-10-31/node_reference_amendment/`.

`matched_node_reference.patch` is the minimal diagnostic change for a separate
reference build: both SHARED and INDEPENDENT use actual sub-millimetre recharge
geometry. Apply it only to a separate reference source copy, build its CLI, and
pass that CLI as `REFERENCE_ENGINE`. Record its source/patch/library hashes and
verify its INDEPENDENT node outputs are identical to the production CLI. This
reference is qualified for the node fixture; it is not a production mode or a
replacement for the SHARED exact-output regression suite.

The eight near-zero-return producer-convergence failures are resolved at unchanged
limits. INDEPENDENT uses actual sub-mm storage geometry with the existing recharge
rate guard; SHARED retains the legacy law and exact output parity. The explicit
stiff-column experiment passes nominal surface-step identity with no aquifer, but
the producer cap adds 25% more accepted surface steps in that fixture. Full-event
performance qualification remains open. SHARED remains the default.
Independent model identity v6 / transport envelope v6 refuse older independent
records; the scheduler record stays v2. Current evidence is under
`tests/output/gw2d_clock_2026-10-31/return_convergence/`; the preceding timing
results, failed rows and fixed-timestamp controls remain in `phase2_timing/`.

### Paired performance qualification

`gw_clock_performance.py ENGINE OUTPUT --suite soa --repeat 3` measures all five
SIGMA, species/heat and mixed t1/t4/t8 fixtures. Use `--suite bellinge --bellinge
INPUT --repeat 1` in a separate output directory for the original full event.
The runner preserves authored threads, dates and report cadence, selecting only
CPU backend, local output path and clock mode. Each mode has separate profiled
and unprofiled executions with exact output/report/HDF5 parity. Process timers
and physical firing counts are retained alongside unprofiled wall times.
Completed measurements resume only with identical script, build and inputs.
Run measurements sequentially from a stable engine copy; overlapping work
invalidates timing comparisons.

The October 31 evidence is under
`tests/output/gw2d_clock_2026-10-31/performance_qualification/`. Five smaller cases
are complete; the four-run full Bellinge comparison is in progress. Inspect its
`bellinge/progress.json` and console log before launching another benchmark.
`gw_clock_performance_report.py OUTPUT_ROOT` verifies saved evidence and refreshes
the dated `RESULTS.md`/`verification.json` snapshots. Partial runs do not qualify
the 10x GW-work or 2% surface-work targets. The approved matched-physics node
accuracy result and the failed legacy compatibility result are both reported.

The patches below are historical diagnostics against the pre-integration Phase 1
source. They are not patches for the current targeted-policy implementation. The
retained `tests/output/gw2d_clock_2026-10-31/held_error/portable/` source and executable
are the reference for the historical held-head study.

### D-10 junction-bed timing experiment

The test-only [instrumentation patch](bed_timing_study.patch) records the accepted
water transfers of the single junction bed in the coupled storm. Apply it to a
separate copy of the Phase 1 engine source and build the CLI in that copy.
The patched binary is limited to this water-only, aquifer-to-junction fixture.
Use a normal Phase 1 CLI to generate the production reference first.

Run from the repository root with fresh output directories. The first command
retains the reference measurements and exits with a failure status while the
production window-sensitivity gate is open.

```sh
mkdir -p "$OUT"
python3 tests/benchmarks/scripts/gw_clock_phase1.py "$PRODUCTION_CLI" "$OUT/reference"
python3 tests/benchmarks/scripts/gw_clock_bed_study.py "$STUDY_CLI" "$OUT/refined" \
    --production-reference "$OUT/reference" --cfl 0.35
python3 tests/benchmarks/scripts/gw_clock_bed_study.py "$STUDY_CLI" "$OUT/timed" \
    --production-reference "$OUT/reference" --cfl 0.35 \
    --events-from "$OUT/refined/live/coupled_INDEPENDENT_1_0.35/bed.csv"
```

Repeat at `--cfl 0.7` for the coarse surface step. Add `--capacity-clock`
to sample aquifer infiltration capacity after the surface check crossing each
`INFIL_STEP`. Use `--replay-from` to keep the original water volumes in this
comparison. Each run records the dated bed volume and the time the surface
delivered it. The runner checks the complete replay against the original,
compares unmodified controls with production at every report, and records
additional early cell updates and the head and return differences. A
`complete` result means all interventions ran and passed the journal checks.
The `within_2x_production_shared` fields report the numerical acceptance tests.
The bed study inherits the hydraulic setup used by its production reference.

### Recession head at update and report times

Build the [held-head study patch](held_head_study.patch) in a separate copy of the
Phase 1 engine. Apply it directly to the engine sources, separately from the
junction-bed study patch. Use a fresh output directory for each command.

```sh
python3 tests/benchmarks/scripts/gw_clock_held_error.py "$STUDY_CLI" "$OUT/day" \
    --case long_recession --capacity-clock
python3 tests/benchmarks/scripts/gw_clock_held_error.py "$STUDY_CLI" "$OUT/dry" \
    --case dry --capacity-clock
GW_STUDY_POLICY=native GW_STUDY_CAPACITY=1 python3 tests/benchmarks/scripts/gw_clock_phase1.py \
    "$STUDY_CLI" "$OUT/targeted" --fixed-windows
python3 tests/benchmarks/scripts/gw_clock_phase1.py "$PRODUCTION_CLI" "$OUT/ceiling600" \
    --fixed-windows --gw-max-step 600
```

The recession runner uses the 10 s routing window and reproduces every HDF5 head
from the accepted update journal before comparing clocks. It retains the signed
error components at each report, all independent updates and their matched
shared states, and the relative L2 norms. Each run includes the original 3600 s
ceiling, a uniform 600 s control and a targeted 3600 s trial.

The default study policy (`native`) uses the held head gradient and deep-loss
rate to select a local period, then checks incoming and outgoing reservations
separately after delivery. The budget fraction is 0.5, the head floor is 0.01 m,
and the default relative head tolerance is 0.05. `--capacity-clock` also publishes
the aquifer infiltration capacity on the first surface check crossing each
absolute `INFIL_STEP` boundary. The study executable assumes one model per process.
Its experimental predictor and event journals are intended for qualification runs.

For the producer-timing comparison, add `GW_STUDY_ALIGN=late` to the targeted
matrix command. In the wet coupled marcher this requests an absolute check
when a negative groundwater rung is occupied and the surface step exceeds the
check period. `GW_STUDY_CHECK_TRACE=1` writes the actual producer times.
`GW_STUDY_SURFACE_CHECK` can shorten this experimental period for a physical
step-refinement comparison while keeping `GW_BASE_STEP` fixed.


### Controlled ceiling and producer timing study

The production qualification runner also provides `--suite controlled`. This suite
requires a separate binary built with [controlled_timing_trace.patch](controlled_timing_trace.patch)
applied to the current production source. The patch is passive when its timing
switch is unset. It writes `producer_checks.csv` and optionally positive-return
updates; it assumes one first-order CPU model per process.

```sh
python3 tests/benchmarks/scripts/gw_clock_qualification.py "$CONTROL_CLI" "$OUT/controlled" --suite controlled
```

The suite retains the authored surface ceiling and varies `GW_MAX_STEP` over
1, 10, 60, 600 and 3600 s at each of four absolute producer intervals (1, .5, .25,
.125 s). Its `GW_CONTROL_STEP` switch caps only executed advance targets in the
study binary. Every timestamp must match the prescribed grid and the other
ceiling runs; mean step and count alone are insufficient. The check journal may
also include the actual clock margin as a second comma-separated column.
`GW_CONTROL_COMMITS=returns` records positive Dunne updates; `all` records every
cell update. Neither switch exists in the production engine.

Reviewable build and verification drivers and the failed constant-margin control
are retained under `tests/output/gw2d_clock_2026-10-31/controlled_ceiling/`.
The historical fixed-timing study identified the ceiling-dependent physical/accuracy
margin. Its constant-margin prototype remains experimental because it fails the long-gap
receipt gate. The current production scheduler instead separates the ceiling constraint
and checks every coarse owner's hard deadline; both ceiling sweeps and the original
receipt gate now pass. Current artifacts are under
`tests/output/gw2d_clock_2026-10-31/margin_separation/`. Its near-zero-return
failures were subsequently traced and resolved by the thin-column correction;
current results and limits are recorded above.
