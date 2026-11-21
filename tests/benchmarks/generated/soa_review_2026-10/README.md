# SoA/vectorization review — Phase 0 process baselines

This is the measurement stage of
`plans/workplans/SOA_VECTORIZATION_REVIEW_2D_GW_PROCESS_PLAN_2026-10-23.md`.
No numerical method, storage layout, checkpoint version or plugin ABI changes here.

## Fixtures and provenance

`provenance.json` records the source fixture hashes and deliberate edits. The
four small `.inp` files are self-contained and registered in `tests/parity/MANIFEST`:

| Fixture | Reached path |
|---|---|
| `soa_sigma.inp` | Water-only SIGMA column, ponded surface and authored node bed |
| `soa_gw_species_heat.inp` | SIGMA, two pollutants, porous temperature, layered UNSAT inventories, dispersion and physical porewater mapping |
| `soa_ueb_mesh.inp` | Warm UEB mesh snow with explicit radiation series |
| `soa_richards_lid.inp` | Native Richards LID storage, supplied pollutant and dynamic-wave drain |

The water-only SIGMA deck is close to equilibrium. The layered heat deck is the
non-equilibrium observer of repeated SIGMA evaluations and porewater-map work.
Normal `.inp` loading refuses common-source ownership, so that workload lives
beside the reviewed internal fixture in
`tests/unit/engine/test_common_source_stage.cpp`, test
`CommonSourcePerf.CompletedIntervals`. Its full component checkpoint is the
exact-state artifact; a fabricated CLI deck would not observe the same path.

## Reproduce the gates

Build the baseline before editing the engine and retain its **engine library**
as well as its CLI. On macOS verify/relocate the CLI's RPATH to the retained
library; copying only the CLI otherwise loads the rebuilt engine. Keep build
options and floating-point flags matched. The usual corpus runner checks the
two caches. Run from the repository root with absolute CLI/output paths:

```sh
bash tests/parity/run_corpus.sh <base-cli> <patched-cli> <fresh-output-dir>
python3 tests/benchmarks/scripts/soa_review_baseline.py \
  <base-cli> <patched-cli> <another-fresh-output-dir>
```

The Python interpreter needs `h5py` and `numpy`. The companion gate stages
inputs and outputs separately for every side/repeat, forces the CPU backend,
enables all 2D output fields, and compares exact `.out` hashes and every HDF5
dataset's type, shape and values. It checks both profiling off and on, refuses
identical engine libraries, invalid profiling totals and existing output directories, and saves run logs,
hashes, phase counters and wall times. HDF5 container bytes/attributes are not
the field-identity gate. Default repeats: three. `--fixtures` and `--pattern`
select separately prepared large cases. For throughput qualification use excluded
warmups (`--warmup 1`) and repeated measurements (`--repeat 5` or more); the
baseline/candidate profiling-off order alternates between repetitions.

For the internal common-source workload, run the test executable built against
each engine, setting `OPENSWMM_SOA_BENCH_OUTPUT` to a separate retained directory.
The workload runs without `OPENSWMM_PERF` too. Compare `common_source.chk` bytes;
then repeat with profiling enabled. Normal unit-suite runs skip this benchmark.

S1's scaling workload is `AquiferBedLookup.PerformanceWorkload` in
`test_2d_aquifer.cpp` (CMake target `test_engine_2d_aquifer`). Set
`OPENSWMM_SOA_S1_BENCH=dt` for 2,048 independent columns with 8,192 interleaved
node beds, or `fire` for 1,024 columns with 4,096 beds. Set
`OPENSWMM_SOA_S1_REPEATS=2048` for the measured-duration runs; normal unit runs
skip this workload. Filter to that test and compare its `bench_dt.chk` or
`bench_fire.chk` under the aquifer output directory across builds and profiling
off/on. Report whole test-body time, including setup/checkpoint writing, as a
targeted workload measurement. Build each helper against its engine's matching
internal headers; S1 adds derived scratch ownership to the internal solver class.
The retained S1 runner, phase splits and independent review are referenced by
`SOA_VECTORIZATION_PHASE1_S1_RESULTS_2026-10-22.md` in `plans/workplans/`.

`prepare_soa_review_cases.py --surface <bench2d_79202.inp> --bellinge <input.inp>
--twinoaks <input.inp> --output <new-directory>` creates the large model slices,
dependencies and provenance without changing the original files. The surface
matrix retains 20 minutes of the original rain (including wetting activation),
both LI/FULL_SWE modes and THREADS 1/4/8. Bellinge is a five-minute slice and
TwinOaks a ten-minute slice. These cuts are recorded explicitly; do not compare
them to timings of the full authored simulations. The local `large/` folder
contains generated model copies and dependency data, not additional hand-edited
source fixtures.

## Timers and allocation probe

With `OPENSWMM_PERF=1`, engine close prints `[PERF-2D]`, `[PERF-GW2D]`,
`[PERF-HYD]` and `[PERF-PROCESS-COUNTS]`. Times are seconds, `n.*` are scope
entries, and SIGMA histograms include the final capture evaluation. Copy counts
count actual attempted snapshots, including empty snow objects. Map-refresh
counts include calls that return without configuring a map. Counters survive
solver rollback. The process counters require isolated engine lifetimes. A fresh
file-opened or programmatically finalized engine resets them; overlapping live
engines invalidate the entire cohort until all close. Such runs emit
`[PERF-PROCESS-INVALID]` instead of process totals and cannot qualify a profile.

Timers use the existing `GatedTimer` nested self-time convention on each thread.
Parallel face- and cell-species times aggregate worker seconds: do **not** add those to
coarse wall phases and call the sum elapsed wall time. Fine-grained enabled
timers add overhead; use native sampling for optimization ranking, and profiling
off for throughput acceptance. The disabled path reads no clock, allocates
nothing, and performs no counter atomics. Water-only faces return before the
species timer is constructed.

The optional macOS helper counts successful malloc/calloc/realloc/aligned
allocation requests on the calling thread. It is loaded only into profiling
runs; the production engine does not replace the allocator:

```sh
clang++ -std=c++20 -dynamiclib tests/benchmarks/scripts/process_malloc_probe.cpp \
  -o <retained-dir>/process_malloc_probe.dylib
python3 tests/benchmarks/scripts/soa_review_baseline.py \
  <base-cli> <patched-cli> <fresh-output-dir> \
  --malloc-probe <retained-dir>/process_malloc_probe.dylib
```

`[PERF-ALLOC]` reports inclusive allocation calls and requested bytes per scope,
not live memory, frees, or allocations on other workers. Nested scopes overlap;
do not sum them. The helper is macOS-only; native allocation profilers cover
other platforms. The timer tests exercise nesting, concurrent scopes, exception
cleanup, reset, disabled profiling and the optional probe's exact scope count.

## Qualification status

Initial results are under `tests/output/soa_review_2026-10-23/`; repeated final
measurements and raw validation are under
`tests/output/soa_review_qualification_2026-10-23/`. The local results and
independent-review notes in `plans/workplans/` record exact gates and limitations.
The supplied Bellinge pump `G71F12Pp1` incorrectly referenced the STORAGE curve
`G80F66Ybas`. Its original PUMP4 curve `G71F12Pp1` was restored with the user's
authorization, preserving every other byte of the input. The corrected five-minute
slice passes exact baseline/candidate output and HDF5 comparison. Repair provenance
is retained under `tests/output/soa_review_qualification_2026-10-23/`.
The Phase-0 measurement table, retained baseline hashes and exact-output gates are
complete. Concurrent host activity leaves throughput equivalence and optimization
speedup acceptance unproven; these measurements do not waive later phase gates.
