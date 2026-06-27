# modified-horton-fmax-saturation-recovery

## Purpose
Verify Modified Horton infiltration over a wet→dry→wet cycle that drives both
the Fmax cap and the exponential dry-recovery branch.

## Physical setup
- **Model**: Modified Horton (`modHorton_getInfil`)
- **Parameters** (ft-s units stored internally):
  - f0 = 4×10⁻⁵ ft/s  (1.728 in/hr)
  - fmin = 4×10⁻⁶ ft/s  (0.1728 in/hr)
  - kd = 1/600 s⁻¹  (6 hr⁻¹, passed as `decay` in horton_init)
  - kr = 1/3600 s⁻¹  (regen from `regen_days = −ln(0.02)/24 ≈ 0.163001 days`)
  - Fmax = 0.016 ft  (0.192 in)
- **Time step**: dt = 60 s
- **Forcing**: 10 wet steps (precip = 10⁻³ ft/s ≫ f0), 6 dry steps, 5 wet steps

## Algorithm (exact Euler)
**Wet step:**
1. fp = f0 − kd·Fe;  fp = max(fp, fmin)
2. If Fmh + fp·dt > Fmax:  f = (Fmax − Fmh)/dt  else f = fp
3. Fmh += f·dt;  Fe += max(f − fmin, 0)·dt;  Fe = min(Fe, Fmax)

**Dry step:**
1. decay_factor = exp(−kr·dt);  Fe *= decay_factor;  Fmh *= decay_factor

**Saturation at step 10** (t=540→600): Fmh cap triggers, f is reduced.
After wet phase 2, Fmh saturates again at step 18 (t=1020→1080).

## Columns
| Column | Units | Description |
|--------|-------|-------------|
| t_s | s | Time at end of step |
| precip_fts | ft/s | Precipitation intensity during step |
| f_fts | ft/s | Infiltration rate returned by solver |
| Fe_ft | ft | Cumulative excess infiltration after step |
| Fmh_ft | ft | Cumulative total infiltration after step |

## Expected accuracy
The Euler update is computed in exact double precision — no iterative solver.
Test tolerance: 1×10⁻⁹ ft for Fe and Fmh at each time step.

## Verification target

`src/engine/hydrology/Infiltration.cpp` — `modHorton_getInfil` (wet and dry branches, Fmax cap).

## Consuming test

`tests/unit/engine/test_infiltration.cpp` — `TEST(ModHortonInfil, SaturationRecoveryTrajectory)`

## References

- Horton, R.E. (1940). An approach toward a physical interpretation of infiltration-capacity. *Soil Science Society of America Proceedings*, 5, 399–417.
- Rossman, L.A. and Huber, W.C. (2016). *Storm Water Management Model Reference Manual Volume I — Hydrology*. EPA/600/R-15/162A. Section 4.3 (Modified Horton infiltration with Fmax cap).
