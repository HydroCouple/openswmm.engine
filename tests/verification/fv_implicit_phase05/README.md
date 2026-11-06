# Native nonlinear pressure-storage candidate — P0.5

`NonlinearPressureStorage` is an internal engine kernel, now connected through
a guarded native-only experiment in `ExplicitFvSolver`. Default production
routing does not select it. It solves actual SLOT area differences on a frozen
pressure graph using damped Newton and native Jacobi-PCG. No third-party solver
implementation is used. Every supplied cell and algebraic junction remains an
unknown; the kernel adds no node storage or artificial head pins.

Configure the strict-FP native build described in the
[P0 README](../fv_implicit_phase0/README.md), then enable unit tests with the
local Google Test configuration, for example:

```sh
cmake -S . -B build/fv-implicit-phase0 \
  -DOPENSWMM_BUILD_UNIT_TESTS=ON -DGTest_DIR=/absolute/path/to/share/gtest

python3 tests/verification/fv_implicit_phase05/run.py \
  --build-dir build/fv-implicit-phase0 \
  --output tests/output/fv_implicit_phase05/candidate
```

The output directory must be new. The standard-library runner builds the
candidate, adapter, four existing FV suites and four selected engine-integration
cases, checks strict FP flags, clears solver
environment overrides, requests one worker, and runs each suite in its own
persistent output directory. Logs and Google Test XML remain reviewable.
The manifest records source hashes before/after, compile commands, engine and
test-binary hashes, and source/artifact evidence copies. Those copies are not a
portable runtime bundle. A failed run keeps its evidence and failed manifest.
Source changes during build/copy reject a capture. Later checkout edits are
listed separately while the tested artifacts must retain their original hashes;
qualification identifies the retained snapshot rather than a subsequently edited
working tree.

The tests include manufactured filling/draining across the crown over celerity
and timestep sweeps, looped zero-storage junction systems at 4/8/16 cells per
branch, an independent dense linear oracle, closed-volume balance, datum shifts,
reversed edge orientation and explicit failure cases. Every successful candidate
is checked against a separate extended-precision continuity assembly. Failures
must preserve the caller's output bytes. Existing suites check production
pressure-implicit, network, TPA and datum-shift behavior.
The old cell area is a conserved input, independently of the initial head guess;
a regression prevents replacing it with a head-to-area reconstruction.

The adapter tests use the real pressure classification/coefficient assembly,
cell/node updates and water ledger. They check failed-solve nonpublication,
shared water/species-facing flux copies, rejection of a modified solved flux,
graph equilibrium/conservation and whole-window rollback, including a failure
after earlier explicit substeps succeeded. The Router checks reached time before
publishing final routing means/receipts. The manifest records the exact filter
for the engine-level integration subset; the entire integration suite is not run.

Native C++ experiments opt in with `solver.enablePressureStorageCandidate()`
after initialization. There is no input keyword, binding, GUI control or plugin
ABI addition. The experiment requires first-order global Euler, closed SLOT,
water-only operation, no LTS/compaction/UF, no storage/ponding or structures,
no gates/culverts, losses, finite reservoir caps or callbacks. Unsupported
combinations throw before the routing window mutates state. The ordinary
pressure path remains selected unless the native experiment is explicitly enabled.

This is a restricted storage-equation qualification. Conductances and predictor
sources are frozen inputs, not a full implicit momentum or Riemann residual.
TPA, real node storage, wet/dry/regime switches, moving-front accuracy,
ponding/flooding active sets, structures, coupled-process rollback,
GUI configuration and performance promotion remain workplan gates. The guarded
water-only path retries rejected pressure/endpoint steps and restores its whole
window on exhaustion. It does not supply a general engine/coupling checkpoint.
A small
residual proves only the supplied equations were solved within their budgets.

## Hydraulic qualification

`run.py` above captures kernel/adapter regressions. The separate hydraulic
runner builds `fv_pressure_qualification` under the same default-off baseline
option and tests physical accuracy as well as completion/conservation:

```sh
python3 tests/verification/fv_implicit_phase05/qualify.py \
  --build-dir build/fv-implicit-phase0 --profile full \
  --output tests/output/fv_implicit_phase05/hydraulic_review

python3 tests/verification/fv_implicit_phase05/review_qualification.py \
  tests/output/fv_implicit_phase05/hydraulic_review

python3 tests/verification/fv_implicit_phase05/secant_sensitivity.py \
  tests/output/fv_implicit_phase05/hydraulic_review \
  --output tests/output/fv_implicit_phase05/secant_review
```

The full matrix contains 111 native component runs: travelling waves, moving
filling fronts, crown draining, seeded steady Manning head loss and a one-step
zero-time momentum probe. Explicit, existing pressure and guarded candidate
modes retain the same physical cells. Component units are feet, cfs and seconds.
The independent audit recomputes wave metrics from cell states and checks saved
source/artifact/mesh/state hashes. `--profile smoke` provides a smaller capture.

The full runner also recovers four existing E2 engine baselines (25/50 m/s,
pressure OFF/ON) and the current implicit-UF valve regression. E2 retains its
tanks/weir and uses SI outputs; the guarded candidate does not support those
features and is not run on that full model. Observation provenance and gauge
location limits are recorded in the workplan report. The separate secant replay
uses the captured build and exact E2 input in fresh processes; unchanged live
artifact hashes are required. It records gauge sensitivity, not nonlinear
residual contraction. All outputs remain in new, persistent review directories.

**Captured is not qualified.** The 2026-10-19 full matrix completed and conserved
water, but both pressure modes fail a zero-time consistency probe: their shared
momentum averaging changes a smooth discharge mode by about 6.7% even as dt
approaches zero. Wave accuracy also fails the exploratory screen. Candidate
promotion is on hold pending a consistent fine-grid momentum update; thresholds
and production defaults were not changed. See the local
[hydraulic qualification report](../../../plans/workplans/IMPLICIT_FV_1D_P05_HYDRAULIC_QUALIFICATION_2026-10-19.md)
for retained evidence, reference limitations and the next gate.
