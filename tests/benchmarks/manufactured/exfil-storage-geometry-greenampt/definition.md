# Benchmark: exfil-storage-geometry-greenampt

## Problem statement

A single storage node uses an analytical storage shape together with the
storage-node exfiltration solver's bottom/bank Green-Ampt bookkeeping. The
storage stage is held fixed at `d = 2.0 ft`, so the benchmark isolates the
geometry partition and Green-Ampt seepage calculation without conflating it
with storage depletion.

The functional storage shape is:

```text
A(d) = 50 + 100 d   [ft^2]
```

At `d = 2.0 ft` this gives:

```text
bottom area = 50 ft^2
bank area   = A(2.0) = 250 ft^2
bank depth  = d / 2 = 1.0 ft
```

The Green-Ampt parameters are:

- suction head `S = 0.5 ft`
- initial moisture deficit `IMD = 0.2`
- saturated conductivity `K_s = 4.32 in/hr` (internal `1.0e-4 ft/s`)

The benchmark forces both bottom and bank states onto the saturated branch at
`t = 0`, so each component follows the exact implicit saturated Green-Ampt
relation:

```text
t(F) = [F - c1 ln(1 + F/c1)] / K_s
c1 = (S + depth) IMD
```

with separate `c1` values for the bottom and bank depths:

```text
c1_bottom = (0.5 + 2.0) * 0.2 = 0.5 ft
c1_bank   = (0.5 + 1.0) * 0.2 = 0.3 ft
```

The exact cumulative exfiltration volume is therefore:

```text
E(t) = 50 F_bottom(t) + 250 F_bank(t)
```

This is a true storage-geometry benchmark because the total seepage combines
the analytically defined bottom area and the analytically defined bank area.

## Parameters

| Symbol | Value | Unit | Notes |
|--------|-------|------|-------|
| `A(d)` | `50 + 100 d` | ft^2 | analytical storage surface area |
| `d` | 2.0 | ft | fixed storage stage |
| `A_bottom` | 50.0 | ft^2 | bottom seepage area |
| `A_bank` | 250.0 | ft^2 | bank seepage area at `d = 2 ft` |
| `d_bank` | 1.0 | ft | bank seepage depth argument |
| `K_s` | 4.32 | in/hr | Green-Ampt saturated conductivity |
| `S` | 0.5 | ft | suction head |
| `IMD` | 0.2 | - | initial moisture deficit |
| `dt` | 60 | s | benchmark spacing |
| `T` | 600 | s | total duration |

## Exact solution

For each component, solve the implicit saturated Green-Ampt equation for
`F_bottom(t)` and `F_bank(t)` with the appropriate `c1`, then compute:

```text
E(t) = 50 F_bottom(t) + 250 F_bank(t)
q(t_n) = [E(t_n) - E(t_{n-1})] / dt
```

At `t = 600 s` the exact cumulative exfiltration is `72.242681946059 ft^3`.

## Verification target

`src/engine/hydraulics/Exfiltration.cpp` — `ExfilSolver::init` and
`ExfilSolver::computeAll`

## Consuming test

`tests/unit/engine/test_exfiltration.cpp` —
`TEST(StorageExfilGeometry, FixedStageGreenAmptGeometryBenchmark)`

## References

- Green, W.H. and Ampt, G.A. (1911). Studies on soil physics, Part I. *The flow of air and water through soils*. Journal of Agricultural Science 4(1), 1-24.
- Rossman, L.A. and Huber, W.C. (2016). *Storm Water Management Model Reference Manual Volume I — Hydrology*. EPA/600/R-15/162A.