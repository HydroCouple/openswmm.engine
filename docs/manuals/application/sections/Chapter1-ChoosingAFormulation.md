@page application_manual_ch1_choosing Chapter 1: Choosing a Formulation

OpenSWMM offers more than one representation of nearly every process it
models. This chapter is the map: six decision workflows that take a
modelling question and end at the literal `[OPTIONS]` lines that answer it,
and a closing table that names, for every formulation, the keys that select
it, its status, the theory that describes it and the worked example that
uses it.

The workflows are drawn as flowcharts you can pan, zoom and click: a
terminal node opens the chapter that documents the formulation it names.
Nothing here decides for you — each branch states the question a modeller
has to answer about their own system, and the cost of the answer.

**One principle runs through all six.** A default is not a recommendation;
it is the choice that reproduces earlier behaviour. `FLOW_ROUTING DYNWAVE`,
`SURCHARGE_METHOD EXTRAN`, `QUALITY_SOLVER LEGACY` and
`MOMENTUM_EQUATION LOCAL_INERTIAL` are all defaults because changing them
would change existing models' answers, not because they are the best
available representation of every system. A model that never sets a key is
choosing the legacy formulation.

## 1.1 Which flow router

The router decides what the momentum equation keeps. Steady flow keeps
nothing and translates hydrographs; kinematic wave keeps the friction
balance and cannot propagate a backwater; dynamic wave keeps the full
one-dimensional momentum equation between nodes; the finite-volume router
discretises the conduit itself and resolves the profile inside it.

<!-- workflow: app_routing_method -->
<pre class="mermaid">
graph TD
    A[How is the conveyance system driven] --> B{Backwater, surcharge or pressurisation anywhere in the network}
    B -- no --> C{Is in-pipe attenuation part of the answer}
    C -- no --> D[FLOW_ROUTING STEADY]
    C -- yes --> E[FLOW_ROUTING KINWAVE]
    B -- yes --> F{Do you need the profile inside the conduit: bores, hydraulic jumps, mixed flow}
    F -- no --> G[FLOW_ROUTING DYNWAVE]
    F -- yes --> H[FLOW_ROUTING FV with FV_CELL_LENGTH and FV_MIN_CELLS]
    G --> I{Does the network surcharge against its crowns}
    I -- yes --> J[Choose a pressurisation closure: section 1.2]
    H --> J
</pre>
<div class="workflow-links" data-workflow="app_routing_method">
<span data-node="D">@ref hydraulics_ref_ch2_hydraulic_model "Hydraulics 2: steady flow routing"</span>
<span data-node="E">@ref hydraulics_ref_ch4_kinematic_wave "Hydraulics 4: kinematic wave"</span>
<span data-node="G">@ref hydraulics_ref_ch3_dynamic_wave "Hydraulics 3: dynamic wave"</span>
<span data-node="H">@ref hydraulics_ref_ch8_finite_volume "Hydraulics 8: explicit finite volume"</span>
<span data-node="J">@ref application_manual_ch2_filling_pipe "Chapter 2: a filling pipe under both routers"</span>
</div>

*Figure 1-1 Choosing a flow router*

The cost ladder is real. Steady flow is free, kinematic wave is cheap,
dynamic wave costs an iteration per node per step, and the finite-volume
router costs a substep bounded by the Courant condition on the shortest
cell. @ref application_manual_ch2_filling_pipe "Chapter 2: a filling pipe under both routers" runs the same
filling pipe through the last two and shows what the extra cost buys.

## 1.2 Which pressurisation closure

A pipe that fills has no free surface, and the shallow-water equations need
one. Every closure invents a narrow slot above the crown so the equations
keep their form; they differ in what the slot remembers and whether the
pressure may go negative.

<!-- workflow: app_pressure_closure -->
<pre class="mermaid">
graph TD
    A[Pipes pressurise] --> B{Which router}
    B -- dynamic wave --> C{Is sub-atmospheric pressure part of the question}
    C -- no --> D[SURCHARGE_METHOD EXTRAN or SLOT]
    C -- yes --> E[SURCHARGE_METHOD TPA with TPA_CELERITY]
    B -- finite volume --> F{Is sub-atmospheric pressure part of the question}
    F -- no --> G[FV_PRESSURE_CLOSURE SLOT with FV_SLOT_CELERITY]
    F -- yes --> H[FV_PRESSURE_CLOSURE TPA]
    D --> I{Is the surge history itself the answer}
    I -- yes --> K[SURCHARGE_METHOD DYNAMIC_SLOT with the DPS keys]
    G --> L[Check the slot's share of stored volume in the report]
    H --> M[Experimental: read section 8.4.5 before trusting a number]
</pre>
<div class="workflow-links" data-workflow="app_pressure_closure">
<span data-node="D">@ref hydraulics_ref_ch3_dynamic_wave "Hydraulics 3: EXTRAN and the static slot"</span>
<span data-node="E">@ref hydraulics_ref_ch3_dynamic_wave "Hydraulics 3: the two-component pressure approach"</span>
<span data-node="G">@ref hydraulics_ref_ch8_finite_volume "Hydraulics 8: the tapered static slot"</span>
<span data-node="H">@ref hydraulics_ref_ch8_finite_volume "Hydraulics 8: the TPA closure"</span>
<span data-node="K">@ref hydraulics_ref_ch3_dynamic_wave "Hydraulics 3: the dynamic Preissmann slot"</span>
<span data-node="L">@ref hydraulics_ref_ch8_finite_volume "Hydraulics 8: slot storage accounting"</span>
<span data-node="M">@ref application_manual_ch3_subatmospheric "Chapter 3: a sub-atmospheric transient"</span>
</div>

*Figure 1-2 Choosing a pressurisation closure*

A slot celerity is an accuracy-cost trade, not a physical constant: the
explicit step is bounded by the celerity, so asking for the acoustic speed
of water collapses the step size. The default of 100 ft/s is the order the
dynamic wave solver's own slot produces.
@ref application_manual_ch3_subatmospheric "Chapter 3: a sub-atmospheric transient" runs a siphon where
the two closures disagree about the sign of the pressure.

## 1.3 Do you need the two-dimensional surface

A one-dimensional model reports water leaving a node as flooding and loses
sight of it. A mesh keeps it: where it goes, how deep it gets and when it
comes back. That is a different question from how much left, and it costs a
mesh, a solver and a coupling.

<!-- workflow: app_add_2d -->
<pre class="mermaid">
graph TD
    A[Does water leave the network and spread over the surface] --> B{Is where it goes part of the answer}
    B -- no --> C[Stay one-dimensional: flooding is reported per node]
    B -- yes --> D{What drives the mesh}
    D -- rain on the grid --> E[RAINFALL_MODE NATURAL_NEIGHBOUR with 2D_INFILTRATION rows]
    D -- spill from the network --> F[2D_VERTEX_NODE_MAP or 2D_TRIANGLE_NODE_MAP]
    D -- both --> G[Both, with subcatchment coverage ownership checked]
    E --> H{Which momentum closure}
    F --> H
    G --> H
    H -- shallow and friction dominated --> I[MOMENTUM_EQUATION LOCAL_INERTIAL]
    H -- transcritical flow or shocks --> J[MOMENTUM_EQUATION FULL_SWE]
    H -- very flat and slow --> K[MOMENTUM_EQUATION DIFFUSIVE_WAVE]
</pre>
<div class="workflow-links" data-workflow="app_add_2d">
<span data-node="C">@ref engine_manual_ch4_reports "Engine 4: the flooding report"</span>
<span data-node="E">@ref hydrology_ref_ch8_mesh_surface "Hydrology 8: rain and infiltration on the mesh"</span>
<span data-node="F">@ref hydraulics_ref_ch9_two_dimensional "Hydraulics 9: coupling to the network"</span>
<span data-node="G">@ref quality_ref_ch10_mesh_quality "Quality 10: coverage ownership"</span>
<span data-node="I">@ref hydraulics_ref_ch9_two_dimensional "Hydraulics 9: the local inertial closure"</span>
<span data-node="J">@ref hydraulics_ref_ch9_two_dimensional "Hydraulics 9: the full shallow-water closure"</span>
<span data-node="K">@ref hydraulics_ref_ch9_two_dimensional "Hydraulics 9: the diffusive-wave closure"</span>
</div>

*Figure 1-3 Deciding whether to add the two-dimensional surface, and how to drive it*

*Chapter 6, building a coupled 1D-2D model* (in preparation) builds a coupled model
from an existing network; *Chapter 7, rain, infiltration and groundwater on the mesh* (in preparation) drives one with rain rather than spill. The GUI tutorials
@ref tutorial_2d_inundation and @ref tutorial_1d2d_coupling walk the same
ground with a mouse.

## 1.4 Which transport engine

All three engines solve the same advection-reaction-dispersion problem and
share the same loads, the same reactions and the same mass-balance ledger.
They differ in how they represent a front.

<!-- workflow: app_transport_engine -->
<pre class="mermaid">
graph TD
    A[Are constituents carried] --> B{What has to be right}
    B -- totals at the outfall only --> C[QUALITY_SOLVER LEGACY]
    B -- the shape of a front --> D{Sharp front or long chain of storages}
    D -- a sharp front, plug flow --> E[QUALITY_SOLVER LAGRANGIAN with MAX_SEGMENTS_PER_LINK]
    D -- dispersion is the mechanism --> F[QUALITY_SOLVER EULERIAN_ARD with TARGET_DX]
    F --> G[DISPERSION FISCHER, or a measured VALUE]
    E --> H[DISPERSION RWPT with RWPT_SEED for a reproducible draw]
    C --> I[Every engine shares the same loads, reactions and ledger]
</pre>
<div class="workflow-links" data-workflow="app_transport_engine">
<span data-node="C">@ref quality_ref_ch5_transport_treatment "Quality 5: the legacy CSTR routing"</span>
<span data-node="E">@ref quality_ref_ch7_ard_transport "Quality 7: the Lagrangian engine"</span>
<span data-node="F">@ref quality_ref_ch7_ard_transport "Quality 7: the Eulerian engine"</span>
<span data-node="G">@ref quality_ref_ch7_ard_transport "Quality 7: dispersion"</span>
<span data-node="H">@ref quality_ref_ch7_ard_transport "Quality 7: dispersion under the Lagrangian engine"</span>
</div>

*Figure 1-4 Choosing a transport engine*

Refining the Eulerian mesh is quadratic: halving `TARGET_DX` doubles both
the cell count and the substep count.
*Chapter 8, the three transport engines* (in preparation) puts a tracer
pulse through all three.

## 1.5 Age, heat and multi-species reactions

These three ride on the transport engine rather than replacing it. Each is
a separate process component with its own configuration file, and each adds
its own reserved species row.

<!-- workflow: app_heat_age_msx -->
<pre class="mermaid">
graph TD
    A[Beyond pollutant concentrations] --> B{Which question}
    B -- how long has this water been in the system --> C[WATER_AGE ON with a WATER_AGE_SOURCES section]
    B -- what temperature is it --> D[HEAT_TRANSPORT ON with a heat configuration file]
    B -- species that react with each other --> E[PROCESS_COMPONENTS reactions with a reaction file]
    D --> F[TEMPERATURE with HUMIDITY, WIND and the radiative sections]
    E --> G{Do the reactions depend on temperature}
    G -- yes --> D
    C --> H[The age row is carried by whichever transport engine is selected]
</pre>
<div class="workflow-links" data-workflow="app_heat_age_msx">
<span data-node="C">@ref quality_ref_ch9_age_heat "Quality 9: water age"</span>
<span data-node="D">@ref quality_ref_ch9_age_heat "Quality 9: heat transport"</span>
<span data-node="E">@ref quality_ref_ch8_msx_reactions "Quality 8: multi-species reactions"</span>
<span data-node="F">@ref hydrology_ref_ch2_meteorology "Hydrology 2: meteorological forcing"</span>
<span data-node="H">@ref quality_ref_ch7_ard_transport "Quality 7: the transport engines"</span>
</div>

*Figure 1-5 Water age, heat and reacting species*

*Chapter 9, multi-species reactions* (in preparation) and
*Chapter 10, water age and heat* (in preparation) configure all three on
one deck.

## 1.6 Which groundwater representation

The lumped two-zone aquifer belongs to a subcatchment and exchanges with
one node. The mesh aquifer belongs to the cells and exchanges with every
node standing in them. They are different models of the same water, and
only one of them can own a given parcel of ground.

<!-- workflow: app_groundwater -->
<pre class="mermaid">
graph TD
    A[Is subsurface exchange part of the question] --> B{Lumped or spatially explicit}
    B -- one store per subcatchment --> C[AQUIFERS with GROUNDWATER rows]
    B -- a store per mesh cell --> D[2D_AQUIFER_OPTIONS with 2D_AQUIFER rows]
    C --> E{Do you need your own flow equation}
    E -- yes --> F[A GWF section on the subcatchment]
    D --> G{How thick is the unsaturated column in capillary lengths}
    G -- thin or unknown --> H[CLOSURE AUTO chooses at initialisation]
    G -- deep, with profile memory --> I[CLOSURE SIGMA integrates the column]
    D --> J{Where does mesh infiltration go}
    J -- to this aquifer --> K[INFIL_DESTINATION AQUIFER_2D]
</pre>
<div class="workflow-links" data-workflow="app_groundwater">
<span data-node="C">@ref hydrology_ref_ch5_groundwater "Hydrology 5: the two-zone aquifer"</span>
<span data-node="D">@ref hydrology_ref_ch9_mesh_groundwater "Hydrology 9: the mesh aquifer"</span>
<span data-node="F">@ref engine_manual_sect_GWF "The GWF section"</span>
<span data-node="H">@ref hydrology_ref_ch9_mesh_groundwater "Hydrology 9: the AUTO closure rule"</span>
<span data-node="I">@ref hydrology_ref_ch9_mesh_groundwater "Hydrology 9: the sigma column"</span>
<span data-node="K">@ref hydrology_ref_ch8_mesh_surface "Hydrology 8: infiltration destinations"</span>
</div>

*Figure 1-6 Choosing a groundwater representation*

The mesh aquifer's non-Gardner soil laws are \status{Experimental}: their
equilibrium is right and their relaxation rate is the engine's own
generalisation rather than a published result.
*Chapter 7, rain, infiltration and groundwater on the mesh* (in preparation) runs one.

## 1.7 The formulations, their keys and where they are documented

| Formulation | Selected by | Status | Theory | Worked example |
|---|---|---|---|---|
| Steady flow routing | `FLOW_ROUTING STEADY` | \status{Implemented} | @ref hydraulics_ref_ch2_hydraulic_model | — |
| Kinematic wave | `FLOW_ROUTING KINWAVE` | \status{Implemented} | @ref hydraulics_ref_ch4_kinematic_wave | — |
| Dynamic wave | `FLOW_ROUTING DYNWAVE` | \status{Implemented} | @ref hydraulics_ref_ch3_dynamic_wave | @ref application_manual_ch2_filling_pipe "Chapter 2: a filling pipe under both routers" |
| Explicit finite volume | `FLOW_ROUTING FV` | \status{Implemented} | @ref hydraulics_ref_ch8_finite_volume | @ref application_manual_ch2_filling_pipe "Chapter 2: a filling pipe under both routers" |
| Local time stepping | `FV_LTS` | \status{Implemented} | @ref hydraulics_ref_ch8_finite_volume | *Chapter 4, virtual junctions on a surveyed trunk* (in preparation) |
| Semi-implicit node continuity | `NODE_CONTINUITY SEMI_IMPLICIT` | \status{Implemented} | @ref hydraulics_ref_ch3_dynamic_wave | — |
| Anderson acceleration | `ANDERSON_ACCEL` | \status{Implemented} | @ref hydraulics_ref_ch3_anderson | — |
| EXTRAN surcharge | `SURCHARGE_METHOD EXTRAN` | \status{Implemented} | @ref hydraulics_ref_ch3_dynamic_wave | @ref application_manual_ch2_filling_pipe "Chapter 2: a filling pipe under both routers" |
| Static Preissmann slot | `SURCHARGE_METHOD SLOT` | \status{Implemented} | @ref hydraulics_ref_ch3_dynamic_wave | @ref application_manual_ch2_filling_pipe "Chapter 2: a filling pipe under both routers" |
| Dynamic Preissmann slot | `SURCHARGE_METHOD DYNAMIC_SLOT` | \status{Implemented} | @ref hydraulics_ref_ch3_dynamic_wave | — |
| Two-component pressure, dynamic wave | `SURCHARGE_METHOD TPA` | \status{Experimental} | @ref hydraulics_ref_ch3_dynamic_wave | @ref application_manual_ch3_subatmospheric "Chapter 3: a sub-atmospheric transient" |
| Two-component pressure, finite volume | `FV_PRESSURE_CLOSURE TPA` | \status{Experimental} | @ref hydraulics_ref_ch8_finite_volume | @ref application_manual_ch3_subatmospheric "Chapter 3: a sub-atmospheric transient" |
| Unsteady friction | `UNSTEADY_FRICTION VITKOVSKY` | \status{Implemented} | @ref hydraulics_ref_ch3_dynamic_wave | @ref application_manual_ch3_subatmospheric "Chapter 3: a sub-atmospheric transient" |
| Virtual junctions | `[VIRTUAL_JUNCTIONS]` | \status{Implemented} | @ref hydraulics_ref_ch3_dynamic_wave | *Chapter 4, virtual junctions on a surveyed trunk* (in preparation) |
| Inlet junctions | `[INLET_JUNCTIONS]` | \status{Implemented} | @ref hydraulics_ref_ch7_advanced_features | *Chapter 5, street inlets and inlet junctions* (in preparation) |
| HEC-22 street inlets | `[STREETS]`, `[INLETS]`, `[INLET_USAGE]` | \status{Implemented} | @ref hydraulics_ref_ch7_advanced_features | *Chapter 5, street inlets and inlet junctions* (in preparation) |
| 2D local inertial | `MOMENTUM_EQUATION LOCAL_INERTIAL` | \status{Implemented} | @ref hydraulics_ref_ch9_two_dimensional | *Chapter 6, building a coupled 1D-2D model* (in preparation) |
| 2D full shallow water | `MOMENTUM_EQUATION FULL_SWE` | \status{Implemented} | @ref hydraulics_ref_ch9_two_dimensional | *Chapter 6, building a coupled 1D-2D model* (in preparation) |
| 2D diffusive wave | `MOMENTUM_EQUATION DIFFUSIVE_WAVE` | \status{Implemented} | @ref hydraulics_ref_ch9_two_dimensional | *Chapter 6, building a coupled 1D-2D model* (in preparation) |
| Rain on the mesh | `RAINFALL_MODE` | \status{Implemented} | @ref hydrology_ref_ch8_mesh_surface | *Chapter 7, rain, infiltration and groundwater on the mesh* (in preparation) |
| Per-cell infiltration | `[2D_INFILTRATION]` | \status{Implemented} | @ref hydrology_ref_ch8_mesh_surface | *Chapter 7, rain, infiltration and groundwater on the mesh* (in preparation) |
| Two-zone aquifer | `[AQUIFERS]`, `[GROUNDWATER]` | \status{Implemented} | @ref hydrology_ref_ch5_groundwater | *Chapter 11, long-term hydrology* (in preparation) |
| Mesh two-layer aquifer | `[2D_AQUIFER]` | \status{Implemented} | @ref hydrology_ref_ch9_mesh_groundwater | *Chapter 7, rain, infiltration and groundwater on the mesh* (in preparation) |
| Non-Gardner soil laws | `SOIL_CHAR` | \status{Experimental} | @ref hydrology_ref_ch9_mesh_groundwater | *Chapter 7, rain, infiltration and groundwater on the mesh* (in preparation) |
| Modified Green-Ampt | `INFILTRATION MODIFIED_GREEN_AMPT` | \status{Implemented} | @ref hydrology_ref_ch4_infiltration | *Chapter 11, long-term hydrology* (in preparation) |
| RDII with decay | `[RDII_DECAY]` | \status{Implemented} | @ref hydrology_ref_ch7_rdii | *Chapter 11, long-term hydrology* (in preparation) |
| Legacy quality routing | `QUALITY_SOLVER LEGACY` | \status{Implemented} | @ref quality_ref_ch5_transport_treatment | *Chapter 8, the three transport engines* (in preparation) |
| Eulerian transport | `QUALITY_SOLVER EULERIAN_ARD` | \status{Implemented} | @ref quality_ref_ch7_ard_transport | *Chapter 8, the three transport engines* (in preparation) |
| Lagrangian transport | `QUALITY_SOLVER LAGRANGIAN` | \status{Implemented} | @ref quality_ref_ch7_ard_transport | *Chapter 8, the three transport engines* (in preparation) |
| Multi-species reactions | `[PROCESS_COMPONENTS]` reactions | \status{Implemented} | @ref quality_ref_ch8_msx_reactions | *Chapter 9, multi-species reactions* (in preparation) |
| Water age | `WATER_AGE ON` | \status{Implemented} | @ref quality_ref_ch9_age_heat | *Chapter 10, water age and heat* (in preparation) |
| Heat transport | `HEAT_TRANSPORT ON` | \status{Implemented} | @ref quality_ref_ch9_age_heat | *Chapter 10, water age and heat* (in preparation) |
| 2D surface quality | `[2D_COVERAGES]`, `[2D_LOADINGS]` | \status{Implemented} | @ref quality_ref_ch10_mesh_quality | — |
| Groundwater transport | `[GW_*]` sections | \status{Planned} | @ref hydrology_ref_ch10_planned | — |
| Sediment transport | — | \status{Planned} | @ref quality_ref_ch11_planned | — |
| LID as storage nodes | — | \status{Planned} | @ref hydrology_ref_ch10_planned | — |
| Chebyshev cross-sections | — | \status{Planned} | @ref hydraulics_ref_ch10_planned | — |
| 1D GPU backend | `FV_BACKEND` | \status{Planned} | @ref hydraulics_ref_ch10_planned | — |
| 2D Metal backend | `BACKEND` | \status{Planned} | @ref hydraulics_ref_ch10_planned | — |

*Table 1-1 Every formulation, the keys that select it, and where it is documented*

Grammar for every key named here is in
@ref engine_manual_ch2_input_file "Chapter 2 of the Engine Manual"; the
GUI's own controls are in @ref manual_simulation_options.
