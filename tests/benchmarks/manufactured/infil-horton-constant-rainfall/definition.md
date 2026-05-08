# Benchmark: infil-horton-constant-rainfall

## Problem statement

Horton infiltration under constant rainfall intensity that always exceeds the
current infiltration capacity (fully ponded, capacity-limited regime throughout).

```
f(t)  = f_min + (f0 - f_min) * exp(-k * t)
F(t)  = f_min * t + (f0 - f_min)/k * (1 - exp(-k * t))
```

where `f` is the instantaneous infiltration rate and `F` is cumulative infiltration depth.

## Parameters

| Symbol | User units | SI (internal) units | Value |
|--------|-----------|---------------------|-------|
| f0     | in/hr      | ft/s                | 3.0 in/hr = 6.94444e-5 ft/s |
| f_min  | in/hr      | ft/s                | 0.5 in/hr = 1.15741e-5 ft/s |
| k      | hr⁻¹       | s⁻¹                 | 4.0 hr⁻¹ = 1.11111e-3 s⁻¹  |

These are the same parameters used in `test_infiltration.cpp` (HortonInfil test group).

## Scenario

Rainfall intensity is held at 5 in/hr throughout — well above f0, so infiltration
is always capacity-limited. The solver should track the exponential decay from f0
to f_min and accumulate F(t) accordingly.

## Verification target

`src/engine/hydrology/Infiltration.cpp` (`openswmm::infil::horton_getInfil`)

## Expected accuracy

Successive calls to `horton_getInfil` over the time grid should reproduce the
rate trajectory to within 1e-8 ft/s (absolute) and the cumulative depth to within
1e-7 ft. Looser tolerances may be appropriate if the implementation uses an ODE
integrator step internally rather than evaluating the formula directly.

## Reference dataset

`reference.csv` columns:
- `t_s` — time in seconds
- `f_ft_per_s` — infiltration rate in ft/s (engine internal unit)
- `F_ft` — cumulative infiltration depth in ft (engine internal unit)
- `f_in_per_hr` — same rate in in/hr (for readability)
- `F_in` — same cumulative depth in inches

Unit conversion: 1 in/hr = 1/12/3600 ft/s; 1 in = 1/12 ft.

## Consuming test

`tests/unit/engine/test_infiltration.cpp` — extend the `HortonInfil` group to
step through this time grid and compare `horton_getInfil` output against each row,
reporting max absolute error and RMS error over the trajectory.
