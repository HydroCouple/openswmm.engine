@page quality_ref_ch8_msx_reactions Chapter 8: Multi-Species Reactions

@tableofcontents

## 8.1 Introduction

Chapters 3 through 5 describe pollutants that build up, wash off, are
transported, and decay by a first-order rate. That model has one reaction per
pollutant and no interaction between them. It cannot express chlorine decaying
in proportion to the concentration of the material it oxidizes, a disinfection
by-product forming as a consequence, or an equilibrium partition between
dissolved and sorbed phases.

OpenSWMM adds a **multi-species reaction system** in which the user declares
species and writes their reaction expressions directly. The conventions follow
EPANET-MSX (Shang, Uber and Rossman, 2008), which introduced the same facility
for drinking-water distribution networks: species are declared, intermediate
terms are named, and each species is given a rate, equilibrium or formula
expression evaluated at every element.

The reaction system is optional and is inert unless configured. Models that do
not declare a reaction system are unaffected.

## 8.2 Declaring a Reaction System

A reaction system is supplied through a **process component**: a row in
`[PROCESS_COMPONENTS]` names the reactions component and points at a
configuration file, conventionally `model.rxn`.

```
[PROCESS_COMPONENTS]
org.hydrocouple.openswmm.reactions  config="model.rxn"
```

The configuration file uses the same bracketed-section dialect as the `.inp`.
Its sections are:

| Section | Purpose |
|---|---|
| `[REACTION_OPTIONS]` | Solver, coupling, tolerances, timestep, units |
| `[REACTION_SPECIES]` | Species declarations: bulk or wall, units, tolerances |
| `[REACTION_COEFFICIENTS]` | Named constants and parameters |
| `[REACTION_TERMS]` | Named intermediate expressions |
| `[REACTION_PIPES]`, `[REACTION_TANKS]` | Per-scope expressions |
| `[REACTION_INITIAL]` | Initial concentrations |

Sections may also be embedded directly in the `.inp`. This is supported as a
convenience, but the external file is the intended layout: embedded sections
are read but are not written back when a model is saved, and the engine warns
when that is about to happen.

A complete small system — chlorine decaying against a reactive wall material,
with the by-product it forms:

```
[REACTION_OPTIONS]
SOLVER            RK5
RTOL              0.001
ATOL              0.0001
TIMESTEP          300

[REACTION_SPECIES]
BULK   CL2   MG   0.01   0.0001      ; free chlorine
BULK   THM   UG   0.01   0.0001      ; trihalomethane by-product
WALL   BIO   UG   0.01   0.0001      ; attached biomass

[REACTION_COEFFICIENTS]
PARAM   Kb     0.30      ; bulk decay, 1/day
PARAM   Kw     1.00      ; wall demand
PARAM   Yield  0.20      ; THM formed per unit CL2 consumed

[REACTION_TERMS]
Kf     1.5826e-4 * RE^0.88 / D       ; mass-transfer coefficient

[REACTION_PIPES]
RATE   CL2   -Kb*CL2 - (4/D)*Kw*Kf/(Kw+Kf)*CL2
RATE   THM    Yield*Kb*CL2
FORMULA BIO   Kw*CL2/(Kw+Kf)

[REACTION_TANKS]
RATE   CL2   -Kb*CL2
RATE   THM    Yield*Kb*CL2

[REACTION_INITIAL]
GLOBAL   CL2   0.8
LINK     C1    CL2   1.2
```

Two things in that listing are worth pointing out. `[REACTION_TERMS]`
defines `Kf` once and both pipe expressions reference it, so the
mass-transfer closure has a single definition rather than two copies that can
drift. And the pipe and tank expressions for `CL2` differ deliberately: a
storage unit has no wall in the sense a pipe does, so the wall-demand term is
absent from the tank form.

## 8.3 Expression Forms

Each species is given an expression in each scope it participates in. Three
forms are available, following the MSX conventions:

- **`RATE`** — the expression gives \f$dc/dt\f$, and the species is
  integrated over the reaction step.
- **`EQUIL`** — the expression is an algebraic residual driven to zero, for
  species assumed to equilibrate faster than the transport step resolves.
- **`FORMULA`** — the expression gives the concentration directly, for a
  species that is a stated function of others rather than a state variable.

Expressions may reference declared species, coefficients, intermediate terms,
declared pollutants, and a set of hydraulic variables supplied by the engine.
The available names are enumerated by the engine rather than fixed in
documentation, so an editor or API can list exactly what the current model
admits — but the usual set is:

| Name | Meaning |
|---|---|
| `D` | Hydraulic diameter |
| `Q` | Flow rate |
| `U` | Flow velocity |
| `RE` | Reynolds number |
| `US` | Shear velocity |
| `AR` | Surface-area-to-volume ratio |
| `LEN` | Element length |
| `T` | Water temperature, when heat transport is on |

`T` is the connection between Chapters 8 and 9: with `HEAT_TRANSPORT` on,
each element's reaction expressions see **that element's own** temperature,
so an Arrhenius or \f$Q_{10}\f$ term responds to the simulated thermal
field rather than to a single constant. Without heat transport it falls back
to the `[REACTION_OPTIONS] TEMPERATURE` constant.

Species declared `WALL` are surface-attached: their concentrations are per
unit wall area rather than per unit volume, they are not advected, and they
appear in bulk expressions through the surface-area-to-volume ratio.

Expressions are **compiled at model open**, not interpreted per element per
step: they are parsed once into a compact form and evaluated by a small stack
machine over a flat pool of values. Compilation is transactional — a change
that leaves any expression uncompilable is rejected and rolled back, so a
model can never be left holding a reaction system that cannot run.

## 8.4 Integration

`[REACTION_OPTIONS] SOLVER` selects the integrator applied to the `RATE`
species:

| Solver | Method | Suited to |
|---|---|---|
| `EUL` | Explicit Euler | Non-stiff systems, small steps |
| `RK5` | Explicit Runge–Kutta with error control | Non-stiff, accuracy-controlled |
| `ROS2` | Second-order Rosenbrock | Moderately stiff systems |
| `BDF2` | Second-order backward differentiation | Stiff systems |

Stiffness arises whenever a system contains reactions on widely separated
timescales — a fast equilibrium alongside a slow decay — and an explicit
method must then take steps set by the fastest reaction rather than by the
accuracy required. The Rosenbrock and BDF families are the standard responses
(Hairer and Wanner, 1996). `EQUIL` species are solved by Newton iteration at
each step, and `FORMULA` species are evaluated directly.

**Choosing one.** Start with `RK5`: it is accuracy-controlled, needs no
Jacobian, and handles most disinfection and by-product systems. Move to
`ROS2` when `RK5` is taking many internal substeps per reaction step, which
is the observable symptom of stiffness. Reserve `BDF2` for systems that
remain expensive under `ROS2`. `EUL` exists for reference and for systems
known to be non-stiff at the chosen `TIMESTEP`; it does no error control, so
a wrong answer from it is silent.

`RTOL` and `ATOL` govern the error-controlled integrators. `ATOL` should be
set near the smallest concentration that matters for each species — set it
too large and a trace species is integrated as zero; too small and the
integrator wastes work resolving numerical noise. The per-species tolerance
columns in `[REACTION_SPECIES]` override the global values for exactly this
reason.

## 8.5 Coupling to Transport

The reaction system is bound to the transport engines rather than duplicated
inside them. Species declared in the reaction system are carried as additional
rows on the same transport state the pollutants use:

- Under **`LEGACY`**, expressions are evaluated on the node and link stores
  after mixing, and a pollutant's `Kdecay` is treated as an equivalent rate
  expression so that the two mechanisms cannot disagree.
- Under **`EULERIAN_ARD`**, species ride the cell mesh and react per cell.
- Under **`LAGRANGIAN`**, species ride the parcels: each parcel carries its
  own species column and reacts against its own pollutant concentrations and
  its own temperature (pipe scope), while node stores react on the node's
  hydraulic residence time (tank scope).

Because the reaction species share the transport representation with the
pollutants, they are advected and dispersed by the same schemes described in
Chapter 7, with the same coefficient. Where a bed zone is configured
(§9.3.11) they also exchange with it, so a reactive species can be stored and
released by the sediment as well as transported by the water.

Pollutant first-order decay and MSX kinetics are integrated in **separate
stages** under every engine. This matters because a pollutant referenced in
an MSX expression is read-only there: its decay is owned by the decay stage,
and the integrator handles only the species' own kinetics, so no pathway
applies a rate twice.

## 8.6 Implementation

The reaction module is in
`src/engine/transport/components/ReactionModule/` — the component and its
parser, the expression compiler and stack machine, the integrators, and
`ReactionsWriter.cpp`, which renders a reaction system back to its canonical
`.rxn` form. The binding to the tanks-in-series engine is in
`src/engine/quality/QualityRouting.cpp` — with `MsxLegacyTransport.cpp`
carrying the species between elements on that engine's CSTR mirror — the
binding to the Eulerian mesh is in the ARD component's
`ReactionArdBinding.hpp`, and the Lagrangian binding is stage 4b of
`LagrangianSolver.hpp`.

The C API for inspecting and editing a reaction system is declared in
@ref openswmm_reactions.h, including `swmm_reaction_validate_expression`,
which compiles an expression against the live model vocabulary without
changing state — the entry point an editor uses to validate as the user types.
