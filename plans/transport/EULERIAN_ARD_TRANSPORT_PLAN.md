# Eulerian Full-ARD Transport Plan (FV-Kernel Engine, All Hydraulic Solvers)

**Status:** Approved direction, 2026-08-12 (rev. 2 — rebased on the existing 1D
FV transport implementation per user decision; supersedes rev. 1's
CSH-style standalone discretization)
**Parent:** `plans/transport/UNIFIED_TRANSPORT_MASTER_PLAN.md` (Phase T2;
decisions D-UT1 as amended, D-UT6, **D-UT7 precedence: in-tree FV
implementation overrides HydroCouple/CSH conventions on conflict**)
**Numerical core:** `plans/EXPLICIT_FV_KOKKOS_1D_SOLVER_PLAN.md` §3.2,
D-FV1/D-FV2 — the transport machinery in
`src/engine/hydraulics/fv/ExplicitFvSolver.cpp` is promoted from FV-solver
internal to the shared Eulerian engine.
**Reaction machinery:** `plans/transport/MULTISPECIES_REACTIONS_MSX_PLAN.md`

---

## 1. Problem statement

Confirmed by audit (master plan §1): no ARD transport is reachable by users
today. The FV solver contains a scheme-verified advection–dispersion layer
(HLLC-consistent species flux ~`ExplicitFvSolver.cpp:809`, MUSCL/QUICKEST
reconstruction ~`:619`, Zalesak FCT ~`:786`, implicit per-chain dispersion
~`:2173`) but it is wired to zero species (`Routing.cpp:844`), has no
reaction term, no junction species handling, and only exists under
`FLOW_ROUTING FV`. STEADY/KINWAVE/DYNWAVE (and FV, in practice) all run the
legacy CSTR `QualitySolver`.

**This plan activates and generalizes that FV transport core** into the
engine selected by `[OPTIONS] QUALITY_SOLVER EULERIAN_ARD`, working
identically under all four routing models — rather than building a second,
parallel Eulerian discretization.

## 2. Formulation and precedence

Governing equation per conduit cell (generalizing `cell_phi` to ARD):

```
∂(Aφ)/∂t = −∂(Qφ)/∂x + ∂/∂x( D A ∂φ/∂x ) + A·r(φ⃗, p⃗) + s_lat
```

Numerics are the **existing FV implementation's** (D-UT7 — where the
HydroCouple CSHComponent conventions differ, the FV choices below win):

- **Mesh:** conduit cell chains from `NetworkMeshBuilder`
  (`src/engine/hydraulics/fv/NetworkMeshBuilder.cpp`), reused as-is. Cell
  count per conduit follows the FV meshing rules; a transport-only
  `ARD_TARGET_DX` override allows coarser transport meshes under non-FV
  hydraulics (default: same rules as FV).
- **Advection:** species flux upwinded on the face mass flux with the HLLC
  contact-speed convention (`k::speciesFlux`); face-value reconstruction via
  `FV_SCALAR_SCHEME`-style options `UPWIND | MUSCL (minmod et al.) |
  QUICKEST_ULTIMATE` (measured front-width ladder recorded in the FV plan
  D-FV2). CSH's central/hybrid (IDW, Péclet-switched) schemes are **not**
  carried over.
  **Riemann-solver clarification:** HLL/HLLC pertain to the *origin* of the
  face mass flux, and only the FV hydraulic solver has a Riemann solver.
  Under `FLOW_ROUTING FV`, species ride the Riemann mass flux and upwind on
  the HLLC contact speed `S*`; `FV_RIEMANN HLLC` (default) is strongly
  recommended whenever species are active, since HLL does not resolve the
  contact wave that carries scalars (D-FV2 front-width data: HLL+UPWIND 69
  cells vs HLLC+UPWIND 25) — selecting HLL with species active emits a
  sharpness warning. Under STEADY/KINWAVE/DYNWAVE there is no Riemann fan:
  the projection layer's continuity-consistent face fluxes are upwinded on
  their **sign** (the advection direction), which is the degenerate
  contact-speed criterion. Reconstruction, FCT, and dispersion are
  identical in both regimes; only the face-flux origin and upwinding
  criterion differ.
- **Monotonicity:** Zalesak FCT (`limitSpeciesFluxes`) — discrete maximum
  principle + exact conservation, always on. This replaces CSH's
  numerical-dispersion subtraction (eq. 4.19), which is dropped.
- **Dispersion:** implicit per-conduit-chain Thomas tridiagonal solve
  (`dispersionSolve`, decision D-FV1), Lie-split after advection (one full
  advection step, one full dispersion step — O(Δt); "Strang" would require
  half-steps. Terminology corrected in E3 validation; see roadmap lesson 13).
  Coefficient model: per-conduit user value, or Fischer et al. (1979)
  auto-computation `D = 0.011 v²B²/(Y·U*)`, `U* = √(gYS)` — the Fischer
  **coefficient model** is physics, not numerics, and is retained from
  CSH §4.2 *without* the numerical-dispersion correction.
- **Reactions:** shared MSX-convention module supplies `r(φ⃗, p⃗)` per cell,
  operator-split after dispersion (advection → dispersion → reaction). E4
  decision point (roadmap lesson 13): implement true Strang half-stepping
  for O(Δt²), or keep first-order Lie and document it. Integrators
  EUL/RK5/ROS2 from the reaction module.
- **Time marching:** explicit subcycling with the FV substep controller and
  CFL control; `QUALITY_STEP` caps the outer transport step. CSH's
  per-element ODE-solver menu (RK4/RKQS/ADAMS/BDF) is not carried over.
  LTS interaction: under FV hydraulics with species active, the existing
  D-FV7 gate applies (LTS disabled) until transport-aware LTS is revisited.

### 2.1 New numerics this plan adds to the FV core

The FV layer is AD-only and mesh-locked today; the additions:

1. **Junction species handling** — complete-mix internal boundary
   `φ_j = Σ Q_i φ_i / Σ Q_i` over inflowing faces (CSH §4.3 / Islam &
   Chaudhry — no conflict with FV, which currently has only zero-gradient
   ghosts), re-evaluated each substep for flow reversal; node BC and inflow
   source injection at junction faces (fixes the FV plan's "no node
   concentration" gap).
2. **Reaction source hook** per cell (AD → ARD).
3. **Storage-node mixing models** `CMSTR | TWO_COMPARTMENT | FIFO | LIFO`
   (shared `[STORAGE] ... MIXING_MODEL` token with LARD).
4. **Element coverage** beyond conduits, matching the LARD table
   (`LAGRANGIAN_QUALITY_STRATEGY.md` §2): pumps/orifices/weirs/outlets =
   zero-volume passthrough; dividers = complete mix; outfalls = sink with
   reverse-flow boundary concentration.

## 3. Architecture

### 3.1 Code motion: fv/transport promoted to shared

```
src/engine/transport/fvkernels/        # moved/refactored from hydraulics/fv/
  SpeciesAdvection.{hpp,cpp}           #   reconstructScalars, speciesFlux, FCT
  SpeciesDispersion.{hpp,cpp}          #   dispersionSolve (per-chain Thomas)
  TransportMesh.hpp                    #   cell/face views over NetworkMeshData
src/engine/transport/components/EulerianArdComponent/
  ArdEngine.{hpp,cpp}                  # driver: substep loop, junctions, BCs,
                                       #   reaction hook, storage mixing
  HydraulicsProjection.{hpp,cpp}       # §3.2 — non-FV solver fields → mesh
  ArdComponent.{hpp,cpp}               # HydroCouple IModelComponent surface
```

Refactor rule (CLAUDE.md §3 — surgical): kernels are **moved, not
rewritten**; `ExplicitFvSolver` calls the relocated kernels so the existing
solver-level tests (`tests/unit/engine/test_fv_solver_network.cpp`:
uniform-field, max-principle, mass-conservation, front-sharpness, implicit-
dispersion) keep passing unchanged against the shared code. The HydroCouple
`IModelComponent` wrapper sits **around** `ArdEngine`; per D-UT7 the wrapper
adapts to the kernels, never the reverse.

### 3.2 HydraulicsProjection (the solver-agnostic seam)

Under `FLOW_ROUTING FV`: `ArdEngine` consumes the solver's cell states and
face mass fluxes directly — zero projection, bitwise the same transport the
FV plan specifies.

Under STEADY/KINWAVE/DYNWAVE: each transport step, link/node solver fields
(`Q, A, y, V` at conduit ends + node heads, via the master plan's
`HydraulicsFieldProvider`) are projected onto the same cell mesh:

- cell areas/volumes interpolated along each conduit (same end-value
  reconstruction `NetworkMeshBuilder` uses for FV initialization);
- **face mass fluxes reconstructed to satisfy discrete continuity exactly**:
  linear interpolation of Q along the chain, then a per-chain correction so
  `ΔV_cell = (F_in − F_out)·Δt` holds cell-by-cell. This is the property
  FCT and uniform-field preservation depend on; it is the projection's
  correctness contract and gets its own unit gate (E1 below). Water lost or
  gained by the hydraulic solver's own non-conservation (if any) is booked
  to the mass-balance ledger, not silently absorbed.

### 3.3 Sources, boundaries, outputs

Unchanged from rev. 1: node inflow pathways deliver mass, volume, age-volume
and (with heat) enthalpy at the same loader seam (master plan §4.3), injected
at junctions — **as parallel per-capability accumulators per D-UT10, not as a
single widened tuple**; `[TRANSPORT_BOUNDARIES]` (per-node VALUE|TIMESERIES BCs) and
`[TRANSPORT_SOURCES]` (distributed kg/s/m, J/s/m over element ranges);
results published into the existing link/node quality arrays (volume-weighted
cell→link aggregation) so `.out`, `analysis_*`, and the GUI work unchanged;
optional per-element HDF5 sidecar under `TRANSPORT_DETAILED_OUTPUT`;
per-species mass-balance ledger rows.

## 4. Options

```
[OPTIONS]
QUALITY_SOLVER        LEGACY* | EULERIAN_ARD | LAGRANGIAN
ARD_SCALAR_SCHEME     UPWIND | MUSCL* | QUICKEST_ULTIMATE   ; alias of FV_SCALAR_SCHEME
ARD_LIMITER           MINMOD* | VANLEER | SUPERBEE           ; alias of FV_LIMITER
ARD_DISPERSION        OFF* | FISCHER | value                 ; global; per-conduit override
ARD_TARGET_DX         meters (transport mesh under non-FV hydraulics; default = FV rules)
QUALITY_STEP          seconds (shared with LARD)
```

`ARD_*` and the existing `FV_SCALAR_SCHEME` / `FV_LIMITER` / `FV_DISPERSION`
keys write the same internal transport config (aliases; FV_* retained for
back-compat, `WARN_FV_OPTION_INERT` retired once the engine activates them).
When `QUALITY_SOLVER EULERIAN_ARD` is active there is exactly one transport
path regardless of routing model — the "in-solver vs standalone" distinction
of rev. 1 disappears.

**Placement (D-UT8):** only `QUALITY_SOLVER` stays in `[OPTIONS]`. The
scheme/limiter/dispersion options and the `[TRANSPORT_BOUNDARIES]` /
`[TRANSPORT_SOURCES]` / `[CONDUIT_DISPERSION]` / `[STORAGE_MIXING]` sections
live in the ARD component's external config file (`model.ard`) registered
via `[PROCESS_COMPONENTS]` — see `TRANSPORT_IO_PLUGIN_CONFIG_PLAN.md` §3.2.

## 5. Python API

Unchanged surface from rev. 1 (`sim.transport`, `openswmm_transport.h`, MCP
`transport_*`), with scheme enums matching §4:

```python
sim.options.quality_solver = "EULERIAN_ARD"
tr = sim.transport
tr.scalar_scheme = "QUICKEST_ULTIMATE"; tr.limiter = "MINMOD"
tr.set_dispersion(link="C12", value=1.5)      # m²/s
tr.set_dispersion_mode("FISCHER")
tr.set_boundary(node="OUT1", species="TDS", timeseries="tds_bc")
tr.element_results("C12", species="TDS")
```

**GUI:** `QUALITY_SOLVER` combo + ARD group on the "Quality & Transport"
options page — scheme/limiter combos reflect the FV set above; per-link
dispersion, storage mixing model, and node boundaries from the property
panel via MapCommands; per-element profiles via the detailed-output sidecar
— see `openswmm.gui/workplans/TRANSPORT_QUALITY_GUI_PLAN_2026-08-12.md`.

## 6. Implementation phases

```
E0  Kernel promotion: move species advection/dispersion/FCT to
    src/engine/transport/fvkernels/, re-point ExplicitFvSolver.
    → verify: all existing FV solver-level transport tests pass unchanged;
      no behavior diff on FV benchmark models (bitwise).
E1  ✅ 2026-08-16 (validated; see E1_VALIDATION_HANDOFF_2026-08-16.md §5 —
    two E1 defects + four pre-existing engine faults fixed in validation;
    gates 3/3; ARD conserves on site_drainage where the legacy CSTR path
    does not). HydraulicsProjection for STEADY/KW/DW +
    continuity-consistent face-flux reconstruction; ArdEngine driver with
    conservative tracer; junction node-stores with donor-upwinded boundary
    fluxes landed here (pulled forward from E2 — a tracer that cannot pass
    junctions is untestable).
E2  Element coverage: structures (pumps/orifices/weirs/outlets) as
    zero-volume donor passthrough; persistent user quality-mass-flux
    forcing into the node stores; loud CFL subcycle clamp (validation §5.3
    note); CSTR-limit gate.
    → verify: structure passthrough gate (orifice deck); CSTR limit
      (FV_MIN_CELLS 1, UPWIND) tracks LEGACY within documented band;
      max principle across structures.
E2b Storage mixing models beyond CMSTR (TWO_COMPARTMENT/FIFO/LIFO — the
    LARD-shared [STORAGE] MIXING_MODEL token; no inert-option parsing
    before semantics exist) + FV direct-cell-state consumption (needs the
    FV solver to accumulate time-integrated face mass fluxes per routing
    step — solver-side work) + tidal outfall reverse-flow boundary
    concentration (with E5's [TRANSPORT_BOUNDARIES]).
E3  ✅ 2026-08-16 (validated, `7684af53`; see
    E3_VALIDATION_HANDOFF_2026-08-16.md §5). Dispersion activation
    (per-conduit D, FISCHER mode) under all solvers via the transport.ard
    component (D-UT8). Kernel per-cell coefficients with bitwise-preserved
    FV scalar path; exact 2·D·dt discrete-variance gate stands in for the
    Taylor-moments harness (deferred to LARD G4); implicit-step
    restriction gated (huge-D bound). Validation added gate 11 +
    warnIfFvDispersionKeyIgnored (FV_DISPERSION under EULERIAN_ARD warns;
    aliasing deferred to E5) and corrected the splitting terminology to
    Lie.
E4  ✅ 2026-08-17 (validated, `4df5cc0f`; see
    E4R6_VALIDATION_HANDOFF_2026-08-16.md §5). Reaction stage per routing
    step, LIE split (decision recorded — revisit trigger: LEGACY-vs-ARD
    convergence data): exact-exponential kdecay on cells + stores (E1
    warning retired) + MSX per cell (pipe) / per store (tank) with
    pollutants readable. MSX species TRANSPORTED on the mesh (R6); WALL →
    LEGACY fallback; MSX inflow conc zero until E5. Validation found one
    production defect (store clamp swept by the np-narrowed load loop —
    roadmap lesson 14) and added the symmetric-row gate 9 (lesson 15).
    D-R10 resolved: RK5 stays default. nh2cl network parity re-deferred:
    needs runnable EPANET-MSX + E5's [TRANSPORT_BOUNDARIES] inlet BC.
E5a ✅ 2026-08-17 (validated, `cbb9d321`; see
    E5A_VALIDATION_HANDOFF_2026-08-17.md §5). [TRANSPORT_BOUNDARIES]
    (MSX inlet BCs on external inflow, VALUE/TIMESERIES) +
    [TRANSPORT_SOURCES] (distributed conduit sources, mass/s →
    conc·ft³/s) + SCALAR_SCHEME/LIMITER model.ard aliases;
    order-independent post-apply row resolution. Validation found + fixed
    the six `np <= 0` loader guards blocking external-inflow VOLUME on
    MSX-only decks (roadmap lesson 20 — the motivating configuration
    belongs in the gate matrix). nh2cl ENGINE side verified by running
    (NH2CL 2.0→1.358 monotone, TOC 4.0→3.856); the only remaining parity
    blocker is an external runnable EPANET-MSX reference. Recorded
    limitation: BCs apply to the node's TOTAL external inflow (no
    per-pathway influent concentrations yet).
E5b ✅ 2026-08-17 (validated, `721ae60c`; see
    E5B_IO3_VALIDATION_HANDOFF_2026-08-17.md §5). Treatment interop
    (legacy evaluator on published conc, treated-only absorb), kdecay
    reacted-ledger booking, CSV detail sidecar, TARGET_DX resolved (§8;
    same line fixed the E1-era raw-mesh-options SI defect, measured
    3.30× on CMS), IO3 save-as carry-alongside (destructive-overwrite
    defect found+fixed: content-compare + warn — lesson 23), IO5 closed.
    BOUNDED claims recorded: treatment mass_lost is step-DEPENDENT
    (pre-existing legacy defect, both engines — carried for its own
    parity round) and the flowing-deck ledger NARROWS (12.678 → 6.504 of
    22.474 unattributed; suspect: unbooked store-resync scale-down) —
    both on the roadmap carry row. Still open here: dry-cell source-share
    accounting; nh2cl parity awaits an external EPANET-MSX reference;
    broader D-R10 sweep.
E6  C API + Python + MCP + GeoPackage/INP round-trip + parity registries
    (G-UT6) + docs.
E7  HydroCouple IModelComponent wrapper + Composer smoke test (G-UT5).
    Wrapper adapts to kernels per D-UT7.
```

## 7. Performance notes

The FV kernels are already SoA/branch-lean; projection adds one linear pass
per chain per transport step. CSTR-limit run cost target: ≤1.1× LEGACY.
OpenMP over conduit chains for dispersion tridiagonals. GPU: the promoted
kernels remain the substrate for the FV plan's device path; standalone-engine
GPU execution stays out of scope (master plan §8) until profiling says
otherwise.

## 8. Open items

- `ARD_TARGET_DX` default under non-FV hydraulics (FV meshing rules vs
  coarser transport-only default) — decide in E1 review with benchmarks.
- Whether STEADY retains the legacy exact-exponential decay shortcut at the
  CSTR limit (`QualityRouting.cpp:517-523`, Gap #38) — proposal: LEGACY
  only; ARD stays scheme-consistent.
- Transport-aware LTS under FV hydraulics (D-FV7 currently disables LTS
  with species) — revisit after E4 with measured cost.
