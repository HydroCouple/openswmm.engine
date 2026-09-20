@page quality_ref_ch11_planned Chapter 11: Planned Formulations

@tableofcontents

**Nothing in this chapter is complete in this release.** \status{Planned}
The formulations below are recorded because a modeller will meet their
traces — a section the parser accepts and warns about, an option key that is
stored but reaches nothing, a component id the registry knows by name — and
the manual should say plainly what each trace means. Where a design record
exists it is cited; where only a roadmap entry exists, that is said too.
Every statement of "parsed but inert" is made against the parser and the
consumer that does not exist.

| Formulation | Status | What exists today | What is missing |
|---|---|---|---|
| Sediment transport (§11.1) | \status{Planned} | A roadmap scope statement | Design record, sections, kernel |
| Groundwater transport (§11.2) | \status{Planned} | Six `[GW_*]` sections parsed, validated and written back; a design plan | The transport kernel and the component that hosts it |
| Finite-volume species transport on `[POLLUTANTS]` (§11.3) | \status{Planned} | Contact-upwinded, flux-corrected species kernels verified on their own gates | The connection to the project's species; `FV_DISPERSION` is inert |
| Per-element heat attributes — constants per tag, link or node, and climate per element through the API (§11.4) | \status{Implemented} | Parsed, resolved, honoured by every flux evaluator, written back | — |
| Per-element heat attributes — time series, computed shading, scoped API and GUI (§11.4) | \status{Planned} | The identity plumbing the constants ride on | Series resolution, a shade model, the editor table |
| The five later-phase `[REACTION_*]` sections (§11.5) | \status{Planned} | Recognised by name and refused with their phase | Sources, per-element parameters, patterns, report selection, subcatchment scope |

<!-- source: ROADMAP.md:139-149; src/engine/2d/gw/GwTransportSections.cpp:452-476, 632-639; src/engine/hydraulics/Routing.cpp:1042-1056, 1080; plans/transport/PER_ELEMENT_HEAT_ATTRIBUTES_PLAN_2026-09-01.md:239-246; src/engine/transport/components/ReactionModule/ReactionsComponent.cpp:455-468 -->

## 11.1 Sediment Transport

Sediment is a planned theme of the engine roadmap and nothing more yet: there
is **no design record** in the transport plans and no code. The roadmap's
scope, quoted so that the reader knows what "planned" covers, is overland
erosion (USLE/RUSLE or physically based detachment driven by rainfall and
runoff shear), bedload and suspended-load routing in channels and pipes by
empirical or process-based closures, multi-fraction particle sizes including
cohesive material, deposition in basins, ponds and low-velocity reaches, and
coupling to the water-quality transport so that particle-bound constituents
ride the sediment. None of those items has been scoped against the engine's
transport representations — the tanks-in-series, Eulerian and Lagrangian
engines of Chapters 5 and 7 or the mesh of Chapter 10 — and no section, key
or warning refers to sediment in this release. The bed *zone* of §9.3.11 is
a heat and solute exchange layer, not a sediment store, and should not be
read as a first step toward this item.

<!-- source: ROADMAP.md:139-149; plans/transport/ (no sediment plan present, verified by listing 2026-09-19) -->

## 11.2 Groundwater Transport

The engine accepts six `[GW_*]` sections that configure solute, reaction
species, age and temperature transport in a spatially explicit two-zone
aquifer beneath the 2D mesh: @ref engine_manual_sect_GW_TRANSPORT_OPTIONS,
@ref engine_manual_sect_GW_TRANSPORT_PARAMS, @ref engine_manual_sect_GW_SORPTION,
@ref engine_manual_sect_GW_INITIAL_QUALITY,
@ref engine_manual_sect_GW_BOUNDARY_QUALITY and
@ref engine_manual_sect_GW_SOURCES. The rows are parsed, validated at open —
species and time-series names, cell and tag references, an `XY` source
snapped to the nearest cell centroid, duplicate source names refused rather
than last-wins — kept, and written back unchanged on save. **No kernel
consumes them.** A model that carries any of them receives this warning once:

> [GW_*] subsurface transport is AUTHORED but INERT this run: the integrated
> 2D groundwater component (org.hydrocouple.openswmm.integrated2d) provides
> the two-zone kernel these sections configure and is not available in this
> release. The rows are validated, kept and written back unchanged.

The component id named in that warning, `org.hydrocouple.openswmm.integrated2d`,
is a *planned* entry in the process-component registry: a
`[PROCESS_COMPONENTS]` row naming it does not fail as an unknown id but is
refused with a diagnostic naming the phase it arrives with. The same registry
convention gives the reactions, ARD, LARD, heat and water-age components
their ids, and it is how a deck written against the design can be recognised
before the implementation lands.

What the groundwater domain does today is recorded by the transport policy of
§10.1: every species class reads *unavailable* there, with the reason. For
pollutants the legacy aquifer supplies a constant groundwater concentration at
the node seam on positive groundwater flow only, and exfiltration carries
nothing; for age and temperature the aquifer is a source volume — new water
enters at age zero and at the `GW` source temperature of `[HEAT_SOURCES]`.

The design is `plans/transport/GW_TRANSPORT_HEAT_MSX_PLAN_2026-09-04.md`,
amended 2026-09-07 for the mixed triangle/quadrilateral mesh. Its saturated
zone carries mass \f$M_s = n\,h_g\,A\,C\f$ and solves the retarded
advection–dispersion equation on the kernel's own Darcy face fluxes,

| | | | |
|---|---|---|---|
| \f[R_f\,\frac{\partial (n h_g C)}{\partial t} = -\nabla\cdot(\mathbf{q}\,C) + \nabla\cdot(n h_g D\,\nabla C) + S_C - \lambda\, n h_g C, \qquad R_f = 1 + \frac{1-n}{n}\,\rho_s K_d\f] | | (11-1) | |

with a hydrodynamic dispersion \f$D = D_m + \alpha_L |v|\f$ and the sorbed
phase folded into the retardation rather than carried as a second species.
The unsaturated zone is a well-mixed store under the kernel's closure A and
a column of \f$\sigma\f$-layers advected by the kernel's moving-grid fluxes
under closure B. Heat uses effective properties from the actual moisture
state, \f$(\rho c)_{\mathrm{eff}} = \theta \rho_w c_w + (1-n)\rho_s c_s\f$,
with conduction, a geothermal boundary and latent removal at
evapotranspiration; age is an ordinary species slot; reaction species run
the shared integrator per aquifer cell under a new `SUBSURFACE` scope. Every
water flux the kernel moves — infiltration, recharge, capillary rise, return
flow, node and conduit exchange — carries the donor's tuple, which is the
rule the surface coupling of §10.4 already follows. The degenerate
`PER_SUBCATCH` mode, one column per subcatchment, is what would replace the
constant groundwater columns above on a network-only model.

<!-- source: src/engine/2d/gw/GwTransportSections.cpp:452-476, 590-639; src/engine/plugins/ProcessComponentRegistry.cpp:146-168; src/engine/transport/TransportPolicy.cpp:164-178; plans/transport/GW_TRANSPORT_HEAT_MSX_PLAN_2026-09-04.md:1-33, 35-58, 61-95; plans/transport/TWOD_TRANSPORT_PLAN.md:122-195 -->

## 11.3 Finite-Volume Species Transport Wired to `[POLLUTANTS]`

The finite-volume routing solver of the Hydraulics Reference Manual (Chapter
8, §8.8) carries a species transport layer at *scheme level*: the species
flux is the same mass flux the water used, upwinded on the HLLC contact
speed so that solute mass is conserved exactly and a uniform field stays
uniform through reversal, wetting and drying; `UPWIND`, `MUSCL` and
`QUICKEST_ULTIMATE` reconstructions are limited by Zalesak's flux-corrected
transport so the discrete maximum principle holds; and longitudinal
dispersion is treated implicitly, one tridiagonal solve per cell chain, so
the parabolic step limit never binds. When species are carried the solver
stands local time stepping down, because the limiter bounds a cell against
the extrema of its whole neighbourhood in one synchronous sweep and a tiered
correction would be a new scheme. The kernels live in
`src/engine/transport/fvkernels/` and are verified on the solver's own
analytic gates.

What is missing is the wiring. The router sizes the finite-volume state with
**zero species rows** — `fv_state_.resize(nc, nn, 0)` — so no `[POLLUTANTS]`
row, land-use load, inflow or treatment ever reaches the mesh, and under
`FLOW_ROUTING FV` water quality is routed by the `QUALITY_SOLVER` engines of
Chapters 5 and 7 as it is under dynamic wave. Three consequences follow, and
the engine states each of them at open rather than leaving a calibrated deck
to discover them:

- `[OPTIONS] FV_DISPERSION` is parsed and stored, converted to internal units
  exactly as the honoured `FV_*` lengths are, and then reaches nothing; a
  non-zero value raises the *FV option inert* warning naming the key.
- `FV_SCALAR_SCHEME` set to anything but its default raises the same warning.
- Under `QUALITY_SOLVER EULERIAN_ARD` a non-zero `FV_DISPERSION` additionally
  warns that the Eulerian engine reads its dispersion from the transport
  component (`[TRANSPORT_OPTIONS] DISPERSION` and `[CONDUIT_DISPERSION]`,
  Chapter 7 §7.3.3) and applies nothing from this key.

The open design questions the roadmap records are how `QUALITY_SOLVER` would
select finite-volume transport beside the three engines that exist, how the
species rows would take their loads through the loader seam every other
engine shares, and how cell-resolved concentrations would be reported.
Unifying the `FV_*` and transport-component option surfaces is the phase the
warning above names.

<!-- source: docs/manuals/reference/hydraulics/sections/Chapter8-FiniteVolume.md:1695-1727, 1739-1747; src/engine/input/handlers/OptionsHandler.cpp:512-513; src/engine/hydraulics/Routing.cpp:1042-1056, 1078-1080; src/engine/hydraulics/fv/ExplicitFvSolver.cpp:1495-1505, 3430-3437; src/engine/hydraulics/fv/FvOptions.hpp:175-178, 298-301; src/engine/transport/components/EulerianArdComponent/ArdConfig.cpp:357-366; ROADMAP.md:84-92 -->

## 11.4 Per-Element Heat Attributes

Chapter 9 documents the radiative and bed-zone parameters as one number per
model. Shading, sky view, land-cover emissivity and temperature, burial
depth, ground temperature and hyporheic velocity are properties of *a place*,
and collapsing them to a scalar removes the spatial signal a heat model
exists to produce. The per-element program
(`plans/transport/PER_ELEMENT_HEAT_ATTRIBUTES_PLAN_2026-09-01.md`) makes them
specifiable per element, with the per-element value overriding the global.

**Landed (PE1, PE2, PE4 — validated and committed 2026-09-01).** Every flux
evaluator takes an element identity, so the same physics can be handed a
different configuration per call. In `[RADIATIVE_FLUXES]` the rows `ALBEDO`,
`SHADE_FACTOR`, `SKY_VIEW`, `EMISS_WATER`, `EMISS_LANDCOVER` and
`LANDCOVER_TEMPERATURE` accept the scopes `GLOBAL`, `TAG name`, `LINK name`
and `NODE name`; `SHORTWAVE`, `ATM_EMISS_COEFF` and `ATM_LW_REFLECTION` stay
`GLOBAL` because they describe the incident resource and the atmosphere, not
the reach. In `[SEDIMENT_EXCHANGE]` the bed attributes accept `GLOBAL`, `TAG`
and `LINK`; `NODE` is refused because the bed zone exists beneath conduits
only, and `INITIAL_TEMPERATURE` is the bed's own seed and stays `GLOBAL`.
Precedence is `GLOBAL` under `TAG` under the named element; two rows at the
same scope for the same target are refused rather than letting one silently
win; global and per-element values pass one validator. Air temperature,
humidity, wind and incident shortwave remain global in the deck and can be
pushed per element only through the runtime forcing API, which is where a
coupled shade or atmospheric model belongs. A deck with no override rows is
byte-identical to one written before the program.

**Open (PE3, PE5, PE6).** Per-element `TIMESERIES` spellings of the same
attributes; computed shading from sun position and bank vegetation, for which
the recommendation on record is *not to build* — a prescribed `SHADE_FACTOR`
series covers the seasonal case at a fraction of the cost; the scoped C API
getters and setters and the GUI's per-element table; per-element
wind-function coefficients; and the `CELL2D` scope that would let the 2D
surface of Chapter 10 take per-cell shading — the element token exists, the
flux evaluation on cells does not (§10.11).

<!-- source: plans/transport/PER_ELEMENT_HEAT_ATTRIBUTES_PLAN_2026-09-01.md:1-28, 57-104, 237-327; plans/transport/PE_PER_ELEMENT_HANDOFF_2026-09-01.md:1-27; src/engine/transport/components/HeatModule/HeatComponent.cpp:152-171, 182-280, 313-337, 444-469, 565-647; src/engine/data/HeatOverrideData.hpp:81-103 -->

## 11.5 Later-Phase `[REACTION_*]` Sections

The reaction component recognises twelve section names (Chapter 8 §8.2).
Seven are consumed. The other five are **recognised and refused** with the
phase that delivers them — never silently accepted, because a section that
parses without effect is a modelling error that looks like a working model:

| Section | Refusal names phase | Intended meaning |
|---|---|---|
| `[REACTION_SOURCES]` | R-sources (post-R3) | Species mass or concentration sources at nodes, in the EPANET-MSX convention |
| `[REACTION_PARAMETERS]` | R-parameters (post-R3) | Per-conduit and per-node overrides of `PARAMETER` coefficients |
| `[REACTION_PATTERNS]` | R-sources (post-R3) | Time patterns that modulate sources |
| `[REACTION_REPORT]` | R5 | Selection of species and elements to report |
| `[REACTION_SUBCATCHMENTS]` | R6 | Subcatchment-scope expressions — an OpenSWMM extension with no MSX counterpart |

The refusal is an **error**, not a warning: a configuration file carrying any
of the five is rejected as a whole, so a deck cannot run with half of its
reaction system honoured. An unknown `[REACTION_*]` name is rejected on the
same terms, as a probable typo. Two of the planned sections already have
working substitutes: under the Eulerian ARD component, `[TRANSPORT_BOUNDARIES]`
and `[TRANSPORT_SOURCES]` (Chapter 7 §7.5) address reaction species at nodes
and along conduits, including negative rates for extraction, and `[INFLOWS]`
rows may name a reaction species. `[REACTION_PARAMETERS]` has none — every
`PARAMETER` coefficient is a single value for the whole model today, which is
why Chapter 8's declarations subsection treats `PARAMETER` and `CONSTANT` as
synonyms for now.

A related refusal belongs in the same list: a `RATE`, `EQUIL` or `FORMULA`
expression on a **pollutant** in `[REACTION_PIPES]` or `[REACTION_TANKS]` is
refused with the note that pollutant kinetics arrive with phase R4b.
Pollutants may be referenced read-only in reaction-species expressions, and
their first-order `Kdecay` applies under every engine.

<!-- source: src/engine/transport/components/ReactionModule/ReactionsComponent.cpp:269-284, 394-403, 455-479; docs/manuals/reference/quality/sections/Chapter7-AdvectionReactionDispersion.md:226-273; src/engine/quality/lard/LagrangianSolver.hpp:359-366 -->
