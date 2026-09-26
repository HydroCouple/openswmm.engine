@page hydrology_ref_ch10_planned Chapter 10: Planned Formulations

@tableofcontents

\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_

**Nothing in this chapter is in this release as designed.** \status{Planned}
Chapters 2 through 9 describe formulations the shipped engine executes; this
chapter records the ones it does not — designed and not built, or built with
a stated gap between what the code does and what its design or the literature
says it should — so that a plan file cited in a commit, a warning met in a
report, or a limitation stated in an earlier chapter can be followed to its
record. *Planned* means designed and not built, whatever the plan's own
optimism; *Experimental* means in the code and usable, with the gap named.
Every section names its plan of record by path; the plans are not part of the
manual, and the code is the source of truth where they disagree. The
hydraulic half of the same record is
@ref hydraulics_ref_ch10_planned "Hydraulics Chapter 10".

| Section | Formulation | Plan of record | Status |
|---|---|---|---|
| 10.1 | Groundwater transport in the mesh aquifer, the hydrology side | `plans/transport/GW_TRANSPORT_HEAT_MSX_PLAN_2026-09-04.md`; `ROADMAP.md` §2.3 | \status{Planned} |
| 10.2 | LID controls as storage nodes, the hydrology side | `plans/LID_StorageNode_Redesign.md`; `ROADMAP.md` §6 | \status{Planned} |
| 10.3 | LID detailed output | `plans/LID_DETAIL_OUTPUT_PLAN_2026-08-13.md` | \status{Planned} |
| 10.4 | The non-Gardner recharge closures and the benchmarks that would promote them | `plans/TWO_ZONE_GROUNDWATER_EXPLICIT_LTS_PLAN_2026-08-15.md` §9 steps 4, 7, 13, 18; `plans/TWO_ZONE_GROUNDWATER_FV_INTEGRATION_PLAN.md` §5 | \status{Experimental} |
| 10.5 | `MODE PER_SUBCATCH` for the mesh aquifer | `plans/TWO_ZONE_GROUNDWATER_EXPLICIT_LTS_PLAN_2026-08-15.md` §9 steps 8, 22 | \status{Experimental} |

*Table 10-1 Formulations recorded in this chapter*

## 10.1 Groundwater transport in the mesh aquifer, the hydrology side

The engine accepts six `[GW_*]` sections — @ref engine_manual_sect_GW_TRANSPORT_OPTIONS,
@ref engine_manual_sect_GW_TRANSPORT_PARAMS, @ref engine_manual_sect_GW_SORPTION,
@ref engine_manual_sect_GW_INITIAL_QUALITY, @ref engine_manual_sect_GW_BOUNDARY_QUALITY
and @ref engine_manual_sect_GW_SOURCES. They are registered and parsed,
validated at open against the mesh, the species registry and the time series
(cells on the mesh, an edge in \f$0 \ldots n_v - 1\f$ of its own cell, species
through the one registry, a duplicate source name an error), written back by
the input writer and editable through the C API of
`openswmm_gw_transport.h`. **They drive nothing.** Every run that authors any
of them puts one warning in the report:

> [GW_*] subsurface transport is AUTHORED but INERT this run: the integrated
> 2D groundwater component (org.hydrocouple.openswmm.integrated2d) provides
> the two-zone kernel these sections configure and is not available in this
> release. The rows are validated, kept and written back unchanged.

**What the design intends.** The plan of record moves every species class the
project carries through the two-zone aquifer of
@ref hydrology_ref_ch9_mesh_groundwater "Chapter 9": pollutants with
retardation, dispersion and first-order decay; MSX species of
@ref quality_ref_ch8_msx_reactions "Quality Chapter 8" under a new
`SUBSURFACE` reaction scope; water age; and temperature with conduction. In
the saturated zone the state is species mass in the cell's water,
\f$n h_g A C\f$, and the balance is

\f[ R_f\,\frac{\partial (n h_g C)}{\partial t} = -\nabla\cdot(\mathbf{q}\,C) + \nabla\cdot\bigl(n h_g \mathbf{D}\,\nabla C\bigr) + S_C - \lambda\, n h_g C, \qquad R_f = 1 + \frac{1-n}{n}\,\rho_s K_d \f]

with \f$\mathbf{D}\f$ from molecular diffusion and longitudinal and transverse
dispersivities on the Darcy velocity. In the unsaturated zone closure A's
single store is a well-mixed reactor fed at the surface-water concentration
and drained by \f$q_0\f$ at its own; closure B's σ layers carry a
one-dimensional vertical advection on the column's own ALE fluxes, first-order
upwind, plus vertical dispersion. Heat follows the same pattern with effective
properties taken from the actual moisture state,

\f[ (\rho c)_{eff}\,\frac{\partial T}{\partial t} = -\rho_w c_w\,\nabla\cdot(\mathbf{q}\,T) + \nabla\cdot\bigl(\lambda_{eff}\,\nabla T\bigr) + \sum S_H, \qquad (\rho c)_{eff} = \theta\,\rho_w c_w + (1-n)\,\rho_s c_s \f]

a vertical conduction chain from the surface boundary through the layers to a
geothermal or fixed deep boundary, lateral conduction on the saturated edges,
and latent removal where evapotranspiration takes water — solutes stay behind
(evapoconcentration). Advection is not a solver of its own: each volume flux
the kernel books in `fireGwFaces` and `fireGwCells` (§9.9) carries a tuple of
species mass, age-volume and enthalpy at the donor's concentration, so
conservation across the tier ladder is inherited from the kernel and proved
once.

**Coupling to the surface transport.** Infiltration enters the top of the
column at the 2D cell's water concentration where the cell is inundated and at
the rainfall or runon concentration otherwise; rejected infiltration and the
Dunne return flow of §9.7 go back up at the top layer's concentration; the
node–aquifer exchange of §9.6 carries the tuple in both directions, so
exfiltration takes mass and heat out of a node and groundwater inflow brings
the aquifer's in. That replaces the constant groundwater concentration,
temperature and age the legacy aquifer supplies at the node seam today — on
positive groundwater flow only, with exfiltration carrying nothing — which the
plan would migrate to `[GW_INITIAL_QUALITY] * SAT` rows on open, as initial
conditions rather than eternal constants.

**No kernel exists.** `src/engine/2d/gw/` holds the data, the parsers, the
validator and the C API only; none of the planned `SubsurfaceTransportState`,
`SubsurfaceTransport` or `SoilThermal` sources is in the tree, no reaction file
accepts a `SUBSURFACE` scope, and phases T7.1 to T7.4 of the plan are gated on
the flow kernel. The plan's own baseline states that the 2D subsurface kernel
does not exist — true when it was written, overtaken by Chapter 9 since — so
its first act would be re-verification against the kernel that now runs. \status{Planned}

<!-- source: src/engine/2d/gw/GwTransportData.hpp:20-22; src/engine/2d/gw/GwTransportSections.cpp:450-476 (registration), :621-627 (duplicate names), :632-639 (the warning, quoted verbatim); src/engine/2d/gw/GwTransportSections.hpp:81-98 (resolveGwTransport, inert warning); src/engine/core/SWMMEngine.cpp:560-569 (emission); src/engine/core/InpWriter.cpp:535-540 (writer); include/openswmm/engine/openswmm_gw_transport.h:17-43; src/engine/2d/api/ApiGwTransport.cpp:102, :461; plans/transport/GW_TRANSPORT_HEAT_MSX_PLAN_2026-09-04.md:1-5, :7-27 (cell-generic amendment), :35-39 (§0), :45-57 (§1 baseline), :65-74 (§2.1-2.2), :76-93 (§2.3-2.4), :95-107 (§2.5), :113-114 (§3.1), :138-156 (§3.5 tuple table), :205 (PER_SUBCATCH migration), :240-244 (§6), :263-278 (§8 phases), :284 (§9 planned files) -->

## 10.2 LID controls as storage nodes, the hydrology side

The LID controls of @ref quality_ref_ch6_lid_controls "Quality Chapter 6"
are subcatchment attributes: a unit occupies a fraction of its subcatchment,
its layers form a downward cascade — surface to soil by infiltration, soil to
storage by percolation at field capacity, storage to underdrain by an orifice
law on the storage depth alone — and it hands its outflow on through the runon
mechanism as a flow rate, never a head. The plan of record's problem statement
is that this cannot express anything hydraulic: a saturated downstream unit
cannot throttle the one above it, a control structure between units has no
place in the topology, and an underdrain beneath a surcharged pipe keeps
draining because its driving head is one-sided.

**What changes for hydrology.** The LID unit becomes a node of the network —
a storage node (@ref engine_manual_sect_STORAGE) carrying a new
`[LID_LAYERS]` attribute that gives each layer a fractional depth, porosity,
conductivity and moisture limits — and takes over the water balance the
subcatchment used to run for it: its own rainfall and evaporation on its
surface, inflow by runon from the contributing area (the existing mechanism;
no new hydrological machinery), and a head resolved at every hydraulic step.
The media layer keeps a hydrological model, a kinematic-wave reduction of
Richards' equation that drops the matric-potential gradient on the argument
that engineered media drain by gravity,

\f[ \frac{\partial\theta}{\partial t} = \frac{\partial}{\partial z}\,K(\theta), \qquad K(\theta) = K_s\left(\frac{\theta}{\theta_s}\right)^{n} \f]

discretised by the method of lines into \f$N\f$ sublayers,
\f$d\theta_i/dt = (q_{i-1} - q_i)/\Delta z_i\f$ plus any link sink mapped to
that elevation, every flux limited to the water the sublayer holds,
\f$q_i = \min\bigl(K(\theta_i),\ (\theta_i - \theta_r)\,\Delta z_i/\Delta t\bigr)\f$.
No retention curve is needed: the node's head partitions the profile into a
saturated lower zone whose head is the node's and an unsaturated upper zone
governed by moisture content alone, and percolation at the media base is free
drainage at \f$K(\theta_N)\f$ above field capacity. The two domains keep
their own clocks — the media on the hydrological step, the gravel layer (the
storage node itself) and its links on the hydraulic step — joined by the
slowly varying percolation flux.

**What stays.** The eight layered unit types, `[LID_CONTROLS]` and
`[LID_USAGE]` (@ref engine_manual_sect_LID_CONTROLS,
@ref engine_manual_sect_LID_USAGE) and the subcatchment cascade remain the
default representation, by the roadmap's compatibility clause; the storage-node
form is an alternative a modeller would opt into for the units whose behaviour
is hydraulic. The other half of the design — the porosity-corrected storage
curve, the underdrain as an offset orifice with the network's head on its far
side, overflow as a weir, chains as ordinary node–link topology — is hydraulic
and recorded in @ref hydraulics_ref_ch10_planned "Hydraulics Chapter 10, §10.8".

The status is design recorded, no implementation. No `[LID_LAYERS]` parser,
no LID node type and no kinematic media solver exists in `src/`; the plan's
code sketches target the legacy C sources and would be re-sited. The
`LidLayerSpeciesData` block that is in the tree is the per-layer species state
of the quality transport, not this design. \status{Planned}

<!-- source: plans/LID_StorageNode_Redesign.md:5-51 (§1), :57-85 (§2), :91-118 (§3.1-3.2), :120-142 (§3.3), :166-219 (§4.1-4.3), :276-286 (§4.6), :300-318 (§4.8), :350-368 (§6), :373-375 (C targets); ROADMAP.md:193-203; src/engine/data/LidLayerSpeciesData.hpp:17-32; grep of src/ and include/ for LID_LAYERS, lid_layers, MAX_LID_SUBLAYERS, lid_node: no hits -->

## 10.3 LID detailed output

Legacy SWMM writes, for every `[LID_USAGE]` row whose ninth field names a
file, a per-unit report at the runoff step: fourteen tab-separated columns —
date, elapsed time, then total inflow, evaporation, surface infiltration,
pavement percolation, soil percolation, storage exfiltration, surface runoff
and drain outflow as rates, and surface, pavement, soil-moisture and storage
levels — with dry spells compressed to their first and last rows. The modern
engine parses that field, resolves it as a path, round-trips it through the
input writer, the GeoPackage tables and the C API, and reads it nowhere: a
model that names report files silently produces none. The only LID output
today is the cumulative LID Performance Summary of the `.rpt`.

**What the plan would report.** One twelve-variable record per unit per
instant — the eight rates in internal ft/s, the four depths in ft, soil
moisture dimensionless — converted once at the sink boundary, to three sinks
selected in `[REPORT]` (@ref engine_manual_sect_REPORT): legacy-compatible
per-unit files written by a runoff-step recorder immediately after the LID
step, so they are byte-comparable with legacy; one consolidated tab-delimited
file keyed by subcatchment and unit, written at the report step with rates
averaged over the report interval and no dry-period compression; and
GeoPackage result time series under a new `LID` object type. Two producers
rather than one because LID advances at the runoff step and snapshots are
built at the report step. The keys the plan proposes,
`LID_DETAIL` (`NONE`, `UNITS`, `FILE` or `ALL`) and `LID_FILE`, are **not
parsed by this release**.

Nothing has shipped: no `LidDetailRecorder`, `LidOutputPlugin`,
`fillLidSnapshot`, `rpt_*` retention arrays or `lid_unit_count` snapshot field
exists in `src/`. Of the four blockers the plan's survey recorded, the one it
made a blocking Phase 0 — LID layer parameters never unit-converted, so a
report would print the wrong internal state — has since been fixed: the LID
solver now converts every layer parameter out of the deck's display units at
initialisation. The others stand as far as the tree shows: the three
inter-layer fluxes have no retention arrays, the plan found `evap_loss` and
`infil_loss` to mean different things in different unit types, and the cadence
mismatch above is structural. \status{Planned}

<!-- source: plans/LID_DETAIL_OUTPUT_PLAN_2026-08-13.md:3-11, :17-35 (§1.1), :37-72 (§1.2), :74-114 (§1.3 B1-B4), :120-133 (§2.1), :135-151 (§2.2), :180-196 (§2.5), :198-226 (§2.6), :228-241 (§2.7), :252-270 (Phase 0), :364-384 (§4), :388-400 (§5); src/engine/input/handlers/HydrologyHandler.cpp:608 (rpt_file parsed); src/engine/input/PostParseResolver.cpp:160 (resolved as a path); src/engine/core/InpWriter.cpp:1990-1991; src/engine/input/geopackage/GeoPackageWriter.cpp:1294-1295; GeoPackageReader.cpp:1522; src/engine/core/openswmm_model_impl.cpp:600-602; include/openswmm/engine/openswmm_model.h:376; src/engine/plugins/DefaultReportPlugin.cpp:1934-1952 (LID Performance Summary); src/engine/core/SWMMEngine.cpp:2395 (lid_.execute at the runoff step); src/engine/hydrology/LID.cpp:168-176, :234-241, :275, :285 (layer parameters converted at init — B3 fixed); grep of src/ and include/ for LidDetailRecorder, LID_DETAIL, LID_FILE, fillLidSnapshot, LidOutputPlugin, rpt_surf_infil, rpt_pave_perc, rpt_soil_perc, rpt_stor_exfil, rpt_total_evap, lid_unit_count, lid_values: no hits -->

## 10.4 The non-Gardner recharge closures and the benchmarks that would promote them

Chapter 9 §9.3 states the caveat and §9.11 carries it. Of the four soil laws,
Gardner's quasi-steady recharge across the water table is Qu and Duffy's
equation 22 verbatim, (9-3), and is verified against it. Russo, Brooks–Corey
and van Genuchten use the engine's generalisation (9-5),

\f[ q_0 = \frac{\bar K}{\sigma(L)}\,\bigl[\sigma(L) - \tilde h_u\bigr], \qquad \bar K = K\!\left(\psi = \tfrac12 L\right) \f]

whose equilibrium (\f$q_0 = 0\f$ at hydrostatic storage) and sign are the
law's own and whose *rate* — the conductivity at the column's mean suction
over the law's equilibrium storage — is a modelling choice the code makes
explicit rather than a published closed form. It reduces to Gardner's order and
limits, which is the consistency it rests on, and no more. The header names
what would license it — "Plan step 18's closure-ladder benchmark and step 4's
HYDRUS-1D comparison" — and until those run the three laws carry
\status{Experimental}, Gardner or Russo are preferred for anything
quantitative, and closure B (`SIGMA`), which integrates the real Richards flux
and needs none of this, is preferred where the answer matters. The caveat is
confined to closure A's recharge: the σ column and the `ENSLAVED` equilibrium
use each law's retention and conductivity directly.

**The benchmarks on the record.** The explicit-kernel plan's G1/G2 track
schedules, in order: step 4, the σ column against the Broadbridge–White exact
Richards solution and a HYDRUS-1D single column with layer-count convergence at
\f$m \in \{4, 8, 16\}\f$ — the in-house reference for everything after it;
step 7, dry-down under evaporative demand against the HYDRUS reference, both
closures; step 13, the Brooks–Corey and van Genuchten closure-A laws behind
the earlier draft's gates, of which the finite-volume plan states the
acceptance — van Genuchten's \f$q_0\f$ agreeing with HYDRUS-1D on a single
column within 2 %, with the cost over Gardner measured; and step 18, the
closure ladder, \f$\alpha L \in \{0.5, 1, 3, 5, 10\}\f$ across closures A and
B and the Russo and Gardner laws, a publishable crossover measurement that
confirms or adjusts the `AUTO` thresholds and sets the `C_GW` and `C_COL`
defaults. The finite-volume plan's acceptance for the ladder is recharge
volume over the storm, peak groundwater outflow timing and magnitude,
wall-clock per simulated day, and the \f$L^2\f$ error against a
one-dimensional Richards reference on a single prism.

**Current state.** None of them has run. No test under `tests/` mentions
HYDRUS or a closure ladder, and Broadbridge–White appears in the tree only as
a comment. The kernel already seeds closure A and closure B from the same
hydrostatic storage so that the ladder will compare trajectories rather than
initial conditions — the harness is anticipated, not built.

**What Implemented would require.** For each of the three laws, the closure-A
recharge trajectory on a single column — wetting, drainage and dry-down under
ET — against closure B and a HYDRUS-1D reference across the ladder's
\f$\alpha L\f$ range, within a stated tolerance (the plan's 2 %), with the
result recorded in the manual. Either that agreement licenses the rate as it
stands, or a published closed form or a fitted rate constant replaces
\f$\bar K/\sigma(L)\f$ in (9-5) and the header's warning comes out. The ladder
as drafted measures Russo and Gardner only, so Brooks–Corey and van Genuchten
would have to be added to it; and since the ladder's purpose in the plan is
the `AUTO` crossover, the per-law HYDRUS comparison is the promotion gate
proper. \status{Experimental}

<!-- source: src/engine/2d/subsurface/SoilCharacteristic.hpp:22-72 (laws, q0, the @warning at :62-72); src/engine/2d/subsurface/SoilCharacteristic.cpp:247-249 (enslavedStorage), :251-284 (rechargeQ0: Gardner :259-268, generalised :270-283); src/engine/2d/subsurface/SubsurfaceSolver.cpp:234-250 (common hydrostatic seed, closure-ladder comment); plans/TWO_ZONE_GROUNDWATER_EXPLICIT_LTS_PLAN_2026-08-15.md:185-192 (§2.4), :798-801 (step 4), :809-811 (step 7), :838 (step 13), :847-849 (step 18), :874 (thresholds at step 18), :914 (D-N2); plans/TWO_ZONE_GROUNDWATER_FV_INTEGRATION_PLAN.md:338-344 (§5 acceptance), :459 (step 11), :461 (step 13), :471 (step 18); grep of tests/ (excluding tests/output) for HYDRUS and closure ladder: no hits; grep of tests/unit and src/ for Broadbridge: SubsurfaceData.hpp only -->

## 10.5 `MODE PER_SUBCATCH` for the mesh aquifer

`[2D_AQUIFER_OPTIONS] MODE` (@ref engine_manual_sect_2D_AQUIFER_OPTIONS)
accepts `MESH`, the default, and `PER_SUBCATCH`. The design of the second is
one degenerate cell per subcatchment, no mesh sections, lateral flux zero and
node exchange with the subcatchment's outlet node — the vehicle by which a
pipe-only project with no mesh would get the Chapter 9 kernel (head-driven
groundwater inflow and exfiltration through the outlet, the legacy `[GWF]`
override winning where authored) and the vehicle the transport plan of §10.1
keys its migration on. Its gates were step 8 of the G1 track and, at step 22,
an overhead under 1.2× the legacy solver of
@ref hydrology_ref_ch5_groundwater "Chapter 5".

The code implements less. The token is parsed, written back and readable
through the API; at initialisation it sets the lateral edge count to zero, so
no Darcy faces are built and no lateral flux is booked. Everything else is the
mesh path: the state is sized to the mesh's cell count — a deck with
`MODE PER_SUBCATCH` and no mesh fails with "[2D_AQUIFER] authored but the
mesh is empty" — and the cell-to-subcatchment and outlet-node mappings the
vehicle needs, `subcatch_outlet_node_` and `cell_subcatch_`, are declared in
the solver and neither assigned nor read anywhere in the tree. (The surface
router has a member of the same name, populated by centroid-in-polygon for the
`SUBCATCH_AQUIFER` infiltration destination of
@ref hydrology_ref_ch8_mesh_surface "Chapter 8" §8.4; it is not the
solver's.) What `PER_SUBCATCH` means today is "the mesh kernel with lateral
flow removed" (§9.1, §9.11): usable, and not what the design says.
\status{Experimental}

<!-- source: src/engine/2d/subsurface/SubsurfaceSections.hpp:46-56 (grammar); src/engine/2d/subsurface/SubsurfaceSections.cpp:150-152 (parse), :651-652 (writer); src/engine/2d/api/ApiGw2D.cpp:118; src/engine/2d/subsurface/SubsurfaceData.hpp:116-119 (intent), :174-177; src/engine/2d/subsurface/SubsurfaceSolver.cpp:225-230 (sized to the mesh; empty-mesh error), :252-264 (ne = 0 under per_subcatch); src/engine/2d/subsurface/SubsurfaceSolver.hpp:268-273 (the two vectors); grep of src/, include/ and tests/ for subcatch_outlet_node_ and cell_subcatch_: the solver's declarations only, plus SurfaceRouter2D's own member (src/engine/2d/SurfaceRouter2D.cpp:1092-1108, SurfaceRouter2D.hpp:456); plans/TWO_ZONE_GROUNDWATER_EXPLICIT_LTS_PLAN_2026-08-15.md:812-816 (step 8), :861-863 (step 22); plans/transport/GW_TRANSPORT_HEAT_MSX_PLAN_2026-09-04.md:37, :205 -->

## 10.6 Where the rest of the record is

`ROADMAP.md` carries three of these items. §2.3 is the groundwater
advection–dispersion model, still listed as planned scope with no status line,
which §10.1 details; §5.2 is the spatially explicit groundwater of Chapter 9,
whose status line still reads "no implementation yet" — the roadmap has not
caught up with the kernel, and the two Experimental items of §10.4 and §10.5
are what remains open in it; §6 is LID as storage nodes, §10.2 here and §10.8
of the hydraulics reference. LID detailed output has no roadmap entry and
lives in its plan alone. For the modeller rather than the developer, the
application manual's "What is coming" chapter lists the same items with their
badges and the option keys that must not be mistaken for working ones.

<!-- source: ROADMAP.md:115-122 (§2.3), :178-190 (§5.2, "No implementation yet" at :182), :193-203 (§6), grep of ROADMAP.md for LID: §6 only; plans/DOCUMENTATION_FORMULATIONS_PLAN_2026-09-19.md:594-595 (application manual Ch13 "What is coming") -->
