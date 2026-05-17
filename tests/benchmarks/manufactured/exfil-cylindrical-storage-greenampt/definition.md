# Benchmark: exfil-cylindrical-storage-greenampt

## Problem statement

Rain-barrel storage unit receiving a constant rainfall rate while draining by
exfiltration through a partially clogged floor.  The clogging model reduces the
effective saturated hydraulic conductivity linearly with cumulative inflow:

```
keff(n) = kSat * max(0, 1 - wb_inflow[n] / clogFactor)
```

where `wb_inflow[n]` is the cumulative inflow depth (ft) accumulated before step
n.  With a constant unit inflow rate `r` and timestep `dt`:

```
wb_inflow[n] = n * r * dt
keff[n]      = kSat * (1 - n * r * dt / clogFactor)
```

The storage depth update per step is:

```
exfil_depth[n] = (keff[n] / phi) * dt   (bounded by available depth)
D[n+1]         = D[n] + r * dt - exfil_depth[n]
```

Because `keff[n]` is a deterministic linear function of n, the depth and
cumulative exfiltration both have closed-form quadratic solutions.  Summing
the per-step updates exactly (sum of n from 0 to N-1 = N*(N-1)/2):

```
D[N] = D[0] + N*r*dt - (kSat*dt/phi) * [N - (r*dt/clogFactor)*N*(N-1)/2]
     = 2.0 - 0.009*N + 0.00015*N*(N-1)

E[N] = kSat * dt * [N - (r*dt/clogFactor)*N*(N-1)/2]
     = 0.006*N - 0.00006*N*(N-1)
```

where E[N] is the cumulative exfiltration in ft of water (volume per unit plan
area), equal to `sum_{n=0}^{N-1} keff[n] * dt`.

The reference dataset covers 10 steps at dt=60 s (t = 0 to 600 s).  At N=10,
clogging has reduced keff to 80% of kSat; the clogging term (0.06 / clogFactor =
0.2) is non-trivial so the trajectory is clearly non-linear.

## Parameters

| Symbol     | Value    | Unit  | Notes                              |
|------------|----------|-------|------------------------------------|
| D[0]       | 2.0      | ft    | initial storage depth              |
| stor_thick | 3.0      | ft    | layer thickness (no overflow)      |
| kSat       | 1.0e-4   | ft/s  | native-soil saturated K            |
| phi        | 0.4      | —     | storage void fraction              |
| clogFactor | 0.3      | ft    | inflow needed to fully clog floor  |
| r          | 1.0e-4   | ft/s  | unit inflow rate (rainfall)        |
| dt         | 60       | s     | timestep per benchmark row         |
| N          | 10       | —     | number of steps (t = 0 to 600 s)  |
| drain_coeff| 0.0      | —     | no underdrain                      |

## Closed-form reference

```
wb_inflow[n] = n * 6.0e-3          [ft]
keff[n]      = 1e-4 * (1 - 0.02*n) [ft/s]
D[N]         = 2.0 - 0.009*N + 0.00015*N*(N-1)
E[N]         = 0.006*N - 0.00006*N*(N-1)
```

Note: `exfil_depth[n] = keff[n]*dt/phi = 0.015*(1-0.02*n)` and
`exfil[n] = keff[n]*dt = 0.006*(1-0.02*n)`.
The inflow contributes 0.006 ft/step and the exfil removes 0.006*(1-0.02*n) ft/step
(as water equivalent), with net depth change = 0.006 - 0.015*(1-0.02*n).

## Mass-balance identity

```
D[0] + sum_{n=0}^{N-1} r*dt - E[N]*1/phi ... (no, use water balance)
wb_inflow[N] - E[N] == (D[N] - D[0]) / phi ... (no)
```

The correct identity balances water volumes:
```
D[N] = D[0] + r*dt*N - exfil_depth_cumul[N]
where exfil_depth_cumul = sum exfil_depth[n] = E[N]/phi
```

Or equivalently (using storage volume = depth * phi):
```
phi*(D[N] - D[0]) = phi*r*dt*N - E[N]
```

## Numerical accuracy and tolerance

Each `batchBarrelFlux` call uses first-order Euler.  For this problem:
- `wb_inflow[n]` is an integer multiple of `r*dt` = 6e-3 ft — no accumulated error.
- `keff[n]` is computed freshly each step from `wb_inflow[n]` — no error.
- The depth and exfil updates are exact rational arithmetic at each step.

Expected FP error: ~1e-15 ft (floating-point rounding per step).
Test tolerance: 1e-9 ft (a factor of 10^6 above expected noise).

**Do not loosen the test tolerance.**  Any failure indicates a real regression in
the clogging formula, the exfil rate, the volume-limiter, or the Euler update.
Fix the code, not the tolerance.

## Verification target

`src/engine/hydrology/LID.cpp` — `LIDSolver::batchBarrelFlux`,
specifically the `getStorageExfil` (linear clogging) → depth update path.

## Reference dataset

`reference.csv` columns:
- `t_s` — time in seconds
- `stor_depth_ft` — exact storage depth in ft
- `E_cumul_ft` — cumulative exfiltration depth in ft of water

## Consuming test

`tests/unit/engine/test_lid.cpp` — `TEST(LIDStorageExfil, CloggingTrajectoryMatchesBenchmark)`

## References

- Rossman, L.A. and Huber, W.C. (2016). *Storm Water Management Model Reference Manual Volume I — Hydrology*. EPA/600/R-15/162A. Chapter 9 (LID storage unit and exfiltration model).
- Argue, J.R. (2004). *Water Sensitive Urban Design*. University of South Australia. (origin of linear clogging model for LID storage floor).
