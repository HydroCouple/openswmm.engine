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

| Section | Purpose | Grammar | Status |
|---|---|---|---|
| `[REACTION_OPTIONS]` | Solver, coupling, rate and area units, timestep, tolerances, fallback temperature | @ref engine_manual_sect_REACTION_OPTIONS | \status{Implemented} |
| `[REACTION_SPECIES]` | Species declarations: `BULK` or `WALL`, units, per-species tolerances | @ref engine_manual_sect_REACTION_SPECIES | \status{Implemented} |
| `[REACTION_COEFFICIENTS]` | Named constants and parameters | @ref engine_manual_sect_REACTION_COEFFICIENTS | \status{Implemented} |
| `[REACTION_TERMS]` | Named intermediate expressions | @ref engine_manual_sect_REACTION_TERMS | \status{Implemented} |
| `[REACTION_PIPES]` | Per-species `RATE`, `EQUIL` or `FORMULA` expressions in conduit scope | @ref engine_manual_sect_REACTION_PIPES | \status{Implemented} |
| `[REACTION_TANKS]` | The same forms in node scope | @ref engine_manual_sect_REACTION_TANKS | \status{Implemented} |
| `[REACTION_QUALITY]` | Initial concentrations: `GLOBAL`, `NODE` or `LINK` | @ref engine_manual_sect_REACTION_QUALITY | \status{Implemented} |
| `[REACTION_SOURCES]` | Species sources at nodes | @ref engine_manual_sect_REACTION_SOURCES | \status{Implemented} |
| `[REACTION_PARAMETERS]` | Per-element overrides of `PARAMETER` coefficients | @ref engine_manual_sect_REACTION_PARAMETERS | \status{Implemented} |
| `[REACTION_PATTERNS]` | Time patterns for sources | @ref engine_manual_sect_REACTION_PATTERNS | \status{Implemented} |
| `[REACTION_REPORT]` | Report selection | @ref engine_manual_sect_REACTION_REPORT | \status{Implemented} |
| `[REACTION_SUBCATCHMENTS]` | Subcatchment-scope expressions | @ref engine_manual_sect_REACTION_SUBCATCHMENTS | \status{Planned} |

`[REACTION_SUBCATCHMENTS]` remains recognised and refused; it is not a
runoff reactor. The four other formerly reserved sections now have explicit
network-source, parameter and species-column selection semantics in the input
reference. Unsupported source kinds and unknown section names are errors.

<!-- source: src/engine/transport/components/ReactionModule/ReactionsComponent.cpp:394-403, 405-453, 455-479 -->

Sections may also be embedded directly in the `.inp`. This is supported as a
convenience, but the external file is the intended layout: embedded sections
are read but are not written back when a model is saved, and the engine warns
when that is about to happen.

A complete small system converts a parent species to a product. Both use
the same mass units and the yield is one, so A + B is conserved in an
isolated wet reactor with `COUPLING FULL`:

```
[REACTION_OPTIONS]
SOLVER RK5
COUPLING FULL
RATE_UNITS HR

[REACTION_SPECIES]
BULK A MG
BULK B MG

[REACTION_COEFFICIENTS]
PARAMETER k 0.30

[REACTION_TERMS]
loss k * A

[REACTION_PIPES]
RATE A -loss
RATE B loss

[REACTION_TANKS]
RATE A -loss
RATE B loss

[REACTION_QUALITY]
GLOBAL A 1
```

After one hour without transport, A = exp(-0.30), B = 1 - exp(-0.30).
The reusable decks under `examples/reactions/` include this analytic case.

### 8.2.1 Species and coefficient declarations

A species is declared once, as `BULK` or `WALL`, with a units token and
optional absolute and relative tolerances. Species names are **globally
unique**: a name that collides with a `[POLLUTANTS]` id, a reserved name or
another species is refused, because every downstream surface —
`[INITIAL_QUALITY]`, `[INFLOWS]`, the results file, the buildup and washoff
tables — addresses a species by name alone. The units token is free text in
the MSX manner (`MG`, `UG`, `MMOL`, a count), and it is load-bearing in one
place beyond labelling: the buildup and washoff machinery of Chapters 3, 4
and 10 converts a species' concentration-mass to user mass from it — `MG`
takes the project mass conversion, `UG` a thousandth of it, anything else is
carried unconverted. The per-species tolerances, when given, replace the
global `ATOL` and `RTOL` of `[REACTION_OPTIONS]` for that species alone.

`WALL` species remain attached to their own network element under LEGACY;
they are excluded from dissolved advection, evaporation concentration and
the BULK water-mass routing ledger. ARD falls back to LEGACY for systems
containing WALL species. LAGRANGIAN refuses WALL systems. Geometry-dependent
wall demand remains unqualified because bindings do not populate wall
area/volume variables, and AREA_UNITS does not supply that missing geometry.
2D surface transport excludes reaction systems containing WALL species.

A coefficient is declared as `CONSTANT` or `PARAMETER` with a value. A `PARAMETER` may be overridden on a named node or link through
`[REACTION_PARAMETERS]`; a `CONSTANT` may not. A coefficient may not share a name with a species or another
coefficient. `[REACTION_TERMS]` names an intermediate expression evaluated
before the species expressions that reference it; a term may not share a
name with a species, a coefficient or another term, and it may use species,
coefficients, other terms and the hydraulic variables of §8.3. Declaration
order in the file does not matter — species are resolved first, then
coefficients and terms, then expressions — but every reference must resolve
or the whole configuration is rejected.

<!-- source: src/engine/transport/components/ReactionModule/ReactionsComponent.cpp:137-239, 416-430; src/engine/quality/MsxSurfaceQuality.cpp:64-71; src/engine/transport/TransportPolicy.cpp:98-102, 187-193 -->

### 8.2.2 Rate and equilibrium terms

Each species may carry **one** expression per scope: `RATE`, `EQUIL` or
`FORMULA` in `[REACTION_PIPES]` for conduits and `[REACTION_TANKS]` for
nodes, with the meanings of §8.3. A species with no expression in a scope is
inert there — it is transported and mixed but not reacted — which is the
normal state of a conservative tracer or of a by-product that forms only in
pipes. A second expression for the same species in the same scope is
refused. The pipe scope is also what the Eulerian engine's cells, the
Lagrangian parcels and the cells of the 2D mesh evaluate; the tank scope is
evaluated on node stores against the node's hydraulic residence time.

An expression on a **pollutant** — a `[POLLUTANTS]` id rather than a
declared species — is refused with the note that pollutant kinetics arrive
with a later phase. Pollutants may be referenced read-only inside a species
expression, and their own first-order `Kdecay` is applied by the decay stage
under every engine, in a separate stage from the reaction integrator (§8.5).

Expressions are compiled at open, transactionally: a configuration that
leaves any expression uncompilable is rejected and rolled back, so the model
never holds a reaction system that cannot run.

<!-- source: src/engine/transport/components/ReactionModule/ReactionsComponent.cpp:241-295, 431-448; docs/manuals/reference/quality/sections/Chapter8-MultiSpeciesReactions.md:96-139, 175-200 -->

### 8.2.3 Initial quality and sources

`[REACTION_QUALITY]` seeds the species rows. A `GLOBAL` row sets a species'
initial concentration everywhere; a `NODE name` or `LINK name` row overrides
it on one element. Values are non-negative; a duplicate row for the same
species on the same element is refused rather than letting the later one
win. A species with no row starts at zero. The element names are resolved
against the model after the whole `.inp` has been read, so the component
file may be listed before or after the elements it names. Reaction-species
rows written in the `.inp`'s own `[INITIAL_QUALITY]` are mirrored into the
same store, so either spelling seeds the same state.

Mass enters through the loader seam every engine shares. `[INFLOWS]` rows
may name a reaction species, exactly as they name a pollutant, and are
delivered as external species loads; under the Eulerian ARD component,
`[TRANSPORT_BOUNDARIES]` sets the concentration carried by water entering at
a node and `[TRANSPORT_SOURCES]` a distributed mass rate along a conduit,
negative for extraction (Chapter 7 §7.5). On the land surface a reaction
species may build up and wash off through `[BUILDUP]`, `[WASHOFF]`,
`[LOADINGS]` and, on the 2D mesh, `[2D_LOADINGS]` (Chapters 3, 4 and 10).
`[REACTION_SOURCES]` also supplies additive CONC or MASS node loads, and
`[REACTION_PATTERNS]` supplies their cyclic multipliers. See the input
reference for exact units, clock origin and supported source kinds.

<!-- source: src/engine/transport/components/ReactionModule/ReactionsComponent.cpp:297-390, 449-453; src/engine/quality/lard/LagrangianSolver.hpp:359-366; src/engine/quality/MsxSurfaceQuality.hpp:17-50; docs/manuals/reference/quality/sections/Chapter7-AdvectionReactionDispersion.md:226-273 -->

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
