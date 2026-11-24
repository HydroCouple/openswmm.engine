# dw-ritter-drybed-strip

Ritter (1892) exact solution for a 1D frictionless dry-bed dam break,
compared against the OpenSWMM DW solver on a 250 ft × 5 ft rectangular
strip discretised into 50 conduits of 5 ft each.  The upstream end is held
at a fixed stage h₀ = 1.0 ft (reservoir BC); the downstream end uses a FREE
outfall; all conduits are frictionless (n = 0) and horizontal.  Reference
depths and velocities are sampled at t ∈ {2, 4, 6, 8} s — before the wet
front reaches the downstream boundary (t_max ≈ 11.0 s).

See [definition.md](definition.md) for equations, setup parameters,
and metric tolerances.

## Findings (assessed 23 November 2026)

Dynamic wave does not meet the analytical targets in definition.md on this
strip, and that is the SWMM 5 formulation, not a defect of this engine:
`FLOW_ROUTING DYNWAVE` in the full engine and SWMM 5.2.4 agree to three
decimals at every node at 8 s, for `INERTIAL_DAMPING NONE` and `PARTIAL`.

- **Front.** The front is detected where the depth last exceeds 0.01 ft, so
  it is compared with the exact position of that depth,
  x_d + (2c₀ − 3√(g·0.01))·t (202.2 ft at 8 s), not the zero-depth tip
  x_d + 2c₀t (215.8 ft). The solver test reaches 147.5, 152.5 and 157.5 ft at
  4, 6 and 8 s against 163.6, 182.9 and 202.2 ft. A 25 times smaller time
  step does not move it toward the exact value.
- **Dam station.** 0.80, 0.60, 0.60 and 0.57 ft at 2 to 8 s against
  4h₀/9 = 0.444 ft.
- **Profile.** With `INERTIAL_DAMPING NONE` the full-engine profile behind
  the front is a cell-to-cell sawtooth (0.05 ft beside 1.38 ft at 8 s, above
  the 1 ft reservoir), in 5.2.4 as well.

### Use finite-volume routing for dry-bed fronts

`FLOW_ROUTING FV` follows the exact solution on the same strip: front within
10.4 ft of the exact 0.01 ft position at all four times, mean wet-depth
error 0.021 to 0.024 ft, no oscillation. Its dam-station depth reads 0.64 ft
at 2 s and 0.51 ft at 8 s; that does not change with second order or 1 ft
cells, so it comes from the junction every 5 ft, not the in-conduit scheme.

### Test posture

- `DWSolverRitter.DryBedRegressionBaseline` (`test_routing.cpp`) pins the
  dynamic-wave behavior above so it cannot get worse unnoticed.
- `DWSolverRitter.DISABLED_DryBedMatchesRitterAnalytical` keeps the
  analytical targets as a record; an opt-in front treatment for dynamic wave
  would have to leave 5.2.4 parity when off.
- `FvEngine.RitterDryBedFrontOnAJunctionChain`
  (`test_fv_engine_integration.cpp`) gates the finite-volume front and depth
  error and records the dam-station depth.
