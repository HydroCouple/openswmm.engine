# Benchmark: groundwater-linearized-recession

## Problem statement

A two-zone groundwater aquifer draining by lateral flow only, with no
infiltration, no evaporation, and no deep percolation.  With a linear lateral
flow law (b1 = 1), the lower-zone head H(t) satisfies:

```
dH/dt = -a1 * (H - h_star) / (ucf_gwflow * (phi - theta))
```

This is a linear first-order ODE with the exact solution:

```
H(t) = h_star + (H_0 - h_star) * exp(-lambda * t)
```

where the recession rate constant is:

```
lambda = a1 / (ucf_gwflow * (phi - theta))
```

With US customary units: `ucf_gwflow = 43560` (converts cfs/acre to ft/s).

## Parameters

| Symbol      | Value    | Unit        | Notes                                         |
|-------------|----------|-------------|-----------------------------------------------|
| H_0         | 3.0      | ft          | initial lower-zone head (lower_depth)         |
| h_star      | 0.5      | ft          | reference head (GW outflow = 0 when H = h_star) |
| a1          | 10.0     | cfs/acre/ft | lateral flow coefficient (linear, b1 = 1)    |
| b1          | 1.0      | —           | exponent (linear recession)                   |
| phi         | 0.4      | —           | porosity                                      |
| theta_0     | 0.2      | —           | initial upper-zone moisture (= field_cap)     |
| field_cap   | 0.2      | —           | set equal to theta_0 to suppress percolation  |
| total_depth | 5.0      | ft          | total aquifer depth                           |
| ucf_gwflow  | 43560.0  | —           | US unit conversion (cfs/acre → ft/s)          |

Derived:
```
lambda = 10.0 / (43560.0 * (0.4 - 0.2)) = 10 / 8712 ≈ 1.1478e-3 s^{-1}
H(0) - h_star = 2.5 ft
```

## Exact solution

```
H(t) = 0.5 + 2.5 * exp(-t / 871.2)   [ft]
```

Reference values at dt = 200 s intervals:

| t (s) | H (ft)   |
|-------|----------|
| 0     | 3.000000 |
| 200   | 2.487195 |
| 400   | 2.079583 |
| 600   | 1.755582 |
| 800   | 1.498043 |
| 1000  | 1.293275 |
| 1200  | 1.130603 |

## Conditions for exact linearization

For the analytical solution to hold exactly:
- `infil_rate = 0` → theta stays constant at field_cap; upper_perc = 0
- `max_evap = 0` → no evapotranspiration
- `lower_loss_coeff = 0` → deep_loss = 0
- `lower_evap_depth = 0` → lower_evap = 0
- `a2 = a3 = 0` → no surface-water terms in GW flow formula
- `sw_head = 0` → irrelevant since a2 = a3 = 0
- `H > h_star` throughout — satisfied since H(1200) ≈ 1.13 > 0.5 ✓

## Integration scheme

`GWSolver` integrates the ODE with RKF45 (error tolerance `GWTOL = 1e-4`).  The
global error of RKF45 for a smooth linear ODE is O(GWTOL) over the integration
interval.  The benchmark tolerance is set to 1e-3 ft — a factor of 10 above
`GWTOL` — to absorb step-accumulation effects.

## Verification target

`src/engine/hydrology/Groundwater.cpp` — `GWSolver::execute`,
specifically the RKF45 integration of the lower-zone head via `getDxDt`.

## Reference dataset

`reference.csv` columns:
- `t_s` — time in seconds
- `H_ft` — exact (analytical) lower-zone head in ft

## Consuming test

`tests/unit/engine/test_groundwater.cpp` — `TEST(GWLinearizedRecession, ExponentialDecayTrajectory)`

## References

- Hantush, M.S. (1956). Analysis of data from pumping tests in leaky aquifers. *Transactions, American Geophysical Union*, 37(6), 702–714. (exponential recession as a classical result of linear aquifer theory).
- Rossman, L.A. and Huber, W.C. (2016). *Storm Water Management Model Reference Manual Volume I — Hydrology*. EPA/600/R-15/162A. Chapter 3 (groundwater formulation).
