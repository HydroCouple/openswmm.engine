@page hydraulics_ref_ch9_two_dimensional Chapter 9: Two-Dimensional Overland Flow Analysis

@tableofcontents

\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_

Chapters 3, 4 and 8 all solve one-dimensional flow along a conduit. When
a sewer surcharges, the water that leaves the network spreads over the
street, ponds behind a kerb, follows the terrain rather than the pipe,
and re-enters the network somewhere else. @ref hydraulics_ref_ch2_hydraulic_model "Chapter 2"'s node-link model
has no representation for any of that: SWMM 5 either discards the
overflow or holds it in a fictitious ponded area above the node, to be
returned to the same node later.

This chapter documents the optional two-dimensional overland-flow
domain OpenSWMM provides for that water — a cell-centred finite-volume
solver for the local-inertial shallow-water equations on an unstructured
triangular mesh, coupled bidirectionally to the node-link network.

The module is activated by the presence of a mesh in the project
(`[2D_VERTICES]` and `[2D_TRIANGLES]`, inline or via `[2D_MESH_FILE]`)
rather than by a routing option. `IGNORE_2D YES` disables it while leaving the
mesh parsed and editable. The 1D network continues to route by whatever
`FLOW_ROUTING` selects; the 2D domain supplements the network routing
rather than replacing it.

## 9.1 What the method adds

**A surface that is a domain in its own right.** Ponded
volume in the node-link model belongs to the node it came from. On the
2D mesh it belongs to the terrain: it flows downhill, splits at a crown,
pools in a sag, and drains into whichever inlet it reaches. Which node
receives the water is an outcome of the calculation rather than an
input to it.

Figure 9-1 places the two domains side by side: the node–link network of
Chapters 3, 4 and 8 with its routing and pressurisation alternatives, the
overland surface of this chapter with its three momentum closures, and the
exchange pathways that join them — vertex and cell coupling, street inlet
capture, and the node–bed exchange of a mesh aquifer.

![Figure 9-1 The one-dimensional network and the two-dimensional surface: exchange pathways](figures/png/hydraulics_conceptual_1d2d.png)

*Figure 9-1 The one-dimensional network and the two-dimensional surface: exchange pathways*

<div class="fig-hotspots" data-fig="hydraulics_conceptual_1d2d">
<span class="hs" data-box="0.0300,0.0798,0.4505,0.1011">@ref hydrology_ref_ch2_meteorology "Precipitation"</span>
<span class="hs" data-box="0.0360,0.1011,0.0963,0.1181">@ref engine_manual_sect_RAINGAGES "rain gages"</span>
<span class="hs" data-box="0.1008,0.1011,0.2279,0.1181">@ref hydrology_ref_ch6_snowmelt "rain · snow split (SCF)"</span>
<span class="hs" data-box="0.2324,0.1011,0.3081,0.1181">@ref hydrology_ref_ch2_meteorology "scale factors"</span>
<span class="hs" data-box="0.4595,0.0798,0.8800,0.1011">@ref hydrology_ref_ch2_meteorology "Climate"</span>
<span class="hs" data-box="0.4655,0.1011,0.5310,0.1181">@ref engine_manual_sect_TEMPERATURE "temperature"</span>
<span class="hs" data-box="0.5355,0.1011,0.8525,0.1181">@ref engine_manual_sect_EVAPORATION "evaporation: constant · monthly · series · Hargreaves · file"</span>
<span class="hs" data-box="0.4655,0.1229,0.4950,0.1400">@ref engine_manual_sect_TEMPERATURE "wind"</span>
<span class="hs" data-box="0.4995,0.1229,0.6112,0.1400">@ref hydrology_ref_ch2_meteorology "humidity · dew point"</span>
<span class="hs" data-box="0.6157,0.1229,0.7274,0.1400">@ref hydrology_ref_ch2_meteorology "prescribed PET (API)"</span>
<span class="hs" data-box="0.0300,0.2160,0.2455,0.2372">@ref hydrology_ref_ch6_snowmelt "Snowmelt"</span>
<span class="hs" data-box="0.0360,0.2372,0.2606,0.2543">@ref hydrology_ref_ch6_snowmelt "degree-day · heat budget · areal depletion"</span>
<span class="hs" data-box="0.2545,0.2160,0.4700,0.2372">@ref hydrology_ref_ch3_surface_runoff "Runoff"</span>
<span class="hs" data-box="0.2605,0.2372,0.3670,0.2543">@ref hydrology_ref_ch3_surface_runoff "nonlinear reservoir"</span>
<span class="hs" data-box="0.2605,0.2591,0.3619,0.2762">@ref engine_manual_sect_SUBAREAS "depression storage"</span>
<span class="hs" data-box="0.2605,0.2809,0.4286,0.2980">@ref engine_manual_sect_SUBAREAS "routing to another subcatchment"</span>
<span class="hs" data-box="0.0300,0.2703,0.2455,0.2915">@ref hydrology_ref_ch4_infiltration "Infiltration"</span>
<span class="hs" data-box="0.0360,0.2915,0.0758,0.3086">@ref hydrology_ref_ch4_infiltration "Horton"</span>
<span class="hs" data-box="0.0803,0.2915,0.1663,0.3086">@ref hydrology_ref_ch4_infiltration "modified Horton"</span>
<span class="hs" data-box="0.1708,0.2915,0.2311,0.3086">@ref hydrology_ref_ch4_infiltration "Green-Ampt"</span>
<span class="hs" data-box="0.0360,0.3134,0.1425,0.3305">@ref hydrology_ref_ch4_infiltration "modified Green-Ampt"</span>
<span class="hs" data-box="0.1470,0.3134,0.2176,0.3305">@ref hydrology_ref_ch4_infiltration "curve number"</span>
<span class="hs" data-box="0.2545,0.3140,0.4700,0.3352">@ref quality_ref_ch6_lid_controls "LID controls"</span>
<span class="hs" data-box="0.2605,0.3352,0.3876,0.3523">@ref quality_ref_ch6_lid_controls "layered units (8 types)"</span>
<span class="hs" data-box="0.2605,0.3571,0.3722,0.3742">@ref quality_ref_ch6_lid_controls "LID as storage nodes"</span>
<span class="hs" data-box="0.3767,0.3571,0.4627,0.3742">@ref quality_ref_ch6_lid_controls "detailed output"</span>
<span class="hs" data-box="0.0300,0.3464,0.2455,0.3677">@ref quality_ref_ch3_pollutant_buildup "Surface quality"</span>
<span class="hs" data-box="0.0360,0.3677,0.1939,0.3848">@ref quality_ref_ch3_pollutant_buildup "buildup · washoff by land use"</span>
<span class="hs" data-box="0.0360,0.3895,0.1220,0.4066">@ref engine_manual_sect_LANDUSES "street sweeping"</span>
<span class="hs" data-box="0.1265,0.3895,0.1920,0.4066">@ref quality_ref_ch10_mesh_quality "MSX species"</span>
<span class="hs" data-box="0.5300,0.2160,0.7005,0.2372">@ref hydrology_ref_ch8_mesh_surface "Rain on the mesh"</span>
<span class="hs" data-box="0.5360,0.2372,0.6323,0.2543">@ref hydrology_ref_ch8_mesh_surface "natural neighbour"</span>
<span class="hs" data-box="0.6368,0.2372,0.6766,0.2543">@ref engine_manual_sect_2D_OPTIONS "system"</span>
<span class="hs" data-box="0.7095,0.2160,0.8800,0.2372">@ref hydrology_ref_ch8_mesh_surface "Per-cell infiltration"</span>
<span class="hs" data-box="0.7155,0.2372,0.9042,0.2543">@ref hydrology_ref_ch8_mesh_surface "Horton · Green-Ampt · CN · constant"</span>
<span class="hs" data-box="0.7155,0.2591,0.9504,0.2762">@ref engine_manual_sect_2D_INFILTRATION_OPTIONS "→ lost · subcatchment aquifer · mesh aquifer"</span>
<span class="hs" data-box="0.5300,0.2703,0.7005,0.2915">@ref hydraulics_ref_ch9_two_dimensional "Overland flow"</span>
<span class="hs" data-box="0.5360,0.2915,0.6169,0.3086">@ref hydraulics_ref_ch9_two_dimensional "local inertial"</span>
<span class="hs" data-box="0.5360,0.3134,0.6374,0.3305">@ref hydraulics_ref_ch9_two_dimensional "full shallow water"</span>
<span class="hs" data-box="0.5360,0.3352,0.6169,0.3523">@ref hydraulics_ref_ch9_two_dimensional "diffusive wave"</span>
<span class="hs" data-box="0.6214,0.3352,0.6920,0.3523">@ref hydraulics_ref_ch10_planned "IMEX / CVODE"</span>
<span class="hs" data-box="0.7095,0.2921,0.8800,0.3134">@ref hydraulics_ref_ch9_two_dimensional "Mesh and closure"</span>
<span class="hs" data-box="0.7155,0.3134,0.7707,0.3305">@ref hydraulics_ref_ch9_two_dimensional "triangles"</span>
<span class="hs" data-box="0.7752,0.3134,0.8099,0.3305">@ref hydraulics_ref_ch9_two_dimensional "quads"</span>
<span class="hs" data-box="0.7155,0.3352,0.7758,0.3523">@ref hydraulics_ref_ch9_two_dimensional "flat cells"</span>
<span class="hs" data-box="0.7803,0.3352,0.8458,0.3523">@ref hydraulics_ref_ch9_two_dimensional "VFR closure"</span>
<span class="hs" data-box="0.5300,0.3683,0.7005,0.3895">@ref quality_ref_ch10_mesh_quality "Surface quality · transport"</span>
<span class="hs" data-box="0.5360,0.3895,0.6939,0.4066">@ref quality_ref_ch10_mesh_quality "coverages · buildup · washoff"</span>
<span class="hs" data-box="0.5360,0.4114,0.6425,0.4285">@ref quality_ref_ch10_mesh_quality "species on the mesh"</span>
<span class="hs" data-box="0.7095,0.3683,0.8800,0.3895">@ref hydraulics_ref_ch9_two_dimensional "Backends"</span>
<span class="hs" data-box="0.7155,0.3895,0.7399,0.4066">@ref hydraulics_ref_ch9_two_dimensional "CPU"</span>
<span class="hs" data-box="0.7444,0.3895,0.7842,0.4066">@ref hydraulics_ref_ch9_two_dimensional "OpenMP"</span>
<span class="hs" data-box="0.7155,0.4114,0.8118,0.4285">@ref hydraulics_ref_ch9_two_dimensional "CUDA · HIP · SYCL"</span>
<span class="hs" data-box="0.8163,0.4114,0.8509,0.4285">@ref hydraulics_ref_ch10_planned "Metal"</span>
<span class="hs" data-box="0.0300,0.5032,0.2455,0.5245">@ref hydrology_ref_ch5_groundwater "Two-zone aquifer"</span>
<span class="hs" data-box="0.0360,0.5245,0.1887,0.5415">@ref hydrology_ref_ch5_groundwater "unsaturated + saturated zone"</span>
<span class="hs" data-box="0.0360,0.5463,0.2247,0.5634">@ref engine_manual_sect_AQUIFERS "percolation · deep percolation · ET"</span>
<span class="hs" data-box="0.0360,0.5682,0.1939,0.5852">@ref engine_manual_sect_GWF "lateral interflow, user [GWF]"</span>
<span class="hs" data-box="0.2545,0.5032,0.4700,0.5245">@ref hydrology_ref_ch9_mesh_groundwater "Groundwater transport"</span>
<span class="hs" data-box="0.2605,0.5245,0.4081,0.5415">@ref hydrology_ref_ch9_mesh_groundwater "advection–dispersion · heat"</span>
<span class="hs" data-box="0.5300,0.5032,0.8800,0.5245">@ref hydrology_ref_ch9_mesh_groundwater "Two-layer aquifer"</span>
<span class="hs" data-box="0.5360,0.5245,0.5809,0.5415">@ref hydrology_ref_ch9_mesh_groundwater "Gardner"</span>
<span class="hs" data-box="0.5854,0.5245,0.6201,0.5415">@ref hydrology_ref_ch9_mesh_groundwater "Russo"</span>
<span class="hs" data-box="0.6246,0.5245,0.6952,0.5415">@ref hydrology_ref_ch9_mesh_groundwater "Brooks–Corey"</span>
<span class="hs" data-box="0.6997,0.5245,0.7754,0.5415">@ref hydrology_ref_ch9_mesh_groundwater "van Genuchten"</span>
<span class="hs" data-box="0.5360,0.5463,0.7144,0.5634">@ref hydrology_ref_ch9_mesh_groundwater "closed form · enslaved · σ column"</span>
<span class="hs" data-box="0.7189,0.5463,0.8152,0.5634">@ref hydrology_ref_ch9_mesh_groundwater "Dunne return flow"</span>
<span class="hs" data-box="0.5360,0.5682,0.6323,0.5852">@ref hydrology_ref_ch9_mesh_groundwater "node–bed exchange"</span>
<span class="hs" data-box="0.6368,0.5682,0.7895,0.5852">@ref hydrology_ref_ch9_mesh_groundwater "capillary rise · boundary ET"</span>
<span class="hs" data-box="0.5360,0.5900,0.6528,0.6071">@ref hydrology_ref_ch9_mesh_groundwater "per-subcatchment mode"</span>
<span class="hs" data-box="0.0300,0.6787,0.3073,0.7000">@ref engine_manual_sect_JUNCTIONS "Nodes"</span>
<span class="hs" data-box="0.0360,0.7000,0.2401,0.7171">@ref engine_manual_sect_JUNCTIONS "junction · storage · divider · outfall"</span>
<span class="hs" data-box="0.0360,0.7219,0.1271,0.7389">@ref hydraulics_ref_ch3_dynamic_wave "virtual junction"</span>
<span class="hs" data-box="0.1316,0.7219,0.2125,0.7389">@ref hydraulics_ref_ch7_advanced_features "inlet junction"</span>
<span class="hs" data-box="0.3163,0.6787,0.5937,0.7000">@ref engine_manual_sect_CONDUITS "Links"</span>
<span class="hs" data-box="0.3223,0.7000,0.4391,0.7171">@ref hydraulics_ref_ch5_cross_section "conduits, 26 sections"</span>
<span class="hs" data-box="0.4436,0.7000,0.5245,0.7171">@ref hydraulics_ref_ch5_cross_section "street · dummy"</span>
<span class="hs" data-box="0.3223,0.7219,0.4289,0.7389">@ref hydraulics_ref_ch10_planned "Chebyshev irregular"</span>
<span class="hs" data-box="0.3223,0.7437,0.5623,0.7608">@ref engine_manual_sect_PUMPS "pumps · orifices · weirs · outlets · culverts"</span>
<span class="hs" data-box="0.3223,0.7656,0.4032,0.7826">@ref hydraulics_ref_ch5_cross_section "storage shapes"</span>
<span class="hs" data-box="0.4077,0.7656,0.4834,0.7826">@ref hydraulics_ref_ch7_advanced_features "HEC-22 inlets"</span>
<span class="hs" data-box="0.6027,0.6787,0.8800,0.7000">@ref hydraulics_ref_ch2_hydraulic_model "Flow routing"</span>
<span class="hs" data-box="0.6087,0.7000,0.6485,0.7171">@ref hydraulics_ref_ch2_hydraulic_model "steady"</span>
<span class="hs" data-box="0.6530,0.7000,0.7338,0.7171">@ref hydraulics_ref_ch4_kinematic_wave "kinematic wave"</span>
<span class="hs" data-box="0.7383,0.7000,0.8089,0.7171">@ref hydraulics_ref_ch3_dynamic_wave "dynamic wave"</span>
<span class="hs" data-box="0.6087,0.7219,0.6844,0.7389">@ref hydraulics_ref_ch8_finite_volume "finite volume"</span>
<span class="hs" data-box="0.6889,0.7219,0.7954,0.7389">@ref hydraulics_ref_ch8_lts "local time stepping"</span>
<span class="hs" data-box="0.6087,0.7437,0.6895,0.7608">@ref hydraulics_ref_ch10_planned "1D GPU backend"</span>
<span class="hs" data-box="0.0300,0.7549,0.3073,0.7762">@ref hydraulics_ref_ch3_dynamic_wave "Pressurisation"</span>
<span class="hs" data-box="0.0360,0.7762,0.0758,0.7932">@ref hydraulics_ref_ch3_dynamic_wave "EXTRAN"</span>
<span class="hs" data-box="0.0803,0.7762,0.1098,0.7932">@ref hydraulics_ref_ch3_dynamic_wave "slot"</span>
<span class="hs" data-box="0.1143,0.7762,0.1849,0.7932">@ref hydraulics_ref_ch3_dynamic_wave "dynamic slot"</span>
<span class="hs" data-box="0.1894,0.7762,0.2138,0.7932">@ref hydraulics_ref_ch3_dynamic_wave "TPA"</span>
<span class="hs" data-box="0.2183,0.7762,0.2684,0.7932">@ref hydraulics_ref_ch8_finite_volume "FV: slot"</span>
<span class="hs" data-box="0.0360,0.7980,0.0809,0.8151">@ref hydraulics_ref_ch8_finite_volume "FV: TPA"</span>
<span class="hs" data-box="0.0854,0.7980,0.1817,0.8151">@ref hydraulics_ref_ch8_finite_volume "FV: implicit head"</span>
<span class="hs" data-box="0.1862,0.7980,0.2825,0.8151">@ref hydraulics_ref_ch3_dynamic_wave "unsteady friction"</span>
<span class="hs" data-box="0.3163,0.7986,0.5937,0.8199">@ref hydraulics_ref_ch3_dynamic_wave "Node solution"</span>
<span class="hs" data-box="0.3223,0.8199,0.4289,0.8369">@ref hydraulics_ref_ch3_dynamic_wave "explicit continuity"</span>
<span class="hs" data-box="0.4334,0.8199,0.5656,0.8369">@ref hydraulics_ref_ch3_dynamic_wave "semi-implicit continuity"</span>
<span class="hs" data-box="0.3223,0.8417,0.4391,0.8588">@ref hydraulics_ref_ch3_anderson "Anderson acceleration"</span>
<span class="hs" data-box="0.4436,0.8417,0.5245,0.8588">@ref hydraulics_ref_ch10_planned "FV_NODE_* keys"</span>
<span class="hs" data-box="0.6027,0.7767,0.8800,0.7980">@ref quality_ref_ch5_transport_treatment "Transport"</span>
<span class="hs" data-box="0.6087,0.7980,0.6382,0.8151">@ref quality_ref_ch5_transport_treatment "CSTR"</span>
<span class="hs" data-box="0.6427,0.7980,0.7133,0.8151">@ref quality_ref_ch7_ard_transport "Eulerian ARD"</span>
<span class="hs" data-box="0.7178,0.7980,0.8038,0.8151">@ref quality_ref_ch7_ard_transport "Lagrangian LARD"</span>
<span class="hs" data-box="0.6087,0.8199,0.7306,0.8369">@ref quality_ref_ch7_ard_transport "random-walk dispersion"</span>
<span class="hs" data-box="0.7351,0.8199,0.8314,0.8369">@ref quality_ref_ch5_transport_treatment "first-order decay"</span>
<span class="hs" data-box="0.6087,0.8417,0.6844,0.8588">@ref quality_ref_ch8_msx_reactions "MSX reactions"</span>
<span class="hs" data-box="0.6889,0.8417,0.7441,0.8588">@ref quality_ref_ch9_age_heat "water age"</span>
<span class="hs" data-box="0.7486,0.8417,0.7781,0.8588">@ref quality_ref_ch9_age_heat "heat"</span>
<span class="hs" data-box="0.7826,0.8417,0.8327,0.8588">@ref quality_ref_ch11_planned "sediment"</span>
<span class="hs" data-box="0.6087,0.8636,0.7152,0.8806">@ref engine_manual_sect_TREATMENT "treatment functions"</span>
<span class="hs" data-box="0.0300,0.8310,0.3073,0.8523">@ref engine_manual_sect_INFLOWS "Inflows"</span>
<span class="hs" data-box="0.0360,0.8523,0.1271,0.8694">@ref engine_manual_sect_DWF "dry weather flow"</span>
<span class="hs" data-box="0.1316,0.8523,0.1868,0.8694">@ref hydrology_ref_ch7_rdii "RDII: RTK"</span>
<span class="hs" data-box="0.1913,0.8523,0.2722,0.8694">@ref hydrology_ref_ch7_rdii "RDII: IA decay"</span>
<span class="hs" data-box="0.0360,0.8742,0.1579,0.8912">@ref engine_manual_sect_INFLOWS "user · interface files"</span>
<span class="hs" data-box="0.1624,0.8742,0.2382,0.8912">@ref engine_manual_sect_CONTROLS "control rules"</span>
</div>

**Flow paths the network does not contain.** Overland routes — a road
acting as a channel, flow over an embankment, a flow path between two
otherwise unconnected catchments — exist in the terrain and nowhere in
the pipe topology.

**A flood extent.** The depth field over a real surface is the quantity
flood mapping needs. A ponded volume at a node is not one.

Several limitations should be noted. The method does not replace hydrology: unless
`RAINFALL_MODE` says otherwise the mesh receives the same rainfall the
subcatchments do, and both would deliver it (§9.8). It does not
infiltrate — the mesh has no soil column, and water on it leaves only by
flowing away, evaporating, or entering the network (§9.12). And its
momentum equation is an approximation of the full shallow-water
system: §9.2 states what is dropped and §9.10 measures what that
costs.

## 9.2 Governing equations

Depth-averaged mass conservation over a surface of bed elevation
\f$z(x,y)\f$ and free surface \f$\eta = z + h\f$, with unit-width discharge
\f$\mathbf{q} = h\mathbf{u}\f$ (m²/s):

| | | | |
|---|---|---|---|
| \f[\frac{\partial h}{\partial t} + \nabla \cdot \mathbf{q} = i - e + s\f] | Continuity | (9-1) | |

where \f$i\f$ is rainfall intensity, \f$e\f$ the evaporation rate and \f$s\f$ the
exchange with the 1D network, all as velocities normal to the surface.

Three momentum closures supply \f$\mathbf{q}\f$ for the same continuity
equation, selected by `[2D_OPTIONS] MOMENTUM_EQUATION`. They share one
time-stepping engine — the tiered local time stepping, the flux-active
set, the positivity share, the network coupling and the transport of
§9.5–§9.7 — and differ only in the face law that supplies the discharge
across each interior face and each boundary edge. The default,
`LOCAL_INERTIAL`, reproduces the marcher as it stood before September 2026
bit for bit; the other two are dispatched by one branch per face firing
and otherwise reuse every array, ledger and rule of §9.5.

### 9.2.1 The local inertial closure

The default momentum equation is the **local-inertial** (or
inertial-wave) approximation of Bates et al. (2010):

| | | | |
|---|---|---|---|
| \f[\frac{\partial \mathbf{q}}{\partial t} + g\,h\,\nabla\eta + \frac{g\,n^{2}\,\lvert\mathbf{q}\rvert\,\mathbf{q}}{h^{7/3}} = 0\f] | Momentum | (9-2) | |

with \f$g = 9.80665\f$ m/s² and \f$n\f$ Manning's roughness. Compared with the
full shallow-water momentum equation, the **convective acceleration term
\f$\nabla\cdot(\mathbf{q}\mathbf{q}/h)\f$ is dropped**. Everything else —
local acceleration, the pressure gradient written as a free-surface
slope, and bed friction — is retained.

This is the single most consequential modelling decision in the chapter,
and it is a deliberate one. Retaining \f$\partial\mathbf{q}/\partial t\f$ is
what separates (9-2) from the diffusive wave: the diffusive wave has no
inertia at all, so a flood front on a flat surface propagates at a speed
set by the numerical step rather than by the physics, and the scheme
becomes stiff exactly where floods are shallow. Dropping the convective
term is what separates (9-2) from the full system, and it costs the
Bernoulli terms: the scheme cannot represent a drawdown over a crest, a
stable hydraulic jump position, or a fully supercritical profile. §9.10
shows both effects measured against closed-form solutions.

The practical justification is that urban flood flows are dominated by
gravity and friction. Bates et al. (2010) and de Almeida and Bates
(2013) delimit the regime: the local-inertial approximation is accurate
for subcritical flows over gentle slopes at Froude numbers below about
0.5, and degrades progressively as \f$Fr \to 1\f$. Flow that is
persistently supercritical, or where the momentum flux through a
contraction sets the answer, is outside it.

For steep faces the model would accelerate without bound, since nothing
in (9-2) limits the velocity a slope can generate. A Froude clamp
supplies that limit numerically (§9.5.1). The prognostic state is the
cell volume \f$V\f$ and one face-normal discharge \f$q_f\f$ per interior face;
the face law is (9-7)–(9-10).

### 9.2.2 The full shallow-water closure (`FULL_SWE`)

`MOMENTUM_EQUATION FULL_SWE` restores the convective term and solves the
shallow-water momentum equation in conservation form,

| | | | |
|---|---|---|---|
| \f[\frac{\partial \mathbf{q}}{\partial t} + \nabla\cdot\left( \frac{\mathbf{q}\,\mathbf{q}}{h} + \tfrac{1}{2}\,g\,h^{2}\,\mathbf{I} \right) = -\,g\,h\,\nabla z - \frac{g\,n^{2}\,\lvert\mathbf{q}\rvert\,\mathbf{q}}{h^{7/3}}\f] | Momentum, conservative form | (9-31) | |

which is (9-2) with the convective flux \f$\mathbf{q}\mathbf{q}/h\f$ kept and
the pressure gradient \f$g h \nabla\eta\f$ split into the hydrostatic
pressure flux \f$\nabla(\tfrac{1}{2} g h^{2})\f$ and the bed-slope source
\f$-g h \nabla z\f$. The prognostic state changes with the equation: each
cell carries its volume and a **cell momentum** \f$(hu, hv)\f$, and faces no
longer hold a prognostic discharge — they evaluate a Godunov flux from the
two adjacent cell states, after a hydrostatic reconstruction of the two
depths against the higher of the two beds (Audusse et al., 2004), with a
rotated HLLC Riemann solver (Toro, 2001) and a per-face bed-slope
correction that cancels the pressure flux exactly at rest. Manning
friction is applied per cell after the flux update, in the same
semi-implicit division the local-inertial face law uses, so it can only
shrink \f$\lvert\mathbf{q}\rvert\f$ and never reverse it (the stopping
condition of Liang and Marche, 2009, is satisfied by construction).
§9.5.10 gives the face flux step by step.

What the term buys is every phenomenon §9.2.1 said the default cannot
represent: transcritical control over a crest, a hydraulic jump held at
its Rankine–Hugoniot position, dam-break fronts at the right speed, the
Bernoulli drawdown of §9.10. The closure is valid at every Froude number
and needs no Froude clamp; `FROUDE_MAX` is not consulted.

What it costs is a smaller step and more work per face. First-order
Godunov with hydrostatic reconstruction is positivity-preserving at a
Courant number of one half on triangles and quadrilaterals, so
`CFL_NUMBER` is reduced to 0.5 with a notice when set higher, and the
Courant length is the isoperimetric \f$2A/P\f$ rather than the
operator-derived \f$L_{char}\f$ of (9-4); the \f$\beta\f$ share of (9-15)
remains as a backstop rather than the guarantee. Measured on the SWASHES
strips of §9.10 the closure runs at roughly two to three times the
wall-clock of the default. Two further switches belong to it:
`RECONSTRUCTION_ORDER 2` adds MUSCL reconstruction of \f$(\eta, u, v)\f$ with
a Barth–Jespersen-limited Green–Gauss gradient and SSP-RK2 time stepping
in global-step mode (§9.5.6, §9.5.10), and is refused with a notice when
overland transport is active; `FRONT_REBUILD` (§9.5.7) is switched on
automatically, because a Godunov front outruns the fixed rebuild cadence.
The Kokkos plugin backends serve only the all-triangle local-inertial
scheme; a `FULL_SWE` model is announced and run on the CPU marcher
(§9.11.1).

`FULL_SWE` keeps every property of §9.5: the lake at rest is exact —
the Audusse correction cancels the \f$\tfrac{1}{2} g h^{*2}\f$ pressure flux
face by face, including at walls, which contribute a mirror-state
Riemann flux — a dry higher neighbour is a wall, \f$\sum V\f$ closes to
round-off across tiers, and species ride the mass flux upwinded on the
contact wave. Measured on the SWASHES strips of §9.10 (relative
\f$L^{1}\f$ depth error, local-inertial in parentheses): Stoker wet dam break
0.7 % (6.5 %), 0.3 % at second order; Ritter dry dam break 1.7 % (13 %),
1.1 % at second order; subcritical bump 0.5 % (6.8 %); transcritical bump
4.4 % (27 %); bump with shock 7.3 % (12 %). The transcritical and shock
residuals are the 25-cell crest resolution of those decks — the 1D
finite-volume solver of @ref hydraulics_ref_ch8_finite_volume "Chapter 8"
sits at 7.5 % and 2.5 % on the same cases.

### 9.2.3 The diffusive-wave closure

`MOMENTUM_EQUATION DIFFUSIVE_WAVE` drops both accelerations, so the
momentum equation reduces to the quasi-steady Manning balance of the
free-surface slope (Hunter et al., 2005; the explicit diffusive wave of
LISFLOOD-FP):

| | | | |
|---|---|---|---|
| \f[\mathbf{q} = -\,\frac{h^{5/3}}{n}\,\frac{\nabla\eta}{\sqrt{\lvert\nabla\eta\rvert}}\f] | Momentum, diffusive wave | (9-32) | |

The prognostic state is the cell volume alone; a face discharge is
recomputed from the two adjacent surfaces at every firing and carries no
memory. Substituting (9-32) into (9-1) gives a nonlinear diffusion
equation, \f$\partial h/\partial t = \nabla\cdot(K\nabla\eta)\f$ with
\f$K = h^{5/3}/(n\sqrt{\lvert\nabla\eta\rvert})\f$, and that is both the
closure's merit and its cost. Steady uniform flow is exact, slow
floodplain inundation is well represented, and there is nothing to damp:
no \f$\theta\f$ blend and no Froude clamp are applied. But an explicit
integration of a diffusion equation is bounded by \f$\Delta t \le L^{2}/(4K)\f$
— the \f$\Delta x^{2}\f$ restriction that the local-inertial term of §9.2.1
exists to remove — and the conductance \f$K\f$ diverges on a flat surface,
the classic explicit diffusive-wave failure. §9.5.11 gives the face law
with its slope floor and the per-cell step bound. The closure is
affordable here only because the tiered local time stepping of §9.5.6
lets each cell pay its own step.

Table 9-2 summarizes the three closures and the retired spelling that
preceded `FULL_SWE`.

| Closure | Prognostic state | Face law | Valid regime | Cost | Status |
|---|---|---|---|---|---|
| `LOCAL_INERTIAL` (default) | \f$V\f$ per cell, \f$q\f$ per face | de Almeida and Bates, (9-7)–(9-10) | \f$Fr \lesssim 0.5\f$: ponds, streets, floodplains | 1× | \status{Implemented} |
| `FULL_SWE` | \f$V\f$, \f$hu\f$, \f$hv\f$ per cell | hydrostatic reconstruction (9-36), rotated HLLC (9-38), bed-slope correction (9-39), semi-implicit friction per cell (9-40) | all Froude numbers: transcritical control, jumps, dam breaks, drawdown over crests | ≈ 2–3× (Courant ≤ ½ on \f$2A/P\f$, (9-41)); CPU marcher only | \status{Implemented} |
| `DIFFUSIVE_WAVE` | \f$V\f$ per cell | Manning quasi-steady balance (9-42) with a slope floor | slow floodplain inundation; steady uniform flow (exact) | \f$\Delta x^{2}\f$-bound steps (9-43), carried by the LTS tiers; CPU marcher only | \status{Implemented} |
| `ADVECTION YES` | as `LOCAL_INERTIAL` | (9-7) plus a staggered upwind convective term (Stelling and Duinmeijer, 2003) | — | — | \status{Retired} |

*Table 9-2 Momentum closures of the 2D marcher*

`ADVECTION YES` is the deprecated (2026-09-06) spelling of the
convective-term experiment that `FULL_SWE` superseded. A deck that still
carries it is accepted with a warning naming the replacement and keeps
the physics it asked for — the local-inertial law with the upwind
convective difference added to (9-7) on wet–wet faces — rather than being
remapped, so an old calibrated model does not change its answer silently.
New models should not use it.

<!-- FIGURE: hydraulics_ch9_closure_compare — the same SWASHES subcritical-bump strip under the three closures: free surface over the crest (flat under LOCAL_INERTIAL, the Bernoulli dip under FULL_SWE, flat under DIFFUSIVE_WAVE) against the analytic profile; inset the per-closure substep count -->

<!-- source: src/engine/2d/input/SectionHandlers2D.cpp:183-194,230-246; src/engine/2d/data/SolverOptions2D.hpp:195-226; src/engine/2d/solver/ExplicitInertialSolver.cpp:82-134,878-888,911-1003; src/engine/2d/solver/SweKernels.hpp:17-47,171-229,299-309; src/engine/2d/solver/DiffusiveKernels.hpp:17-39,60-81; src/engine/2d/solver/InertialKernels.hpp:268-299; src/engine/2d/solver/SurfaceSolverFactory.cpp:286-301; docs/manuals/reference/hydraulics/sections/Chapter9-TwoDimensional.md:1382-1419 (former §9.11a) -->

## 9.3 The computational mesh

Unlike the internal discretization of
@ref hydraulics_ref_ch8_finite_volume "Chapter 8", the mesh is an
explicit part of the model. Its cells appear in results, carry per-cell parameters,
and are addressable by the API.

Cells are triangles, listed in `[2D_TRIANGLES]` by three vertex indices
from `[2D_VERTICES]`, with Manning's \f$n\f$ and an optional initial depth
per cell:

```
[2D_VERTICES]
;;X          Y          Z        [TAG]

[2D_TRIANGLES]
;;V1  V2  V3  MANNINGS_N  [INIT_DEPTH]  [TAG]
```

Bed elevation is carried at the **vertices**. A cell's centroid
elevation \f$z_c\f$ is the mean of its three vertex elevations, so the cell
bed is the plane through them — a fact the closure of §9.4 uses
directly. Nothing in the solver reads a cell-constant bed.

Edge–neighbour adjacency is built by hashing each triangle's three
sorted vertex pairs: an edge claimed twice is interior and the two
triangles become neighbours; an edge claimed once is a domain boundary
and carries a boundary condition (§9.6). The convention throughout is
that **local edge \f$e\f$ is opposite vertex \f$e\f$**, so its endpoints are the
triangle's other two vertices. Both incident cells of an interior edge
therefore compute the same endpoint elevations, which is what makes the
face depth of §9.5.2 antisymmetric and the flux conservative.

Interior edges are then enumerated once each as **unique faces**, with
the left cell \f$L\f$, the right cell \f$R\f$, the edge length \f$\xi\f$, the
outward normal of \f$L\f$'s slot (which points \f$L \to R\f$), the edge midpoint,
and the face-normal centroid separation

| | | | |
|---|---|---|---|
| \f[d_{n} = \max\left( \lvert (\mathbf{x}_{R} - \mathbf{x}_{L})\cdot\hat{\mathbf{n}} \rvert,\ 0.3\,\lvert \mathbf{x}_{R} - \mathbf{x}_{L}\rvert \right)\f] | | (9-3) | |

The floor keeps a near-degenerate sliver — where the centroid chord is
nearly parallel to the shared edge — from producing an unbounded
surface slope. Face roughness is the mean of the two cells' \f$n\f$.

Each cell also carries a characteristic length derived from the discrete
operator rather than from geometry, for the time-step bound of §9.5.5:

| | | | |
|---|---|---|---|
| \f[L_{char} = \sqrt{\frac{2A}{\sum_{f} \xi_{f}/d_{n,f}}}\f] | | (9-4) | |

A cell none of whose edges is an interior face has an empty sum in
(9-4) and keeps the altitude proxy \f$2A/\xi_{max}\f$ instead; such a cell
carries no interior flux until a neighbouring face opens, so the proxy
never governs a coupled update.

**Every quantity above is planimetric.** Areas, lengths and normals are
computed in plan rather than on the sloped surface. On terrain steep enough for
that distinction to matter the shallow-water equations are themselves
the wrong model, so this is a consistent rather than an incidental
choice.

Per-edge conveyance factors in \f$[0,1]\f$ may be attached with
`[2D_EDGE_CONVEYANCE]`, multiplying the flux across that edge — the
transmissivity \f$\psi\f$ of the integral-porosity shallow-water literature
(Sanders et al., 2008; Bruwier et al., 2017), used to represent
sub-grid obstructions such as walls and fences without meshing them.
Values are mirrored onto both slots of an interior edge, so the
restriction stays symmetric and the flux stays conservative.

Initial conditions come from `INIT_DEPTH` in `[2D_TRIANGLES]` (a depth
in mesh units, converted to a cell volume through the closure of §9.4)
and optionally `[2D_INITIAL_VELOCITY]`, which seeds face momentum from
the cell velocities. Without the latter a model can only start from
rest, which excludes solutions such as Thacker's oscillating basins
whose initial state has \f$\mathbf{u} \neq 0\f$.

### 9.3.1 Quadrilateral cells

The mesh may mix triangles with convex quadrilaterals. Quads are listed
in `[2D_QUADS]` after every triangle, by four vertex indices in cyclic
order (either orientation — the builder orients every edge normal
outward from the centroid), with the same roughness, initial-depth and
tag columns as `[2D_TRIANGLES]`:

```
[2D_QUADS]
;;V1  V2  V3  V4  MANNINGS_N  [INIT_DEPTH]  [TAG]
```

**Cells are numbered triangles first, then quads**, each in file order:
the \f$j\f$-th quad row is cell \f$n_{tri} + j\f$. Every cell-addressed
section — `[2D_BOUNDARY_CONDITIONS]`, `[2D_BOUNDARY_QUALITY]`,
`[2D_INITIAL_VELOCITY]`, `[2D_TRIANGLE_NODE_MAP]`, `[2D_INFILTRATION]` —
uses this unified index, and their column headers keep the word `TRI`. A
`[2D_TRIANGLES]` row appearing after a `[2D_QUADS]` row is a parse error,
which is what keeps the numbering unambiguous. A non-convex or
self-intersecting quad fails mesh validation with the cell index and
vertex list; the GUI mesher must never emit one.

Every per-cell and per-edge array is padded to four slots per cell
(`kMaxCellVerts`), so slot \f$4t + k\f$ addresses local edge \f$k\f$ of cell
\f$t\f$ whatever its vertex count \f$n_v\f$; a padding slot has zero geometry
and a neighbour index of \f$-2\f$, so a stray read is inert. **Local edge
\f$k\f$ has endpoints \f$v_{(k+1) \bmod n_v}\f$ and \f$v_{(k+2) \bmod n_v}\f$.** For
a triangle this is exactly the opposite-vertex rule of §9.3, so every
existing `TRI EDGE` row keeps its meaning; for a quad it is a fixed
rotation of the natural numbering — edge 0 is \f$(v_2, v_3)\f$, edge 1
\f$(v_3, v_4)\f$, edge 2 \f$(v_4, v_1)\f$ and edge 3 \f$(v_1, v_2)\f$ — and the
`EDGE` column of a boundary row ranges 0..3. The adjacency hash of §9.3
is unchanged: an edge claimed twice is interior whichever shapes claim
it, and a face between a triangle and a quad is an ordinary face. All
of the face kernels — the three closures of §9.2, the positivity share,
the transport — see only \f$(h_f, S, n_f, \xi, 1/d_n)\f$ and carry no
shape branch.

Three quantities need care on a quad. Its **area** is the shoelace
area, computed as the sum of the two sub-triangles \f$(v_0, v_1, v_2)\f$ and
\f$(v_0, v_2, v_3)\f$. Its **centroid** is the area-weighted centroid of
those two sub-triangles — a property of the polygon, independent of
which diagonal is used — and not the vertex mean, which is not the
centroid of a skewed quad and would bias the Perot arms of (9-16) and
the ghost-cell distance of §9.6. Its **mean bed** \f$z_c\f$, used by the
`FLAT` closure, is the mean of the four vertex elevations. The
centroid-to-edge distance that a triangle's stage boundary wrote as
\f$2A/(3L)\f$ is generalized to the normal distance from the centroid to
the edge midpoint,

| | | | |
|---|---|---|---|
| \f[d_{e} = (\mathbf{m}_{e} - \mathbf{x}_{c})\cdot\hat{\mathbf{n}}_{e}\f] | | (9-33) | |

which equals \f$2A/(3L)\f$ for a triangle (that expression *is* the
centroid-to-edge distance) and is exact for a quad; the triangle
expression is retained verbatim in the solver so an all-triangle mesh
stays bit-identical. Two other constants scale with the vertex count:
the positivity share of (9-15) becomes \f$\beta/n_v\f$ per exporting face,
since a cell's at most \f$n_v\f$ outgoing faces may take at most \f$\beta V\f$
in total, and a quad none of whose edges is an interior face keeps
\f$L_{char} = 2\min_k d_{e,k}\f$ instead of the altitude proxy (which is
\f$\Delta x\f$ for a square and conservative for a skewed cell). The
isoperimetric length \f$2A/P\f$ that bounds the `FULL_SWE` step (§9.5.10)
is computed over all \f$n_v\f$ edges.

Under `CELL_CLOSURE VFR` a quad uses the two-plane storage model of
§9.4.2. Results of a mixed mesh are written as UGRID mixed topology —
`Mesh2_face_nodes [nFace, 4]` with `_FillValue = -1`, plus a
`Mesh2_face_nv` vertex-count array — and the edge datasets and the bulk
C-API edge arrays use stride 4; an all-triangle mesh keeps the
`[nFace, 3]`, stride-3 layout byte for byte. The Kokkos plugin backends
serve all-triangle meshes only; a mixed mesh is announced and run on the
CPU marcher (§9.11.1).

<!-- figure spec: a quad beside a triangle sharing one face: local edge numbering k = 0..3 with endpoints v[(k+1)%nv], v[(k+2)%nv]; the padded slot layout 4t + k with the unused fourth slot of the triangle greyed; the centroid-to-edge normal distance d_e of (9-33) drawn for both cells -->
![Figure 9-4](figures/png/hydraulics_ch9_mesh_stencil.png)

*Figure 9-4 A quadrilateral and a triangle sharing a face: local edge numbering, the padded slot layout and the centroid-to-edge distance*

<!-- source: plans/2D_TRI_QUAD_MESH_PLAN_2026-09-06.md §2.1,§2.3,§2.4,§2.5; plans/2D_INPUT_FORMAT_SPEC.md:140-152; src/engine/2d/mesh/MeshBuilder.cpp:21-27,82-115,117-176,179-233; src/engine/2d/solver/InertialEdges.cpp:112-147; src/engine/2d/solver/ExplicitInertialSolver.cpp:916-921,1552-1560; src/engine/2d/input/SectionHandlers2D.cpp:870-877; src/engine/2d/solver/SurfaceSolverFactory.cpp:286-301; docs/manuals/reference/hydraulics/sections/Chapter9-TwoDimensional.md:1421-1441 (former §9.11b) -->

## 9.4 Volume–free-surface closure

The conserved variable is cell **volume** \f$V\f$ rather than depth. Depth follows
as the cell-mean \f$\bar{h} = V/A\f$. The closure is the relation that
recovers a free-surface elevation \f$\eta\f$ from that volume, and it is
where a triangular mesh with sloping cell beds differs sharply from a
raster of flat cells.

`CELL_CLOSURE FLAT` (the default) uses

| | | | |
|---|---|---|---|
| \f[\eta = z_{c} + \bar{h}\f] | | (9-5) | |

which is exact for a fully wetted cell and wrong for a partially wet
one. On a cell whose bed spans a slope or a step, spreading the water
uniformly over the whole cell raises the computed surface above the true
waterline — by up to two-thirds of the cell's relief. That error is a
head, and a head drives flux: the classic symptom is thin films creeping
*uphill* at a shoreline, and a lake at rest that is not a steady state
where it meets the bank.

`CELL_CLOSURE VFR` uses the exact stage–storage relation of the plane
bed through the cell's three vertex elevations — the volume/free-surface
relationships of Begnudelli and Sanders (2006, 2007). With sorted
elevations \f$z_1 \le z_2 \le z_3\f$ and \f$\bar{z} = (z_1+z_2+z_3)/3\f$:

| | | | |
|---|---|---|---|
| \f[\bar{h}(\eta) = \frac{(\eta - z_{1})^{3}}{3(z_{2}-z_{1})(z_{3}-z_{1})}\f] | \f$z_{1} < \eta \le z_{2}\f$ | (9-6a) | |
| \f[\bar{h}(\eta) = (\eta - \bar{z}) + \frac{(z_{3}-\eta)^{3}}{3(z_{3}-z_{1})(z_{3}-z_{2})}\f] | \f$z_{2} < \eta \le z_{3}\f$ | (9-6b) | |
| \f[\bar{h}(\eta) = \eta - \bar{z}\f] | \f$\eta \ge z_{3}\f$ | (9-6c) | |

The solver needs the inverse \f$\eta(\bar{h})\f$. On the lower branch it is
a closed-form cube root; on the upper branch a safeguarded Newton
iteration bracketed by \f$[z_2, z_3]\f$, which converges unconditionally
because \f$d\bar{h}/d\eta = A_{wet}/A > 0\f$ there. Note that (9-6c) is
identical to the flat closure (9-5), since \f$z_c = \bar{z}\f$ — the two
closures differ only on partially wet cells, which is precisely the
claim.

As a cell dries, \f$d\eta/dV = 1/A_{wet}\f$ diverges. `VFR_MIN_WET_FRAC`
(\f$\varepsilon\f$, default 0.01) bounds it by continuing the exact relation
below wetted fraction \f$\varepsilon\f$ along its tangent line, keeping the
closure \f$C^1\f$ and monotone with \f$d\eta/dV \le 1/(\varepsilon A)\f$. The
regularized forward and inverse relations are exact inverses of each
other for the same \f$\varepsilon\f$, so seeding a state from heads and
reading it back as volumes round-trips.

**VFR is correct and is not the default.** It restores the C-property at
shorelines and removes the uphill-creep artifact, but in doing so it
resolves a wetting and drying front that the flat closure freezes out —
measured at three to eight times more solver substeps. For deep urban
flooding, where partially wet cells are a thin fringe around a large
wetted area, the flat closure is both faster and adequate. For shallow
sheet flow on gentle slopes, where most cells are partially wet, VFR is
the right choice and the cost is the price of the answer. Pair it with
`FACE_RECONSTRUCTION VFR_FACE` (§9.5.2); the two halves address the same
artifact from the cell side and the face side.

### 9.4.1 The closure algorithm as implemented

The relations (9-6) are evaluated by a single header-only routine
shared by the serial marcher, the boundary path and the GPU kernels, so
every backend reconstructs the identical surface. Three auxiliary
quantities appear. The wetted-area fraction of the planar bed —
which is also \f$d\bar{h}/d\eta\f$ — with sorted elevations
\f$z_1 \le z_2 \le z_3\f$ and relief \f$R = z_3 - z_1\f$:

| | | | |
|---|---|---|---|
| \f[w(\eta) = \frac{(\eta - z_{1})^{2}}{(z_{2}-z_{1})\,R}\f] | \f$z_{1} < \eta \le z_{2}\f$ | (9-20a) | |
| \f[w(\eta) = 1 - \frac{(z_{3} - \eta)^{2}}{R\,(z_{3}-z_{2})}\f] | \f$z_{2} < \eta < z_{3}\f$ | (9-20b) | |

with \f$w = 0\f$ below \f$z_1\f$ and \f$w = 1\f$ above \f$z_3\f$. The stage at which
the wetted fraction equals the regularization floor \f$\varepsilon\f$:

| | | | |
|---|---|---|---|
| \f[\eta_{s} = z_{1} + \sqrt{\varepsilon\,(z_{2}-z_{1})\,R}\f] | \f$\varepsilon \le (z_{2}-z_{1})/R\f$ | (9-21a) | |
| \f[\eta_{s} = z_{3} - \sqrt{(1-\varepsilon)\,R\,(z_{3}-z_{2})}\f] | otherwise | (9-21b) | |

Below the switch depth \f$\bar{h}_s = \bar{h}(\eta_s)\f$ from (9-6) the
inverse continues along the tangent line,

| | | | |
|---|---|---|---|
| \f[\eta(\bar{h}) = \eta_{s} - \frac{\bar{h}_{s} - \bar{h}}{\varepsilon}\f] | \f$\bar{h} \le \bar{h}_{s}\f$ | (9-22) | |

and on the lower branch it is closed-form,

| | | | |
|---|---|---|---|
| \f[\eta = z_{1} + \left\lbrack 3\,\bar{h}\,(z_{2}-z_{1})\,R \right\rbrack^{1/3}\f] | \f$\bar{h} \le (z_{2}-z_{1})^{2}/(3R)\f$ | (9-23) | |

The inverse \f$\eta(V)\f$ is evaluated by the following case ladder, in
this order:

1. Compute \f$\bar{h} = \max(V, 0)/A\f$ and sort the three vertex
   elevations in place.
2. **Flat cell.** \f$R < 10^{-9}\f$ m: the flat closure (9-5) is exact —
   \f$\eta = \bar{z} + \max(\bar{h}, 0)\f$.
3. **Fully wet.** \f$\bar{h} \ge z_3 - \bar{z}\f$: \f$\eta = \bar{z} + \bar{h}\f$,
   again identical to (9-5). This case is tested before the
   \f$\varepsilon\f$-tail because it dominates in deep water and the two
   branches never overlap — the fully-wet threshold always exceeds
   \f$\bar{h}_s\f$.
4. **Regularized tail.** \f$\varepsilon > 0\f$ and \f$\bar{h} \le \bar{h}_s\f$:
   the tangent line (9-22). With \f$\varepsilon = 0\f$ — the exact
   relation, used by the rendering reconstruction — a dry cell returns
   \f$\eta = z_1\f$ instead.
5. **Lower branch.** \f$\bar{h} \le (z_2 - z_1)^2/(3R)\f$: the cube root
   (9-23). When \f$z_2 = z_1\f$ to rounding this bound is zero and the
   branch is never selected.
6. **Degenerate upper interval.** \f$z_3 - z_2 < 10^{-9}\f$ m:
   \f$\eta = \bar{z} + \bar{h}\f$, so a one-ulp sliver never reaches the
   divisions of the Newton branch.
7. **Upper branch.** Safeguarded Newton on the bracket \f$[z_2, z_3]\f$
   for (9-6b), starting from the flat-closure guess
   \f$\eta = \bar{z} + \bar{h}\f$, with derivative
   \f$d\bar{h}/d\eta = 1 - (z_3-\eta)^2/(R(z_3-z_2)) = A_{wet}/A > 0\f$.
   Any Newton step leaving the bracket is replaced by bisection;
   iteration stops at \f$\lvert\Delta\eta\rvert < 10^{-12}(1 + R)\f$, with
   a 64-iteration cap that is never reached in practice.

The forward relation \f$\bar{h}(\eta)\f$ applies the same regularization —
exact above \f$\eta_s\f$, the tangent below, floored at zero — so forward
and inverse are exact inverses for the same \f$\varepsilon\f$ and
head-seeded and volume-seeded states round-trip. A dry cell's head
under VFR is seeded at \f$\eta(0) = \eta_s - \bar{h}_s/\varepsilon \in (z_1, \eta_s)\f$, the value the closure itself returns at \f$V = 0\f$, so
that seeding the head back through the forward relation reproduces
exactly zero volume. Figure 9-3 (§9.5.9) sketches the wetting cases.

Implementation: the closure lives in `src/engine/2d/mesh/VfrClosure.hpp`
— @ref openswmm::twoD::vfrSort3, @ref openswmm::twoD::vfrWetFraction,
@ref openswmm::twoD::vfrStageAtWetFraction,
@ref openswmm::twoD::vfrMeanDepthFromEtaExact,
@ref openswmm::twoD::vfrMeanDepthFromEta,
@ref openswmm::twoD::vfrEtaFromMeanDepth and
@ref openswmm::twoD::vfrDryEta. The solver enters through
@ref openswmm::twoD::inertial::etaDepthScalar and
@ref openswmm::twoD::inertial::volumeFromEtaScalar in
`src/engine/2d/solver/InertialKernels.hpp`, whose `FLAT` branch is
(9-5); the flat-relief guard is `kVfrFlatRelief` (\f$10^{-9}\f$ m).

### 9.4.2 The two-plane VFR closure of a quadrilateral

Four vertices do not in general lie on a plane, so the planar-bed
relations (9-6) do not apply to a quad directly. Begnudelli and Sanders
(2007) budget the storage of a quadrilateral cell by splitting it along
one diagonal into two planar sub-triangles, and their piecewise cubic,
quadratic and linear stage–storage relations (their Eqs. 8–13, with the
coefficient tables of their appendix) are exactly the area-weighted sum
of the two planar-triangle closures. The diagonal is chosen from the
elevation ordering of the four vertices, \f$n_1 \le n_2 \le n_3 \le n_4\f$,
so that the split follows the fold of the surface:

| Case | Topology | Diagonal | Sub-triangles |
|---|---|---|---|
| 1 | \f$n_1\f$ and \f$n_4\f$ diagonally opposite | \f$n_1\f$–\f$n_4\f$ | \f$(n_1, n_2, n_4)\f$ and \f$(n_1, n_3, n_4)\f$ |
| 2 | \f$n_1\f$ and \f$n_4\f$ adjacent, \f$n_2\f$ adjacent to \f$n_1\f$ | \f$n_2\f$–\f$n_4\f$ | \f$(n_1, n_2, n_4)\f$ and \f$(n_2, n_3, n_4)\f$ |
| 3 | \f$n_1\f$ and \f$n_4\f$ adjacent, \f$n_2\f$ adjacent to \f$n_4\f$ | \f$n_3\f$–\f$n_4\f$ | \f$(n_1, n_3, n_4)\f$ and \f$(n_2, n_3, n_4)\f$ |

*Table 9-3 Diagonal selection of the two-plane quadrilateral closure
(Begnudelli and Sanders, 2007, Cases 1–3)*

Ties in elevation keep cyclic order (a stable sort), and any consistent
choice is a valid split. With sub-triangle areas \f$A_1, A_2\f$ and each
sub-triangle's sorted elevations, the cell-mean depth at stage \f$\eta\f$ is

| | | | |
|---|---|---|---|
| \f[\bar{h}(\eta) = \frac{A_{1}\,\bar{h}_{1}(\eta) + A_{2}\,\bar{h}_{2}(\eta)}{A_{1} + A_{2}}\f] | | (9-34) | |

where \f$\bar{h}_{1}, \bar{h}_{2}\f$ are the regularized planar-triangle
relations of §9.4.1 — (9-6) above each sub-triangle's switch stage, the
tangent (9-22) below it, floored at zero — evaluated with the same
\f$\varepsilon\f$. Quads and triangles therefore share one regularization
and one round-trip contract, and the closed-form appendix coefficients
are an optimization the engine does not need. The slope of the sum,
which the Newton inverse uses, is

| | | | |
|---|---|---|---|
| \f[\frac{d\bar{h}}{d\eta} = \frac{A_{1}\,w_{1}^{*}(\eta) + A_{2}\,w_{2}^{*}(\eta)}{A_{1} + A_{2}}\f] | | (9-35) | |

with \f$w_{k}^{*}\f$ the wetted fraction (9-20) of sub-triangle \f$k\f$ above
its switch stage, \f$\varepsilon\f$ inside its linear tail, and zero once
that tail has reached zero depth.

The inverse \f$\eta(\bar{h})\f$ follows the same case ladder as §9.4.1,
with two differences in the fully wet and dry limits. Let \f$\bar{z}_w\f$ be
the area-weighted mean of the two sub-triangle mean beds — the true
mean bed of the two-plane surface — and \f$z_{top}\f$, \f$z_{low}\f$ the
highest and lowest of the six sorted elevations:

1. **Flat cell.** \f$z_{top} - z_{low} < 10^{-9}\f$ m: \f$\eta = \bar{z}_w + \max(\bar{h}, 0)\f$.
2. **Fully wet.** \f$\bar{h} \ge z_{top} - \bar{z}_w\f$: both sub-triangles
   are submerged and \f$\eta = \bar{z}_w + \bar{h}\f$ exactly.
3. **Dry.** \f$\bar{h} \le 0\f$: \f$\eta\f$ is the lower of the two
   sub-triangles' dry stages \f$\eta_s - \bar{h}_s/\varepsilon\f$ — the
   stage at which the regularized sum reaches zero.
4. **Otherwise** a safeguarded Newton iteration on the monotone sum
   (9-34) over the bracket \f$[\eta_{dry}, z_{top}]\f$, started from the
   flat guess \f$\bar{z}_w + \bar{h}\f$ (or the bracket midpoint if that
   lies outside it), with derivative (9-35); a step leaving the bracket
   is replaced by bisection, and iteration stops at
   \f$\lvert\Delta\eta\rvert < 10^{-13}(1 + R)\f$ with an 80-iteration cap.

The derivative the coupling needs, \f$d\eta/d\bar{h} = 1/\max(w^{*}, \varepsilon)\f$,
is bounded by \f$1/\varepsilon\f$ as on a triangle.

One consequence deserves stating. The `FLAT` closure (9-5) writes a quad's
bed as the vertex mean \f$z_c = \tfrac{1}{4}\sum z\f$, whereas the fully wet
branch of the two-plane relation uses \f$\bar{z}_w\f$, the mean bed of the
folded surface. The two coincide for a planar quad and differ otherwise,
so on a non-planar quad the two closures disagree even when the cell is
fully wet, by the difference of the two bed means — the flat closure is
exact for a fully wetted cell only when \f$z_c\f$ is the cell's true mean
bed, which for a quad the vertex mean is not. Wetting and drying
classification is unchanged: the `H_MOVE` hysteresis of §9.5.7 acts on
the cell-mean depth, and the paper's alternative vertex-wise criterion is
not adopted — its threshold needed per-roughness tuning in the authors'
own tests, and the flux-active set already plays that role.

Implementation: `src/engine/2d/mesh/QuadVfr.hpp` — `quadVfrPrecompute`
selects the diagonal once per quad and stores the six sorted elevations
and the two areas; `quadMeanDepthFromEta`, `quadWetFraction`,
`quadMeanDepthSlope`, `quadEtaFromMeanDepth` and `quadDEtaDMeanDepth`
are (9-34), (9-35) and the ladder above. The solver enters through
@ref openswmm::twoD::inertial::etaDepthQuadScalar and
@ref openswmm::twoD::inertial::volumeFromEtaQuadScalar in
`src/engine/2d/solver/InertialKernels.hpp`; the quad branch of
`cellEtaDepth` is kept out of line on purpose, because inlining its
Newton loop stopped the compiler inlining the triangle path that every
cell of every mesh runs. The `MeshBuilder` computes the per-quad data
(`quad_vfr_z`, `quad_vfr_a`) after the cell geometry.

<!-- source: src/engine/2d/mesh/QuadVfr.hpp:17-58,72-155,168-229; src/engine/2d/solver/InertialKernels.hpp:120-200; src/engine/2d/mesh/MeshBuilder.cpp:172-175; plans/2D_TRI_QUAD_MESH_PLAN_2026-09-06.md §2.2 -->

## 9.5 Numerical scheme

The discretization is a cell-centred finite-volume method with a
staggered face variable: cells hold volume, faces hold the unit-width
discharge \f$q\f$ normal to the face, positive from \f$L\f$ to \f$R\f$. Time
integration is explicit, with per-cell local time stepping.

### 9.5.1 The face momentum update

Each face integrates (9-2) along its own normal over its own step
\f$\Delta t_f\f$:

| | | | |
|---|---|---|---|
| \f[q^{n+1} = \frac{\hat{q} - g\,h_{f}\,\Delta t_{f}\,S}{1 + g\,\Delta t_{f}\,n_{f}^{2}\,\lvert\mathbf{q}_{f}\rvert / h_{f}^{7/3}}\f] | | (9-7) | |

with the free-surface slope

| | | | |
|---|---|---|---|
| \f[S = \frac{\eta_{R} - \eta_{L}}{d_{n}}\f] | | (9-8) | |

Three details in (9-7) carry weight.

**Friction is semi-implicit.** Writing the friction term with \f$q^{n+1}\f$
in the numerator and \f$\lvert\mathbf{q}\rvert\f$ from the previous state
puts it in the denominator, which is unconditionally stable: however
large the friction coefficient, the update can only shrink \f$q\f$ towards
zero, never overshoot through it. An explicit friction term would impose
a step limit that scales as \f$h^{7/3}/n^{2}\f$ — unusable on thin films,
which is where most of the cells are.

**The friction magnitude is taken from the flow vector rather than the
face-normal component.** Manning friction acting on the normal component is
\f$n^{2} q_{n} \lvert\mathbf{q}\rvert / h^{7/3}\f$. Using \f$\lvert q_n \rvert\f$ instead makes the damping a face applies depend on the face's
orientation relative to the flow — a face at 45° to a uniform sheet
under-damps by \f$\sqrt{2}\f$ — so no smooth surface can satisfy every face
of a triangulated slope simultaneously, and the steady state corrugates
cell to cell. The vector at the face is the mean of the two incident
cells' Perot-reconstructed discharge vectors (§9.5.4, Eq. 9-16), and
its magnitude is floored at the face's own discharge:

| | | | |
|---|---|---|---|
| \f[\lvert\mathbf{q}_{f}\rvert = \max\!\left( \lvert q_{f} \rvert,\ \left\lvert \tfrac{1}{2}(\mathbf{q}_{L} + \mathbf{q}_{R}) \right\rvert \right)\f] | | (9-24) | |

so a face whose cell reconstruction lags its own discharge — the first
firing after a front arrives, or immediately after activation — never
under-damps. With \f$\theta = 1\f$ the cell discharge vectors are not
allocated at all: the update uses \f$\hat{q} = q_f\f$ and
\f$\lvert\mathbf{q}_f\rvert = \lvert q_f \rvert\f$, recovering the original
Bates et al. (2010) scheme exactly. The friction exponent is evaluated
as \f$h^{7/3} = h^{2}\sqrt[3]{h}\f$ rather than through `pow()`.

**\f$\hat{q}\f$ is a lateral average, not \f$q\f$ itself.** With
`THETA` \f$= \theta\f$,

| | | | |
|---|---|---|---|
| \f[\hat{q} = \theta\,q_{f} + (1-\theta)\,\tfrac{1}{2}\left( \mathbf{q}_{L} + \mathbf{q}_{R} \right)\cdot\hat{\mathbf{n}}\f] | | (9-9) | |

\f$\theta = 1\f$ recovers the original Bates et al. (2010) scheme, which
carries no numerical diffusion and is prone to a checkerboard
oscillation in thin films on steep faces. \f$\theta < 1\f$ blends in the
neighbouring cells' reconstructed discharge and damps it — the weighted
formulation of de Almeida et al. (2012). The default \f$\theta = 0.8\f$
applies enough diffusion to suppress the oscillation without visibly
smearing fronts.

Finally the result is clamped:

| | | | |
|---|---|---|---|
| \f[\lvert q^{n+1} \rvert \le Fr_{max}\,h_{f}\sqrt{g\,h_{f}}\f] | | (9-10) | |

`FROUDE_MAX` defaults to 1.5. This is the steep-face guard: with no
convective term there is nothing in (9-2) to arrest acceleration down a
steep face, so the supercritical limit must be imposed rather than
resolved. Raising it lets genuinely transcritical cases run (§9.10) at
the cost of the guard.

As coded, one face firing over its step \f$\Delta t_f\f$ proceeds in a
fixed order: (i) evaluate the face depth (§9.5.2) and wall the face if
\f$h_f \le\f$ `DRY_DEPTH`, zeroing its momentum; (ii) form \f$\hat{q}\f$ by
(9-9) and the friction magnitude by (9-24); (iii) zero the free-surface
difference if it lies below the \f$10^{-12}\f$ m deadband (§9.5.3) and
form the slope (9-8); (iv) apply (9-7) and then the clamp (9-10);
(v) rescale by the positivity share (9-15) where it binds; (vi) book
\f$\pm\Delta M\f$ by (9-13). The stored face discharge is the post-clamp,
post-rescale value, so the momentum a face carries always matches the
mass it moved.

### 9.5.2 Face flow depth and wetting/drying

The depth in (9-7) is a property of the face, and how it is defined
decides when water is allowed to cross. Under
`FACE_RECONSTRUCTION MEAN` (the default):

| | | | |
|---|---|---|---|
| \f[h_{f} = \max(\eta_{L}, \eta_{R}) - \max(z_{c,L}, z_{c,R})\f] | | (9-11) | |

\f$h_f \le\f$ `DRY_DEPTH` makes the face a wall for that substep and its
momentum is zeroed. This is the standard "flow depth above the higher
bed" rule, and it is what makes the scheme handle wetting and drying
without regime-switching logic — but its bed is the higher *centroid*
elevation. A thin crest resolved as a line of high vertices — a levee, a
kerb, a road crown — has its height diluted by roughly a third when
averaged into the flanking centroids, so water crosses it before it
reaches it.

`FACE_RECONSTRUCTION VFR_FACE` uses instead the exact mean depth of the
driving surface over the wetted portion of the shared edge, evaluated
against the edge's **true endpoint elevations** \f$z_{lo} \le z_{hi}\f$
(Begnudelli and Sanders, 2007, Eq. 14):

| | | | |
|---|---|---|---|
| \f[h_{f} = 0\f] | \f$\eta \le z_{lo}\f$ | (9-12a) | |
| \f[h_{f} = \frac{(\eta - z_{lo})^{2}}{2(z_{hi}-z_{lo})}\f] | \f$z_{lo} < \eta \le z_{hi}\f$ | (9-12b) | |
| \f[h_{f} = \eta - \tfrac{1}{2}(z_{lo}+z_{hi})\f] | \f$\eta > z_{hi}\f$ | (9-12c) | |

with \f$\eta = \max(\eta_L, \eta_R)\f$. The quadratic branch matches value
and slope at both joins, so overtopping onset is \f$C^1\f$ and the flux does
not jump when the waterline crosses the edge. The gate (9-12a) is the
substantive part: a cell holding water pooled below the whole shared
edge conveys nothing across it. Embankments hold to their real crest,
and drainage no longer strands water on slopes.

Both branches are single-sourced and used identically by the interior
faces, the boundary edges and the GPU kernels, so all backends agree.

### 9.5.3 Well-balancedness

A body of water at rest over arbitrary bathymetry must stay at rest.
This is the C-property, and it is not automatic — a scheme that
discretizes the bed slope and the pressure gradient separately will
generally produce a spurious flux from their imbalance.

Writing the pressure gradient as the free-surface slope (9-8) makes the
property structural: at rest \f$\eta_L = \eta_R\f$, so \f$S = 0\f$ exactly, and
(9-7) with \f$\hat{q} = q = 0\f$ returns zero for any bed whatsoever.
Likewise a dry neighbour standing higher gives \f$h_f \le 0\f$ and the face
is a wall — there is no uphill creep to suppress.

One numerical guard is needed. The closure round-trip \f$V \to \eta\f$
introduces rounding noise of order 1 ulp, and the square-root character
of the friction balance amplifies it: a persistent \f$\Delta\eta \sim 10^{-16}\f$ m sustains \f$q \sim 10^{-6}\f$ m²/s. A slope below
\f$10^{-12}\f$ m is therefore set to exactly zero, far below any physical
head, after which the friction denominator decays \f$q\f$ geometrically and
rest states are exact rather than merely small. §9.10 measures the
result at \f$10^{-16}\f$ relative error on the SWASHES lake-at-rest cases.

### 9.5.4 The cell update, conservation and positivity

A face firing books the identical volume transfer into a per-side
accumulator:

| | | | |
|---|---|---|---|
| \f[\Delta M = q^{n+1}\,\xi\,\Delta t_{f}, \qquad \text{acc}_{L} \mathrel{-}= \Delta M, \quad \text{acc}_{R} \mathrel{+}= \Delta M\f] | | (9-13) | |

and a cell firing gathers and clears its own side of each incident
accumulator:

| | | | |
|---|---|---|---|
| \f[V^{n+1} = V^{n} + \sum_{f} \text{acc}_{f,i} + \Delta t_{c}\,A\,(i - e + s)\f] | | (9-14) | |

after which \f$\eta\f$ and \f$\bar{h}\f$ are recomputed through the closure of
§9.4. Because the two sides of a face are written from the *same*
floating-point product, conservation is exact by construction rather
than to within a tolerance — including across a local-time-stepping tier
interface, where the two sides apply their halves at different times
(§9.5.6). The sum of cell volumes plus pending accumulators is an
invariant of the face phase, and the engine can assert it directly
(`OPENSWMM_2D_MARCHER_CHECK`).

Positivity is enforced at face cadence rather than by a post-hoc clamp.
A cell has at most three outgoing faces, so capping each exporting face
at a share \f$\beta/3\f$ of its exporting cell's volume bounds the total
export at \f$\beta V\f$ per cell step without any cross-face coordination:

| | | | |
|---|---|---|---|
| \f[\lvert q^{n+1}\rvert\,\xi\,\Delta t_{f} \le \frac{\beta}{3}\,\frac{V_{exp}}{2^{\,k_{exp} - k_{f}}}\f] | | (9-15) | |

\f$\beta\f$ is `exchange_beta` (0.8). The tier ratio in the denominator
matters: an exporting cell republishes its volume only at its own
firings, and a finer face fires \f$2^{k_{exp}-k_f}\f$ times in between, so
without dividing the share the repeated takes drain the cell. When a
face is rescaled, the *same* rescaled flux updates both sides, so the
cap costs nothing in conservation. A zero floor at the cell update
remains as a backstop; with the face caps in place it does not engage.

The cell's discharge vector — needed for the friction magnitude and the
\f$\theta\f$ blend — is reconstructed at the cell's own cadence from its
face fluxes by the Perot (2000) formula:

| | | | |
|---|---|---|---|
| \f[\mathbf{q}_{i} = \frac{1}{A_{i}}\sum_{f} s_{f}\,q_{f}\,\xi_{f}\,\left( \mathbf{x}_{f} - \mathbf{x}_{i} \right)\f] | | (9-16) | |

with \f$s_f = \pm 1\f$ the outward orientation of face \f$f\f$ for cell \f$i\f$ and
\f$\mathbf{x}_f\f$ the edge midpoint.

### 9.5.5 The time step

Each cell's stable step follows from the gravity-wave celerity and its
own characteristic length:

| | | | |
|---|---|---|---|
| \f[\Delta t_{i} = \alpha \frac{L_{char,i}}{\sqrt{g h_{i}} + \lvert \mathbf{u}_{i}\rvert}\f] | | (9-17) | |

with \f$\alpha =\f$ `CFL_NUMBER`, default 0.7, and the base step of a macro
cycle \f$\Delta t_0 = \min_i \Delta t_i\f$, further capped by
`MAX_TIMESTEP`. Only active cells wetter than `DRY_DEPTH` enter the
census — a film the solver will not move imposes no constraint — and a
fully quiescent active set falls back to \f$\Delta t_0 =\f$ `MAX_TIMESTEP`.
The advective augmentation \f$\lvert\mathbf{u}_i\rvert = \lvert\mathbf{q}_i\rvert/h_i\f$ is evaluated from the Perot vector when
the depth exceeds \f$10^{-6}\f$ m and \f$\theta < 1\f$, and taken as zero
otherwise.

\f$L_{char}\f$ is the operator-derived length of (9-4) rather than a
geometric proxy, and the difference matters. The face update couples cells through \f$g h \xi_f/(A\,d_{n,f})\f$;
the worst (odd–even) mode of that operator has eigenvalue \f$\lambda = 2(gh/A)\sum_f \xi_f/d_{n,f}\f$, and the explicit update is linearly stable
for \f$\Delta t \le 2/\sqrt{\lambda}\f$, which is exactly (9-4) divided by
the celerity. Defining \f$L_{char}\f$ this way makes \f$\alpha\f$ a **true
Courant fraction**: \f$\alpha = 1\f$ is the linear stability limit on any
mesh, and the default 0.7 is a uniform 30 % margin. A raster of squares
recovers the classical \f$c\,\Delta t/\Delta x \le 1/\sqrt{2}\f$; a
union-jack pair of right triangles gets \f$0.408\,\Delta x\f$. The obvious
geometric proxy \f$2A/\xi_{max}\f$ returns \f$0.707\,\Delta x\f$ on that same
union-jack mesh — an overstatement by \f$\sqrt{3}\f$, and the reason
frictionless basins seiched at nominal Courant numbers that looked
conservative.

Between full rebuilds the tier lists are frozen while depths keep
evolving, so \f$\Delta t_0\f$ is re-minimized every macro cycle. It may be
**tightened** at any time — every tier still satisfies \f$\Delta t_i \ge 2^{k}\Delta t_0\f$ — but growing it requires reassigning tiers and
therefore waits for a rebuild.

### 9.5.6 Local time stepping

A flood mesh is heterogeneous by nature: a 0.5 m cell at a coupled
manhole and a 20 m cell on a floodplain differ by two orders of
magnitude in stable step. Marching the whole mesh at the smallest one
wastes almost all of the work.

The solver instead assigns each cell a power-of-two tier \f$k\f$ from the
ratio \f$\Delta t_i/\Delta t_0\f$, capped at `LTS_TIERS` (default 4,
allowing an 8× spread; up to 8 tiers, 128×). A macro cycle is
\f$2^{K-1}\f$ base substeps; tier \f$k\f$ fires every \f$2^{k}\f$ substeps with
\f$\Delta t = 2^{k}\Delta t_0\f$. Within a substep all due faces fire first,
then all due cells — faces read the surfaces their incident cells
published at those cells' last firings.

**A face belongs to the finer of its two incident cells' tiers.** It
therefore always integrates at the rate the sharper side needs, reading
the coarser side's surface frozen since that cell last fired. This is
what makes tier interfaces safe without interpolation, and (9-13) is
what makes them conservative: the same \f$\pm\Delta M\f$ is booked once and
applied by each side at its own firing.

Cells whose forcing changes at the fastest cadence are pinned to tier 0
regardless of their Courant number — boundary cells and cells carrying a
1D coupling point. For these cells the forcing, rather than the
celerity, sets the resolution requirement.

The per-cell step that feeds the tier assignment is closure-specific:
(9-17) under the local-inertial law, the Courant bound (9-41) on the
isoperimetric length under `FULL_SWE`, and the diffusion bound (9-43)
under `DIFFUSIVE_WAVE`. Each is capped by `MAX_TIMESTEP`, and the tiers
are built from whichever applies.

**Global-step mode.** `RECONSTRUCTION_ORDER 2` under `FULL_SWE`
(§9.5.10) integrates with a two-stage SSP-RK2 scheme over the global
active lists, and a two-stage scheme cannot be tiered: `LTS_TIERS` is
reduced to 1 with a notice and every active cell and face fires each
substep at \f$\Delta t_0\f$. `LTS_TIERS 1` selects the same mode by hand for
any closure; it is bit-identical to the tiered path on a mesh where
tiering finds nothing to separate.

**Front-cadence tiers.** Under `FRONT_REBUILD` (§9.5.7) the dry cells of
the widened halo would otherwise be assigned the coarsest tier — a dry
cell's (9-17) is unbounded — so a front could enter them only every
\f$2^{K-1}\f$ substeps regardless of how wide the halo is. After the tier
assignment, every dry active cell is therefore pulled down to the finest
tier among its active neighbours, and the pull is propagated across the
halo rings by a Gauss–Seidel sweep over the faces (at most five passes,
stopping early when nothing changes). The sweep is kept serial on
purpose: a Jacobi form reaches a different fixed point inside the ring
budget, and on the Bellinge 30-minute slice that difference changed the
`DIFFUSIVE_WAVE` inflow and spill volumes materially for a 4 % saving on
the `FULL_SWE` deck alone. The receiving cells then update at the
front's own cadence.

<!-- source: src/engine/2d/solver/ExplicitInertialSolver.cpp:59-63,102-107,700-722,748-783,1859-1909; docs/manuals/reference/hydraulics/sections/Chapter9-TwoDimensional.md:579-604 (retained text) -->

### 9.5.7 The active set

Most of a rain-on-grid mesh is not flowing. A cell is **flux-active**
only above `H_MOVE` (default 3 mm), with hysteresis: entering cells need
\f$h_{move} + \delta\f$, active cells stay until \f$h_{move} - \delta\f$, where
\f$\delta = \min(1\ \text{mm},\ h_{move}/2)\f$. Scaling the band with
`H_MOVE` matters on shallow benchmarks — a fixed ±1 mm band made
`H_MOVE` \f$= 10^{-4}\f$ require 1.1 mm to activate, ten times the requested
threshold, which freezes wetting fronts in place.

Two rules complete the set. **A face flows only when both incident cells
are active** — a one-sided face would export volume into a cell whose
update never runs, and measured as an 18 % basin loss when it was
allowed. A **one-ring halo** around the active set therefore guarantees
an advancing front always has an active receiving cell.

Inactive cells are not skipped, they are integrated **lazily**: rainfall
and held coupling accumulate as pure storage over the whole interval
since the last synchronization, in one pass, because a cell below
`H_MOVE` has no face flux by construction. This is what makes
rain-on-grid over a large dry mesh nearly free. A rainfall rate as such
never activates a cell — activation follows only from the accumulated
depth crossing the threshold at a rebuild, which is the point of the
lazy tier — whereas a nonzero coupling flux activates its cell
immediately, as do the pinned boundary and coupling-point cells of
§9.5.6.

The active set and the tier assignment are rebuilt every four macro
cycles rather than every substep; the cost of the rebuild is \f$O(n_{cells})\f$
and dominated everything else when it ran per routing step on a large
mesh.

**Front-triggered rebuilds (`FRONT_REBUILD`).** Between rebuilds the
active set is frozen, so a wetting front can cross at most one cell ring
per rebuild period. Under the local-inertial law that is not a practical
restriction (§9.5.9): a fast front implies a small \f$\Delta t_0\f$, so the
four-cycle period shrinks with the front's own time scale. A Godunov
front under `FULL_SWE` is different — it travels at about \f$2\sqrt{gh}\f$,
one cell per base substep at Courant ½, and crosses the one-ring halo
inside a single macro cycle, after which it stalls until the next
rebuild. That stall is what pinned the Ritter and Thacker results before
the option existed. `FRONT_REBUILD` replaces the fixed cadence with a
breach trigger. The halo is widened to five rings (`kFrontHaloRings`,
enough for such a front to cross a macro cycle of \f$2^{K-1}\f$ substeps),
its outer ring — an active cell with an inactive neighbour — is marked
as the *frontier*, and a cell firing that finds a frontier cell at or
above the activation depth \f$h_{on}\f$ of the last rebuild raises a breach
flag; the next macro cycle then rebuilds immediately instead of waiting
out the cadence. The dry cells of the widened halo are pulled to the
front's tier cadence (§9.5.6), since a wide halo of cells that fire only
at the coarsest tier would stall the front just as surely.

`AUTO` (the default) switches the trigger on under `FULL_SWE` only. It
stays off under `LOCAL_INERTIAL`, which keeps that closure bit-identical
to its earlier results, and off under `DIFFUSIVE_WAVE`, whose
\f$\Delta x^{2}\f$-bounded step moves a front a fraction of a cell per macro
cycle so the cheap cadence suffices. `YES` and `NO` force it either way
for any closure.

<!-- figure spec: plan view of a wetting front on a dry mesh: the flux-active cells, the one-ring halo of the default cadence, the five-ring halo and marked frontier ring under FRONT_REBUILD, and the breach cell whose depth crossing h_on triggers the early rebuild -->
![Figure 9-5](figures/png/hydraulics_ch9_active_set.png)

*Figure 9-5 The flux-active set at a wetting front: the default one-ring halo, the front-rebuild halo and the breach that forces an early rebuild*

<!-- source: src/engine/2d/solver/ExplicitInertialSolver.cpp:59-63,126-134,595-603,627-676,759-783,1443-1446; src/engine/2d/data/SolverOptions2D.hpp:214-221; src/engine/2d/input/SectionHandlers2D.cpp:195-199; docs/manuals/reference/hydraulics/sections/Chapter9-TwoDimensional.md:606-636 (retained text) -->

### 9.5.8 Data layout and the substep algorithm

The solver's working set is three structure-of-arrays blocks, sized
once at initialization.

**Per unique interior face** (built by the edge enumeration of §9.3):
the incident cells \f$c_L < c_R\f$; the edge length \f$\xi\f$; the unit normal
\f$\hat{\mathbf{n}}\f$ oriented \f$L \to R\f$ and the edge midpoint; the
reciprocal face-normal centroid separation \f$1/d_n\f$ of (9-3); the
squared face roughness \f$n_f^2 = \left(\tfrac{1}{2}(n_L + n_R)\right)^2\f$; the sorted true endpoint bed elevations \f$z_{lo} \le z_{hi}\f$ of the shared edge (for `VFR_FACE`); and the two flat
edge-slot indices \f$3t + e\f$ used to publish fluxes back into the
per-cell edge arrays. The prognostic state per face is the discharge
\f$q\f$ and the two pending-transfer accumulators \f$\text{acc}_L\f$,
\f$\text{acc}_R\f$ of (9-13), plus a face tier.

**Per cell**: the conserved volume \f$V\f$ and the reconstructed \f$\eta\f$ and
\f$\bar{h}\f$; the source rates (rainfall, evaporation demand, coupling
flux, all m/s); the Perot discharge vector \f$(q_{cx}, q_{cy})\f$,
allocated only when \f$\theta < 1\f$; the characteristic length \f$L_{char}\f$
of (9-4); the tier, the active flag and the tier-0 pin flag; and a CSR
incidence — for cell \f$i\f$ the incident unique faces with orientation
signs \f$\pm 1\f$ — so the continuity gather is a race-free per-cell loop.

**Per vertex**: coordinates and bed elevation, plus the reconstruction
stencils of §9.9. Per flat edge slot: the published volumetric flux
and the boundary-condition tables of §9.6.

One solver advance over a window \f$[t,\ t + \Delta t]\f$ executes:

1. Reset the per-advance ledgers: boundary accumulators, coupling
   accumulators \f$\int Q\,dt\f$, and the per-node spill budget.
2. Loop until the window is filled:
   1. Every fourth macro cycle, **rebuild**: settle all pending face
      accumulators into their cells; integrate lazy sources on
      inactive cells over the interval since the last synchronization;
      reseed the active set with the hysteretic threshold of §9.5.7
      plus the pinned cells; grow the one-ring halo; recompute the CFL
      census and \f$\Delta t_0\f$; assign cell tiers
      \f$k = \min\!\left(K - 1, \lfloor \log_2(\Delta t_i/\Delta t_0) \rfloor\right)\f$ with pinned and coupled cells forced to \f$k = 0\f$;
      set each face's tier to the finer of its cells and zero the
      momentum of any face with an inactive side. Between rebuilds,
      only re-minimize \f$\Delta t_0\f$ from the live depths — tightening
      is always safe, growing waits for the rebuild.
   2. If the active set is empty, stride to the end of the window; the
      lazy tier keeps accumulating.
   3. Set \f$\Delta t_0 \leftarrow \min(\Delta t_0, \text{remaining})\f$.
      If a full macro cycle of \f$2^{K-1}\f$ base substeps would overshoot
      the window, settle the accumulators, collapse every active cell
      to tier 0, and finish the window with single global substeps
      (the *tail*); a rebuild is forced afterwards.
   4. Run the macro cycle: for each base substep \f$s\f$, fire the faces
      of every due tier (\f$s \bmod 2^k = 0\f$) with \f$\Delta t_f = 2^k\Delta t_0\f$ in the order of §9.5.1, then fire the due cells
      with \f$\Delta t_c = 2^k \Delta t_0\f$ — each cell gathers and
      clears its own side of every incident accumulator, applies its
      sources, floors the volume at zero, reruns the closure of §9.4
      and refreshes its Perot vector (9-16). Tier-0 cell firings
      additionally integrate the boundary edges of §9.6 and the live
      junction exchange of §9.7.
3. Land any remaining lazy sources at the window end.
4. Publish the flux picture: interior faces re-limit \f$q\f$ against the
   published surfaces (the update's own clamp used the depths it saw;
   the subsequent cell pass moved them) and write \f$\pm q\,\xi\f$ into
   both edge slots; boundary slots carry the window-mean applied flux
   so the router's booking recovers the exact applied volume.

The face and cell passes are OpenMP-parallel with static scheduling
and the project's `THREADS` setting. Each face is written by exactly
one iteration and touches only its own accumulator slots, and each
cell gathers only its own accumulator sides, so both passes are
race-free and bit-identical to serial execution for any thread count.
The boundary and coupling loops are serial; they are perimeter- and
point-count-sized. The quantity \f$\sum_i V_i + \sum_f (\text{acc}_{L,f} + \text{acc}_{R,f})\f$ is invariant under the face phase and is asserted
directly when `OPENSWMM_2D_MARCHER_CHECK` is set.

Figure 9-2 assembles the co-advance batch of §9.7.3 and the marcher's
substep loop into one workflow.

<!-- workflow: coupling_batch -->
<pre class="mermaid">
flowchart TD
    A[1D routing step completes - node heads current] --> B{Pending span reaches the sync batch}
    B -- no --> A
    B -- yes --> C[Save state and seed withdrawal budgets]
    C --> D[Accumulate outfall discharge and inject as batch-rate source]
    D --> E[Refresh rainfall, forcing overrides and boundary values]
    E --> F[Marcher advance over the batch span]
    F --> G{Rebuild due}
    G -- yes --> H[Settle accumulators, lazy sources, active set, tiers, dt0]
    G -- no --> I[Tighten dt0 from live depths]
    H --> J[Macro cycle of base substeps]
    I --> J
    J --> K[Fire due faces - depth, blend, update, clamp, positivity, book dM]
    K --> L[Fire due cells - gather, sources, closure, Perot]
    L --> M[Tier-0 firings - boundary edges and live junction exchange]
    M --> N{Batch span filled}
    N -- no --> G
    N -- yes --> O[Book junction, outfall and boundary ledgers into the 2D mass balance]
    O --> P[Queue exchange volumes for uniform delivery to 1D lateral inflow]
    P --> Q[Clear one-shot forcings, reset window accumulators]
    Q --> A
</pre>

*Figure 9-2 One 1D–2D co-advance batch and the explicit marcher's
substep loop within it (rendered diagram)*

Implementation:
@ref openswmm::twoD::ExplicitInertialSolver::advance drives the loop;
@ref openswmm::twoD::ExplicitInertialSolver::fireFaces,
@ref openswmm::twoD::ExplicitInertialSolver::fireCells,
@ref openswmm::twoD::ExplicitInertialSolver::syncAndRebuild,
@ref openswmm::twoD::ExplicitInertialSolver::settleAccumulators and
@ref openswmm::twoD::ExplicitInertialSolver::refreshDt0 implement the
numbered steps. The face layout and CSR incidence are built by
@ref openswmm::twoD::InertialEdges::build
(`src/engine/2d/solver/InertialEdges.cpp`); the scalar kernels live in
`src/engine/2d/solver/InertialKernels.hpp`; the cell state arrays are
@ref openswmm::twoD::SurfaceStateData
(`src/engine/2d/data/SurfaceStateData.hpp`).

### 9.5.9 Wetting and drying: the complete rule set

The scheme has no regime-switching logic; wetting and drying emerge
from a small set of thresholds applied uniformly. Table 9-1 collects
them.

| Constant | Value | Origin | Role |
|---|---|---|---|
| `DRY_DEPTH` | 0.001 m | `[2D_OPTIONS]` | Face-wall depth, friction depth floor, evaporation taper scale, coupling ramp scale, CFL census cutoff |
| `H_MOVE` | 0.003 m | `[2D_OPTIONS]` | Flux-activation depth |
| \f$\delta\f$ | \f$\min(0.001\ \text{m},\ h_{move}/2)\f$ | derived | Activation hysteresis half-band |
| \f$\varepsilon\f$ | 0.01 | `VFR_MIN_WET_FRAC` | Wetted-fraction floor of the VFR closure |
| slope deadband | \f$10^{-12}\f$ m | fixed | Free-surface differences treated as exactly zero |
| flat-relief guard | \f$10^{-9}\f$ m | fixed | Cell or edge relief below which the geometry is flat |
| \f$\beta\f$ | 0.8 | fixed | Positivity and exchange availability fraction |
| rebuild cadence | 4 macro cycles | fixed | Active-set and tier refresh period |

*Table 9-1 Wetting and drying thresholds and guards of the 2D solver
(SI units)*

**The face-wet criterion.** A face conveys only when its flow depth
exceeds `DRY_DEPTH`: under `MEAN` the depth (9-11), under `VFR_FACE`
the wetted-edge depth (9-12) of the driving surface over the edge's
true endpoint beds. A face that fails the test is a wall for that
substep and its momentum is set to exactly zero — walls carry no stale
discharge into their next wet substep. A face also requires both
incident cells active (§9.5.7); faces bordering an inactive cell have
their momentum zeroed at the rebuild.

**How a dry cell wets.** The activation thresholds carry hysteresis,

| | | | |
|---|---|---|---|
| \f[h_{on} = h_{move} + \delta, \qquad h_{off} = \max(0,\ h_{move} - \delta), \qquad \delta = \min(0.001,\ h_{move}/2)\f] | | (9-25) | |

entering cells needing \f$h_{on}\f$ and active cells persisting to
\f$h_{off}\f$. A dry cell gains water in one of three ways. Distributed
sources (rainfall) accumulate lazily as pure storage; the cell joins
the active set at the first rebuild whose census finds its depth at or
above \f$h_{on}\f$. A concentrated source — a nonzero coupling flux —
activates the cell at the next rebuild regardless of depth, as does
membership in the pinned set. A neighbouring active cell activates it
through the one-ring halo, after which the shared face joins the face
lists; the first term to act on the newly wet cell is then the mass
transfer (9-13), driven by the slope term of (9-7) integrated from
\f$q = 0\f$ — the friction denominator is near unity at \f$q = 0\f$, so the
initial specific discharge after one face step is \f$-g\,h_f\,\Delta t_f\,S\f$.

**How a front advances.** Between rebuilds the active set is frozen,
so a wetting front can cross at most one cell ring per rebuild period
(four macro cycles). This is not a practical restriction: a fast front
implies a small \f$\Delta t_0\f$ through (9-17), so the rebuild period
shrinks with the front's own time scale, and the halo guarantees the
front always finds an active receiving cell — a one-sided face is
never allowed to fire (§9.5.7).

**Shorelines and the lake at rest.** A dry neighbour standing higher
gives \f$h_f \le 0\f$ under (9-11) and the face is a wall — there is no
uphill creep to suppress. Under the VFR closure the partially wet
shoreline cell's \f$\eta\f$ is exact rather than biased high (§9.4), and
under `VFR_FACE` the gate (9-12a) blocks conveyance across any edge
whose low point stands above the driving surface, so an emerged bump
holds a lake at rest exactly. The \f$10^{-12}\f$ m slope deadband removes
the closure round-trip noise that would otherwise sustain a
\f$\sim 10^{-6}\f$ m²/s residual discharge (§9.5.3); the emerged-bump
verification case of §9.10 measures the combined result at
\f$1.4 \times 10^{-16}\f$ relative depth error.

**Draining and positivity.** Every exporting face is capped at the
tier-scaled \f$\beta/3\f$ share of its exporting cell's volume, Eq.
(9-15), so a cell's at most three outgoing faces can remove at most
\f$\beta V\f$ per cell step with no cross-face coordination. Boundary
edges and the coupling exchange clamp in volume space — the applied
change is recomputed after flooring the provisional volume at zero, so
booking matches application exactly. The cell update keeps a plain
zero floor as a backstop; with the caps in place it does not engage
(a debug build with `OPENSWMM_2D_MARCHER_CHECK` reports any clamp
below \f$-10^{-12}\f$ m³).

**The friction depth floor.** No separate floor exists: the face-wall
test guarantees \f$h_f >\f$ `DRY_DEPTH` \f$= 1\f$ mm before (9-7) is
evaluated, so \f$h_f^{7/3} \ge 10^{-7}\f$ m\f$^{7/3}\f$ and the semi-implicit
denominator is always finite. Because friction enters only through the
denominator, it can shrink \f$q\f$ toward zero and never reverse it.

**Rain and evaporation on dry cells.** The evaporation sink is the
demand rate tapered by a cubic Hermite ramp below `DRY_DEPTH`,

| | | | |
|---|---|---|---|
| \f[e_{eff} = e \cdot \sigma\!\left( h/h_{dry} \right), \qquad \sigma(t) = \min(1, t)^{2}\left( 3 - 2\min(1, t) \right)\f] | | (9-26) | |

with \f$e_{eff} = 0\f$ for \f$h \le 0\f$ or \f$e \le 0\f$ — a drying cell cannot
evaporate more water than it holds, and negative demand is treated as
zero rather than as a condensation source. Rainfall on an inactive
cell integrates in a single lazy pass over the whole interval since
the last synchronization; the result is floored at zero volume, so a
forced evaporation override on a dry cell is harmless.

Figure 9-3 sketches the geometry the rules act on: the three wetting
cases of the planar-bed cell (§9.4.1) and the wetted-edge face gate
(§9.5.2).

![Figure 9-3](figures/png/hydraulics_ch9_vfr_wetting_cases.png)

*Figure 9-3 Wetting cases of a planar-bed triangular cell and the
wetted-edge face gate (placeholder — to be replaced by a final
drawing)*

Implementation: the face-wet tests are
@ref openswmm::twoD::inertial::faceFlowDepth and
@ref openswmm::twoD::inertial::faceFlowDepthVfr over
@ref openswmm::twoD::inertial::faceDepthFromEta; the positivity share
is applied in @ref openswmm::twoD::ExplicitInertialSolver::fireFaces
with the scale factor of
@ref openswmm::twoD::inertial::positivityScale; the hysteretic
activation, halo and lazy source passes are
@ref openswmm::twoD::ExplicitInertialSolver::syncAndRebuild and
@ref openswmm::twoD::ExplicitInertialSolver::lazySourcesOnly; the
evaporation taper is @ref openswmm::twoD::evapSink
(`src/engine/2d/solver/SurfaceFluxCalculator.hpp`). The slope deadband
is `kEtaDeadband` in `src/engine/2d/solver/InertialKernels.hpp`.

### 9.5.10 The full shallow-water face flux

Under `MOMENTUM_EQUATION FULL_SWE` a face firing evaluates one Godunov
flux from the two adjacent cell states — free surface \f$\eta\f$, mean depth
\f$\bar{h}\f$ and cell momentum \f$\mathbf{q} = (hu, hv)\f$ — and books mass and
momentum on both sides from the same products. The steps, in the order
coded:

**Hydrostatic reconstruction.** Each side's bed is taken consistent with
its storage, \f$z_L = \eta_L - \bar{h}_L\f$ and \f$z_R = \eta_R - \bar{h}_R\f$ —
the cell mean bed under `FLAT`, the flat-equivalent bed under `VFR`,
which is what keeps a uniform \f$\eta\f$ an exact rest state under either
closure — and both depths are reconstructed against the higher of the
two (Audusse et al., 2004):

| | | | |
|---|---|---|---|
| \f[z_{f} = \max(z_{L}, z_{R}), \qquad h_{L}^{*} = \max(0,\ \eta_{L} - z_{f}), \qquad h_{R}^{*} = \max(0,\ \eta_{R} - z_{f})\f] | | (9-36) | |

A side whose cell depth is at or below `DRY_DEPTH`, or whose
reconstructed depth vanishes, carries zero velocity, which removes the
\f$q/h\f$ blow-up at fronts; the reconstruction alone already removes the
"dry cell standing higher" flux. A face dry on both sides carries
nothing and books nothing. The velocities are rotated into the face
frame, \f$u_n = \mathbf{u}\cdot\hat{\mathbf{n}}\f$ and
\f$u_t = \mathbf{u}\cdot\hat{\mathbf{t}}\f$ with \f$\hat{\mathbf{t}} = (-n_y, n_x)\f$.

**Wave speeds.** With \f$c = \sqrt{g h^{*}}\f$ on each side, the
two-rarefaction estimate (Toro, 2001, §10.4) for a wet–wet face and the
exact dry-bed front speeds otherwise:

| | | | |
|---|---|---|---|
| \f[u^{\star} = \tfrac{1}{2}(u_{L} + u_{R}) + c_{L} - c_{R}, \qquad c^{\star} = \tfrac{1}{2}(c_{L} + c_{R}) + \tfrac{1}{4}(u_{L} - u_{R})\f] | wet–wet | (9-37a) | |
| \f[S_{L} = \min(u_{L} - c_{L},\ u^{\star} - c^{\star}), \qquad S_{R} = \max(u_{R} + c_{R},\ u^{\star} + c^{\star})\f] | wet–wet | (9-37b) | |
| \f[S_{L} = u_{L} - c_{L}, \qquad S_{R} = u_{L} + 2c_{L}\f] | dry right | (9-37c) | |
| \f[S_{L} = u_{R} - 2c_{R}, \qquad S_{R} = u_{R} + c_{R}\f] | dry left | (9-37d) | |

**The HLLC flux.** On the state \f$\mathbf{U} = [h,\ h u_n,\ h u_t]\f$ with
physical flux \f$\mathbf{F} = [h u_n,\ h u_n^{2} + \tfrac{1}{2} g h^{2},\ h u_n u_t]\f$,

| | | | |
|---|---|---|---|
| \f[\mathbf{F}_{f} = \mathbf{F}_{L}\ \ (S_{L} \ge 0); \qquad \mathbf{F}_{f} = \mathbf{F}_{R}\ \ (S_{R} \le 0); \qquad \text{else}\ \ F_{h,n} = \frac{S_{R}F_{L} - S_{L}F_{R} + S_{L}S_{R}(U_{R} - U_{L})}{S_{R} - S_{L}}\f] | mass and normal momentum | (9-38a) | |
| \f[F_{t} = F_{h}\,u_{t}^{up}, \qquad S^{\star} = \frac{S_{L}\,d_{R} - S_{R}\,d_{L}}{d_{R} - d_{L}}, \quad d = h^{*}(u_{n} - S)\f] | tangential momentum | (9-38b) | |

The HLL middle state serves mass and normal momentum; the tangential
momentum is a passive scalar upwinded on the contact speed \f$S^{\star}\f$
(Toro, 2001, Eq. 10.70), which falls back to the mean normal velocity
when both sides are at rest. Species ride the mass flux from the side
\f$S^{\star}\f$ points away from. The momentum flux is rotated back to
\f$(x, y)\f$.

**The bed-slope correction.** Each side receives, per unit face length,

| | | | |
|---|---|---|---|
| \f[\mathbf{c}_{L} = \tfrac{1}{2}\,g\left( h_{L}^{*2} - \bar{h}_{L}^{2} \right)\hat{\mathbf{n}}, \qquad \mathbf{c}_{R} = -\,\tfrac{1}{2}\,g\left( h_{R}^{*2} - \bar{h}_{R}^{2} \right)\hat{\mathbf{n}}\f] | | (9-39) | |

At rest the HLLC flux is \f$[0,\ \tfrac{1}{2} g h^{*2},\ 0]\f$ and (9-39)
cancels it face by face, so a lake at rest over any bed is an exact
steady state — the C-property of §9.5.3 in conservative form. Walls
close the same balance: every `WALL` boundary slot of a wet cell
contributes a mirror-state Riemann flux (same depth, reversed normal
velocity) that carries no mass but \f$\tfrac{1}{2} g h^{2}\f$ of normal
momentum, booked at the cell's own cadence from a per-cell list of wall
slots; without it a cell at rest beside a wall would accelerate away
from it.

**Booking.** The per-edge conveyance factor of §9.3 scales the whole
flux vector. The positivity share of (9-15), with \f$\beta/n_v\f$ per face,
scales mass and momentum together so the two stay consistent; under
this closure it is a backstop rather than the guarantee (below). The
stored face discharge is the limited mass flux, \f$\pm\Delta M\f$ is booked
into the volume accumulators exactly as in (9-13), and the momentum
accumulators receive \f$(-\mathbf{F} + \mathbf{c}_{L})\,\xi\,\Delta t_f\f$ on
the exporter side and \f$(+\mathbf{F} + \mathbf{c}_{R})\,\xi\,\Delta t_f\f$
on the receiver side.

**The cell update.** A cell firing gathers its volume by (9-14) and its
momentum by

| | | | |
|---|---|---|---|
| \f[\mathbf{q}^{\,n+1} = \frac{A\,\mathbf{q}^{\,n} + \sum_{f}\text{macc}_{f,i}}{A}\ \Big/\ \left( 1 + \frac{g\,\Delta t_{c}\,n^{2}\,\lvert\mathbf{q}\rvert}{h_{new}^{7/3}} \right)\f] | | (9-40) | |

— the gathered momentum, then semi-implicit Manning friction against the
**new** depth, which can only shrink \f$\lvert\mathbf{q}\rvert\f$. Sources
(rain, coupling, infiltration, evaporation) move volume only, so the
velocity adjusts with the depth; a cell that fell to `DRY_DEPTH` or below
carries no momentum. The cell momentum stands in for the Perot vector
(9-16) of the default closure wherever the marcher needs a cell velocity.

**The step bound.** First-order Godunov with hydrostatic reconstruction
is positivity-preserving at a Courant number of one half on triangles
and quadrilaterals, and the bound is enforced rather than assumed:

| | | | |
|---|---|---|---|
| \f[\Delta t_{i} = \alpha\,\frac{2A_{i}/P_{i}}{\sqrt{g h_{i}} + \lvert\mathbf{u}_{i}\rvert}, \qquad \alpha \le \tfrac{1}{2}\f] | | (9-41) | |

with \f$P_i\f$ the cell perimeter. `CFL_NUMBER` above ½ is reduced to ½ with
a notice. The isoperimetric length replaces \f$L_{char}\f$ of (9-4) because
the positivity argument is about the volume a cell's faces can remove in
one step, not about the odd–even mode of the staggered operator.

**Second order (`RECONSTRUCTION_ORDER 2`).** A Green–Gauss gradient of
\f$(\eta, u, v)\f$ is formed over each active cell — the face value is the
mean of the two cells, and a boundary face contributes the cell's own
value (zero gradient) — and limited per variable with the
Barth–Jespersen factor against the minimum and maximum over the cell
and its face neighbours. Cells that are dry, thin (below ten times
`DRY_DEPTH`) or touching a dry or inactive cell fall back to first order,
so the wet–dry front keeps the monotone update. Each side's face state
is extrapolated along the precomputed centroid-to-midpoint arm; the bed
stays piecewise constant per cell, and the correction (9-39) pairs
\f$h^{*}\f$ with the reconstructed face depth \f$\eta_f - z_{cell}\f$, so a
linear surface keeps its interface pressure difference and zero
gradients reduce exactly to the first-order well-balanced form. Time
integration is SSP-RK2 (Heun): \f$\mathbf{U}^{1} = \mathbf{U}^{0} + \Delta t\,L(\mathbf{U}^{0})\f$,
\f$\mathbf{U}^{2} = \mathbf{U}^{1} + \Delta t\,L(\mathbf{U}^{1})\f$,
\f$\mathbf{U}^{n+1} = \tfrac{1}{2}(\mathbf{U}^{0} + \mathbf{U}^{2})\f$, over
the global active lists (§9.5.6), with every per-advance ledger that both
stages incremented — boundary and coupling volumes, infiltration, the
node spill budgets — reset to its trapezoidal value. Second order is
refused with a notice when overland transport is active.

Implementation: `src/engine/2d/solver/SweKernels.hpp` — `faceFlux`
(first order), `faceFluxRecon` (second order), `hllcFlux`, `waveSpeeds`,
`bjLimiter`, `frictionUpdate`; the face firing is
@ref openswmm::twoD::ExplicitInertialSolver::fireFacesSwe, the momentum
gather and wall booking are the `FULL_SWE` branch of `fireCells`, the
gradients are `computeLimitedGradientsSwe` and the two-stage step is
`runRk2Step` in `src/engine/2d/solver/ExplicitInertialSolver.cpp`; the
perimeter length is `cell_lpos` in `InertialEdges`.

<!-- source: src/engine/2d/solver/SweKernels.hpp:17-47,75-115,119-159,171-229,231-288,290-309; src/engine/2d/solver/ExplicitInertialSolver.cpp:82-121,1005-1111,1453-1497,1790-1909; src/engine/2d/solver/InertialEdges.cpp:141-147; src/engine/2d/solver/InertialKernels.hpp:312-316 -->

### 9.5.11 The diffusive-wave face law and its \f$\Delta x^{2}\f$ bound

Under `MOMENTUM_EQUATION DIFFUSIVE_WAVE` a face firing evaluates the
face depth exactly as the default closure does — (9-11) under `MEAN`,
(9-12) under `VFR_FACE`, a wall at or below `DRY_DEPTH` with the
discharge zeroed — applies the \f$10^{-12}\f$ m deadband of §9.5.3 to the
free-surface difference, and then replaces (9-7) with the quasi-steady
Manning balance,

| | | | |
|---|---|---|---|
| \f[q_{f} = -\,\frac{h_{f}^{5/3}\,S}{n_{f}\,\sqrt{\max(\lvert S \rvert,\ S_{\varepsilon})}}, \qquad S = \frac{\eta_{R} - \eta_{L}}{d_{n}}, \qquad S_{\varepsilon} = \frac{\text{FLUX\_DH\_EPS}}{d_{n}}\f] | | (9-42) | |

with \f$n_f = \tfrac{1}{2}(n_L + n_R)\f$ the face roughness. The floor
\f$S_{\varepsilon}\f$ is what makes an explicit diffusive wave usable:
without it the conductance \f$h^{5/3}/(n\sqrt{\lvert S\rvert})\f$ diverges
as the surface flattens, and the flux across a nearly level pond grows
without bound. Below the floor the discharge is linear in \f$S\f$, which
keeps the C-property — the flux still vanishes with the slope — while
bounding the transmissivity. `FLUX_DH_EPS` is a head difference (default
4 mm), so the floor scales with the face spacing. The conveyance factor,
the positivity share \f$\beta/n_v\f$ of (9-15) and the booking (9-13) are
those of the default closure, and the face discharge is recomputed from
scratch at every firing — nothing is remembered from the previous one.

Substituting (9-42) into (9-1) makes the scheme a nonlinear diffusion
\f$\partial h/\partial t = \nabla\cdot(K\nabla\eta)\f$, \f$K = h^{5/3}/(n\sqrt{\lvert S\rvert})\f$,
and the explicit step of such an equation is bounded by
\f$\Delta t \le L^{2}/(4K)\f$. Per cell,

| | | | |
|---|---|---|---|
| \f[\Delta t_{i} = \alpha\,\frac{L_{char,i}^{2}\,n_{i}\,\sqrt{S_{max,i}}}{4\,h_{i}^{5/3}}, \qquad S_{max,i} = \max\!\left( \max_{f}\lvert S_{f}\rvert,\ \frac{\text{FLUX\_DH\_EPS}}{L_{char,i}} \right)\f] | | (9-43) | |

with \f$\alpha =\f$ `CFL_NUMBER` reused as the safety fraction,
\f$L_{char}\f$ the operator length of (9-4), and \f$S_{max}\f$ the largest
free-surface slope over the cell's faces at the rebuild (walled faces
count as zero); a dry cell is unbounded, and `MAX_TIMESTEP` caps the
result. The slope census is refreshed at every rebuild and every
\f$\Delta t_0\f$ re-minimization. Because the bound scales as \f$L^{2}\f$
while (9-17) scales as \f$L\f$, a fine cell at a coupling point is
punished far more severely under this closure, and the tier spread of
§9.5.6 is what keeps the rest of the mesh from paying for it. No Froude
clamp and no \f$\theta\f$ blend are applied — there is no inertia to damp.

Boundary edges under this closure use the collapsed-Manning conductance
of §9.6 for every type, and `FRONT_REBUILD` stays off by default because
a diffusive front cannot outrun the fixed rebuild cadence (§9.5.7).

Implementation: `src/engine/2d/solver/DiffusiveKernels.hpp` —
`faceDischarge` is (9-42) and `cellDiffusiveDt` is (9-43); the face
firing is @ref openswmm::twoD::ExplicitInertialSolver::fireFacesDiffusive
and the slope census `refreshDiffusiveSlopes` in
`src/engine/2d/solver/ExplicitInertialSolver.cpp`.

<!-- source: src/engine/2d/solver/DiffusiveKernels.hpp:17-39,60-81; src/engine/2d/solver/ExplicitInertialSolver.cpp:123-124,684,716-720,845,865-867,890-909,1113-1160,1523-1527 -->

## 9.6 Boundary conditions

Boundary edges — those claimed by only one cell — default to no-flux
walls. `[2D_BOUNDARY_CONDITIONS]` assigns any of five types per edge:

```
[2D_BOUNDARY_CONDITIONS]
;;TRI  EDGE  TYPE  [PARAM_1  [PARAM_2  [GROUP]]]
```

| Type | Parameter | Meaning |
|---|---|---|
| `WALL` | — | Zero flux (default) |
| `NORMAL_FLOW` | signed bed slope \f$S\f$ | Manning flow \f$q = \mathrm{sign}(S)\,h^{5/3}\sqrt{\lvert S\rvert}/n\f$ per metre of edge; a positive slope falls away from the domain and drains it, a negative one feeds it with the interior depth (it cannot wet a dry cell) |
| `SPECIFIED_STAGE` / `TS_STAGE` | head, or time series | Prescribed free-surface elevation |
| `SPECIFIED_FLOW` / `TS_FLOW` | discharge per metre, or time series | Prescribed unit discharge, outward positive |
| `RATING_CURVE` | curve name | Stage → unit discharge lookup, resolved each step from the boundary cell's stage |

`TRI` is the cell index of §9.3.1 and `EDGE` the local edge — 0..2 for a
triangle, 0..3 for a quad. `PARAM_2` is reserved and is written as `*`;
it must be present whenever `GROUP` is. **`GROUP`** is an optional
label, the sixth token of the row (`*` for none). It is stored with the
row and written back by the `.inp` writer unchanged, and the engine
never acts on it: it exists so that the edges of one shoreline, tidal
reach or outfall line can be addressed together by a user interface or
a script — assigned, re-typed or deleted as a set — without the engine
having an opinion about which edges belong together. A row that writes a
group also writes its `*` placeholder, so the two are never confused.

Time series and curve names are resolved to registry indices once, on
the first advance, and evaluated every routing step thereafter.
Prescribed stages share the mesh's vertical datum and prescribed flows
the project's flow units, so both are converted to SI on the same terms
as the mesh itself (§9.7.4). Under `FACE_RECONSTRUCTION VFR_FACE` every
type conveys with the wetted-edge depth (9-12) of the driving surface
over the boundary edge's own endpoint beds, so a cell whose water pools
away from an outlet edge does not leak through it.

How a boundary edge is integrated depends on the momentum closure.

**Local inertial.** A stage boundary is integrated with the interior
momentum law, not with a conductance. The ghost state holds
\f$\eta = \eta_{bc}\f$ with a zero-gradient discharge, sitting across the edge
at the centroid-to-edge distance \f$d_e\f$ of (9-33) — \f$2A/(3L)\f$ for a
triangle — and (9-7) is applied to it exactly as to an interior face,
followed by the Froude clamp (9-10). The earlier treatment — a collapsed
Manning flux toward the prescribed stage — was a diffusive-wave law
grafted onto an inertial interior, and it showed: its conductance
saturated the equilibrium clamp into a Dirichlet cell, and every
boundary-driven steady case floated one head jump, of order
\f$v^{2}/2g\f$, above the stage it had been given. The bump cases of §9.10
pass because of this change. A per-substep equilibrium clamp remains as
a backstop: one substep may move a cell at most to the prescribed stage,
never past it. At the inertial law's gravity-scale fluxes it rarely
binds; on a tiny cell it prevents overshoot. The other three types apply
their per-metre discharge directly — the Manning law above for
`NORMAL_FLOW`, the prescribed value for `SPECIFIED_FLOW` and
`RATING_CURVE` — and the edge's contribution is added to the cell's
Perot vector (9-16), without which a cell fed through its boundary
carried a systematic \f$(1-\theta)\f$ drag on every face.

**Full shallow water.** Every boundary edge is a ghost-cell Riemann
problem: the ghost state per type is chosen in the outward-normal frame
and (9-36)–(9-39) are evaluated against it, giving both a mass flux and
a momentum change for the cell.

- `WALL` mirrors the normal velocity; the mass flux is zero and the
  pressure balance is closed (§9.5.10).
- `NORMAL_FLOW` prescribes the Manning mass flux above, with the ghost
  velocity \f$q/h_i\f$; a zero slope, depth or roughness conveys nothing and
  the ghost is a mirror — a bare zero-gradient ghost is an absorbing
  boundary and drained a lake at rest through an inert outlet.
- `SPECIFIED_STAGE` holds \f$\eta_{bc}\f$. For subcritical flow one
  characteristic leaves the domain, so its Riemann invariant fixes the
  ghost velocity for the prescribed depth,

| | | | |
|---|---|---|---|
| \f[u_{n,g} = u_{n} + 2\left( \sqrt{g h_{i}} - \sqrt{g h_{g}} \right), \qquad h_{g} = \max(0,\ \eta_{bc} - z_{i})\f] | | (9-44) | |

  rather than a bare copy of the interior velocity; under supercritical
  outflow (\f$u_n \ge \sqrt{g h_i}\f$) no characteristic enters and the
  ghost is transmissive — the prescribed stage cannot act.
- `SPECIFIED_FLOW` and `RATING_CURVE` are flux boundaries: the mass flux
  is the prescribed value verbatim, because the Riemann solve's own
  mass flux is a wave-speed-weighted blend of the interior and
  prescribed states that under-delivered the SWASHES bump inflows by
  7–21 % and reversed a supercritical inlet outright. The ghost still
  sets the momentum flux and keeps the Audusse pressure balance, so its
  depth is the physical boundary depth: the critical depth
  \f$(q^{2}/g)^{1/3}\f$ on a dry cell (dry-bed inflow), the interior depth
  for outflow or supercritical inflow, and for subcritical inflow the
  depth that the outgoing invariant \f$r = u_n + 2\sqrt{g h_i}\f$ and the
  discharge together fix,

| | | | |
|---|---|---|---|
| \f[2\sqrt{g}\,s^{3} - r\,s^{2} + q = 0, \qquad s = \sqrt{h_{g}}\f] | | (9-45) | |

  solved by a safeguarded Newton iteration from the interior depth and
  falling back to it when no physical root converges — a fallback costs
  accuracy in the momentum flux, never conservation.

No equilibrium clamp is applied under this closure — the Riemann flux is
bounded by the two states — and when the availability clamp below
shrinks the requested mass, the momentum already booked is rescaled in
the same ratio.

**Diffusive wave.** Every type is served by the collapsed-Manning
conductance the local-inertial stage boundary retired: for
`SPECIFIED_STAGE` the per-metre flux toward the prescribed stage is
\f$-\,h_{up}^{5/3}\,\mathrm{sign}(\Delta\eta)\,\rho(\lvert\Delta\eta\rvert)/(n\sqrt{d_e})\f$
with \f$h_{up}\f$ the upwind depth and \f$\rho\f$ the regularized square root
of (9-27) with `FLUX_DH_EPS` in place of \f$\varepsilon_o\f$ — the same
\f$C^{1}\f$ quadratic below the floor, the bare root above it — so the
transmissivity stays bounded as the two surfaces level; the other three
types apply their per-metre discharge as above. Under the default
closure `FLUX_DH_EPS` is not consulted at all: the inertial stage law
replaced the only place it acted.

Whatever the closure, exchange is clamped in **volume** space and the
booked flux re-derived from the applied change, so what is reported is
exactly what was applied: the provisional volume is floored at zero, and
under the local-inertial and diffusive laws a stage boundary is also
held at the prescribed stage by the equilibrium clamp. Cumulative
boundary volume is tracked per edge, outward positive, and enters the
2D mass balance of §9.9.

<!-- source: plans/2D_INPUT_FORMAT_SPEC.md:210-235; src/engine/2d/input/SectionHandlers2D.cpp:862-916; src/engine/core/InpWriter.cpp:1025-1045; src/engine/2d/data/BoundaryData.hpp:44-55; src/engine/2d/solver/SurfaceFluxCalculator.cpp:70-83,92-176; src/engine/2d/solver/ExplicitInertialSolver.cpp:1505-1660,1911-2069; src/engine/2d/solver/SweKernels.hpp:313-349; docs/manuals/reference/hydraulics/sections/Chapter9-TwoDimensional.md:880-919 (retained text) -->

## 9.7 Coupling to the one-dimensional network

Coupling is bidirectional and mass-conservative, and it is where a 2D
module earns or loses its credibility. Two mechanisms exist, one for
junctions and one for outfalls.

### 9.7.1 Junction exchange

`[2D_VERTEX_NODE_MAP]` and `[2D_TRIANGLE_NODE_MAP]` associate a mesh
vertex or cell with a SWMM node, with a discharge coefficient \f$C_d\f$
(default 0.65) and an exchange area. Exchange is an orifice law on the
head difference:

| | | | |
|---|---|---|---|
| \f[Q = C_{d}\,A_{eff}\,\mathrm{sign}(\Delta h)\,\sqrt{2g}\ \varphi\!\left(\lvert\Delta h\rvert\right), \qquad \Delta h = h_{2D} - h_{1D}\f] | | (9-18) | |

positive draining the surface into the network. Three regularizations
turn (9-18) from a stiffness source into something an explicit solver
can integrate:

**A bounded square root.** \f$dQ/d\Delta h \to \infty\f$ as \f$\Delta h \to 0\f$
is exactly the regime a fill-and-spill manhole hovers in. Below 2 cm,
\f$\varphi\f$ is a \f$C^1\f$ quadratic matching \f$\sqrt{x}\f$ in value and slope at
the join and having finite slope at zero.

**A capped-pipe gate.** A manhole with its lid on exchanges through the
network only when the higher of the two heads reaches the crown
elevation \f$z_{inv} + D_{full}\f$. A Hermite smoothstep over a 5 cm band
above the crown opens the exchange, and the effective area transitions
smoothly from the inlet area to twice it over the same scale as the node
surcharges.

**Source-side wet/dry ramps.** \f$Q\f$ is multiplied by a smoothstep on the
*source* side's depth relative to `DRY_DEPTH`, so a drain self-limits to
zero as the cell empties and a spill self-limits as the node empties.
This replaces a held-flux availability cap and is what makes the
exchange stable inside the solver's inner loop rather than only across
a window.

The exchange is evaluated **live, at tier-0 cadence**, against the
current 2D heads and the routing step's 1D heads, and \f$\int Q\,dt\f$ is
accumulated exactly per point. Two hard caps make the ledger
unfalsifiable: a drain may take at most \f$\beta\f$ of the source cell's
volume per substep, and a spill draws against a per-node budget of the
node's stored volume for the whole advance — so the same water cannot
spill twice within a routing step.

The 2D head at a coupling point is not simply a cell head. Vertex-coupled
points use a **wet-masked, depth-weighted mean** of the incident cells'
free surfaces under the VFR closure: a manhole vertex is commonly
carved below the surrounding terrain, and a geometric average over
incident cells would read dry-cell bed elevations as a water surface and
pin the exchange at a phantom head from the first step.

Under dynamic wave routing the exchange would otherwise be a
zero-sensitivity explicit source in the node continuity equation, which
churns the Picard iteration. The head sensitivity

| | | | |
|---|---|---|---|
| \f[G = -\frac{\partial Q}{\partial h_{1D}} = C_{d}A_{eff}\sqrt{2g}\ \varphi'\!\left(\lvert\Delta h\rvert\right)\cdot(\text{gate})\cdot(\text{ramp}) \ \ge\ 0\f] | | (9-19) | |

is scattered into the node's \f$\sum dQ/dH\f$ denominator each iteration.
The gate and ramp derivatives are deliberately dropped so the term can
only be positive — a pure damping contribution, never a destabilizing
one.

Exchange volumes reach the 1D side through the lateral-inflow **delivery
queue**, drained at a uniform rate over the batch span rather than as a
single-step pulse.

### 9.7.2 Outfalls

An outfall coupled to the mesh works in both directions.

Outward, the node's net discharge for the routing step is accumulated
and injected into the 2D cells as a constant-rate source over the
subcycle. Withdrawals — a submerged outfall drawing surface water back
into the pipe — are capped by a per-cell budget seeded from the state
the batch started from, so a batch's cumulative withdrawal can never
overdraw it.

Inward, the 2D surface acts as **dynamic tailwater**. The 2D stage at
the coupling point is cached, and the outfall's boundary condition
becomes \f$\max(h_{standard}, h_{2D})\f$, applied inside the dynamic-wave
iteration so it survives every Picard pass. Flap gates are honoured:
the gate decision is made where the current \f$h_{standard}\f$ is visible.

The wet/dry gate here is keyed on depth *in excess of* `DRY_DEPTH`,
not on depth. The reason is specific: a draining cell comes to rest at a
film at or just below `DRY_DEPTH`, which the solver treats as immovable.
A ramp keyed on depth alone would read ≈ 1 at that resting film and pin
the outfall at a tailwater it can never drain below — a deadlock in
which the pipe cannot discharge and the cell cannot dry.

### 9.7.3 Cadence

By default the two domains **co-advance every routing step**: the 2D
solver advances over exactly \f$[t, t+\Delta t]\f$, and exchange volumes
reach the 1D side with at most one routing step of lag. This keeps
fill-and-spill coupling free of the batch-delay ringing that a longer
exchange interval produces at weir and culvert ponds.

`COUPLING_SYNC` batches the 2D advance over a longer span (clamped to
between one routing step and 60 s). It is a wall-clock lever for large
meshes, where per-routing-step advances degenerate into the tail
handling of §9.5.6, and it should be understood as trading accuracy for
speed: the held-exchange error grows with the span.

### 9.7.4 Units

The 2D solver runs internally in SI — metres, m³, m³/s, \f$g = 9.80665\f$.
The 1D engine always computes internally in **feet**, for every project,
US or metric: its reader converts metric input to feet on load and
converts back only at the display boundary. The 1D↔2D coupling factors
are therefore always the feet–metres conversion, independent of
`FLOW_UNITS`.

The mesh is different: it is authored in the project's display length
units, so it is scaled to SI on load for US projects and left alone for
metric ones. A mesh file may declare `;; UNITS: SI (m)` to assert it is
already metric regardless of `FLOW_UNITS`. Boundary stages share the
mesh's datum and scale with it; boundary flows scale with `FLOW_UNITS`.

Getting this wrong is silent and severe — an earlier version tied the
coupling factors to `FLOW_UNITS`, which collapsed them to 1.0 on metric
projects and left every coupled head off by 3.28× and every exchanged
volume off by 35×.

### 9.7.5 The exchange algorithm step by step

**Parameters.** Each coupling point carries the SWMM node index, the
discharge coefficient \f$C_d\f$ (default `COUPLING_CD`, 0.65), the
exchange area \f$A\f$ and, for outfalls, the flap-gate flag. A vertex row
that authors no area defaults to 1.0 in mesh area units (scaled to m²
with the mesh); with `COUPLING_AREA AUTO`, unauthored areas are
derived at resolve time as \f$\mathrm{clamp}(1.25 \times A_{conduit,max},\ 0.05,\ 2.0)\f$ m², where \f$A_{conduit,max}\f$ is the
full-flow area of the largest conduit connected to the node. A
vertex-coupled point records the first triangle incident on its
vertex as its host cell; that cell is pinned to tier 0 (§9.5.6).

**The regularized exchange law.** The pieces of (9-18), with all heads
in the 2D metre frame (\f$h_{1D} = 0.3048 \times\f$ the node head, crown
\f$z_{cr} = 0.3048 \times (z_{inv} + D_{full})\f$, \f$h_{max} = \max(h_{1D}, h_{2D})\f$) and \f$\varepsilon_o = 0.02\f$ m:

| | | | |
|---|---|---|---|
| \f[\varphi(x) = \sqrt{x}\f] | \f$x \ge \varepsilon_{o}\f$ | (9-27a) | |
| \f[\varphi(x) = \frac{3\,x}{2\sqrt{\varepsilon_{o}}} - \frac{x^{2}}{2\,\varepsilon_{o}^{3/2}}\f] | \f$0 \le x < \varepsilon_{o}\f$ | (9-27b) | |

which matches \f$\sqrt{x}\f$ in value and slope at \f$\varepsilon_o\f$ and has
the finite slope \f$3/(2\sqrt{\varepsilon_o})\f$ at zero. The effective
area grows linearly from the inlet area at the crown to twice it 5 cm
above,

| | | | |
|---|---|---|---|
| \f[A_{eff} = A\left\lbrack 1 + \min\!\left( 1,\ \frac{h_{max} - z_{cr}}{0.05} \right) \right\rbrack\f] | \f$h_{max} \ge z_{cr}\f$; else \f$A_{eff} = A\f$ | (9-28) | |

and the capped-pipe gate and the wet/dry ramps multiply the orifice
flow by cubic Hermite smoothsteps over the same scales,

| | | | |
|---|---|---|---|
| \f[Q \leftarrow Q\,\sigma(c), \qquad c = \mathrm{clamp}\!\left( \frac{h_{max} - z_{cr}}{0.05},\ 0,\ 1 \right)\f] | gate | (9-29a) | |
| \f[Q \leftarrow Q\,\sigma\!\left( \mathrm{clamp}\!\left( d_{src}/h_{dry},\ 0,\ 1 \right) \right)\f] | wet/dry ramp | (9-29b) | |

with \f$\sigma(t) = t^2(3 - 2t)\f$ and \f$h_{dry} =\f$ `DRY_DEPTH`. The gate
reads exactly zero at the crown, so a capped node exchanges nothing
until one side surcharges past it. For a drain (\f$Q > 0\f$) the source
depth \f$d_{src}\f$ is the maximum depth over the vertex stencil (or the
single cell for centroid coupling); for a spill it is the node depth,
converted. The driving head \f$h_{2D}\f$ is the wet-masked, depth-weighted
stencil mean under the VFR closure, the pseudo-Laplacian vertex head
under `FLAT`, and the cell head for centroid coupling.

**Caps.** Inside the marcher, at each tier-0 substep of length
\f$\Delta t_0\f$:

| | | | |
|---|---|---|---|
| \f[Q_{drain} \le \frac{\beta \max(V_{c}, 0)}{\Delta t_{0}}\f] | drain | (9-30a) | |
| \f[\lvert Q_{spill} \rvert\,\Delta t_{0} \le V_{node}\,f_{v} - D_{node}\f] | spill | (9-30b) | |

where \f$\beta = 0.8\f$, \f$V_c\f$ is the live coupling-cell volume, \f$f_v\f$ the
ft³-to-m³ factor and \f$D_{node}\f$ the volume already drawn from that
node during the current advance — the same stored water cannot spill
twice within a routing step. The capped \f$Q\,\Delta t_0\f$ is applied
directly to the coupling cell's volume (floored at zero) and
accumulated into the point's ledger \f$\int Q\,dt\f$.

**One routing step, in order:**

1. *Pre-routing.* For every coupled outfall, cache the 2D stage — the
   head of the deepest cell in the vertex stencil, converted to feet —
   and a wet/dry factor \f$\sigma(\mathrm{clamp}((d_{2D} - h_{dry})/h_{dry}, 0, 1))\f$ keyed on depth in excess of `DRY_DEPTH`
   (§9.7.2). The outfall boundary logic applies
   \f$\max(h_{standard}, h_{2D})\f$ inside every dynamic-wave iteration,
   blending by the cached factor and honouring flap gates.
2. *1D routing.* Each coupled junction's head sensitivity \f$G\f$ of
   (9-19) is scattered into the node's \f$\sum dQ/dH\f$ denominator every
   iteration (converted by \f$f_{Q,2D \to 1D} \times f_{L,1D \to 2D}\f$,
   m³/s per m to ft³/s per ft). The previous batch's junction
   exchange volumes drain from the delivery queue as a uniform
   lateral-inflow rate over the batch span.
3. *Post-routing.* The routing step's span joins the pending batch;
   when the pending span reaches the sync span (one routing step by
   default; `COUPLING_SYNC` clamps to between one routing step and
   60 s) the co-advance batch fires:
   1. Save the batch-start state; it seeds the per-cell outfall
      withdrawal budgets.
   2. Accumulate each coupled outfall's net discharge this step,
      \f$Q_{net} = (Q_{in} - Q_{out}) \times f_{Q,1D \to 2D}\f$;
      withdrawals are capped by the remaining budget of the cells the
      point taps. The batch total is injected as a constant-rate
      `coupling_flux` source, scattered over the vertex stencil with
      upwind-HGL weights — downhill cells for a source, uphill for a
      sink — normalized to unity, with the geometric
      partition-of-unity weights as the flat-surface fallback.
   3. Refresh rainfall, evaporation and forcing overrides on a 30 s
      cadence (immediately when the forcing API has marked the state
      dirty); resolve boundary time series and rating curves every
      step.
   4. Advance the marcher over the batch. Junction exchange is
      evaluated live at tier-0 cadence — Eq. (9-18) with
      (9-27)–(9-30) against the current 2D surface and the 1D heads
      frozen at batch start — and applied immediately.
   5. Book the ledgers: each point's \f$\int Q\,dt\f$ converts by
      \f$f_{Q,2D \to 1D}\f$ into the per-node exchange volume; the batch's
      boundary-edge volumes accumulate from the published window-mean
      fluxes; the 2D mass balance then ingests rainfall, evaporation,
      junction, outfall and boundary terms — every one the applied
      (post-cap) volume.
   6. Move the junction volumes to the delivery queue for step 2 of
      the following batch, clear one-shot forcings, and reset the
      window accumulators.

**Forcing override surface.** The C API can replace or augment the
computed exchange per cell: `swmm_2d_force_coupling_flux(engine, idx,
value, mode, persist)` prescribes a coupling rate (m/s, positive into
the 2D domain) with `mode` selecting override or add and `persist`
selecting one-shot or persistent application; `swmm_2d_force_rainfall`
/ `swmm_2d_force_evap` (and their `_uniform` variants) do the same for
the meteorological sources, and `swmm_2d_force_clear_all` clears every
prescription. One-shot prescriptions expire after the step they apply
to; any change marks the forcing state dirty so it takes effect on the
very next batch regardless of the 30 s refresh cadence.

Implementation: the exchange law and its sensitivity are
@ref openswmm::twoD::computeNodeCouplingQ and
@ref openswmm::twoD::computeNodeCouplingDQdh1d
(`src/engine/2d/coupling/NodeCoupling.cpp`, which also holds the
file-local `orificePhi`, `effectiveArea`, `wetVertexEta`,
`scatterCouplingFlux`, `budgetAvail`/`budgetDraw`/`budgetCredit`
helpers); outfall accumulation and tailwater caching are
@ref openswmm::twoD::accumulateOutfallDischargeStep and
@ref openswmm::twoD::updateOutfallBoundaries; the in-marcher exchange
loop is the tier-0 tail of
@ref openswmm::twoD::ExplicitInertialSolver::fireCells; the batch
orchestration and booking are
@ref openswmm::twoD::SurfaceRouter2D::advancePostRouting,
@ref openswmm::twoD::SurfaceRouter2D::coAdvanceStep and
@ref openswmm::twoD::SurfaceRouter2D::accumulateMassBalance; the
forcing entry points are declared in
`include/openswmm/engine/openswmm_2d.h`.

### 9.7.6 Where the spill is booked (`COUPLING_IN_FLOODING`)

The 1D routing continuity table of the status report carries a
**2D Coupling Outflow** row for the water that leaves a coupled junction
onto the mesh, separate from **Flooding Loss**. That split is the
accurate reporting: a coupling transfer is not flooding — the water is
still in the model, on the surface, and returns through the drains and
the coupled outfalls. `COUPLING_IN_FLOODING YES` books the spill into the
Flooding Loss row instead, and exists for one reason: comparing a run
against a build from before the split, where the spill sat inside
Flooding Loss.

The key changes the report only. The spill is an outflow of the 1D
system under either grouping, so the routing continuity error is
identical; every routed volume is identical; and the per-step flooding
rate that drives the flood reporting and the node flooding statistics
keeps the spill in both cases, since those statistics are about water
leaving the node upward, which a spill is whatever row the continuity
table puts it in. The 2D → 1D drain is external inflow under either
setting.

<!-- figure spec: the 1D routing continuity table under COUPLING_IN_FLOODING NO (spill in the "2D Coupling Outflow" row) and YES (spill inside "Flooding Loss"), the same totals and the same continuity error in both; the 2D block of §9.9 unchanged beneath -->
![Figure 9-6](figures/png/hydraulics_ch9_coupling_modes.png)

*Figure 9-6 Where a node spill is booked under COUPLING_IN_FLOODING: the same totals and the same continuity error, in two different rows*

<!-- source: src/engine/2d/input/SectionHandlers2D.cpp:247-253; src/engine/2d/data/SolverOptions2D.hpp:420-429; src/engine/core/SWMMEngine.cpp:4975-4992 -->

## 9.8 Rainfall and evaporation on the mesh

Rainfall reaches the mesh from the project's rain gages, mapped by
`RAINFALL_MODE`: `NATURAL_NEIGHBOUR` (the default) interpolates the
located gages onto every cell centroid with natural-neighbour (Laplace)
weights inside the gages' convex hull and inverse-distance weights
outside it; `SYSTEM` applies the arithmetic mean of all gages uniformly
and is the automatic fallback when no gage has a map location; `NONE`
applies no rain to the mesh. The interpolation — its weights, its
Delaunay construction, its degenerate cases and the way the weights are
built once and applied each step — is hydrology, and
@ref hydrology_ref_ch8_mesh_surface "Hydrology Chapter 8" gives it.
**`NONE` is not an optimization, it is a modelling decision.** If the
project's subcatchments already convert the storm to runoff and deliver
it to nodes, rain on the mesh double-counts the same storm. Use `NONE`
whenever the surface is meant to receive only what the network gives it.

Evaporation on the mesh is governed by `EVAPORATION`:

- **`YES`** (the default) makes evaporation a forcing-only sink: the
  demand rate is zero unless the C API prescribes one per cell or
  uniformly (`swmm_2d_force_evap`, §9.7.5). The project's `[EVAPORATION]`
  section does not reach the mesh.
- **`CLIMATE`** takes the project's climate evaporation rate — the
  same rate the subcatchments see, converted to m/s — as the demand on
  every cell not carrying a forcing override; an override replaces or
  adds to it per cell as the forcing API directs.
- **`NO`** holds the sink at zero and ignores forcing.

Whatever supplies the demand, the sink actually applied is the demand
tapered by the cubic Hermite ramp (9-26) below `DRY_DEPTH`, with
\f$e_{eff} = 0\f$ for a dry cell or a negative demand: a drying cell cannot
evaporate more water than it holds, and negative demand is not a
condensation source. The taper is applied in the same form at every
point the sink enters — the lazy pass over inactive cells and the cell
firing of §9.5.8 — and evaporation is taken first, with infiltration
applied against what is left, so a surface-only project is bitwise
unchanged by the infiltration machinery; both sinks feed a volume update
floored at zero, so the pair cannot leave a cell with negative water.

Infiltration on the mesh — the per-cell methods, the destination of the
infiltrated water (lost, the subcatchment aquifer, or the mesh aquifer)
and its own step — is hydrology and is documented in
@ref hydrology_ref_ch8_mesh_surface "Hydrology Chapter 8"; the mesh
aquifer it can feed is @ref hydrology_ref_ch9_mesh_groundwater
"Hydrology Chapter 9". With `INFILTRATION NO`, or no infiltration rows,
water on the surface leaves only by flowing away, evaporating, or
entering the network.

<!-- source: src/engine/2d/input/SectionHandlers2D.cpp:155-163,347-354; src/engine/2d/data/SolverOptions2D.hpp:143-165,306; src/engine/2d/SurfaceRouter2D.cpp:1376-1400; src/engine/2d/solver/SurfaceFluxCalculator.hpp:66-108; src/engine/2d/solver/ExplicitInertialSolver.cpp:511-516,563-566,1343-1346 -->

## 9.9 Reporting

The 2D domain carries its own mass balance, printed as a separate block
of the status report because its sign conventions are opposite to the 1D
routing balance:

```
  2D Surface Routing Continuity   cubic meters      10^6 ltr
  Initial Stored Volume ....
  Rainfall Inflow ..........
  1D -> 2D Spill Inflow ....
  Outfall Inflow ...........
  Boundary Inflow ..........
  2D -> 1D Drain Outflow ...
  Outfall Withdrawal .......
  Boundary Outflow .........
  Evaporation Loss .........
  Final Stored Volume ......
  Continuity Error (%) .....
```

Volumes are SI. Every term is booked from the volume actually applied —
after every cap, clamp and rescale — so the balance reflects the
volumes the solver actually applied. A mesh that infiltrates, or that
carries an aquifer, gains the rows those processes book; they are
described with the processes in @ref hydrology_ref_ch8_mesh_surface
"Hydrology Chapter 8" and @ref hydrology_ref_ch9_mesh_groundwater
"Hydrology Chapter 9". Where the spill appears in the *1D* table is the
subject of §9.7.6.

A **2D Solver Statistics** block reports cumulative substeps,
face-kernel evaluations, mean and last internal step, the minimum, mean
and maximum active-cell fraction over the rebuild samples, and the
occupancy share of each local-time-stepping tier. Those last two are the
diagnostic for the two performance mechanisms of §9.5.6 and §9.5.7: a
mesh with 100 % active cells is telling you the active set is not
helping, and a tier histogram concentrated in tier 0 is telling you the
same about local time stepping.

Per-cell results — depth, free-surface elevation, velocity, gradients,
maximum-depth and maximum-velocity envelopes, cumulative volume and a
per-cell continuity residual — are refreshed on a report-scale cadence
rather than every routing step, because the six full-mesh passes cost
several times the solver's own advance on a large mesh, and nothing
consumes them faster than the reporting interval. Coupling and outfall
heads read the solver's live state directly, never these derived fields.
`REPORT_2D NO` skips the derived-gradient refresh; it does not suppress
the results file. The reported gradients are Green–Gauss gradients of
the free surface limited by the Jawahar–Kamath (2000) limiter, whose
regularization is `LIMITER_EPSILON`; the limiter affects the output
fields only and enters no solver update.

Two distinct vertex reconstructions exist and must not be confused. The
**solver** field is the pseudo-Laplacian stencil of Kumar et al. (2009),
built once from cell-centre geometry with Lagrange multipliers enforcing
linear exactness; dry cells contribute their bed elevations, which the
solver relies on. The **rendering** field is a wet-masked, depth-weighted
mean of the incident wet cells' free surfaces, with a wetted-contact
gate so that a cell votes at a corner only where its water actually
reaches it. Interpolating the solver field for display would drag water
surfaces up dry banks and down into thin films; interpolating the
rendering field in the solver would break the active-set logic.

**The results file.** With `OUTPUT_FILE` set in `[2D_OPTIONS]`, results
are written to an HDF5 file following the CF-1.11 and UGRID-1.0
conventions for unstructured meshes — mesh topology (mixed topology for
a mesh with quads, §9.3.1), node and face coordinates, bed elevations
and roughness written once, then the selected time-varying fields
appended. The file opens directly in ParaView, QGIS, or any
CF/UGRID-aware reader. Four keys shape it:

- **`OUTPUT_PRECISION`** — `FLOAT32` (the default, halving the file at
  about seven significant digits) or `FLOAT64` for bit-for-bit parity
  tooling. Mesh geometry always stays float64: projected coordinates are
  of order \f$10^{6}\f$ m and single precision would lose centimetres
  there, whereas depth and velocity fields carry no such offset.
- **`OUTPUT_COMPRESSION`** — a deflate level 0–9 applied, with the
  byte-shuffle filter, to the chunked time-varying datasets; 0 writes
  them uncompressed.
- **`REPORT_2D_STEP`** — the cadence of the time-varying datasets, in
  the `HH:MM:SS` spelling of `REPORT_STEP`. It must be a positive
  multiple of `REPORT_STEP`; unset, the 2D file follows the report
  step.
- **`REPORT_2D_VARIABLES`** — which dataset groups are written. The
  value is a preset or a list of the tokens of Table 9-4; `DEPTH` is
  always included, since every reader keys the time axis on it, and a
  group not selected is not created in the file, so readers treat every
  time-varying dataset as optional. `DEFAULT` is everything a user
  renders or plots with the solver diagnostics off; `MINIMAL` is the
  depth map, the render reconstruction and the envelopes; `ALL` is every
  group; `NONE` is `DEPTH` alone.

| Token | Datasets | In `DEFAULT` |
|---|---|---|
| `DEPTH` | `Mesh2_face_depth`, `Mesh2_face_head` | always |
| `VELOCITY` | `Mesh2_face_vx`, `Mesh2_face_vy` | yes |
| `EDGE_FLUX` | `Mesh2_edge_flux` (GUI velocity reconstruction, profile flux) | yes |
| `NODE_HEAD` | `Mesh2_node_head`, `Mesh2_node_depth` (the rendering reconstruction) | yes |
| `SPECIES` | `Mesh2_face_species_conc`, when transport rows exist | yes |
| `RAINFALL` | `Mesh2_face_rainfall`, `Mesh2_face_rain_cum` | yes |
| `INFILTRATION` | `Mesh2_face_infil_rate`, `Mesh2_face_infil_cum` | yes |
| `COUPLING` | `Mesh2_face_coupling_flux`, `Mesh2_face_net_source` | no |
| `GRADIENTS` | `Mesh2_face_grad_hx`, `Mesh2_face_grad_hy` and their limited pair (solver diagnostics) | no |
| `CONTINUITY` | `Mesh2_face_continuity_err` (solver diagnostic) | no |
| `ENVELOPES` | `Mesh2_face_max_depth`, `Mesh2_face_max_velocity`, `Mesh2_face_max_continuity_err` | yes |
| `BUILDUP` | `Mesh2_face_buildup`, when `[2D_COVERAGES]` rows resolved | yes |
| `GROUNDWATER` | the per-cell `Mesh2_face_gw_*` fields, the groundwater ledger and the node-exchange cumulative, when a mesh aquifer resolved | yes |
| `GW_DETAILED` | `Mesh2_face_gw_theta_sigma` per layer and face — \f$m\f$ layers × nFace per step | no |

*Table 9-4 The fourteen `REPORT_2D_VARIABLES` tokens*

**`REPORT_2D_SPECIES`** narrows the `SPECIES` group to a list of species
names (`ALL`, the default, writes every transported species). The
species themselves — pollutants, MSX species, water age and temperature
on the mesh — are the subject of @ref quality_ref_ch10_mesh_quality
"Quality Chapter 10"; the groundwater datasets and the aquifer block of
the status report are described in @ref hydrology_ref_ch9_mesh_groundwater
"Hydrology Chapter 9".

<!-- source: src/engine/2d/data/Report2DVars.hpp:28-36,63-88; src/engine/2d/data/SolverOptions2D.hpp:167-206,255-270; src/engine/2d/input/SectionHandlers2D.cpp:164-170,273-310; src/engine/2d/output/Default2DOutputPlugin.cpp:58-65,229-256; src/engine/2d/SurfaceRouter2D.cpp:48-52; docs/manuals/reference/hydraulics/sections/Chapter9-TwoDimensional.md:1220-1278 (retained text) -->

## 9.10 Verification against analytic solutions

The solver is verified against the SWASHES compilation of analytic
shallow-water solutions (Delestre et al., 2013), with the reference
formulas implemented independently of the engine. The figures below are
the relative \f$L^1\f$ depth error and the mass-balance error over the run;
steady cases are graded on the time mean of the final half of the
simulation.

| Case | SWASHES § | rel. \f$L^1\f$ depth error | mass error | graded against |
|---|---|---|---|---|
| Lake at rest, immersed bump | 3.1.1 | \f$6.3\times10^{-11}\f$ | \f$-7\times10^{-14}\f$ % | analytic |
| Lake at rest, emerged bump | 3.1.2 | \f$1.4\times10^{-16}\f$ | 0 | analytic |
| Subcritical flow over a bump | 3.1.3 | 0.67 % | \f$-6\times10^{-13}\f$ % | analytic |
| MacDonald 1000 m, subcritical | 3.2.1 | 2.0 % | \f$2\times10^{-11}\f$ % | analytic |
| Transcritical, no shock | 3.1.4 | 27 % | \f$-7\times10^{-13}\f$ % | baseline |
| Transcritical with shock | 3.1.5 | 12 % | \f$-8\times10^{-13}\f$ % | baseline |
| Stoker wet-bed dam break | — | 6.2 % | \f$-1\times10^{-12}\f$ % | baseline |
| Ritter dry-bed dam break | — | 10 % | \f$-3\times10^{-12}\f$ % | baseline |
| Thacker planar, 1D | — | 78 % | \f$7\times10^{-13}\f$ % | baseline |
| Thacker radial, 2D | — | 29 % | \f$-7\times10^{-14}\f$ % | baseline |
| Thacker planar, 2D | — | 43 % | \f$7\times10^{-14}\f$ % | baseline |
| MacDonald 1000 m, supercritical | 3.2.1 | 19 % | \f$-2\times10^{-13}\f$ % | expected failure |

Four observations follow.

**Well-balancedness is exact.** Both lake-at-rest cases sit at rounding,
including the emerged bump, which is a wetting and drying problem. The
C-property of §9.5.3 holds exactly.

**Mass conservation is exact.** Every case closes to \f$10^{-11}\f$ % or
better, which is round-off for the volumes involved. This is the
structural guarantee of (9-13) and (9-14), and it holds through wetting,
drying, positivity rescaling and boundary clamping.

**Accuracy is good where the approximation holds and degrades where it
does not.** The subcritical bump and the subcritical MacDonald channel
meet analytic tolerances. The transcritical, dam-break and oscillating
cases are graded against recorded baselines instead, because the
missing convective term is a physics limit rather than a discretization
error that a finer mesh would remove.

**The failure mode is specific and worth recognizing.** On the
subcritical bump, the computed free surface is dead flat over the crest,
where the analytic solution dips. That is not a defect: a flat \f$\eta\f$ is
the *exact* frictionless steady state of (9-2), because the dip is
\f$\Delta(v^{2}/2g)\f$ and there is no \f$q^{2}/h\f$ term to produce it. The
residual 0.7 % error is that dip and nothing else. The supercritical
MacDonald channel is recorded as an expected failure for the same
reason, one order more severely: with no convective inertia the fully
supercritical profile never steadies at all, and develops a roll-wave-like
unsteadiness. If your problem looks like that case, this is not the
solver for it.

## 9.11 Options

All keys live in `[2D_OPTIONS]`. This section is an index, not a
reference: the theory each key selects or tunes is in the section named
beside it, and the accepted values and defaults are documented once, in
the engine manual (@ref engine_manual_sect_2D_OPTIONS), so that a
default changed there is not contradicted here. The 2D grammar as a
whole is @ref engine_manual_ch2_input_file, with the boundary and mesh
sections at @ref engine_manual_sect_2D_BOUNDARY_CONDITIONS,
@ref engine_manual_sect_2D_QUADS and @ref engine_manual_sect_2D_MESH_FILE.

| Key | Theory section |
|---|---|
| `MAX_TIMESTEP` | §9.5.5; also caps the co-advance span of §9.7.3 |
| `DRY_DEPTH` | §9.5.9, Table 9-1 |
| `COUPLING_CD` | §9.7.1, §9.7.5 |
| `COUPLING_SYNC` | §9.7.3 |
| `LIMITER_EPSILON` | §9.9 |
| `FLUX_DH_EPS` | §9.5.11, §9.6 |
| `RAINFALL_MODE` | §9.8; @ref hydrology_ref_ch8_mesh_surface |
| `REPORT_2D` | §9.9 |
| `CELL_CLOSURE` | §9.4, §9.4.2 |
| `FACE_RECONSTRUCTION` | §9.5.2 |
| `VFR_MIN_WET_FRAC` | §9.4.1 |
| `OUTPUT_FILE` | §9.9 |
| `INTEGRATOR` | §9.5; `EXPLICIT` is the only value, the others are retired (below) |
| `MOMENTUM_EQUATION` | §9.2, Table 9-2 |
| `RECONSTRUCTION_ORDER` | §9.5.10, §9.5.6 |
| `FRONT_REBUILD` | §9.5.7 |
| `THETA` | §9.5.1 |
| `CFL_NUMBER` | §9.5.5; clamped to ½ under `FULL_SWE`, §9.5.10; the diffusion safety fraction of §9.5.11 |
| `H_MOVE` | §9.5.7 |
| `LTS_TIERS` | §9.5.6 |
| `FROUDE_MAX` | §9.5.1 |
| `ADVECTION` | deprecated spelling of the convective term — §9.2.3, Table 9-2, and the retired keys below |
| `COUPLING_AREA` | §9.7.5 |
| `COUPLING_IN_FLOODING` | §9.7.6 |
| `BACKEND` | §9.11.1 |
| `OUTPUT_PRECISION` | §9.9 |
| `OUTPUT_COMPRESSION` | §9.9 |
| `REPORT_2D_VARIABLES` | §9.9, Table 9-4 |
| `REPORT_2D_SPECIES` | §9.9; @ref quality_ref_ch10_mesh_quality |
| `REPORT_2D_STEP` | §9.9 |
| `INFILTRATION` | @ref hydrology_ref_ch8_mesh_surface |
| `INFIL_STEP` | @ref hydrology_ref_ch8_mesh_surface |
| `INFIL_DEFAULT_METHOD` | @ref hydrology_ref_ch8_mesh_surface |
| `INFIL_DESTINATION` | @ref hydrology_ref_ch8_mesh_surface; the `AQUIFER_2D` destination is @ref hydrology_ref_ch9_mesh_groundwater |
| `EVAPORATION` | §9.8 |
| `TRANSPORT_POLLUTANTS` | @ref quality_ref_ch10_mesh_quality |
| `TRANSPORT_MSX` | @ref quality_ref_ch10_mesh_quality |
| `TRANSPORT_AGE` | @ref quality_ref_ch10_mesh_quality |
| `TRANSPORT_TEMPERATURE` | @ref quality_ref_ch10_mesh_quality |
| `GROUNDWATER` | @ref hydrology_ref_ch9_mesh_groundwater |
| `GW_ET` | @ref hydrology_ref_ch9_mesh_groundwater |
| `DISPERSION` | @ref quality_ref_ch10_mesh_quality |

*Table 9-5 The forty-two live `[2D_OPTIONS]` keys and the theory each
selects*

### 9.11.1 The computational backend

The computational backend is chosen by `BACKEND`. The built-in `CPU`
marcher is OpenMP-threaded on the host and uses the project's `THREADS`
setting; `OMP`, `CUDA`, `HIP` and `SYCL` name a Kokkos plugin and load it
outright (a plugin that is not installed, or reports no usable device,
falls back to `CPU` with a notice — never a hard failure). `AUTO`, the
default, prefers a device plugin when one is installed and the mesh is
above the device floor (`OPENSWMM_2D_MIN_PARALLEL_CELLS_DEVICE`, default
10 000 cells), then the OpenMP plugin above its own, much higher floor
(`OPENSWMM_2D_MIN_PARALLEL_CELLS`, default 50 000), else `CPU`. The
`OPENSWMM_2D_BACKEND` environment variable overrides the option when set
(the same precedence `OPENSWMM_FV_BACKEND` has over `FV_BACKEND`). The
OpenMP floor is deliberately high — on a 25 000-cell coupled model the
OpenMP plugin measured an order of magnitude *slower* than the built-in
marcher, because the plugin pays a launch cost on every substep while the
built-in path is already threaded. Whether a discrete GPU pays off is
likewise problem-dependent (§9.7.3: every routing step is a host↔device
round trip, and the junction-exchange and boundary passes run serially),
so a model that measures slower on `AUTO` should pin `BACKEND CPU`.

The plugins implement the all-triangle local-inertial scheme without
transport. A model that mixes in quads (§9.3.1), selects `FULL_SWE` or
`DIFFUSIVE_WAVE` (§9.2), or carries species, age or temperature rows is
refused by the plugin loudly — under `AUTO` as well, since a model that
silently lost its plugin would look like a performance regression — and
served by the CPU marcher. There is no silent physics substitution.

### 9.11.2 Retired keys

Keys retired with the earlier implicit integrator are accepted with a
warning and ignored on file load, so legacy models still open; on the
programmatic option-set path they are errors. Each row names where the
behaviour it once configured is now described.

| Key | Theory section |
|---|---|
| `MIN_TIMESTEP` | retired with the CVODE/ARKODE integrators — @ref hydraulics_ref_ch10_planned §10.9; the marcher's step is §9.5.5 |
| `REL_TOLERANCE` | retired with the CVODE/ARKODE integrators — @ref hydraulics_ref_ch10_planned §10.9 |
| `ABS_TOLERANCE` | retired with the CVODE/ARKODE integrators — @ref hydraulics_ref_ch10_planned §10.9 |
| `MAX_CVODE_STEPS` | retired with the CVODE/ARKODE integrators — @ref hydraulics_ref_ch10_planned §10.9 |
| `MAX_KRYLOV_DIM` | retired with the CVODE/ARKODE integrators — @ref hydraulics_ref_ch10_planned §10.9 |
| `LINEAR_SOLVER` | retired with the CVODE/ARKODE integrators — @ref hydraulics_ref_ch10_planned §10.9 |
| `PRECONDITIONER` | retired with the CVODE/ARKODE integrators — @ref hydraulics_ref_ch10_planned §10.9 |
| `JACOBIAN` | retired with the CVODE/ARKODE integrators — @ref hydraulics_ref_ch10_planned §10.9 |
| `ATOL_AREA_REF` | retired with the CVODE/ARKODE integrators — @ref hydraulics_ref_ch10_planned §10.9 |
| `COUPLING_INTERVAL` | superseded by the per-routing-step co-advance and `COUPLING_SYNC` — §9.7.3 |
| `COUPLING_WINDOW` | superseded by the per-routing-step co-advance and `COUPLING_SYNC` — §9.7.3 |
| `ACTIVE_SET` | the flux-active set is always on — §9.5.7 |
| `ACTIVE_SET_HALO` | the halo is the one-ring (five rings under `FRONT_REBUILD`) — §9.5.7 |
| `MOMENTUM` | superseded by `MOMENTUM_EQUATION` — §9.2; the IMEX blend it selected is @ref hydraulics_ref_ch10_planned §10.9 |
| `INTEGRATOR` other than `EXPLICIT` | the CVODE/ARKODE selections — @ref hydraulics_ref_ch10_planned §10.9 |
| `ADVECTION` | deprecated 2026-09-06 in favour of `MOMENTUM_EQUATION FULL_SWE` — §9.2.2, Table 9-2; still honoured with a warning so an old deck keeps its physics |

*Table 9-6 The sixteen retired `[2D_OPTIONS]` keys and values*

<!-- source: src/engine/2d/input/SectionHandlers2D.cpp:111-405 (every handler), 120-125 (DISPERSION), 175-182 (INTEGRATOR), 230-246 (ADVECTION), 393-405 (is2DRetiredOptionKey), 408-427 (is2DOptionKey); src/engine/2d/solver/SurfaceSolverFactory.cpp:280-310; plans/2D_INPUT_FORMAT_SPEC.md:65-123; docs/manuals/reference/hydraulics/sections/Chapter9-TwoDimensional.md:1363-1380 (retained backend text) -->

## 9.12 Limitations

- **No convective acceleration under the default closure.** (9-2) omits
  it. Persistently supercritical flow, drawdown over a crest, and
  momentum-dominated contractions fall outside the local-inertial
  model's validity rather than merely being under-resolved in it. §9.10
  quantifies this; `MOMENTUM_EQUATION FULL_SWE` (§9.2.2) restores the
  term, at the cost of a half-Courant step and CPU-only execution.
- **Infiltration on the mesh is a hydrology process, not a hydraulic
  one.** The face laws of this chapter know nothing of the soil; what
  leaves the surface downward, and where it goes, is set by the
  infiltration options of @ref hydrology_ref_ch8_mesh_surface
  "Hydrology Chapter 8". With infiltration off, water on the surface
  leaves by flowing away, evaporating, or entering the network, and
  losses to the ground must be represented through the subcatchments.
- **The Froude clamp is a numerical device** (local-inertial only). It
  bounds a velocity the momentum equation would otherwise leave
  unbounded. Results that sit on the clamp should not be regarded as
  physically meaningful.
- **Junction exchange is represented as an orifice.** The
  capped-pipe gate and its 5 cm transition band are a smooth
  approximation of grate hydraulics. Where inlet capacity governs, use the storm drain
  inlet models of §7.6 on the 1D side.
- **Geometry is planimetric.** Areas, lengths and normals are computed
  in plan (§9.3).
- **The mesh is fixed.** There is no adaptive refinement; adaptivity is
  in time (§9.5.6), never in space. Mesh quality is the modeller's
  responsibility, and it is not a cosmetic one: cell size enters the
  stable step through (9-4), so a handful of tiny cells at a coupling
  point can set the substep for the entire mesh — and under
  `DIFFUSIVE_WAVE` the penalty is quadratic in the cell size (9-43).
- **Second order excludes transport, and tiers.** `RECONSTRUCTION_ORDER 2`
  runs in global-step mode and falls back to first order when overland
  transport is active (§9.5.10).
- **The Kokkos backends serve one configuration.** All-triangle meshes
  under the local-inertial closure without transport; everything else
  runs on the CPU marcher (§9.11.1).
- **Frozen 1D heads within a batch.** With `COUPLING_SYNC` > 0 the 1D
  heads a 2D advance sees are held for the batch. The error grows with
  the span.
- **Hot start files do not carry 2D state.**

<!-- source: docs/manuals/reference/hydraulics/sections/Chapter9-TwoDimensional.md:1443-1471 (retained text); src/engine/2d/solver/ExplicitInertialSolver.cpp:89-114; src/engine/2d/solver/SurfaceSolverFactory.cpp:286-301; src/engine/2d/input/SectionHandlers2D.cpp:311-346 -->

## 9.13 References for this chapter

Audusse, E., Bouchut, F., Bristeau, M.-O., Klein, R., and Perthame, B.
(2004). "A fast and stable well-balanced scheme with hydrostatic
reconstruction for shallow water flows." *SIAM Journal on Scientific
Computing*, 25(6), 2050–2065.

Barth, T. J., and Jespersen, D. C. (1989). "The design and application
of upwind schemes on unstructured meshes." *AIAA Paper* 89-0366, 27th
Aerospace Sciences Meeting, Reno.

Bates, P. D., Horritt, M. S., and Fewtrell, T. J. (2010). "A simple
inertial formulation of the shallow water equations for efficient
two-dimensional flood inundation modelling." *Journal of Hydrology*,
387(1–2), 33–45.

Begnudelli, L., and Sanders, B. F. (2006). "Unstructured grid
finite-volume algorithm for shallow-water flow and scalar transport with
wetting and drying." *Journal of Hydraulic Engineering*, 132(4),
371–384.

Begnudelli, L., and Sanders, B. F. (2007). "Conservative wetting and
drying methodology for quadrilateral grid finite-volume models."
*Journal of Hydraulic Engineering*, 133(3), 312–322.

Belikov, V. V., Ivanov, V. D., Kontorovich, V. K., Korytnik, S. A., and
Semenov, A. Y. (1997). "The non-Sibsonian interpolation: A new method of
interpolation of the values of a function on an arbitrary set of
points." *Computational Mathematics and Mathematical Physics*, 37(1),
9–15.

Bowyer, A. (1981). "Computing Dirichlet tessellations." *The Computer
Journal*, 24(2), 162–166.

Bruwier, M., Archambeau, P., Erpicum, S., Pirotton, M., and Dewals, B.
(2017). "Shallow-water models with anisotropic porosity and merging for
flood modelling on Cartesian grids." *Journal of Hydrology*, 554,
693–709.

de Almeida, G. A. M., Bates, P., Freer, J. E., and Souvignet, M. (2012).
"Improving the stability of a simple formulation of the shallow water
equations for 2-D flood modeling." *Water Resources Research*, 48,
W05528.

de Almeida, G. A. M., and Bates, P. (2013). "Applicability of the local
inertial approximation of the shallow water equations to flood
modeling." *Water Resources Research*, 49(8), 4833–4844.

Delestre, O., Lucas, C., Ksinant, P.-A., Darboux, F., Laguerre, C., Vo,
T.-N.-T., James, F., and Cordier, S. (2013). "SWASHES: a compilation of
shallow water analytic solutions for hydraulic and environmental
studies." *International Journal for Numerical Methods in Fluids*,
72(3), 269–300.

Gottlieb, S., Shu, C.-W., and Tadmor, E. (2001). "Strong
stability-preserving high-order time discretization methods." *SIAM
Review*, 43(1), 89–112.

Hunter, N. M., Horritt, M. S., Bates, P. D., Wilson, M. D., and Werner,
M. G. F. (2005). "An adaptive time step solution for raster-based
storage cell modelling of floodplain inundation." *Advances in Water
Resources*, 28(9), 975–991.

Jawahar, P., and Kamath, H. (2000). "A high-resolution procedure for
Euler and Navier–Stokes computations on unstructured grids." *Journal of
Computational Physics*, 164(1), 165–203.

Kumar, M., Duffy, C. J., and Salvage, K. M. (2009). "A second-order
accurate, finite volume-based, integrated hydrologic modeling (FIHM)
framework for simulation of surface and subsurface flow." *Vadose Zone
Journal*, 8(4), 873–890.

Liang, Q., and Marche, F. (2009). "Numerical resolution of
well-balanced shallow water equations with complex source terms."
*Advances in Water Resources*, 32(6), 873–884.

Perot, B. (2000). "Conservation properties of unstructured staggered
mesh schemes." *Journal of Computational Physics*, 159(1), 58–89.

Sanders, B. F., Schubert, J. E., and Gallegos, H. A. (2008). "Integral
formulation of shallow-water equations with anisotropic porosity for
urban flood modeling." *Journal of Hydrology*, 362(1–2), 19–38.

Stelling, G. S., and Duinmeijer, S. P. A. (2003). "A staggered
conservative scheme for every Froude number in rapidly varied shallow
water flows." *International Journal for Numerical Methods in Fluids*,
43(12), 1329–1354.

Thacker, W. C. (1981). "Some exact solutions to the nonlinear
shallow-water wave equations." *Journal of Fluid Mechanics*, 107,
499–508.

Toro, E. F. (2001). *Shock-Capturing Methods for Free-Surface Shallow
Flows*. Wiley, Chichester.

Watson, D. F. (1981). "Computing the n-dimensional Delaunay tessellation
with application to Voronoi polytopes." *The Computer Journal*, 24(2),
167–172.

<!-- source: docs/manuals/reference/hydraulics/sections/Chapter9-TwoDimensional.md:1473-1540 (retained entries); new entries cited from src/engine/2d/solver/SweKernels.hpp:17-47 (Audusse, Toro, Liang and Marche), src/engine/2d/solver/DiffusiveKernels.hpp:17-39 (Hunter), src/engine/2d/solver/ExplicitInertialSolver.cpp:1790-1795,1859-1861 (Barth–Jespersen, SSP-RK2), src/engine/2d/solver/InertialKernels.hpp:277-292 (Stelling and Duinmeijer) -->
