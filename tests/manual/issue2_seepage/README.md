# Conduit seepage regression: OpenSWMM issue #2

Validated on macOS arm64, 2026-10-29, with the shipping (exact geometry) build.
Both engine versions are built from this repository's `swmm6_rel` tree:
5.3.0 is `src/legacy/engine`, and 6.0.0 is `src/engine`.

## Provenance and scope

- OpenSWMM issue #2 (reported by the user).
- [USEPA issue #216](https://github.com/USEPA/Stormwater-Management-Model/issues/216):
  low-inflow dynamic-wave seepage instability.
- [USEPA PR #234](https://github.com/USEPA/Stormwater-Management-Model/pull/234):
  per-barrel stored-volume cap. The upstream description explicitly says this
  does **not** resolve #216's single-barrel reproducer. The PR was still a draft
  when inspected on the validation date.
- [Original OpenSWMM discussion](https://www.openswmm.org/Topic/38196/routing-problems-when-inflow-time-series-is-below-the-conduits-seepage).

`reported.inp` preserves the user's hydraulic inputs and both time series.
Only comments/title and unused map coordinates/empty display sections differ.

## Corrections

1. Conduit evaporation and seepage rates are **per barrel**, while stored link
   volume includes **all barrels**. Divide the dynamic-wave volume cap by the
   barrel count in both engines. Apply the same correction to 6.0.0's
   finite-volume precomputation, which uses the same rate convention.
2. Before each dynamic-wave node depth update, aggregate all incident natural
   conduit losses at each node. Split each loss evenly unless an endpoint is
   an outfall, in which case charge the whole loss to the other endpoint.
   Limit each conduit's accepted loss using the more restrictive endpoint's
   fraction. Reduce the endpoint outflow totals by the same amount, and retain
   the accepted evaporation/seepage rates for quality and continuity accounting.
3. Exclude the numerical `FUDGE` wetting depth from removable water. Treating
   that floor as renewable storage every step still produced excessive losses.

For a node with natural conduit loss `L`, net flow excluding that loss `Q`,
old net flow `Qold`, effective surface area `A`, and old depth `y`, let
`S = max(0, y - FUDGE) * A / dt`. The available rate is bounded by both
`Q + S` (current water budget) and `Q + Qold + 2*S` (the trapezoidal depth
update). The fraction is `clamp(min(Q+S, Q+Qold+2*S)/L, 0, 1)`.
Aggregating `L` before calculating fractions prevents multiple conduits from
independently spending the same node supply.

Kinematic/steady routing retain their inflow-based caps. The new limiter leaves
signed aquifer gains intact and preserves the existing contract that a host's
ADD/OVERRIDE seepage forcing is authoritative. Conduits with a host-prescribed
exchange are excluded from this additional natural-loss limiter.

## Reproduce

From the repository root, using an existing configured build:

```sh
cmake --build build/darwin --target test_engine_conduit_losses
ctest --test-dir build/darwin -R '^test_engine_conduit_losses$' --output-on-failure
```

The test writes reviewable inputs and reports to this directory's `runs/`.
Other platforms can use their own build directory. The test contains:

- Single-, two- and three-barrel cap cases with shallow standing water.
- Seepage only, evaporation only, combined proportional losses, zero losses,
  and a small timestep for which the volume cap should not bind.
- The reported Chicago hydrograph, 0.1-second steps, constant low inflow,
  two barrels, seepage on the outfall conduit, THREADS 2, and no seepage.
- Finite-volume loss bounds for one, two and three barrels.

The cap fixtures use large storage areas at both ends so a node-supply limit
cannot conceal an incorrect per-barrel cap. They check against analytic
rectangular inventory and loss potential, rather than just engine parity.

## Evidence and remaining limits

The unmodified local binaries reproduced **-304.589%** routing continuity in
both versions: approximately **0.628 million L** inflow and **2.408 million L**
exfiltration. Merely applying PR #234 cannot change this single-barrel case.

The corrected reported case has approximately **+1.021%** routing continuity;
at 0.1-second steps it is approximately **+0.724%**. The tests bound the reported
case at 1.5% and the constant-low-inflow, two-barrel and outfall-loss variations
at 3%. These are explicit residual-error bounds, not claims of exact closure.
Dry/wet transitions, finite Picard tolerances, and the existing dynamic-wave
storage approximation still limit accuracy. This change does not provide a
general positivity-preserving/conservative reformulation of dynamic wave.

Related CTest suites also exercised during validation:
`test_engine_routing`, `test_engine_quality_routing`, `test_engine_exfiltration`,
`test_engine_massbalance`, `test_engine_kinwave_storage`, and
`test_engine_2d_aquifer`. The aquifer suite covers gaining/losing conduits and
prescribed host seepage. Full cross-platform and full repository suites were
not run. Test outputs and build logs remain local and are ignored by Git.
