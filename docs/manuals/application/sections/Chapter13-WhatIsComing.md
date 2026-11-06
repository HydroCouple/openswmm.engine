@page application_manual_ch13_planned Chapter 13: What Is Coming

Everything in this chapter is \status{Planned}: designed, recorded, and not
in this release. No key named here does anything useful today, and where a
section is parsed but inert this chapter says so and quotes the warning the
engine prints. The reference manuals' planned-formulation chapters carry the
detail; this is the modeller's-eye summary of what would change.

| Formulation | What it would let a model do | Where the design is recorded |
|---|---|---|
| Groundwater transport | Carry species and heat in the saturated zone, and exchange them with the surface and the network | @ref hydrology_ref_ch10_planned, @ref quality_ref_ch11_planned |
| Sediment transport | Erode, carry and deposit solids rather than treating them as a dissolved pollutant | @ref quality_ref_ch11_planned |
| LID controls as storage nodes | Give an LID unit its own node, so it can be routed to, controlled, and reported like any other storage | @ref hydrology_ref_ch10_planned |
| Chebyshev cross-sections | Fit an arbitrary measured section with a spectral expansion instead of a lookup table | @ref hydraulics_ref_ch10_planned |
| Implicit finite volume | Remove the Courant bound on the 1D finite-volume step for slow, long runs | @ref hydraulics_ref_ch10_planned |
| 1D GPU backends | Run the 1D finite-volume solver on the same accelerators the 2D marcher already uses | @ref hydraulics_ref_ch10_planned |
| 2D Metal backend | Run the 2D marcher on Apple GPUs | @ref hydraulics_ref_ch10_planned |
| Per-element heat attributes | Vary the heat exchange parameters element by element rather than globally | @ref quality_ref_ch11_planned |

*Table 13-1 Planned formulations, and what each would change for a model*

## 13.1 Groundwater transport

The six `[GW_*]` sections parse, validate, round-trip through the writer and
are editable through the C API. They compute nothing. A deck that carries
them runs, and the engine says why:

> Groundwater transport sections are parsed but inert in this release.

What the design would add: advection and dispersion of species in the
saturated zone of the mesh aquifer of
@ref hydrology_ref_ch9_mesh_groundwater "Hydrology 9", with sorption and
retardation, heat carried on the same sweep, and exchange with both the
surface and the nodes standing in the cells. Until then, a model that needs
subsurface water quality has no representation of it, and a model that
carries the sections gets the warning and nothing else.

## 13.2 Sediment transport

Solids are transported today as a pollutant: a concentration that advects,
disperses and decays. Sediment would add what a concentration cannot carry —
a bed, a critical shear stress, deposition and re-entrainment, and the
feedback from a changing bed to the hydraulics. There is no design record
beyond the roadmap entry, which means this one is further out than the
others in this table.

## 13.3 LID controls as storage nodes

An LID unit is a layered column inside its subcatchment. It cannot be routed
to, controlled by a rule, or reported like a node, and two subcatchments
cannot share one. The redesign gives a unit its own node, so an underdrain
becomes a link and a control rule can throttle it. Layered units would stay:
the two representations answer different questions, and the existing one is
right for a rain garden on one subcatchment.

## 13.4 Cross-sections, solvers and backends

**Chebyshev sections** would fit a measured irregular section with a
spectral expansion, replacing the transect lookup with a smooth function
whose derivatives are continuous — which matters for the Riemann solver of
@ref hydraulics_ref_ch8_finite_volume "Hydraulics 8", where a kink in
\f$dA/dh\f$ becomes a spurious reflection.

**An implicit finite-volume solver** would remove the Courant bound and let
a long continuous simulation take hydrological steps through quiescent
periods. The explicit solver's cost is bounded by the shortest cell in the
model whatever the flow is doing.

**GPU backends** exist for the 2D marcher on CUDA, HIP and SYCL. The 1D
finite-volume solver has the option surface and no kernel; Metal is
designed and not built. A backend a build does not carry is refused at open,
so a deck naming one fails loudly rather than running on the CPU and
reporting a speed-up that never happened.

## 13.5 Reading this chapter against the engine

Three rules govern how planned work appears in OpenSWMM's documentation, and
they are worth knowing when reading any of the manuals:

1. A section that is parsed but inert says so in the Engine Manual's
   grammar entry, prints a warning at runtime, and carries
   \status{Planned} wherever it is listed.
2. A formulation that ships but is not yet validated carries
   \status{Experimental}, names what would license it, and says which
   alternative to prefer meanwhile.
3. A key that is parsed and ignored carries \status{Retired} and is listed
   with the retired keys, not quietly dropped: an old deck keeps running,
   and the report says the key had no effect.

## Where to go next

- @ref hydraulics_ref_ch10_planned — planned and retired hydraulics
- @ref hydrology_ref_ch10_planned — planned hydrology
- @ref quality_ref_ch11_planned — planned water quality
- `ROADMAP.md` in the source tree — the engineering roadmap these entries
  summarise
