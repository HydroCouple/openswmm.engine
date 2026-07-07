# Benchmark: kinwave-normal-depth-rect-open

## Problem statement

Steady uniform flow in a rectangular open channel governed by Manning's equation.
At steady state the kinematic-wave (KW) Newton solver must recover the normal-depth
flow exactly, because the continuity residual is identically zero at the
normal-depth operating point.

The normal-depth (Manning's) relationship for a rectangular channel is:

```
Q_n = beta * A_n * R_n^(2/3)
    = beta * S(A_n)
```

where:
- `beta   = PHI * sqrt(slope) / n`  (PHI = 1.486 in US customary)
- `A_n    = w * d_n`                (area at normal depth d_n)
- `R_n    = A_n / (w + 2*d_n)`     (hydraulic radius)
- `S(A)   = A * R(A)^(2/3)`        (section factor, ft^(8/3))

## Why the Newton solve is exact at steady state

The KW finite-difference continuity equation is:

```
f(a_out) = beta1*S(a_out) + C1*a_out + C2 = 0
```

with `beta1 = 1/s_full` (the normalisation constant) and coefficients C1, C2
derived from previous-timestep state (q1, a1, q2, a2) and inlet flow q_in.

When the channel is pre-loaded at normal depth — `q1=q2=q_in=Q_n`,
`a1=a2=A_n` — direct substitution gives:

```
C2 = -A_n/a_full * dxdt - Q_n/q_full        (after WX=WT=0.6 cancel)
f(A_n/a_full) = S_n/s_full + dxdt*(WT/WX)*(A_n/a_full) + C2
              = Q_n/q_full + dxdt*(A_n/a_full) - A_n/a_full*dxdt - Q_n/q_full
              = 0  (exactly, because WT/WX = 1)
```

Newton starts at the previous outlet area `A_n`, finds `f = 0` immediately,
and exits after zero iterations.  The only error is floating-point rounding
(~1e-15 relative), so the output matches the reference to machine precision.

## Parameters

| Symbol    | Value   | Unit      | Notes                              |
|-----------|---------|-----------|------------------------------------|
| shape     | RECT_OPEN | —       | rectangular open channel           |
| w         | 10.0    | ft        | channel width                      |
| y_full    | 5.0     | ft        | full depth (bank-full)             |
| n         | 0.013   | —         | Manning roughness coefficient      |
| slope     | 0.001   | ft/ft     | longitudinal channel slope         |
| PHI       | 1.486   | ft^(1/3)/s| US-customary Manning constant      |
| beta      | ≈3.6147 | ft^(1/3)/s| PHI*sqrt(slope)/n                  |
| q_full    | ≈332.9  | cfs       | beta * s_full                      |
| a_full    | 50.0    | ft²       | w * y_full                         |
| s_full    | ≈92.10  | ft^(8/3)  | a_full * r_full^(2/3)              |

## Exact solution

For depth `d_n` in the reference table:

```
A_n   = 10 * d_n            [ft²]
R_n   = 10*d_n / (10+2*d_n) [ft]
S_n   = A_n * R_n^(2/3)     [ft^(8/3)]
Q_n   = beta * S_n           [cfs]
```

## Numerical accuracy and tolerance guidance

`KWSolver::solveConduit` uses a Newton-Raphson iteration.  At the normal-depth
operating point the residual is **analytically zero** — not just small.
Newton therefore converges in 0 iterations and the output error is pure
floating-point rounding (~1e-15 relative).  The benchmark tolerance is 1e-9
relative to q_full / a_full.

**Do not loosen the test tolerance.**  If `TEST(KWSolverSteadyState, NormalDepthRecovered)`
ever fails, it means a real regression in the Newton solve, the section-factor
inversion, or the state normalisation — investigate the implementation, not the
tolerance.  The gap between the expected error (~1e-15) and the threshold (1e-9)
is a million-fold, so platform FP differences will never cause false failures.

## Verification target

`src/engine/hydraulics/KinematicWave.cpp` — `KWSolver::solveConduit`,
specifically the inlet-area inversion and Newton iteration at steady state.

## Reference dataset

`reference.csv` columns:
- `d_n_ft`    — normal depth in ft
- `A_n_ft2`   — normal-depth cross-sectional area (ft²)
- `R_n_ft`    — normal-depth hydraulic radius (ft)
- `S_n_ft83`  — section factor at normal depth (ft^(8/3))
- `Q_n_cfs`   — normal-depth flow (cfs), exact Manning value

## Consuming test

`tests/unit/engine/test_routing.cpp` — `TEST(KWSolverSteadyState, NormalDepthRecovered)`
