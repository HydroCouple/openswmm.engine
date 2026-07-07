# Benchmark: exfil-storage-constant-area

## Problem statement

Rain-barrel storage unit draining by constant-rate exfiltration with no inflow,
no drain, and no clogging.  The governing ODE is:

```
d(depth)/dt  = -kSat / phi
depth(0)     = depth_0
```

where phi is the void fraction of the storage medium.  Because the ODE has a
constant right-hand side, the exact solution is linear:

```
depth(t)    = depth_0 - (kSat / phi) * t
E_cumul(t)  = kSat * t
```

where E_cumul is the cumulative exfiltration depth (volume per unit plan area).

The linear regime holds while `depth(t) * phi >= kSat * dt_step`, i.e., while
the available water per timestep exceeds the constant exfil rate.  The reference
dataset is confined to this regime so no volume-limiting logic is exercised.

## Parameters

| Symbol    | Value    | Unit  | Notes                        |
|-----------|----------|-------|------------------------------|
| depth_0   | 2.0      | ft    | initial storage depth (full) |
| stor_thick| 2.0      | ft    | storage layer thickness      |
| kSat      | 1.0e-4   | ft/s  | native-soil saturated K      |
| phi       | 0.4      | —     | storage void fraction        |
| clogFactor| 0.0      | ft    | no clogging                  |
| drain_coeff| 0.0    | —     | no underdrain                |

## Exact solution

```
depth(t)   = 2.0 - 2.5e-4 * t   [ft]
E_cumul(t) = 1.0e-4 * t          [ft of water]
```

Time to empty at constant rate: `depth_0 * phi / kSat = 2.0 * 0.4 / 1e-4 = 8000 s`.
Reference table covers t = 0–6000 s so the storage never empties.

## Mass-balance identity

```
E_cumul(t) == (depth_0 - depth(t)) * phi
```

The cumulative exfiltration must equal the water lost from storage.

## Numerical accuracy and tolerance guidance

`batchBarrelFlux` uses a first-order Euler step.  For a constant-rate ODE,
Euler is exact; the only error is floating-point rounding (~1e-15 ft).  The
benchmark tolerance is set to 1e-9 ft — a factor of ~10⁶ above the expected
noise floor.

**Do not loosen the test tolerance.**  If the trajectory test ever fails, it
means a real bug in the exfil rate, volume-limiting logic, or Euler update.
The gap between the expected error (~1e-15 ft) and the threshold (1e-9 ft) is
large enough that platform floating-point differences will never cause a false
failure.  A failure always indicates that the code changed in a way that
introduces a physically meaningful error — investigate the implementation,
not the tolerance.

## Verification target

`src/engine/hydrology/LID.cpp` — `LIDSolver::batchBarrelFlux`
specifically the `getStorageExfil` → depth update path.

## Reference dataset

`reference.csv` columns:
- `t_s` — time in seconds
- `stor_depth_ft` — exact storage depth in ft
- `E_cumul_ft` — exact cumulative exfiltration depth in ft (water volume / area)

## Consuming test

`tests/unit/engine/test_lid.cpp` — `TEST(LIDStorageExfil, TrajectoryMatchesBenchmark)`
