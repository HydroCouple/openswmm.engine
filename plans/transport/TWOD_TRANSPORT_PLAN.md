# 2D Transport Plan (Surface + Planned Groundwater: ARD, Multispecies Reactions, Heat)

**Status:** Approved direction, 2026-08-12
**Parent:** `plans/transport/UNIFIED_TRANSPORT_MASTER_PLAN.md` (Phases T6–T7)
**Hydrodynamic substrate:** `plans/2d/2dModelStrategy.md`,
`plans/2D_EXPLICIT_MARCHER_REPORT.md` (local-inertial explicit marcher is the
only 2D integrator; CVODE stack retired), `src/engine/2d/solver/ExplicitInertialSolver.cpp`,
GPU: `src/engine/2d/gpu/ExplicitKokkosSurfaceSolver.cpp` + `GpuPluginAbi.h`.
**Groundwater substrate:** `plans/TWO_ZONE_GROUNDWATER_FV_INTEGRATION_PLAN.md`
(PIHM-style two-zone kernel on the 2D mesh — still draft, needs refresh against
the explicit-marcher stack; this plan adds its transport layer).
**GW formulations guide:** HydroCouple GWComponent (Buahin, Neilson & Horsburgh
2019 §7) — vertically-averaged FV heat (eqs. 13–24) and solute (eqs. 25–27)
transport with MODFLOW River-package channel exchange.

---

## 1. Current state (audit)

The 2D model is hydrodynamics-only: state = cell volume; `grep` over
`src/engine/2d/` finds zero transport code; the 1D↔2D coupling
(`src/engine/2d/coupling/NodeCoupling.cpp`, `coupling_volume` →
`coupling_queue` → `lat_flow`) carries **volume only**, so any scalar would
be silently lost at every exchange. No groundwater kernel exists in code.

## 2. Scope

**Packaging (D-UT9 as amended):** everything in this plan ships inside
**one unified HydroCouple component** — `org.hydrocouple.openswmm.integrated2d`
— because the 2D surface router and the two-zone groundwater model share
the same triangular mesh and are tightly coupled (infiltration, Dunne
flow, return flow, capillary-rise ET). One component id, one config file
(`model.i2d`, IO plan §3.2), with capability toggles for surface-only /
groundwater-only (incl. `PER_SUBCATCH`) / fully integrated runs. The scope
items below are component capabilities, not separate modules.

1. Eulerian ARD for the 2D surface mesh (species + `__WATER_AGE__` +
   `__TEMPERATURE__`), sharing the species registry and reaction module.
2. Species/age/enthalpy channel through 1D↔2D coupling (both directions)
   and 2D boundary conditions.
3. Surface heat fluxes (radiative/latent/sensible) on wet cells, reusing
   the Phase T4 flux modules per cell.
4. Groundwater transport (solute + heat) on the planned two-zone aquifer,
   per GWComponent formulations, including channel–aquifer mass/heat
   exchange.
5. GPU parity for surface transport via the existing Kokkos plugin ABI.

## 3. Surface transport formulation

Depth-averaged ARD per triangle cell i:

```
∂(V_i φ_i)/∂t = − Σ_e F_e φ_up(e)  +  Σ_e (D A_e / d_e)(φ_j − φ_i)
                + V_i r(φ⃗)  +  A_i s_i
```

- **Advection rides the hydrodynamic face fluxes.** `F_e` are the exact
  face volume fluxes the marcher computes (`SurfaceFluxCalculator.cpp` /
  `InertialEdges.cpp`); species are upwinded on `sign(F_e)`. Flux
  consistency with mass — the same principle the 1D FV plan enforces —
  guarantees uniform-field preservation. FCT limiting (Zalesak, as in
  `ExplicitFvSolver::limitSpeciesFluxes`) for the discrete max principle.
- **Transport substeps ride the marcher's LTS tiering** (`fireFaces` /
  `fireCells`): a cell's species update fires when its hydraulic update
  fires, using the face-flux time integrals accumulated since the last
  firing (mass-conservative by construction).
- **Dispersion/diffusion:** isotropic depth-averaged coefficient
  `D = ε + α_L |u| h` (velocity from the RT0 reconstruction,
  `2D_EXPLICIT_MARCHER_REPORT.md`); explicit two-point flux on the TIN
  (mesh is Delaunay; note anisotropy limitation), stability-capped by the
  same substep controller. Options `2D_DISPERSION`, `2D_DIFFUSION`.
- **Reactions:** per wet cell via the shared VM (Strang split at cell
  firing granularity). Dry cells hold state mass (no reaction below
  `H_DRY`; documented).
- **Wet/dry:** species mass is conserved through wetting/drying —
  concentration recomputed from cell species-mass / volume with the same
  Hermite-ramp guards the hydraulics use; no mass is created at re-wetting.
- **Heat:** `__TEMPERATURE__` transported identically; per-cell surface
  fluxes Jsn/Jan/Jbr/Jlc/Je/Jc from the Phase T4 modules using cell area,
  water temperature, and met forcing (`twod_force_*` runtime forcing tools
  gain heat variants). Bed conduction optional (sediment layer per cell,
  HTS-style).

### 3.1 State and memory

```
SurfaceTransportState (SoA, sized n_cells × n_species):
  cell_species_mass[]   // conserved variable (mass, age-volume, enthalpy)
  cell_conc[]           // derived, published for output
  + per-species BC arrays keyed to edge BCs
```

Registered in the 2D hotstart (version bump) and the HDF5 output plugin
(`Default2DOutputPlugin` — add species/temperature UGRID variables).

### 3.2 GPU

Extend `GpuPluginAbi.h` with optional transport entry points
(probe-gated so existing plugins remain valid); Kokkos kernels mirror the
CPU face-flux/upwind/FCT path. CPU is the reference; GPU parity gate
mirrors the 2D_GPU_PHASE* verification pattern.

## 4. 1D↔2D and boundary coupling

- **Node↔cell exchange:** everywhere `coupling_volume` moves water, a
  parallel per-species tuple moves `(mass, age_volume, enthalpy)` at the
  donor's concentration/temperature (1D node quality when draining to 2D;
  cell values when draining to 1D). The uniform `coupling_queue` drain
  carries the same tuple fractions. Outfall head-BC path
  (`head_2d`/`ramp_2d`) likewise gains donor-concentration transport.
- **Edge BCs:** inflow edge BCs get species/temperature/age values
  (`twod_set_edge_bc` extension + `BOUNDARY_2D` rows in
  `[WATER_AGE_SOURCES]` / `[HEAT_SOURCES]` / `[REACTION_QUALITY]`).
- **Rainfall/evap on cells:** rainfall enters at rain concentration
  (`rain_conc`), RAINFALL age, `RAINFALL_TEMP`; evaporation removes volume
  at cell concentration (species mass conserved for solutes, enthalpy
  removed with latent flux for heat — consistent with §2.1 of the heat plan).
- **Mass balance:** per-species rows in the 2D ledger
  (`twod_get_mass_balance`) and the coupled-system ledger so 1D+2D species
  totals close (G-UT4).

## 5. Groundwater transport (Phase T7) — spatially explicit two-zone model

**Substrate:** the planned spatially explicit two-zone aquifer of
`plans/TWO_ZONE_GROUNDWATER_FV_INTEGRATION_PLAN.md` — per-triangle
unsaturated equivalent depth `h_u` and saturated thickness `h_g` on the 2D
mesh (PIHM lineage, Qu & Duffy 2007), with Darcy lateral saturated exchange
(its eq. 21 stencil), enslaving/kinematic-wave unsaturated columns, and the
four unlocked feedbacks (Dunne saturation-excess, return flow, head-driven
node/pipe↔aquifer exchange, capillary-rise ET). That plan is authoritative
for the flow structure; the HydroCouple GWComponent (2019 report §7, a
single vertically-averaged saturated layer) guides the transport
discretization of the **saturated** zone and does not override the two-zone
structure (precedence note: no in-tree implementation exists yet, so the
vetted two-zone plan is the baseline this section extends).

### 5.1 State — both zones, per triangle

```
SubsurfaceTransportState (SoA, n_tri × n_species × 2 zones):
  unsat_species_mass[] , unsat_conc[]   // C_u, T_u, age_u  (per θ·D_u water volume)
  sat_species_mass[]   , sat_conc[]     // C_g, T_g, age_g  (per n·h_g water volume)
```

Registered in the 2D hotstart alongside `h_u/h_g` (same version bump) and
in the HDF5 output plugin as per-zone UGRID variables.

### 5.2 Zone processes

- **Unsaturated zone (vertical column, no lateral transport):** consistent
  with the two-zone plan's column reduction, transport is 1D vertical —
  advection by the infiltration/recharge flux through the column,
  retardation `R_f`, reactions via the shared VM, and vertical heat
  conduction between the ground surface (or 2D cell water when inundated)
  and the saturated zone. Effective thermal properties use the actual
  moisture state (`ρm cm = θ ρw cw + …`), not fixed porosity.
  **Update (2026-08-15, per
  `plans/TWO_ZONE_GROUNDWATER_EXPLICIT_LTS_PLAN_2026-08-15.md`):** under
  the σ-based closure B, unsat species/heat/age state is **per σ-layer**
  (`n_species × m × Ntri_B`), riding the same moving-grid fluxes
  (`F_{j±1/2}` carry donor concentrations; `F_bot` is the §5.3 donor
  tuple); closure-A cells keep the bulk per-zone state of §5.1.
- **Saturated zone (lateral 2D transport):** GWComponent formulations on
  the two-zone plan's Darcy stencil — solute (eq. 25)
  `R_f ∂C/∂t = ∇·[(D_m + α v)∇C] − ∇·(vC) + S_c − λ₁C`,
  `R_f = 1 + ((1−n)/n) ρ_s K_d` (eq. 26); heat (eqs. 14–16) effective-medium
  conduction–dispersion–advection with
  `ρm cm = n ρw cw + (1−n) ρs cs`, `λm = n λw + (1−n) λs`. Advection
  upwinded on the saturated Darcy face fluxes the flow kernel already
  computes (upwind first; the GWComponent hybrid/TVD menu is optional
  follow-up). Reactions via the shared VM; rate/equil expressions
  generalize λ₁; scope token `SUBSURFACE` joins `SURFACE2D` in the
  reactions plan's recorded MSX deviations.

### 5.3 Inter-zone and boundary exchanges (all donor-upwinded tuples)

Every water flux in the two-zone plan carries the master plan §4.3
`(mass, volume, age_volume, enthalpy)` tuple at donor concentration:

| Flux (two-zone plan) | Transport behavior |
|---|---|
| Infiltration: 2D cell / ground surface → unsat | surface donor conc/temp/age |
| Recharge: unsat → sat (incl. kinematic column outflow) | unsat column donor |
| Capillary-rise ET: sat → atmosphere | removes water only — solutes and age-volume stay (evapoconcentration), latent enthalpy removed per heat plan §2.1 |
| Return flow / Dunne exfiltration: sat → 2D cell | saturated donor |
| Node/pipe ↔ aquifer head-driven Darcy exchange (RDII/exfiltration mechanism; MODFLOW River-package config a/b/c, GWComponent eqs. 10–12) | `Q_C·C_donor`, `ρw cw Q_C T_donor` (eqs. 23–24); donor = channel when Q_C > 0 into aquifer, saturated cell otherwise |
| Lateral sat↔sat between triangles | upwinded on eq. 21 face fluxes |

### 5.4 Watershed tie-in and reserved species

The subcatchment GW age/temperature states (age plan §3, heat plan §3) are
the degenerate `PER_SUBCATCH` mode of this layer — one two-zone column per
subcatchment — keeping 1D-only and fully 2D configurations on one code
path. `__WATER_AGE__` and `__TEMPERATURE__` are ordinary species slots in
both zones (age source `GW` initial-age rows seed `sat_conc` at t=0).

## 6. Options and API

```
[OPTIONS]      2D_TRANSPORT ON|OFF*   ; legacy .inp — coarse toggle only (D-UT8)

; --- integrated2d UNIFIED component config (model.i2d), via [PROCESS_COMPONENTS]
;     (D-UT9: shared mesh + surface hydraulics + two-zone subsurface +
;      transport + heat — one file, one component):
[INTEGRATED2D_OPTIONS]  SURFACE_ROUTING ON|OFF  GROUNDWATER ON|OFF
                        TRANSPORT ON|OFF  HEAT ON|OFF
[2D_VERTICES] [2D_TRIANGLES] [2D_*_NODE_MAP]      ; shared mesh (or [2D_MESH_FILE] ref within)
[2D_OPTIONS] [2D_BOUNDARY_CONDITIONS] ...          ; surface hydraulics & parameterization
[2D_AQUIFER]-lineage sections (two-zone plan)      ; subsurface zonation/closures/K/Sy on same triangles
[2D_TRANSPORT_OPTIONS]  2D_DISPERSION αL [ε]  2D_REACTIONS ON|OFF
[2D_TRANSPORT_BC] / [2D_INITIAL_QUALITY] / [2D_HEAT_OPTIONS]
[GW_TRANSPORT_OPTIONS] / [GW_TRANSPORT_PARAMS]
               DISPERSIVITY, DIFFUSION, RF params (rho_s, Kd), LAMBDA_S,
               C_S (sediment heat capacity), THERMAL_COND_S
```

Placement per `TRANSPORT_IO_PLUGIN_CONFIG_PLAN.md` §3.2 (D-UT8/D-UT9): the
2D surface router and the two-zone groundwater model form **one unified
HydroCouple component** (`org.hydrocouple.openswmm.integrated2d`) — they
share the mesh, so mesh, parameterization, transport, and heat for both
domains live in one config file. No separate transport components. The
component consumes the shared reaction module and species registry
(chemistry is not duplicated). Legacy embedded `[2D_*]` sections and the
top-level `[2D_MESH_FILE]` remain as a deprecated path; the component
config is authoritative when registered.

**C API** — new header `include/openswmm/engine/openswmm_transport2d.h`
(house rules: snake_case, int status, caller-allocated buffers, `*_count`
companions), implemented with phases S1–S5:

```c
swmm_t2d_enabled(h, int* on);
swmm_t2d_set_options(h, const SWMM_T2DOptions*);   /* dispersion αL/ε, reactions, heat */
swmm_t2d_get_conc_bulk(h, int species, double* c, int n);       /* per triangle */
swmm_t2d_get_temperature_bulk(h, double* t, int n);
swmm_t2d_get_age_bulk(h, double* a, int n);
swmm_t2d_set_cell_conc(h, int tri, int species, double c);      /* init/BC seeding */
swmm_t2d_set_edge_bc(h, int edge, int species, double value);   /* + timeseries variant */
swmm_t2d_get_species_mass_balance(h, int species, SWMM_T2DMassBalance*);
swmm_t2d_get_coupling_mass(h, int node, int species, double* m); /* 1D↔2D tuple diagnostics */
```

GW-zone transport getters live beside the flow API in `openswmm_gw2d.h`
(per-zone/per-σ-layer concentration getters mirror `swmm_gw2d_get_theta_column`).
Python: `sim.transport2d` (cell/edge getters-setters, BC configuration,
`cell_concentration(species)`, `cell_temperature()`, `cell_age()`); MCP
`twod_*` transport tools; parity registries per G-UT6. Reaction
configuration is the same `sim.reactions` surface (expressions apply to 2D
cells via `[REACTION_PIPES]`-analogue scope `SURFACE2D` — one new scope
token, recorded as an MSX deviation in the reactions plan).

**GUI:** 2D transport group on the existing 2D Surface Routing options page;
species/temperature/age rendered through a `ScalarFillSublayer` feed with
variable metadata in the HDF5 sidecar (name + units attributes so the GUI
enumerates variables without hard-coding) — see
`openswmm.gui/workplans/TRANSPORT_QUALITY_GUI_PLAN_2026-08-12.md` §1, §3.6, G6.

## 7. Implementation phases

> **⚠ Superseded for the SURFACE track (2026-09-01).** S1–S6 below are the
> outline; `OVERLAND_TRANSPORT_HEAT_MSX_PLAN_2026-09-01.md` is the
> implementable design — mass-form state, LTS-consistent advection inside
> `fireFaces`/`fireCells`, the coupling tuple, per-cell heat and MSX, and
> acceptance criteria with falsifiers. **S7 (groundwater) stays here.**


```
S1  SurfaceTransportState + conservative tracer advection on face fluxes
    (LTS-consistent) + wet/dry conservation + HDF5 output.
    → verify: uniform-concentration preservation under arbitrary flow;
      rotating-slope dam-break tracer mass conservation to 1e-12;
      discrete max principle.
S2  Dispersion + FCT + boundary conditions + rainfall/evap species rules.
    → verify: 2D point-release vs analytical Gaussian (flat plane,
      uniform flow); Péclet sweep.
S3  1D↔2D species/age/enthalpy coupling channel + coupled mass balance.
    → verify: 1D→2D→1D round-trip conservation on the weir/road test
      models (plans/2d/phase3d_ab fixtures); age continuity across the
      coupling.
S4  Reactions on cells + water age + temperature transport.
    → verify: batch-reactor parity vs 1D engines at zero flow; age =
      travel-time on a tilted-plane steady flow.
S5  Surface heat fluxes per cell (reuse T4 modules) + met forcing.
    → verify: pond diurnal temperature cycle vs CSH-style column model;
      energy closure.
S6  GPU transport kernels behind extended plugin ABI.
    → verify: CPU/GPU bitwise-tolerance parity per 2D_GPU verification
      pattern.
S7  GW transport (after two-zone kernel lands): per-zone state (§5.1),
    unsat column transport, saturated lateral ARD + heat, all §5.3
    exchange tuples, PER_SUBCATCH degenerate mode.
    → verify: GWComponent golden cases (1D lateral head-driven plume,
      heat plume vs Hecht-Méndez benchmarks cited in §7); inter-zone tuple
      conservation (closed column: infiltration → recharge → return flow,
      species/age/enthalpy ledger closes); evapoconcentration test
      (capillary-rise ET raises C, conserves mass); channel-aquifer
      exchange symmetry; PER_SUBCATCH mode reproduces the age plan §3 /
      heat plan §3 subcatchment states on a 1D-only model.
```

## 8. Open items

- Anisotropic dispersion tensor on TIN (v1 isotropic; revisit with field
  cases).
- Sediment-bed heat layer per cell (couples to GW heat when both exist) —
  **the decision now has a shape**: with H6b landed, `BedZoneState` gains a
  `cell2d_temp` array and `bedCouplingFromContact` takes the cell's wetted
  area. The overland plan §3 argues for deferring it until AFTER S5 measures
  whether it matters on a thin transient sheet.
- Two-zone plan refresh (explicit marcher, not CVODE) is a prerequisite
  for S7 and is tracked as an amendment task on that document.
