# Phase 11: remaining gradient and cell-update costs

## Decision

**Retain no production change.** Five isolated prototypes did not establish a substantial, consistent CPU improvement over phase 10. The strongest candidate, limiting once per variable using face-extrapolation extrema, improved one larger triangular case by a median 2.5%, but quad, four-thread and frictional complete-model comparisons were nearly unchanged. Those results do not justify replacing the current implementation for this performance objective.

The solver remains byte-for-byte identical to the phase-11 starting working tree. The pre-existing rain-activation edit remains untouched. This phase leaves only review artifacts and rejected experimental patches. **One phase remains: phase 12, final cumulative qualification.**

## Fresh baseline and profile

Baseline commit: `59d808de10f43d973ad35c1ec501ff5a13a61d13`. Exact current source hashes are recorded in `baseline_manifest.json`; isolated builds use that implementation and the existing review harness. No earlier timing result was used to accept a prototype.

Fresh wall-clock profiles of 64-division, three-period planar/radial bowls placed gradient reconstruction at roughly 23–28% of advance time and cell updates at 18–20%. Face evaluation excluding gradients still accounted for roughly 38–44%. These profiles were taken under heavy host load and guide candidate selection; they are not uncontended CPU-cost measurements.

The initial 64-division screening job was interrupted after completed groups when host contention made it too expensive for exploratory screening. `screen64_interrupted.*` preserves those partial observations, which are excluded from the decision tables below. Screening restarted at 32 divisions. Larger confirmations then used 64 divisions.

## Prototypes evaluated

- **Skip nonbinding limiter divisions:** avoid evaluating a ratio when the face extrapolation already lies within its neighbor bounds.
- **Skip zero-roughness friction:** omit the positive-depth friction calculation when Manning roughness is exactly zero. This specifically targets friction-free analytical problems; it does not remove friction from other cases.
- **Avoid duplicate gradient clearing:** zero gradients only on first-order fallback exits rather than zeroing and then overwriting them for reconstructed cells.
- **Combined shortcuts:** test the interaction of the preceding three changes.
- **Face-extrema limiter:** preserve the rounded face extrapolations, collect their positive/negative extrema, then calculate the final limiting factor with at most two relevant ratios per variable.

All prototypes stayed in review-local source trees. The original numerical thresholds, reconstruction formulas, pressure source, wet/dry rules, timestep controls and compiler floating-point settings were preserved. The accepted production code was never edited.

## Screening results

32-division bowls, three periods. One warm-up group followed by three randomized measured groups per case, comparing the baseline and all five prototypes. CPU time is child-process user plus system time, including setup and final scalar error evaluation. Entries are median paired CPU reductions: positive is faster; negative is slower.

| Prototype | Planar triangles, 1 thread | Planar quads, 1 thread | Radial quads, 1 thread | Planar quads, 4 threads |
|---|---:|---:|---:|---:|
| Skip nonbinding limiter divisions | -1.8% | -2.8% | -2.9% | -10.1% |
| Skip zero-roughness friction | +0.2% | -1.0% | +6.0% | -0.1% |
| Avoid duplicate gradient clearing | +4.3% | -4.8% | -2.1% | -1.6% |
| Combine those three shortcuts | -3.8% | -2.0% | -2.1% | -3.9% |
| Reduce face extrapolations to extrema | -0.8% | +2.5% | +1.9% | -8.1% |

No shortcut showed a convincing general win. For example, delayed gradient clearing improved the triangular screen but worsened the quad screen, and the combined version did not reliably improve either. The friction shortcut helped one radial screen but was effectively unchanged elsewhere.

## Larger and frictional confirmations

The face-extrema candidate received further evaluation rather than being accepted from the small radial-screen gain. The 64-division planar cases run one period, with one warm-up pair and five randomized measured pairs. The complete-model probe runs a fully wet 32,768-triangle sloping surface for 20 seconds, including nonzero roughness, rainfall, infiltration, a drain/pipe connection and five-second HDF5 output. It uses one warm-up pair and three randomized measured pairs. This is an authored qualification fixture, not a calibrated catchment.

| Confirmation case | Median CPU reduction | Range across paired repeats |
|---|---:|---:|
| Planar triangles, 1 thread(s) | +2.51% | -1.1% to +4.6% |
| Planar quads, 1 thread(s) | +0.15% | -2.7% to +7.4% |
| Planar quads, 4 thread(s) | +0.60% | -2.1% to +4.4% |
| Wet 32,768-cell coupled surface, 1 thread | +0.52% | -9.5% to +6.6% |

The frictional complete-model result is inconclusive: paired changes range from about a 9.5% slowdown to a 6.6% improvement, with a median CPU reduction of only 0.52%. One-minute host load ranged from 23.4 to 56.6 over recorded screening and confirmation runs. Randomization and CPU-time accounting reduce some timing confounding but do not remove core placement, frequency, cache and scheduling effects. These experiments do not prove small improvements are impossible on an idle machine or other architectures; they do not provide evidence strong enough to retain this extra implementation complexity now.

## Numerical checks and scope

- **96 screening executions** and **36 larger-confirmation executions**: every recorded non-timing scalar metric matched the corresponding baseline exactly, including error measures, water-volume error, depth extrema and step counts. These comparisons are not claimed as full per-cell/history equality.
- **Eight complete-model executions**: final state hashes, water/species/groundwater ledger data as applicable, solver statistics, and normalized reports matched exactly. Native `h5diff` also found identical output datasets in every paired run, including warm-up.
- These are targeted prototype checks. No prototype was accepted, so the broader analytical matrix and full regression suite were not rerun in this phase. The production solver and existing tests are unchanged from the already validated phase-10 implementation. Phase 12 will perform final cumulative qualification.
- The component build uses native Apple ARM64 Clang, double precision, `-O3 -fno-fast-math -ffp-contract=off -mcpu=native`. Full-engine comparisons use the same internally consistent frozen support objects and headers on both sides, with only the candidate solver recompiled. No GPU experiment was run.

## Evidence

`profiles.json`, `screen.json`, `screen_summary.json`, `confirmation.json`, `confirmation_summary.json`, `coupled_probe.json`, `coupled_probe_summary.json`, `gradient_probe_provenance.json` and `final_verification.json` record the observations. `rejected_*.patch` preserves every prototype. `retained.patch` is empty. Binaries, full source snapshots and model outputs remain ignored local artifacts.

The next step is the final cumulative performance/accuracy assessment and supported-configuration review. No additional optimization phase is proposed on the evidence here.
