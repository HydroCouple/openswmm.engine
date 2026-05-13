# Green-Ampt Saturated Trajectory — Benchmark Definition

## Purpose

Validates `grnampt_getInfil` in the fully-saturated branch against the exact
implicit Green-Ampt equation.

## Physics

The Green-Ampt infiltration rate under saturated surface conditions is:

```
dF/dt = Ks * (1 + c1/F),   c1 = (S + depth) * IMD
```

Integrating from F(0) = 0 yields the implicit exact solution parametrized by
cumulative infiltration F:

```
t(F) = [F - c1 * ln(1 + F/c1)] / Ks
```

## Parameters

| Symbol | Value | Unit | Description |
|--------|-------|------|-------------|
| S | 1.93/12 | ft | Capillary suction head (sandy loam) |
| Ks | 0.43/43200 | ft/s | Saturated hydraulic conductivity |
| IMD | 0.25 | — | Initial moisture deficit |
| depth | 0.0 | ft | Ponded water depth (none) |
| c1 | (S+depth)·IMD = 1.93/48 ≈ 0.040208 | ft | Effective suction term |

## Reference Dataset

`reference.csv` columns:

- `t_s`: time in seconds (computed from the formula t(F) with F as parameter)
- `F_ft`: cumulative infiltration in ft

## Test Setup

```cpp
GreenAmptState state{};
state.S         = 1.93 / 12.0;
state.Ks        = 0.43 / 12.0 / 3600.0;
state.IMD       = 0.25;
state.IMDmax    = 0.25;
state.T         = 1.0e10;    // timer never expires during wet run
state.saturated = true;      // force saturated branch from t=0
// state.Lu = state.Fumax = state.Fu = 0 (default-zero, upper zone absent)
```

Call `grnampt_getInfil(state, precip=1.0, depth=0.0, dt, InfilModel::GREEN_AMPT)`
with `dt = 1.0 s` (final step may be fractional), stepping from each reference
time to the next and comparing `state.F` to the reference `F_ft`.

The `precip = 1.0 ft/s` is chosen so that `ia >> dF/dt` at all points, keeping
the `fp = min(fp, ia)` clamp inactive throughout.

## Acceptance Criterion

```
|state.F - F_ref| < 1e-3 ft   (absolute, at each checkpoint)
```

`grnampt_getF2` converges each Newton solve to |step| < 1e-5 ft, and the
implicit G-A update is contractive (errors damp over successive steps), so
1e-3 ft provides three decades of headroom above the solver tolerance.

**Do not loosen this tolerance** — if the test fails, the solver is wrong.
