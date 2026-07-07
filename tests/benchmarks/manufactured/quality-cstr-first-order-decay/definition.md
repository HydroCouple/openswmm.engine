# Benchmark: quality-cstr-first-order-decay

## Problem statement

A completely mixed reactor (CSTR) with a single dissolved constituent subject to
first-order decay.  There is no inflow, no outflow, and constant volume.  The
governing ODE is:

```
dC/dt = -k * C,    C(0) = C_0
```

The continuous exact solution is exponential:

```
C(t) = C_0 * exp(-k * t)
```

SWMM's quality engine (`QualitySolver::execute`) applies a discrete first-order
approximation instead of the continuous exponential.  For the KINWAVE/DYNWAVE
routing path, the decay factor per step is:

```
factor = 1 - k * dt
C[n+1] = C[n] * factor
```

Because the factor is constant, the discrete solution is:

```
C[N] = C_0 * factor^N = C_0 * (1 - k * dt)^N
```

This is the reference used in the benchmark — NOT the continuous exponential.
The test verifies that the engine applies decay correctly over multiple timesteps.

## Parameters

| Symbol  | Value  | Unit  | Notes                              |
|---------|--------|-------|------------------------------------|
| C_0     | 100.0  | mg/L  | initial concentration              |
| k       | 1.0e-3 | 1/s   | first-order decay coefficient      |
| dt      | 60.0   | s     | timestep per benchmark row         |
| factor  | 0.94   | —     | 1 - k*dt (per-step decay factor)  |
| N       | 10     | —     | number of steps (t = 0 to 600 s)  |

## Exact solution (discrete)

```
C[N] = 100.0 * 0.94^N
```

Values at benchmark times:
- t=0:    C = 100.0
- t=60:   C = 94.0
- t=120:  C = 88.36
- ...
- t=600:  C = 53.8615114094900 mg/L

## Note on continuous vs discrete decay

The test benchmarks the discrete (linear-approximation) solver path, NOT the
continuous exponential.  The STEADY routing path uses `exp(-k*dt)` (see
`QualitySolver::updateLinkQuality`), but the node-level decay in `applyDecay`
uses the linear factor `1 - k*dt`.  These are equivalent only at very small dt.

At k*dt = 0.06, the two formulas differ by about 0.18%:
- Discrete: (1 - 0.06)^10 = 0.5386
- Continuous: exp(-0.06)^10 = exp(-0.6) = 0.5488

The benchmark tests the discrete path to machine precision.

## Numerical accuracy and tolerance

The factor `1 - k*dt` is not exactly representable in IEEE 754 double (since
0.001 and 60.0 are both exact, but 0.001*60 = 0.06 has a tiny rounding error).
However, the factor is computed identically in both the reference CSV and the
engine, so the error between them is at most 1-2 ULP per step (~10^{-15} per
step, ~10^{-14} over 10 steps).

Test tolerance: 1e-9 mg/L — a factor of 10^5 above expected rounding noise.

## Multi-step test protocol

For the trajectory test, after each `solver.execute()` call the test must advance
the "old" state by copying `conc → conc_old` at all nodes and links.  This
mirrors what the simulation loop does between routing steps.

## Verification target

`src/engine/quality/QualityRouting.cpp` — `QualitySolver::applyDecay`
specifically the linear factor `1 - k_decay * dt` applied at each node.

## Reference dataset

`reference.csv` columns:
- `t_s` — time in seconds
- `C_mgl` — exact (discrete) concentration in mg/L

## Consuming test

`tests/unit/engine/test_quality_routing.cpp` — `TEST(QualityCSTR, FirstOrderDecayTrajectory)`

## References

- Fogler, H.S. (2016). *Elements of Chemical Reaction Engineering*, 5th ed. Prentice Hall. Section 2.3 (CSTR with first-order batch decay).
- Rossman, L.A. and Huber, W.C. (2016). *Storm Water Management Model Reference Manual Volume III — Water Quality*. EPA/600/R-16/093.
