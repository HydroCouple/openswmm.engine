# Richards 1D LID implementation — 2026-10-05

This is the first implementation milestone of the surface–subsurface program,
authorized by the user on 2026-10-05. The updated existing LID model remains
the default. UEB and original rounds R1–R10 are not completed by this milestone.

## Model and numerics

For porous control volume i, evolve complete water volume

`W_i = V_i [theta(h_i) + Ss_i max(h_i, 0)]`,

`dW_i/dt = Q_(i-1/2) - Q_(i+1/2) - ET_i`.

Here V is geometric volume, h is pressure head, and total head is z+h. The
unsaturated law is van Genuchten retention with m=1-1/n; conductivity is
Mualem with independently authored l. Pressure is recovered by the inverse
storage law, including positive saturated pressure. Theta remains bounded by
porosity; elastic water is included in continuity, not clipped away.

Each authored porous layer has a uniform cell count. Within a homogeneous
material use arithmetic conductivity interpolation, following the spatial
formulation documented in [openRE, Ireson et al. (2023), equations 11 and 14](https://gmd.copernicus.org/articles/16/659/2023/).
At different-material interfaces use the two half-cell resistances in series.
The pond/contact boundary uses the surface saturation conductivity and first
cell conductivity. Downward supply tends continuously to zero over the last
1 micrometre of ponding; upward flow remains possible. This regularization
changes behavior only at that thin pond scale and does not discard water.

The first ODE method is adaptive BDF1, with a full-step/two-half-step error
estimate; the two half steps are committed. Newton uses three-color finite
differences of the local flux Jacobian and a tridiagonal solve, with a
positivity-preserving line search. This is not the existing dense BDF2/ROS2
reaction solver. Higher-order integration is a later measured optimization.

Pack equal-size columns cell-major/lane-minor in batches of at most 64.
Each lane has its own clock, step size, error decision and active mask. A
failed column rolls back the entire submitted batch. This implementation
uses a batched CPU layout; no GPU or guaranteed hardware SIMD speedup is
claimed. Record accepted/rejected steps, RHS/Newton counts, minimum step and
integrated water-balance residual for each coupling interval.

## Ownership and coupling

The porous column owns all pore/elastic storage; the node owns surface
ponding only. No saturated water is duplicated in a shared node volume.
Positive lateral supply is captured once at the start of the interval.
Pressure and moisture used by ports are local to the intercepted cell.
Hydraulic port trials accumulate a donor/recipient storage delta, reset for
repeated trials, and commit once. The native BOTTOM supplies free drainage
limited by both column and native conductivity; a zero conductivity seals it.
Existing clogging/climate factors are retained.

The routing interval remains an operator split: complete the vertical ODE
against frozen boundary inputs, solve hydraulic ports, then commit port
storage. A new application requires routing-step convergence as well as
spatial and ODE-tolerance convergence. Numerical cell count is not physical
layer count. Both MEDIA and AGGREGATE participate in Richards integration.

Accepted chronological directional volumes supply the existing pollutant
ledger. Bound a Richards donor debit by available mass to avoid amplifying
roundoff in a nearly dry pond. Preserve dry pond solute and redissolve it on
subsequent supply. The existing flow/quality paths retain their original
calculations when Richards is disabled.

## Input, API and GUI

See [the LID manual](../docs/manuals/engine/sections/Chapter6-LidStorageNodes.md)
for `[LID_RICHARDS]` syntax and units. Every porous material requires explicit
residual theta, alpha, n, l and positive specific storage. No conversion from
conductivity slope or Green-Ampt suction is inferred. Alpha and Ss use 1/m in
both project unit systems. Existing geometry conversions are retained.

The complete stack/treatment/flow edit is atomic in
`swmm_lid_node_configure_flow`. INP and GeoPackage preserve mode and material
parameters, including materials retained while the existing mode is selected.
Inactive numerical options return to defaults on file reload. Hotstart 11
stores complete porous water plus material identity; mismatches are rejected.
Existing-model hotstarts retain extension 10.

The editor exposes model choice, per-layer retention data and curve preview,
with expandable numerical settings. The live section exposes cell moisture,
pressure in project length units, and last-interval solver/balance information.
The API additionally exposes total head and complete cell water.

## Validation and current limits

Evidence is under
`tests/verification/surface_subsurface_program_2026-10-05/` and GUI artifacts
under `openswmm.gui/tests/gui/data/`.

- Nine kernel tests cover retention inversion through saturation, sealed
  infiltration, saturated hydrostatic equilibrium, capillary rise, independent
  versus batched clocks, whole-batch rollback, native drainage/ET continuity,
  a low-conductivity material barrier, and tolerance convergence.
- LID integration tests cover complete water ownership, trial port budgets,
  solver failure rollback, US/SI state using existing geometry conversions,
  invalid atomic edits, inactive material persistence, INP/GeoPackage,
  hotstart identity/continuation and routed drainage/backwater with orifices
  and conduits. Standard pollutant balances include dry-weather inputs.
- An independent NumPy/SciPy Radau implementation of the same spatial equations
  compares 8/16/32/64-cell sealed profiles after 600 seconds. Maximum moisture
  discrepancies are 2.40e-6, 9.57e-6, 1.17e-5 and 1.22e-5 at the tighter
  production export tolerances. These are time-integrator comparisons, not
  independent field calibration or proof of mesh convergence for all soils.
- A runtime fixture verifies that active aquifer beds fail explicitly while
  `EXCHANGE NO` permits the native-soil boundary.

The kernel supports prescribed-head bottom flow for verification, but runtime
2D aquifer coupling must await the original R5 bottom-interface adapter.
Reject active aquifer beds rather than applying the previous surface-node
exchange at the wrong depth. Reject Richards water-age, heat and MSX options
until their porous-cell transport adapters exist. Saved profile time histories
are also pending; the current profile is live state only.

BDF1 can require hundreds of accepted steps for a sharp wetting front.
`richards_cost.csv` is preliminary timing collected during builds, not a
controlled whole-model performance measurement. `richards_cost_quick.csv`
records a smaller repeated kernel workload with operation counts. Benchmark
calibrated application profiles before choosing production cell counts or
claiming a speedup. Specific storage must be measured rather than inflated
for solver speed. Broader soil/front, coupling-step and long-duration
verification remains before treating this option as universally validated.

No matched-source full corpus result is claimed for this milestone. Existing
LID regression tests are rerun; the original program's round-wide corpus gates
remain requirements of subsequent shared-process/default changes.
