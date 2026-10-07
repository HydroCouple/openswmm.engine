# CPU phase 6: final qualification and scaling

## Scope and outcome

The planned CPU performance review is complete within the available host and model corpus. Phase 5's fused species/source implementation preserves the tested results, improves larger multispecies workloads, and adds no new persistent storage. This qualification does **not** establish production-catchment performance, GPU behavior, or high accuracy for long-duration oscillating bowls.

Two references are used and kept distinct: the phase-4 working-tree snapshot is the baseline for phase-5 equivalence/scaling, while a separately rebuilt corrected pre-optimization solver is used only for the direct cumulative comparison. Source and common-object provenance are recorded. No historical speedup percentages are added together.

## Larger meshes and threads

96 fresh timed executions: three adjacent randomized baseline/final pairs for each mesh/species/thread combination, with discarded warmups. Local-inertial wave problems run five simulated seconds. Hashing is outside the timed region. Baseline/final full fields are exactly equal for each configuration. Physical water, head, flux and species fields are also bitwise equal across 1/2/4/8 threads; this cross-thread check excludes global ledger totals whose reduction order can vary.

The table gives median advancement times and final process peak RSS. Positive reduction means faster. Speedup compares the final implementation at the requested thread count with its measured one-thread time; it is host-observed, not an ideal scaling estimate.

| Cells | Species | Threads | Baseline wall s | Final wall s | Wall reduction | CPU reduction | Final speedup | Peak RSS MiB |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 32,768 | 32 | 1 | 1.2835 | 0.7964 | +38.0% | +36.8% | 1.00× | 86.0 |
| 32,768 | 32 | 2 | 1.0555 | 0.8083 | +23.4% | +24.1% | 0.99× | 86.7 |
| 32,768 | 32 | 4 | 0.7826 | 0.6897 | +11.9% | +17.2% | 1.15× | 86.4 |
| 32,768 | 32 | 8 | 0.7776 | 0.4865 | +37.4% | +19.1% | 1.64× | 86.7 |
| 131,072 | 0 | 1 | 0.3746 | 0.3662 | +2.2% | -0.4% | 1.00× | 205.5 |
| 131,072 | 0 | 2 | 0.3026 | 0.3418 | -12.9% | -3.0% | 1.07× | 206.0 |
| 131,072 | 0 | 4 | 0.3734 | 0.3468 | +7.1% | -4.5% | 1.06× | 206.4 |
| 131,072 | 0 | 8 | 0.3610 | 0.4106 | -13.7% | +6.5% | 0.89× | 206.2 |
| 131,072 | 8 | 1 | 1.1593 | 1.0934 | +5.7% | +5.1% | 1.00× | 238.5 |
| 131,072 | 8 | 2 | 1.0844 | 0.9538 | +12.0% | +3.3% | 1.15× | 240.8 |
| 131,072 | 8 | 4 | 0.9983 | 0.8112 | +18.7% | +4.1% | 1.35× | 241.2 |
| 131,072 | 8 | 8 | 0.9608 | 0.8528 | +11.2% | +5.3% | 1.28× | 241.4 |
| 524,288 | 0 | 1 | 1.4441 | 1.4746 | -2.1% | -0.3% | 1.00× | 838.8 |
| 524,288 | 0 | 2 | 1.1345 | 1.3095 | -15.4% | -3.9% | 1.13× | 846.6 |
| 524,288 | 0 | 4 | 0.5931 | 0.7320 | -23.4% | -5.8% | 2.01× | 841.4 |
| 524,288 | 0 | 8 | 0.9053 | 1.2067 | -33.3% | -3.1% | 1.22× | 844.6 |

The one-minute load average ranged from 46.0 to 58.0 on a host with 10 logical CPUs. No review build/test ran concurrently, but unrelated work was active. More threads were not consistently faster. These data cannot identify an optimal production thread count.

The larger eight-species workload reduces CPU time modestly; 32 species shows a larger benefit. Water-only controls do not benefit, and some regress in CPU as well as wall time. The 524,288-cell, four-thread control regresses about 5.8% in CPU time; its elapsed regression is larger. Host contention is a confounder, not proof that a regression is harmless. Replicate these controls on an idle machine before a release performance claim. Peak RSS shows no material systematic increase; it measures the entire process, not solver allocations alone.

## Repeated storms

24 executions cover ten rain/infiltration cycles over 600 simulated seconds, all three momentum modes, triangles/quadrilaterals, one/four threads, four local time-step tiers and nine species including signed temperature. Every matched baseline/final history and final field is exact. Each closed domain receives 12.8 m³; final surface storage plus accumulated infiltration recovers that input. Species budgets include rain and infiltration. Depths/volumes remain nonnegative.

Maximum absolute water residual: 9.24e-14 m³. Maximum aggregate species residual: 3.01e-12. Separate phase-5 regressions check each species row, including changing coupling and evaporation.

## Long analytical oscillations

36 executions extend radial and planar Thacker bowls to five/ten periods on 32×32 grids with triangles/quads and first/second order, plus 64×64 triangular first-order five-period checks. All use full SWE, one tier and four threads. Every baseline/final final field and non-timing metric is identical. Depths are nonnegative and closed-domain volume is conserved to roundoff.

The error below is depth L1 relative to the integral of exact depth at the final time. This is an accuracy measurement, not just a regression check. These coarse long-duration runs have substantial error; refinement and higher order do not uniformly fix it over many periods.

| Bowl | Divisions | Shape | Order | Periods | Relative depth L1 | Maximum depth error m |
|---|---:|---|---:|---:|---:|---:|
| radial | 32 | triangle | 1 | 5 | 22.23% | 0.02482 |
| radial | 32 | triangle | 1 | 10 | 22.22% | 0.02481 |
| radial | 32 | quad | 1 | 5 | 22.25% | 0.02455 |
| radial | 32 | quad | 1 | 10 | 22.25% | 0.02455 |
| radial | 32 | triangle | 2 | 5 | 22.89% | 0.02504 |
| radial | 32 | triangle | 2 | 10 | 22.15% | 0.02476 |
| radial | 32 | quad | 2 | 5 | 22.24% | 0.02448 |
| radial | 32 | quad | 2 | 10 | 22.25% | 0.02454 |
| planar | 32 | triangle | 1 | 5 | 82.18% | 0.07571 |
| planar | 32 | triangle | 1 | 10 | 84.72% | 0.07433 |
| planar | 32 | quad | 1 | 5 | 86.52% | 0.07338 |
| planar | 32 | quad | 1 | 10 | 82.46% | 0.06886 |
| planar | 32 | triangle | 2 | 5 | 82.21% | 0.07572 |
| planar | 32 | triangle | 2 | 10 | 87.89% | 0.07840 |
| planar | 32 | quad | 2 | 5 | 88.17% | 0.07749 |
| planar | 32 | quad | 2 | 10 | 84.24% | 0.06978 |
| radial | 64 | triangle | 1 | 5 | 22.92% | 0.02552 |
| planar | 64 | triangle | 1 | 5 | 74.85% | 0.07121 |

Maximum absolute relative volume error is 6.01e-15. The planar cases have roughly 75–88% relative depth L1 after five/ten periods. Radial cases are about 22–23%. This extends the damping/phase/shape limitation already documented in `../FIXES_AND_VALIDATION.md`; it is unchanged by phase 5. A numerical-accuracy investigation should measure energy, momentum, phase and shoreline error, audit wetting/drying and flux dissipation, and establish spatial/temporal convergence. Conservation and baseline equality alone do not close that work.

## Complete models, groundwater and output histories

Eight baseline/final runs use frozen copies of four authored input models. These are examples and constructed qualification models, not real calibrated catchments. Final surface fields, water budgets, routing steps and normalized textual reports match exactly. Report decoding uses Latin-1 losslessly because the report includes a non-UTF-8 superscript unit; only start/end/elapsed clock lines are removed.

| Model | Exercise | Result |
|---|---|---|
| parking_lot | Original six-hour example, eight cells, mixed boundaries, 1D coupling, HDF5 output | Exact state, normalized report and complete HDF5 file contents |
| two_way_groundwater | Authored two-way groundwater/node fixture | Exact final groundwater fields and water ledgers |
| dunne_species9 | Infiltration and groundwater return with nine transported pollutants | Exact surface/groundwater state and each species ledger; water/species residuals near roundoff |
| surface_output_species32 | 32,768-cell sloping surface, 32 pollutants, four threads, three minutes, output enabled | Exact state, normalized report and complete HDF5 file contents |

Groundwater checks read all 14 bulk variables, both-zone species concentrations, 13 water-ledger terms and 15 terms per species. The nine-species model has positive infiltration and groundwater return, so it exercises both transfer directions. Native `h5diff` compares the entire produced HDF5 files without exclusions. The engine owns file compression/storage details; byte-for-byte file-container equality is not claimed.

Maximum groundwater water residual: 1.34e-12 m³. Maximum groundwater species residual: 1.71e-12.

## Direct cumulative comparison

The corrected solver from before CPU phases 1–5 is recompiled against the same frozen engine support objects as the final solver. Only the solver header/implementation are substituted; source hashes and commands are recorded in `start_provenance.json` and `start_commands.json`. This controls the surrounding engine code and does not reconstruct an entire historical checkout.

The two synthetic 32,768-cell, four-thread engine models run one simulated minute with eight or 32 species. Three randomized pairs per case plus discarded warmups give 12 timed executions, 16 total. Final depth, water budget, infiltration, routing-step counts and normalized reports must match exactly.

| Species | Start total s | Final total s | Elapsed reduction | CPU reduction |
|---:|---:|---:|---:|---:|
| 8 | 2.536 | 1.583 | +37.6% | +41.1% |
| 32 | 9.366 | 4.909 | +47.6% | +41.3% |

These cumulative gains apply to these two constructed workloads under the recorded host conditions. They must not be extrapolated to every mesh, species count or boundary/source mix.

## Remaining work outside this CPU pass

1. Numerical accuracy of long oscillations: substantial existing damping/phase/shape error needs a dedicated investigation before such problems can be called well resolved.
2. Idle-host performance replication, especially water-only regressions, followed by large representative calibrated catchments and realistic output settings. The available corpus did not provide those production datasets.
3. GPU correctness and performance qualification once GPU hardware is available. No GPU speed or correctness claim is made here.

Phase 5's code, two regression tests and the phase 5/6 reports are included in the CPU species/source optimization commit. The prior correctness and CPU phases remain as recorded in their reports. Phase 6 itself adds review scripts and evidence, not another production algorithm change.

## Reproduction and evidence

`prepare.py` builds the qualification harness and copies source input decks; `deck_provenance.json` identifies originals and modified copies. `check_science.py`, `check_models.py`, `scaling.py` and `cumulative.py` record the run protocols. Their JSON outputs retain exact hashes, timing observations, scientific errors and load averages. `run_engine.py` reads surface and groundwater state through the public API before ending the run. `final_verification.json` records counts and source identities. Generated snapshots, binaries and run directories are ignored. Supporting scripts, raw results and manifests remain local review artifacts; the reports contain the committed findings.
