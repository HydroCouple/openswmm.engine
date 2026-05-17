# ODE Logistic Growth — Benchmark Definition

## Purpose

Validates the adaptive RK45 integrator (`OdeSolver.cpp`) on a nonlinear
autonomous ODE with a known closed-form solution. Complements the linear
exponential-decay benchmark by exercising the sigmoid region of the solution
and a nonlinear right-hand side.

## Physics

Logistic growth model:

```
dy/dt = r * y * (1 - y/K),   y(0) = y0
```

Exact closed-form solution:

```
y(t) = K / (1 + (K/y0 - 1) * exp(-r*t))
     = 1 / (1 + 9 * exp(-0.5*t))    [for the chosen parameters]
```

## Parameters

| Symbol | Value | Unit | Description |
|--------|-------|------|-------------|
| r | 0.5 | s⁻¹ | Growth rate |
| K | 1.0 | — | Carrying capacity |
| y0 | 0.1 | — | Initial condition |
| t_end | 14.0 | s | End time (y ≈ 0.992 at this point) |

## Reference Dataset

`reference.csv` columns:

- `t_s`: time in seconds (uniform 2 s spacing, t = 0, 2, 4, …, 14)
- `y_exact`: exact solution `1 / (1 + 9*exp(-0.5*t))`

## Test Setup

```cpp
double y = 0.1;
double prev_t = 0.0;
auto derivs = [](double, const double* y, double* dydx) {
    dydx[0] = 0.5 * y[0] * (1.0 - y[0]);
};
for each row {
    openswmm::ode::integrate(&y, 1, prev_t, row.t_s, 1e-6, 0.1, derivs);
    EXPECT_NEAR(y, row.y_exact, 1e-4);
    prev_t = row.t_s;
}
```

## Acceptance Criterion

```
|y_solver - y_exact| < 1e-4   (absolute, at each checkpoint)
```

RK45 with `eps=1e-6` and `h1=0.1 s` tracks smooth nonlinear trajectories to
well below 1e-4 over this interval.

**Do not loosen this tolerance** — if the test fails, the integrator is wrong.
