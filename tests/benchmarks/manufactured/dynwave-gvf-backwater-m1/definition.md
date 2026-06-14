# Benchmark: dynwave-gvf-backwater-m1

## Purpose

Verify that the DW solver (St. Venant equations) converges to the analytically
computed **M1 backwater profile** (GVF, subcritical, tailwater above normal depth)
after sufficient time under steady inflow and a fixed tailwater boundary condition.

## Physical setup

A 1000 ft reach of rectangular open channel (b = 5 ft, y_full = 4 ft, S₀ = 0.001,
n = 0.013) is discretised into 5 conduits of 200 ft each (6 nodes J0–J5).
A constant lateral inflow Q = 10 cfs enters at the upstream junction J0.
The downstream node J5 is a FIXED outfall at water-surface elevation y_d = 1.5 × y_n.

## Parameters

| Symbol    | Value     | Unit    | Notes                                      |
|-----------|-----------|---------|--------------------------------------------|
| b         | 5.0       | ft      | channel width (RECT_OPEN)                  |
| y_full    | 4.0       | ft      | conduit full depth (prevents surcharge)    |
| S₀        | 0.001     | ft/ft   | bed slope                                  |
| n         | 0.013     | —       | Manning roughness                           |
| L_conduit | 200.0     | ft      | per-conduit length (5 conduits total)      |
| Q         | 10.0      | cfs     | steady lateral inflow at J0                |
| dt        | 30.0      | s       | routing timestep                           |
| N_steps   | 120       | —       | steps (T = 3600 s = 1 hr)                  |

Derived quantities (PHI = 1.486, US customary):
```
beta      = PHI * sqrt(S₀) / n  ≈ 3.6163 ft^{1/3}/s
y_n       = 0.781692 ft  (Manning normal depth at Q = 10 cfs)
y_c       = 0.498963 ft  (critical depth at Q = 10 cfs)
Fr_n      ≈ 0.510  (mild slope: y_n > y_c ✓)
y_d       = 1.5 × y_n = 1.172539 ft  (fixed tailwater BC)
```

## Analytical reference: M1 GVF profile

The GVF ODE (gradually varied flow, subcritical regime):

    dy/dx = (S₀ - Sf(y)) / (1 - Fr²(y))

where `Sf = (Q·n / (PHI·A·R^{2/3}))²` and `Fr² = Q² / (g·A²·(A/B))`.

Integrated by RK4 from x = 1000 ft (y = y_d) upstream to x = 0 ft,
sampling at each node:

| Node | x (ft) | z_inv (ft) | y_GVF (ft) |
|------|--------|------------|------------|
| J0   | 0      | 1.000      | 0.792075   |
| J1   | 200    | 0.800      | 0.809203   |
| J2   | 400    | 0.600      | 0.848366   |
| J3   | 600    | 0.400      | 0.921831   |
| J4   | 800    | 0.200      | 1.032399   |
| J5   | 1000   | 0.000      | 1.172539   |

The M1 profile asymptotes toward y_n from above as x → 0. At J0, the depth
is within 1.3% of normal depth (0.792075 vs 0.781692), reflecting the finite
reach length.

## Expected accuracy

After T = 3600 s (≈ 18 wave-travel times), the DW solver should have converged
to quasi-steady state. Test tolerance is **5% of y_n ≈ 0.039 ft** at J0–J4.
J5 is not checked (it is the fixed boundary condition).

The GVF reference was computed with g = 32.174 ft/s²; the DW solver uses
g = 32.2 ft/s². The 0.08% difference in g is negligible versus the 5% tolerance.

## Verification target

`src/engine/hydraulics/DynamicWave.cpp` — `DWSolver::execute`

## Consuming test

`tests/unit/engine/test_routing.cpp` — `TEST(DWSolverGVF, BackwaterM1Benchmark)`

## References

- Henderson, F.M. (1966). *Open Channel Flow*. Macmillan. Chapter 5 (gradually varied flow, M1 backwater curve).
- Manning, R. (1891). On the flow of water in open channels and pipes. *Transactions of the Institution of Civil Engineers of Ireland*, 20, 161–207.
