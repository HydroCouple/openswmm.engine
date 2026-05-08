# Benchmark: odesolve-exponential-decay

## Problem statement

Scalar first-order linear ODE with exact closed-form solution.

```
dy/dt = -lambda * y,   y(0) = y0
y(t)  =  y0 * exp(-lambda * t)
```

## Parameters

| Symbol   | Value | Unit |
|----------|-------|------|
| lambda   | 0.5   | s⁻¹ |
| y0       | 1.0   | —    |
| t_start  | 0.0   | s    |
| t_end    | 10.0  | s    |

## Verification target

`src/engine/math/OdeSolver.cpp` (`openswmm::ode::integrate`)

The integrator is a 5th-order adaptive Runge-Kutta (Cash-Karp) scheme with error tolerance
control. This benchmark exercises the core stepping and accuracy under smooth exponential decay.

## Expected accuracy

With error tolerance `eps = 1e-6` and initial step `h1 = 0.1 s`, the integrator should
reproduce the exact solution to within 1e-5 absolute error at any output point in [0, 10] s.

## Reference dataset

`reference.csv` columns:
- `t_s` — time in seconds
- `y_exact` — exact solution value

Values are computed as `exp(-0.5 * t_s)` in double precision.

## Consuming test

`tests/unit/engine/test_infiltration.cpp` — `TEST(OdeSolver, ExponentialDecay)` spot-checks
a single end-point; extend that test (or add `test_ode_solver.cpp`) to walk the full grid and
compute max absolute and RMS error against this table.
