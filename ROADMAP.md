# OpenSWMMCore Roadmap

This document describes the planned development direction for OpenSWMMCore. It is maintained by the Technical Manager, [Caleb Buahin](https://github.com/cbuahin), and updated at each release and after significant community discussions.

Items are organized by theme rather than strict release targeting, as scientific software timelines depend heavily on validation rigor and community bandwidth. Release assignments will be updated as work matures through experimental branches.

Community members wishing to influence priorities should participate in the [GitHub Discussions — Ideas](../../discussions) section. Threads that shape roadmap decisions are linked directly from the relevant entries below.

---

## Status Key

| Symbol | Meaning                                                  |
|--------|----------------------------------------------------------|
| 🔬     | Under exploration in an experimental branch              |
| 🔧     | Actively in development                                  |
| 📋     | Planned — accepted for future development                |
| ⏸      | Deferred — not currently scheduled                       |
| ✅     | Completed — available in a stable release                |

---

## 1. Flow Routing

### 1.1 Implicit 1D Flow Routing — Full Saint-Venant Equations 📋

**Motivation:** The current explicit solver for 1D flow routing imposes Courant-number-dependent time step constraints that can be prohibitively small for steep or rapidly varying flows. An implicit (or semi-implicit) formulation of the full Saint-Venant equations will remove this restriction, enabling stable simulation of subcritical, supercritical, and mixed-regime flows with significantly larger time steps.

**Planned scope:**
- Implicit or Crank-Nicolson discretization of the continuity and momentum equations for pipe and channel flow.
- Handling of hydraulic jumps and transitions between flow regimes.
- Newton-Raphson or Picard iteration with convergence diagnostics.
- Backward compatibility with existing input formats; solver selection via a configuration flag.
- Regression testing against the explicit solver on standard benchmark cases and against analytical solutions for steady gradually-varied flow profiles.

**Validation approach:** Comparison to known analytical solutions (e.g., steady uniform flow, backwater curves) and to published benchmark results from the hydraulic literature.

---

## 2. Water Quality Transport

### 2.1 Advection-Dispersion Model — Pipe Flow 🔬

**Motivation:** OpenSWMMCore currently supports simplified first-order water quality routing in pipes. A full advection-dispersion equation (ADE) solver will enable physically accurate simulation of constituent mixing and longitudinal dispersion in pressurized and open-channel conduits.

**Numerical approach — Lagrangian formulation:** The ADE will be solved using a **Lagrangian (particle-tracking) method**. Parcels of water (and their constituent loads) are advected along the flow field using the velocity provided by the flow routing solver. Dispersion is applied as a superimposed Fickian random-walk step on the parcel positions at each time step. This approach is inherently free of numerical diffusion, eliminates the Courant-number stability constraint on the advection step, and conserves mass exactly at the parcel level — all of which are significant advantages over fixed-grid Eulerian schemes for transport in pipe networks with highly variable velocities and geometries.

**Planned scope:**
- Lagrangian parcel-tracking advection for 1D pipe and conduit flow, driven by velocity fields from the flow routing solver.
- Fickian random-walk dispersion superimposed on parcel trajectories using user-specified or empirically estimated longitudinal dispersion coefficients.
- Parcel injection, merging, and splitting logic to maintain solution resolution while controlling computational cost.
- Mass-conservative interpolation of parcel concentrations onto the fixed computational grid for output and coupling.
- Coupling to the multispecies reaction module (Section 2.4) for reaction evaluation carried along parcel trajectories.
- Verification of mass conservation and comparison to analytical solutions for simple pipe transport problems.

### 2.2 Advection-Dispersion Model — Overland Flow 🔬

**Motivation:** Surface runoff carries dissolved and particulate constituents across the land surface. A 2D or quasi-2D ADE formulation for overland flow will extend water quality modeling to the catchment scale.

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

## 5. Deferred Items

The following items have been raised in community discussions but are not currently scheduled. They may be reconsidered in future release cycles as resources permit or as community interest grows.

| Item                                           | Reason for Deferral                                                        |
|------------------------------------------------|----------------------------------------------------------------------------|
| 2D overland flow (full shallow water equations) | High implementation complexity; currently scoped to 1D/quasi-2D approaches |
| Real-time data assimilation & sensor fusion     | Requires external telemetry infrastructure not yet in scope                |
| GPU-accelerated solvers                         | Dependency and portability considerations; deferred pending solver maturity |
| Machine learning surrogate models               | Research area; may be introduced as an optional experimental module        |

---

## 6. Completed Items

| Item                                          | Version |
|-----------------------------------------------|---------|
| *(To be populated as stable releases ship)*   |         |

---

*Last updated: April 2026 — Caleb Buahin, Technical Manager*
