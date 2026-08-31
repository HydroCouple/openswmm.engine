# OpenSWMM Engine Roadmap

This document describes the planned development direction for the OpenSWMM Engine (formerly OpenSWMMCore). It is maintained by the Technical Manager, [Caleb Buahin](https://github.com/cbuahin), and updated at each release and after significant community discussions.

Items are organized by theme rather than strict release targeting, as scientific software timelines depend heavily on validation rigor and community bandwidth. Release assignments will be updated as work matures through experimental branches.

Community members wishing to influence priorities should participate in the [GitHub Discussions — Ideas](../../discussions) section. Threads that shape roadmap decisions are linked directly from the relevant entries below.

---

## Status Key

| Symbol | Meaning                                                  |
|--------|----------------------------------------------------------|
| 🔬     | Under exploration — prototype or recorded design study    |
| 🔧     | Actively in development                                  |
| 📋     | Planned — accepted for future development                |
| ⏸      | Deferred — not currently scheduled                       |
| ✅     | Completed — implemented and shipping (see §8 for version) |

The 6.0.0 line is in alpha. Items marked ✅ are implemented and exercised by the
test suites, but ship in pre-release builds until 6.0.0 is final.

---

## 1. Flow Routing

### 1.1 Explicit Finite Volume 1D Flow Routing — Full Saint-Venant Equations 🔧

**Motivation:** The node-link dynamic wave formulation writes momentum in non-conservative form, carries one discharge per conduit, and handles transcritical flow by suppressing it. An explicit finite volume formulation in conservation form conserves volume identically, resolves the flow field *inside* a reach, and reproduces the propagation speed of hydraulic jumps and pressurization fronts from the Rankine-Hugoniot conditions rather than tracking them heuristically.

**Status:** Implemented and selectable with `FLOW_ROUTING FV` in the 6.0.0 development line — an addition alongside dynamic wave analysis, which remains the default. Documented as Chapter 8 of the Hydraulics Reference Manual.

**Delivered:**
- Godunov-type explicit finite volume discretization of the conservation-form continuity and momentum equations on a cell mesh cut from the conduits, with hydrostatic (Audusse) reconstruction, HLL interface flux and semi-implicit Manning friction. Mesh resolution is set by `FV_CELL_LENGTH`; the default is one cell per conduit, matching the dynamic wave element count.
- Mixed free-surface/pressurized flow with no regime-switching logic: the Preissmann slot is folded into the cross-section closure with a tapered mouth, so a filling bore is captured and its speed is an output of the scheme.
- Second-order MUSCL reconstruction (`FV_ORDER 2`) and SSP-RK2 time integration (`FV_TIME_INTEGRATION RK2`), both preserving the still-water property to machine precision.
- Local time stepping (`FV_LTS`, on by default): each control volume takes a power-of-two tier from its own Courant limit, tiers are graded so no face spans more than one level, and conservation across a tier interface is exact by construction. Where tiering finds nothing to separate, the solver falls through to global stepping bit-for-bit.
- Semi-implicit node coupling (`FV_NODE_COUPLING`, default `SEMI_IMPLICIT`), which linearizes each coupling face's flux in the node head and so removes the junction storage floor — not the pipe — from the explicit stability limit.
- Backward compatible: seventeen `FV_*` `[OPTIONS]` keys, readable and writable through `swmm_options_get`/`set`, the Python bindings (`RouteModel.FV`) and the MCP server, and inert rather than rejected under the other routing models, so switching `FLOW_ROUTING` never invalidates a file. Virtual junctions become ordinary interior faces and reproduce the unsplit conduit cell for cell.

**Validation to date:** 29 analytic gates (closure, scheme, network) plus 7 local-time-stepping gates and 5 engine-level gates. Ritter and Stoker match the closed-form solutions including shock speed; lake at rest holds to 1e-9 ft across a slope break and while pressurized; mass is conserved to 1e-12 over 10⁵ substeps. On the EPA reference drainage model the routing continuity error is **0.000 % at every mesh resolution**, against 0.026 % for dynamic wave routing on the same file. Finite-volume routing is deliberately outside the legacy bit-parity contract — it is a different discretization, gated on analytic and engineering tolerances instead.

**Remaining scope:**
- **Performance.** The solver runs ~7× dynamic wave wall-clock at one cell per conduit and ~34× at Δx = 20 ft on the reference model. The original expectation of parity at equal element counts is recorded as refuted; the two largest costs found so far (the depth inversion, 3.2×, and the node stability constraint, 2.9×) are already addressed. Remaining levers are kernel fusion and CFL-census interval tuning.
- **Parallel and GPU backends.** `FV_BACKEND` and the plugin loader are in place, but no plugin yet exports the 1D network-solver entry point, so execution is CPU-serial today.
- **Peak attenuation at the default mesh.** One cell per conduit attenuates the reference model's peaks by 37 % on average (7 % at Δx = 20 ft). The cause is geometric rather than diffusive — a cell-centred scheme places a single cell's bed at the conduit mid-point, presenting an artificial bed step at every manhole — so higher-order reconstruction does not rescue it. Setting `FV_CELL_LENGTH` is the present workaround; a bed-step treatment at junction faces is the fix.
- **Broader regression** against published benchmark cases beyond the analytic suite, and quantification of the short-steep-pipe cell-length floor.
- **Sub-atmospheric transients.** The two-component pressure approach (TPA) is IMPLEMENTED in both solvers (#156): `FV_PRESSURE_CLOSURE TPA` in the finite-volume solver (Hydraulics Reference Manual §8.4.5) and `SURCHARGE_METHOD TPA` in the dynamic wave solver (§3.3.11), both default-off and bit-inert when off, validated on the Vasconcelos et al. (2006) laboratory cases with sub-atmospheric heads observable via `REPORT_SIGNED_HEADS`. Remaining work: the pinned high-celerity filling divergence (explicit FV TPA at a = 150 m/s diverges at the reflected surge — the paper's own post-shock frontier; a local conservative filter was measured ineffective and the Vasconcelos & Wright (2009) hybrid flux is the designated fix path; gate `FvTpa.KnownIssueHighCelerityFillingDiverges`).

(`FLOW_ROUTING FV` and the FV keys, including the #156 additions, are exposed in the OpenSWMM GUI's Routing & Hydraulics options as of openswmm.gui #10; the former desktop-exposure gap is closed.)

### 1.2 2D Overland Flow — Local Inertial Finite Volume Model 🔧

**Status:** A dynamically coupled 1D/2D overland flow solver using an explicit finite volume, local inertial formulation is available in the 6.0.0 alpha releases, with mass-conservative 1D–2D exchange, mesh generation from digital terrain data, and GUI support for mesh visualization and 1D↔2D coupling.

**Since the last roadmap update:**
- The implicit CVODE/ARKODE integrators carried through development have been **retired**. The explicit local-inertial marcher is the only 2D integrator; the retired `[2D_OPTIONS]` keys warn and are ignored on file load, and the SUNDIALS and hypre dependencies are gone.
- Kokkos-based plugin backends (OpenMP, CUDA, HIP, SYCL) execute the 2D kernels, selected at runtime above a cell-count gate.
- Per-cell parameter surfaces (roughness, initial depth, initial velocity), the full set of 2D boundary condition types, and 1D↔2D exchange booked per routing step by default.
- Standing analytic validation against the SWASHES benchmark set — lake at rest (emerged and immersed), Ritter and Stoker dam breaks, Thacker planar and radial oscillations, MacDonald gradually-varied profiles, and subcritical, transcritical and shocked bump flows.
- Mesh generation reworked to stream arbitrarily large DEMs, with a mesh cache sidecar and tiled level-of-detail rendering in the GUI.

**Remaining scope:**
- Continued validation against published inundation benchmarks (e.g. the UK Environment Agency test cases) and field-validated case studies; the analytic suite above is green but is not a substitute for these.
- Performance and robustness hardening on large regional meshes based on beta testing feedback.
- 2D water quality transport (Section 2.2) on the same mesh.

### 1.3 Spatially Explicit Inlets — Mode-Switching Junction Nodes 🔬

**Motivation:** Since SWMM 5.2, inlets are attributes of street and channel conduits (`[INLET_USAGE]`): capture flow is computed from approach hydraulics and applied as a flow modification inside the link, with ponding tracked at a separate reference node. The inlet has no independent hydraulic presence, so inlet-controlled surface/sewer exchange cannot respond to the hydraulic state of the node it drains to.

**Status:** Design recorded. No implementation yet.

**Planned scope:**
- Promote inlets to first-class junction nodes that switch mode based on approach hydraulics: capturing street flow when gutter spread exceeds a threshold, and reverting to passive junctions otherwise.
- HEC-22 grate and curb-opening capture retained as the capture closure, evaluated at the node rather than as a link post-processing step.
- Ponding, bypass, and re-entry resolved at the inlet node itself, enabling two-way street ↔ sewer exchange under surcharge.

---

## 2. Water Quality Transport

### 2.1 Advection-Dispersion Model — Pipe Flow 🔧

**Motivation:** The OpenSWMM Engine currently supports simplified first-order water quality routing in pipes. A full advection-dispersion equation (ADE) solver will enable physically accurate simulation of constituent mixing and longitudinal dispersion in pressurized and open-channel conduits.

Two routes are being developed, and they are complementary rather than competing — one rides the finite-volume hydraulic mesh, the other is a stand-alone quality engine usable under any flow routing model.

**Route A — Eulerian transport on the finite-volume mesh (implemented at scheme level).** The cell mesh of Section 1.1 already carries advected species. The species flux is the same mass flux the water used, upwinded on the contact speed, which makes solute mass conservation exact and keeps a uniform concentration field uniform under any flow, including reversal and drying. First-order upwind, MUSCL and QUICKEST-ULTIMATE reconstructions are available, limited by flux-corrected transport so the discrete maximum principle holds without sacrificing conservation, and longitudinal dispersion is treated implicitly (`FV_DISPERSION`) so the Δx²/2D_L step restriction never binds. Verified against analytic transport gates.

*Remaining for Route A:* the transport layer is not yet connected to the project's `[POLLUTANTS]`, land-use buildup/washoff, inflows or treatment — it is exercised through the solver's own gates. Wiring it up, deciding how `QUALITY_SOLVER` selects between the legacy Eulerian solver, Route B and finite-volume transport, and reporting cell-resolved concentrations are the open items. Note that local time stepping is disabled while species are transported, because the flux-corrected transport limiter needs one synchronous sweep.

**Route B — Lagrangian parcel tracking (design study recorded, not implemented).** Parcels of water and their constituent loads are advected along the flow field, with dispersion applied as a superimposed Fickian random-walk step. The approach is free of numerical diffusion, carries no Courant constraint on advection, conserves mass at the parcel level, and — unlike Route A — is independent of the hydraulic discretization, so it works under dynamic wave and kinematic wave routing as well. It is the natural host for the multispecies reaction system of Section 2.4, since reactions are evaluated along parcel trajectories.

*Planned scope for Route B:*
- Lagrangian parcel-tracking advection for 1D pipe and conduit flow across all SWMM link and node types, driven by velocity fields from the flow routing solver.
- Random-walk dispersion on parcel trajectories using user-specified or empirically estimated longitudinal dispersion coefficients.
- Parcel injection, merging, and splitting logic to maintain solution resolution while controlling computational cost.
- Mass-conservative interpolation of parcel concentrations onto the fixed computational grid for output and coupling.
- Water age as a built-in reserved species.
- Numerical alignment with the legacy Eulerian solver in the degenerate case (single bulk species, first-order decay, no dispersion, complete-mixing storage) as a parity gate, plus comparison to analytical solutions for simple pipe transport problems.

### 2.2 Advection-Dispersion Model — Overland Flow 📋

**Motivation:** Surface runoff carries dissolved and particulate constituents across the land surface. A 2D or quasi-2D ADE formulation for overland flow will extend water quality modeling to the catchment scale.

**Status:** No implementation yet. The 2D marcher solves water only; this work is sequenced behind Route A of Section 2.1, whose contact-upwinded, flux-corrected species update is the same construction applied to an unstructured 2D mesh.

**Planned scope:**
- 2D depth-averaged ADE for overland flow domains.
- Coupling to the surface runoff and infiltration modules for flow field and source term input.
- Boundary conditions for rainfall-driven constituent loading and inlet/outlet fluxes.

### 2.3 Advection-Dispersion Model — Groundwater Flow 📋

**Motivation:** Subsurface transport of dissolved constituents is relevant to infiltration-based stormwater controls, groundwater recharge, and contaminant fate. An ADE formulation for the groundwater module will complete the full subsurface-surface-pipe transport chain.

**Planned scope:**
- 1D or 2D ADE for the saturated zone, coupled to the existing groundwater module.
- Dispersion tensor formulation accounting for mechanical dispersion and molecular diffusion.
- Coupling to the surface and pipe water quality modules at shared boundaries.

### 2.4 Multispecies Reaction Support — All Flow Domains 🔬

**Motivation:** Real-world water quality problems involve interacting chemical and biological species (e.g., nitrogen cycling, dissolved oxygen–BOD interactions, pathogen decay). A general multispecies reaction framework will allow users to define arbitrary reaction networks without modifying source code.

**Status:** Design recorded — an EPANET-MSX-equivalent reaction system (user-defined rate ODEs and equilibrium DAEs, bulk and wall species, selectable integrators with adaptive error control) specified as part of the Route B strategy in Section 2.1. No implementation yet. Whichever transport route ships first, the reaction system is intended to be written once and shared, not duplicated per transport scheme.

**Planned scope:**
- A general reaction network specification (user-defined stoichiometry, rate laws, and kinetic parameters) applicable to pipe, overland, and groundwater transport.
- Built-in implementations of common reaction sets: nitrification/denitrification, DO-BOD, first-order decay.
- Operator-splitting approach separating transport and reaction steps for numerical stability.
- Validation against published multispecies benchmark problems and analytical solutions for simple reaction networks.
- Applicable across all ADE-enabled flow domains (Sections 2.1–2.3) with a unified species definition interface.

---

## 3. Sediment Transport 📋

**Motivation:** Sediment is a primary pollutant and transport vehicle for nutrients, metals, and pathogens in urban stormwater systems. A sediment transport module will enable simulation of erosion, deposition, and sediment routing through catchments, channels, and pipe networks.

**Planned scope:**
- Overland erosion: USLE/RUSLE-based or physically based detachment models driven by rainfall and surface runoff shear stress.
- Sediment routing in channels and pipes: bedload and suspended load transport using empirical (e.g., Engelund-Hansen, Yang) or process-based formulations.
- Particle size fractionation: multi-fraction sediment transport supporting cohesive and non-cohesive particles.
- Deposition modeling in detention basins, retention ponds, and low-velocity pipe reaches.
- Coupling to the water quality transport module for sediment-associated constituent transport (e.g., particle-bound phosphorus).
- Regression testing against published flume data and field-validated benchmark cases.

---

## 4. Heat Transport 📋

**Motivation:** Thermal loading from urban surfaces, impervious cover, and stormwater infrastructure is a significant stressor on receiving water bodies. A heat transport module will enable simulation of stormwater temperature dynamics from catchment to receiving water.

**Planned scope:**
- Surface energy balance model for catchment-scale water temperature estimation, accounting for solar radiation, long-wave exchange, evaporation, and conduction.
- 1D longitudinal heat transport in pipes and channels, coupled to the ADE solver (Section 2.1).
- Coupling to the groundwater module for subsurface heat exchange and baseflow temperature.
- Thermal stratification in detention basins (simplified layer model).
- Validation against field data from monitored urban catchments and published benchmark cases for stream temperature modeling.

---

## 5. Groundwater

### 5.1 Two-Zone Model ✅

**Motivation:** Groundwater interaction is central to infiltration-based stormwater controls, baseflow generation, and subsurface drainage. A two-zone (unsaturated/saturated) groundwater model provides subsurface flow representation in the new engine and serves as the flow foundation for the groundwater advection-dispersion transport module (Section 2.3).

**Status:** Implemented in the 6.0.0 line. Each subcatchment carries an independent aquifer with an upper unsaturated and a lower saturated zone, integrated as a batch of independent ODE systems (moisture accounting, percolation, deep percolation and evapotranspiration from both zones), with the user-definable lateral flow equation to drainage nodes and channels. `[AQUIFERS]`, `[GROUNDWATER]` and `[GWF]` parse with SWMM 5.x semantics, and aquifer parameters are readable and writable at runtime through the C API and the Python bindings.

**Remaining scope:**
- The coupling interface for the groundwater ADE transport module (Section 2.3).
- Continued legacy-parity verification alongside the rest of the hydrology suite.

### 5.2 Spatially Explicit Groundwater — FV Mesh Integration 🔬

**Motivation:** The two-zone aquifer of Section 5.1 is integrated per subcatchment: lateral exchange is the empirical SWMM cubic polynomial, there is no inter-subcatchment communication, and the aquifer's only feedback to the rest of the engine is an end-of-step infiltration cap. Four physically real processes are therefore not representable: saturation-excess (Dunne) overland flow when the water table reaches the surface, return flow re-emerging downslope after lateral subsurface transport, head-driven pipe and node ↔ aquifer exchange (the physical mechanism behind RDII, groundwater inflow, and sewer exfiltration), and true capillary rise feeding evaporation.

**Status:** Design recorded, following the semidiscrete finite volume multiprocess watershed formulation of Qu & Duffy (2007). No implementation yet.

**Planned scope:**
- A vertically integrated, complementary two-layer subsurface kernel on the 2D FV mesh — unsaturated moisture depth and saturated thickness per cell, joined at a moving water table.
- Closed-form interfacial recharge flux with a runtime-selectable soil characteristic (Gardner, Russo, Brooks–Corey, van Genuchten), smooth in both states so no piecewise switching is needed.
- Lateral saturated Darcy exchange on the same FV stencil the 2D surface router already assembles, giving the four missing processes above without new closures.
- An enslaved shallow-water-table reduction collapsing each cell to a single ODE in saturated thickness, and a benchmarked comparison against a kinematic-wave approximation of unsaturated-zone dynamics across behavioural regimes from pipe-only urban projects to fully meshed catchments.
- Legacy alignment: all existing SWMM aquifer behaviour continues to route through the unchanged per-subcatchment path (Section 5.1); the mesh-coupled kernel is opt-in.

---

## 6. LID Hydraulic Integration — LID as Storage Nodes 🔬

**Motivation:** LID units are currently subcatchment attributes connected through the runon mechanism — a one-way hydrological cascade that passes volume fluxes with no hydraulic mediation. Real green-infrastructure trains are hydraulically connected devices: flow between units is head-dependent, a saturated downstream unit exerts backpressure on the upstream one, and control structures between units regulate flow based on hydraulic state.

**Status:** Design recorded. No implementation yet.

**Planned scope:**
- Map LID layers (surface, media, gravel) onto extended storage nodes using a reduced-physics kinematic Richards ODE for the media layer.
- Head-dependent flow between LID units and to/from the conveyance network, with two-way feedback so network surcharge propagates upstream through the LID train.
- Orifice, weir, and underdrain control structures at LID connections, participating in the standard link hydraulics and control-rule machinery.
- Backward compatibility with the existing subcatchment-attribute LID representation, which remains the default.

---

## 7. Deferred Items

The following items have been raised in community discussions, or explored and set aside, and are not currently scheduled. They may be reconsidered in future release cycles as resources permit or as community interest grows.

| Item                                           | Reason for Deferral                                                        |
|------------------------------------------------|----------------------------------------------------------------------------|
| 2D overland flow (full shallow water equations) | Local inertial finite volume model implemented (Section 1.2); full dynamic-wave SWE not currently scheduled |
| Implicit 2D time integration (CVODE/ARKODE)     | Explored and retired — the explicit marcher outperformed it on the benchmark models and removed two heavy dependencies |
| Real-time data assimilation & sensor fusion     | Requires external telemetry infrastructure not yet in scope                |
| Machine learning surrogate models               | Research area; may be introduced as an optional experimental module        |

**No longer deferred.** *GPU-accelerated solvers* have moved into development: the
2D marcher's kernels run through Kokkos-based plugin backends (OpenMP, CUDA, HIP,
SYCL) selected at runtime above a cell-count gate, shipped as optional plugin
binaries so the base build carries no GPU dependency. The 1D finite volume solver
(Section 1.1) has the same loader hooks and option surface but no plugin
implementation yet.

---

## 8. Completed Items

The 6.0.0 line is still in pre-release, so the version column records the
pre-release the work first shipped in rather than a stable release. "Unreleased"
means merged and gated but not yet carried by a tag.

| Item                                                                                  | Version         |
|---------------------------------------------------------------------------------------|-----------------|
| Data-oriented, reentrant engine core (SoA state, opaque handle, plugin I/O, lifecycle state machine) | 6.0.0-alpha.1   |
| Public C API, Python bindings and MCP server over the model, options, results and runtime state | 6.0.0-alpha.1 → ongoing |
| Two-zone groundwater model (Section 5.1)                                              | 6.0.0-alpha.1   |
| 1D/2D coupled overland flow — explicit local inertial finite volume marcher (Section 1.2) | 6.0.0-alpha.1 (hardened through alpha.3) |
| Kokkos GPU/threaded plugin backends for the 2D solver (OpenMP, CUDA, HIP, SYCL)        | 6.0.0-alpha.2   |
| Decoupled 1D/2D timesteps with conservative per-window exchange booking                | 6.0.0-alpha.3   |
| Retirement of the implicit 2D integrators and their SUNDIALS/hypre dependencies        | unreleased      |
| Per-cell 2D parameter surfaces and 2D initial conditions                               | unreleased      |
| Explicit finite volume 1D routing, `FLOW_ROUTING FV` (Section 1.1)                     | unreleased      |
| Cell-resolved Eulerian scalar transport on the finite volume mesh (Section 2.1, Route A) | unreleased      |

---

*Last updated: August 2026 — Caleb Buahin, Technical Manager*
