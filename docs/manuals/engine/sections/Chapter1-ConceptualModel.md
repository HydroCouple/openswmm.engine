@page engine_manual_ch1_conceptual_model CHAPTER 1 - The Conceptual Model

@tableofcontents

---

This chapter discusses how SWMM models the objects and operational parameters that constitute a stormwater drainage system. Details about how this information is entered into the program are presented in later chapters. An overview is also given on the computational methods that SWMM uses to simulate the hydrology, hydraulics and water quality behavior of a drainage system.

## Introduction

SWMM conceptualizes a drainage system as a series of water and material flows between several major environmental compartments. These compartments and the SWMM objects they contain include:
- The Atmosphere compartment, which generates precipitation and deposits pollutants onto the land surface compartment. SWMM uses Rain Gage objects to represent rainfall inputs to the system.
- The Land Surface compartment, which is represented through one or more Subcatchment objects. It receives precipitation from the Atmospheric compartment in the form of rain or snow; it sends outflow in the form of infiltration to the Groundwater compartment and also as surface runoff and pollutant loadings to the Transport compartment.
- The Groundwater compartment receives infiltration from the Land Surface compartment and transfers a portion of this inflow to the Transport compartment. This compartment is modeled using Aquifer objects.
- The Transport compartment contains a network of conveyance elements (channels, pipes, pumps, and regulators) and storage/treatment units that transport water to outfalls or to treatment facilities. Inflows to this compartment can come from surface runoff, groundwater interflow, sanitary dry weather flow, or from user-defined hydrographs. The components of the Transport compartment are modeled with Node and Link objects

Not all compartments need appear in a particular SWMM model. For example, one could model just the transport compartment, using pre-defined hydrographs as inputs.

Figure 1-1 maps the compartments, the processes inside each of them and the
alternative formulations OpenSWMM offers for those processes. Every
alternative carries its status — Implemented, Experimental, Planned or
Retired — in the same vocabulary the tables of this manual use.

![Figure 1-1 Compartments, processes and the formulations that represent them](figures/png/eng_conceptual_map.png)

*Figure 1-1 Compartments, processes and the formulations that represent them*

<div class="fig-hotspots" data-fig="eng_conceptual_map">
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




## Visual Objects

Figure 1-2 depicts how a collection of OpenSWMM’s visual objects might be arranged together to represent a stormwater drainage system. These objects can be displayed on a map in the SWMM workspace. The following sections describe each of these objects.

 
![Figure 1-2 Objects of an OpenSWMM model](figures/png/eng_object_sketch.png)

*Figure 1-2 Objects of an OpenSWMM model*

<div class="fig-hotspots" data-fig="eng_object_sketch">
<span class="hs" data-box="0.0900,0.0768,0.2400,0.1375">@ref engine_manual_sect_RAINGAGES "Rain gage"</span>
<span class="hs" data-box="0.0400,0.1696,0.2400,0.4911">@ref engine_manual_sect_SUBCATCHMENTS "Subcatchment S1"</span>
<span class="hs" data-box="0.2400,0.0982,0.4400,0.3839">@ref engine_manual_sect_SUBCATCHMENTS "Subcatchment S2"</span>
<span class="hs" data-box="0.1500,0.4018,0.3000,0.5089">@ref engine_manual_sect_JUNCTIONS "Junction"</span>
<span class="hs" data-box="0.3000,0.5446,0.4000,0.6071">@ref engine_manual_sect_CONDUITS "Conduit"</span>
<span class="hs" data-box="0.3900,0.4821,0.5000,0.5893">@ref engine_manual_sect_VIRTUAL_JUNCTIONS "Virtual junction"</span>
<span class="hs" data-box="0.5500,0.3482,0.8200,0.4375">@ref engine_manual_sect_INLET_JUNCTIONS "Inlet junction"</span>
<span class="hs" data-box="0.5900,0.2232,0.6700,0.3482">@ref engine_manual_sect_STREETS "Street conduit"</span>
<span class="hs" data-box="0.7200,0.5089,0.8000,0.6161">@ref engine_manual_sect_DIVIDERS "Divider"</span>
<span class="hs" data-box="0.8100,0.5089,0.9000,0.6429">@ref engine_manual_sect_WEIRS "Weir"</span>
<span class="hs" data-box="0.6900,0.6339,0.8000,0.7232">@ref engine_manual_sect_ORIFICES "Orifice"</span>
<span class="hs" data-box="0.7600,0.7411,0.8400,0.8482">@ref engine_manual_sect_STORAGE "Storage unit"</span>
<span class="hs" data-box="0.8400,0.7411,0.9200,0.8482">@ref engine_manual_sect_PUMPS "Pump"</span>
<span class="hs" data-box="0.9200,0.5982,1.0000,0.6875">@ref engine_manual_sect_OUTFALLS "Outfall"</span>
<span class="hs" data-box="0.4500,0.6696,0.6700,0.9375">@ref engine_manual_sect_2D_TRIANGLES "2D mesh"</span>
<span class="hs" data-box="0.4700,0.5804,0.5600,0.6696">@ref engine_manual_sect_2D_VERTEX_NODE_MAP "1D–2D exchange"</span>
</div>

### Rain Gages

Rain Gages supply precipitation data for one or more subcatchment areas in a study region. The rainfall data can be either a user-defined time series or come from an external file. Several different popular rainfall file formats currently in use are supported, as well as a standard user-defined format. More details on these formats are presented in @ref engine_manual_ch3_files.

The principal input properties of rain gages include:
- rainfall data type (e.g., intensity, volume, or cumulative volume)
- recording time interval (e.g., hourly, 15-minute, etc.)
- source of rainfall data (input time series or external file)
- name of rainfall data source

### Subcatchments

Subcatchments are hydrologic units of land whose topography and drainage system elements direct surface runoff to a single discharge point. The user is responsible for dividing a study area into an appropriate number of subcatchments, and for identifying the outlet point of each subcatchment. Discharge outlet points can be either nodes of the drainage system or other subcatchments.

Subcatchments are divided into pervious and impervious subareas. Surface runoff can infiltrate into the upper soil zone of the pervious subarea, but not through the impervious subarea. Impervious areas are themselves divided into two subareas - one that contains depression storage and another that does not. Runoff flow from one subarea in a subcatchment can be routed to the other subarea, or both subareas can drain to the subcatchment outlet.

Infiltration of rainfall from the pervious area of a subcatchment into the unsaturated upper soil zone can be described using five different models:
- Classic Horton infiltration
- Modified Horton infiltration
- Green-Ampt infiltration
- Modified Green-Ampt infiltration
- SCS Curve Number infiltration

To model the accumulation, re-distribution, and melting of precipitation that falls as snow on a subcatchment, it must be assigned a Snow Pack object. To model groundwater flow between an aquifer underneath the subcatchment and a node of the drainage system, the subcatchment must be assigned a set of Groundwater parameters. Pollutant buildup and washoff from subcatchments are associated with the Land Uses assigned to the subcatchment. Capture and retention of rainfall/runoff using different types of low impact development practices (such as bio-retention cells, infiltration trenches, porous pavement, vegetative swales, and rain barrels) can be modeled by assigning a set of pre-designed LID controls to the subcatchment.

The other principal input parameters for subcatchments include:
- assigned rain gage
- outlet node or subcatchment
- total area
- percent imperviousness area
- average slope
- characteristic width of overland flow
- Manning's roughness (n) for overland flow on both pervious and impervious areas
- depression storage in both pervious and impervious areas
- percent of impervious area with no depression storage.

### Junction Nodes

Junctions are drainage system nodes where links join together. Physically they can represent the confluence of natural surface channels, manholes in a sewer system, or pipe connection fittings. External inflows can enter the system at junctions. Excess water at a junction can become partially pressurized while connecting conduits are surcharged and can either be lost from the system or be allowed to pond atop the junction and subsequently drain back into the junction.

The principal input parameters for a junction are:
- invert (channel or manhole bottom) elevation
- height to ground surface
- ponded surface area when flooded (optional)
- external inflow data (optional).

### Outfall Nodes

Outfalls are terminal nodes of the drainage system used to define final downstream boundaries under Dynamic Wave flow routing. For other types of flow routing they behave as a junction. Only a single link can be connected to an outfall node, and the option exists to have the outfall discharge onto a subcatchment's surface.

The boundary conditions at an outfall can be described by any one of the following stage relationships:
- the critical or normal flow depth in the connecting conduit
- a fixed stage elevation
- a tidal stage described in a table of tide height versus hour of the day
- a user-defined time series of stage versus time.

The principal input parameters for outfalls include:
- invert elevation
- boundary condition type and stage description
- presence of a flap gate to prevent backflow through the outfall.
Flow Divider Nodes

Flow Dividers are drainage system nodes that divert inflows to a specific conduit in a prescribed manner. A flow divider can have no more than two conduit links on its discharge side. Flow dividers are only active under Steady Flow and Kinematic Wave routing and are treated as simple junctions under Dynamic Wave routing.

There are four types of flow dividers, defined by the manner in which inflows are diverted:

Cutoff Divider:    diverts all inflow above a defined cutoff value.
Overflow Divider:    diverts all inflow above the flow capacity of the non-diverted conduit.
Tabular Divider:    uses a table that expresses diverted flow as a function of total inflow.
Weir Divider:    uses a weir equation to compute diverted flow.

The flow diverted through a weir divider is computed by the following equation

\f[
Q_{div} = C_W \left( f H_W \right)^{1.5}
\f]

where Qdiv = diverted flow, Cw = weir coefficient, Hw = weir height and f is computed as

\f[
f = \frac{Q_{in} - Q_{min}}{Q_{max} - Q_{min}}
\f]

where Qin is the inflow to the divider, Qmin is the flow at which diversion begins, and Q_max=C_W H_W^1.5. The user-specified parameters for the weir divider are Qmin, Hw, and Cw.

The principal input parameters for a flow divider are:
- junction parameters (see above)
- name of the link receiving the diverted flow
- method used for computing the amount of diverted flow.

### Storage Units

Storage Units are drainage system nodes that provide storage volume. Physically they could represent storage facilities as small as a catch basin or as large as a lake. The volumetric properties of a storage unit are described by a function or table of surface area versus height. In addition to receiving inflows and discharging outflows to other nodes in the drainage network, storage nodes can also lose water from surface evaporation and from seepage into native soil.

The principal input parameters for storage units include:
- invert (bottom) elevation
- maximum depth
- depth-surface area data
- evaporation potential
- seepage parameters (optional)
- external inflow data (optional).

### Conduits

Conduits are pipes or channels that move water from one node to another in the conveyance system. Their cross-sectional shapes can be selected from a variety of standard open and closed geometries as listed in Table 1-1.

Most open channels can be represented with a rectangular, trapezoidal, or user-defined irregular cross-section shape. For irregular sections a Transect object is used to define how depth varies with distance across the cross-section (see @ref engine_manual_ch1_conceptual_model below). Most new drainage and sewer pipes are circular while culverts typically have elliptical, rectangular or arch shapes. Elliptical and Arch pipes come in standard sizes that are listed in @ref manual_reference_tables and A.13. The Filled Circular shape allows the bottom of a circular pipe to be filled with sediment and thus limit its flow capacity. The Custom Closed Shape allows any closed geometrical shape that is symmetrical about the center line to be defined by supplying a Shape Curve for the cross section (see @ref engine_manual_ch1_conceptual_model below).

SWMM uses the Manning equation to express the relationship between flow rate (Q), cross-sectional area (A), hydraulic radius (R), and slope (S) in all conduits. For standard U.S. units,

\f[
Q = \frac{1.49}{n} A R^{2/3} S^{1/2}
\f]

where n is the Manning roughness coefficient. The slope S is interpreted as either the conduit slope or the friction slope (i.e., head loss per unit length), depending on the flow routing method used. 


Table 1-1 Available cross section shapes for conduits
Name    Parameters    Shape    Name    Parameters    Shape
Circular    Full Height         Circular Force Main    Full Height,
Roughness     
Filled Circular    Full Height,
Filled Depth         Rectangular - Closed    Full Height,
Width     
Rectangular – Open    Full Height,
Width         Trapezoidal    Full Height,
Base Width,
Side Slopes     
Triangular    Full Height,
Top Width         Horizontal Ellipse    Full Height,
Max. Width     
Vertical Ellipse    Full Height,
Max. Width         Arch    Full Height,
Max. Width     
Parabolic    Full Height,
Top Width         Power    Full Height,
Top Width,
Exponent     
Rectangular-Triangular    Full Height,
Top Width,
Triangle Height         Rectangular-Round    Full Height,
Top Width,
Bottom Radius     
Modified Baskethandle    Full Height,
Bottom Width,
Top Radius         Egg    Full Height     
Horseshoe    Full Height         Gothic    Full Height     
Catenary    Full Height         Semi-Elliptical    Full Height     
Baskethandle    Full Height         Semi-Circular    Full Height     
Irregular Channel    Transect Coordinates         Custom Closed Shape    Full Height, Shape Curve
Coordinates      
Street or Roadway    See @ref engine_manual_ch1_conceptual_model         



For pipes with Circular Force Main cross-sections either the Hazen-Williams or Darcy-Weisbach formula is used in place of the Manning equation for fully pressurized flow. For U.S. units the Hazen-Williams formula is:

\f[
Q = 1.318\, C A R^{0.63} S^{0.54}
\f]

where C is the Hazen-Williams C-factor which varies inversely with surface roughness and is supplied as one of the cross-section’s parameters. The Darcy-Weisbach formula is:

\f[
Q = \sqrt{\frac{8g}{f}}\, A R^{1/2} S^{1/2}
\f]

where g is the acceleration of gravity and f is the Darcy-Weisbach friction factor. For turbulent flow, the latter is determined from the height of the roughness elements on the walls of the pipe (supplied as an input parameter) and the flow’s Reynolds Number using the Colebrook-White equation. The choice of which equation to use is a user-supplied option.

- A conduit does not have to be assigned a Force Main shape for it to pressurize. Any of the closed cross-section shapes can potentially pressurize and thus function as force mains that use the Manning equation to compute friction losses.

A constant rate of exfiltration of water along the length of the conduit can be modeled by supplying a Seepage Rate value (in/hr or mm/hr). This only accounts for seepage losses, not infiltration of rainfall dependent groundwater. The latter can be modeled using SWMM’s RDII feature (see @ref engine_manual_ch1_conceptual_model).

A conduit can also be designated to act as a culvert (see Figure 1-4) if a Culvert Inlet Geometry code number is assigned to it. These code numbers are listed in @ref manual_reference_tables. Culvert conduits are checked continuously during dynamic wave flow routing to see if they operate under Inlet Control as defined in the Federal Highway Administration’s publication Hydraulic Design of Highway Culverts Third Edition (Publication No. FHWA-HIF-12-026, April 2012). Under inlet control a culvert obeys a particular flow versus inlet depth rating curve whose shape depends on the culvert’s shape, size, slope, and inlet geometry.

Street and channel conduits with storm drain inlet structures (see Figure 1-5) use the methods described in the Federal Highway Administration's publication Urban Drainage Design Manual - HEC-22 (Publication No. FHWA-NHI-10-009, August 2013) to determine the amount of flow they capture.

 
![Figure 1-4 Concrete box culvert](figures/fig3-02-box-culvert.jpg)

*Figure 1-4 Concrete box culvert*
 
![Figure 1-5 Storm drain inlet](figures/fig3-03-storm-drain-inlet.png)

*Figure 1-5 Storm drain inlet*

The principal input parameters for conduits are:
- names of the inlet and outlet nodes
- offset height or elevation above the inlet and outlet node inverts
- length
- Manning's roughness coefficient (n)
- cross-sectional geometry
- entrance/exit losses (optional)
- seepage rate (optional)
- presence of a flap gate to prevent reverse flow (optional)
- culvert type code number if the conduit acts as a culvert (optional)
- name of any inlet structure placed in a street or channel conduit (optional).


### Pumps

Pumps are links used to lift water to higher elevations. A pump curve describes the relation between a pump's flow rate and conditions at its inlet and outlet nodes. Five different types of pump curves are supported:

Type1 (Fixed/Volume)
Consists of a series of constant flow rates that apply over a series of volume intervals at the pump’s inlet node.     
Type2 (Fixed/Depth)
Similar to a Type1 pump except that the fixed flow rate levels vary over a set of depth intervals at the pump’s inlet node.     
Type3 (Variable/Head)
Uses a pump characteristic curve at some nominal impeller speed to relate flow rate and delivered head.     
Type4 (Variable/Depth)
A variable speed pump where flow varies continuously with inlet node water depth.
     
Type5 (Variable/Affinity)
A variable speed version of the Type3 pump where the pump curve shifts position when control rules change the pump’s relative speed setting (see @ref engine_manual_ch1_conceptual_model).     

SWMM also supports an "Ideal" transfer pump that does not require a pump curve and is used mainly for preliminary analysis. Its flow rate equals the inflow rate to its inlet node no matter what the head difference is between its inlet and outlet nodes. 

The on/off status of pumps can be controlled dynamically by specifying startup and shutoff water depths at the inlet node or through user-defined Control Rules. Rules can also be used to simulate variable speed drives that modulate pump flow. For a Type 5 pump, its operating curve shifts position such that flow changes in direct proportion to the controlled speed setting while head changes in proportion to the setting squared.

The principal input parameters for a pump include:
- names of its inlet and outlet nodes
- name of its pump curve (or * for an Ideal pump)
- initial on/off status
- startup and shutoff depths (optional).

### Flow Regulators

Flow Regulators are structures or devices used to control and divert flows within a conveyance system. They are typically used to:
- control releases from storage facilities
- prevent unacceptable surcharging
- divert flow to treatment facilities and interceptors

SWMM can model the following types of flow regulators: Orifices, Weirs, and Outlets.

### Orifices

Orifices are used to model outlet and diversion structures in drainage systems, which are typically openings in the wall of a manhole, storage facility, or control gate. They are internally represented in SWMM as a link connecting two nodes. An orifice can have either a circular or rectangular shape, be located either at the bottom or along the side of the upstream node, and have a flap gate to prevent backflow.

Orifices can be used as storage unit outlets under all types of flow routing. If not attached to a storage unit node, they can only be used in drainage networks that are analyzed with Dynamic Wave flow routing.

The flow through a fully submerged orifice is computed as
Q=CA√2gh
where Q = flow rate, C = discharge coefficient, A = area of orifice opening, g = acceleration of gravity, and h = head difference across the orifice. The height of an orifice's opening can be controlled dynamically through user-defined Control Rules. This feature can be used to model gate openings and closings. Flow through a partially full orifice is computed using an equivalent weir equation.

The principal input parameters for an orifice include:
- names of its inlet and outlet nodes
- configuration (bottom or side)
- shape (circular or rectangular)
- height or elevation above the inlet node invert
- discharge coefficient
- time to open or close (optional).

### Weirs

Weirs, like orifices, are used to model outlet and diversion structures in a drainage system. Weirs are typically located across a channel, along its side, or at the top of a storage unit. They are internally represented in SWMM as a link connecting two nodes, where the weir itself is placed at the upstream node. A flap gate can be included to prevent backflow.

Five varieties of weirs are available, each incorporating a different formula for computing flow across the weir as listed in Table 1-2.

Table 1-2 Available types of weirs
Weir Type    Cross Section Shape    Flow Formula
Transverse     Rectangular    C_W Lh^(3/2)
Side flow     Rectangular    C_W Lh^(5/3)
V-notch     Triangular    C_W Sh^(5/2)
Trapezoidal    Trapezoidal    C_W Lh^(3/2)+C_WS Sh^(5/2)
Roadway    Rectangular    C_W Lh^(3/2)
Cw = weir discharge coefficient, L = weir length, S = side slope of 
V-notch or trapezoidal weir, h = head difference across the weir,
Cws = discharge coefficient through sides of trapezoidal weir.

The Roadway weir is a broad crested rectangular weir used model roadway crossings usually in conjunction with culvert-type conduits (see Figure 1-4). It uses curves from the Federal Highway Administration publication Hydraulic Design of Highway Culverts Third Edition (Publication No. FHWA-HIF-12-026, April 2012) to determine CW as a function of h and roadway width.

Weirs can be used as storage unit outlets under all types of flow routing. If not attached to a storage unit, they can only be used in drainage networks that are analyzed with Dynamic Wave flow routing.

The height of the weir crest above the inlet node invert can be controlled dynamically through user-defined Control Rules. This feature can be used to model inflatable dams.

Weirs can either be allowed to surcharge or not. A surcharged weir will use an equivalent orifice equation to compute the flow through it. Weirs placed in open channels would normally not be allowed to surcharge while those placed in closed diversion structures or those used to represent storm drain inlet openings would be allowed to.

The principal input parameters for a weir include:
- names of its inlet and outlet nodes
- shape and geometry
- crest height or elevation above the inlet node invert
- discharge coefficient.

### Outlets

Outlets are flow control devices that are typically used to control outflows from storage units. They are used to model special head-discharge relationships that cannot be characterized by pumps, orifices, or weirs. Outlets are internally represented in SWMM as a link connecting two nodes. An outlet can also have a flap gate that restricts flow to only one direction.

Outlets attached to storage units are active under all types of flow routing. If not attached to a storage unit, they can only be used in drainage networks analyzed with Dynamic Wave flow routing.

A user-defined rating curve determines an outlet's discharge flow as a function of either the freeboard depth above the outlet's opening or the head difference across it. Control Rules can be used to dynamically adjust this flow when certain conditions exist.
 
The principal input parameters for an outlet include:
- names of its inlet and outlet nodes
- height or elevation above the inlet node invert
- function or table containing its head (or depth) - discharge relationship.

### Map Labels

Map Labels were optional text annotations carried in the `[LABELS]` section. The engine parses the section and discards it; annotations are a concern of the graphical client, which stores its own. See @ref manual_map_editing.

## Non-Visual Objects

In addition to physical objects that can be displayed visually on a map, SWMM utilizes several classes of non-visual data objects to describe additional characteristics and processes within a study area.

### Climatology

### Temperature

Air temperature data are used when simulating snowfall and snowmelt processes during runoff calculations. They can also be used to compute daily evaporation rates. If these processes are not being simulated then temperature data are not required. Air temperature data can be supplied to SWMM from one of the following sources:
- a user-defined time series of point values (values at intermediate times are interpolated)
- an external climate file containing daily minimum and maximum values (SWMM fits a sinusoidal curve through these values depending on the day of the year).
For user-defined time series, temperatures are in degrees F for US units and degrees C for metric units. The external climate file can also be used to directly supply evaporation and wind speed as well.

### Evaporation

Evaporation can occur for standing water on subcatchment surfaces, for subsurface water in groundwater aquifers, for water traveling through open channels, and for water held in storage units. Evaporation rates can be stated as:
- a single constant value
- a set of monthly average values
- a user-defined time series of values
- values computed from the daily temperatures contained in an external climate file
- daily values read directly from an external climate file.
These values represent potential rates. The actual amount of water evaporated will depend on the amount available.

If rates are read directly from a climate file, then a set of monthly pan coefficients should also be supplied to convert the pan evaporation data to free water-surface values. An option is also available to allow evaporation only during periods with no precipitation.

### Wind Speed

Wind speed is an optional climatic variable that is used only for snowmelt calculations.  SWMM can use either a set of monthly average speeds or wind speed data contained in the same climate file used for daily minimum/maximum temperatures.

### Snowmelt

Snowmelt parameters are climatic variables that apply across the entire study area when simulating snowfall and snowmelt. They include:
- the air temperature at which precipitation falls as snow
- heat exchange properties of the snow surface
- study area elevation, latitude, and longitude correction

### Areal Depletion

Areal depletion refers to the tendency of accumulated snow to melt non-uniformly over the surface of a subcatchment. As the melting process proceeds, the area covered by snow gets reduced. This behavior is described by an Areal Depletion Curve that plots the fraction of total area that remains snow covered against the ratio of the actual snow depth to the depth at which there is 100% snow cover. A typical ADC for a natural area is shown in Figure 1-6. Two such curves can be supplied to SWMM, one for impervious areas and another for pervious areas.

 
![Figure 1-6 Areal depletion curve for a natural area](figures/png/hydrology_ch6_areal_depletion_curve.png)

*Figure 1-6 Areal depletion curve for a natural area*

### Climate Adjustments

Climate Adjustments are optional modifications applied to the temperature, evaporation rate, and rainfall intensity that SWMM would otherwise use at each time step of a simulation. Separate sets of adjustments that vary periodically by month of the year can be assigned to these variables. They provide a simple way to examine the effects of future climate change without having to modify the original climatic time series.

A set of monthly adjustments can also be applied to the hydraulic conductivity used in computing rainfall infiltration on all pervious land surfaces, including those in all LID units, and for exfiltration from all storage nodes and conduits. These can reflect the increase of hydraulic conductivity with increasing temperature or the effect that seasonal changes in land surface conditions, such as frozen ground, can have on infiltration capacity. They can be overridden for individual subcatchments (and their LID units) by assigning a monthly infiltration adjustment Time Pattern to a subcatchment.  Monthly adjustment time patterns for depression storage and pervious surface roughness coefficient (Mannings n) can also be specified for individual subcatchments



### Snow Packs

Snow Pack objects contain parameters that characterize the buildup, removal, and melting of snow over three types of subareas within a subcatchment: 
- The Plowable snow pack area consists of a user-defined fraction of the total impervious area. It is meant to represent such areas as streets and parking lots where plowing and snow removal can be done.
- The Impervious snow pack area covers the remaining impervious area of a subcatchment.
- The Pervious snow pack area encompasses the entire pervious area of a subcatchment.

Each of these three areas is characterized by the following parameters:
- minimum and maximum snow melt coefficients
- minimum air temperature for snow melt to occur
- snow depth above which 100% areal coverage occurs
- initial snow depth
- initial and maximum free water content in the pack.

In addition, a set of snow removal parameters can be assigned to the Plowable area. These parameters consist of the depth at which snow removal begins and the fractions of snow moved onto various other areas.

Subcatchments are assigned a snow pack object through their Snow Pack property. A single snow pack object can be applied to any number of subcatchments. Assigning a snow pack to a subcatchment simply establishes the melt parameters and initial snow conditions for that subcatchment. Internally, SWMM creates a "physical" snow pack for each subcatchment, which tracks snow accumulation and melting for that particular subcatchment based on its snow pack parameters, its amount of pervious and impervious area, and the precipitation history it sees.

### Aquifers

Aquifers are sub-surface groundwater zones used to model the vertical movement of water infiltrating from the subcatchments that lie above them. They also permit the infiltration of groundwater into the drainage system, or exfiltration of surface water from the drainage system, depending on the hydraulic gradient that exists. Aquifers are only required in models that need to explicitly account for the exchange of groundwater with the drainage system or to establish base flow and recession curves in natural channels and non-urban systems. The parameters of an aquifer object can be shared by several subcatchments but there is no exchange of groundwater between subcatchments. A drainage system node can exchange groundwater with more than one subcatchment.

Aquifers are represented using two zones – an un-saturated zone and a saturated zone. Their behavior is characterized using such parameters as soil porosity, hydraulic conductivity, evapotranspiration depth, bottom elevation, and loss rate to deep groundwater. In addition, the initial water table elevation and initial moisture content of the unsaturated zone must be supplied.

Aquifers are connected to subcatchments and to drainage system nodes through a subcatchment's Groundwater Flow property. This property also contains parameters that govern the rate of groundwater flow between the aquifer's saturated zone and the drainage system node. 

### Unit Hydrographs

Unit Hydrographs (UHs) estimate rainfall-dependent infiltration and inflow (RDII) into a sewer system. A UH set contains up to three such hydrographs, one for a short-term response, one for an intermediate-term response, and one for a long-term response. A UH group can have up to 12 UH sets, one for each month of the year. Each UH group is considered as a separate object by SWMM, and is assigned its own unique name along with the name of the rain gage that supplies rainfall data to it.

Each unit hydrograph, as shown in Figure 1-7, is defined by three parameters:
- R: the fraction of rainfall volume that enters the sewer system
- T: the time from the onset of rainfall to the peak of the UH in hours
- K: the ratio of time to recession of the UH to the time to peak

A unit hydrograph can also have a set of Initial Abstraction (IA) parameters associated with it. These determine how much rainfall is lost to interception and depression storage before any excess rainfall is generated and transformed into RDII flow by the hydrograph. The IA parameters consist of:
- a maximum possible depth of IA (inches or mm),
- a recovery rate (inches/day or mm/day) at which stored IA is depleted during dry periods,
- an initial depth of stored IA (inches or mm).

 
![Figure 1-7 An RDII unit hydrograph](figures/png/eng_rdii_unit_hydrograph.png)

*Figure 1-7 An RDII unit hydrograph*

To generate RDII into a drainage system node, the node must identify (through its Inflows property) the UH group and the area of the surrounding sewershed that contributes RDII flow.

- An alternative to using unit hydrographs to define RDII flow is to create an external RDII interface file, which contains RDII time series data. See @ref engine_manual_ch3_files.

- Unit hydrographs could also be used to replace SWMM's main rainfall-runoff process that uses Subcatchment objects, provided that properly calibrated UHs are utilized. In this case what SWMM calls RDII inflow to a node would actually represent overland runoff.

### Transects

Transects refer to the geometric data that describe how bottom elevation varies with horizontal distance over the cross-section of a natural channel or irregular-shaped conduit. Figure 1-8 displays an example transect for a natural channel.

Each transect must be given a unique name. Conduits refer to that name to represent their shape. A special Transect Editor is available for editing the station-elevation data of a transect. SWMM internally converts these data into tables of area, top width, and hydraulic radius versus channel depth. In addition, as shown in Figure 1-8, each transect can have a left and right overbank section whose Manning's roughness coefficient can be different from that of the main channel. This feature can provide more realistic estimates of channel conveyance under high flow conditions.

 
![Figure 1-8 Example of a natural channel transect](figures/fig3-06-natural-transect.jpg)

*Figure 1-8 Example of a natural channel transect*

### Streets

Streets are a specialized form of transect that describes the typical cross-section geometry of a street or roadway. The Figure 1-9 shows a half-street layout along with the dimensions a user needs to provide.

 
![Figure 1-9 Definitional sketch of a street cross-section](figures/png/hydraulics_ch5_street_section.png)

*Figure 1-9 Definitional sketch of a street cross-section*

Each street section object is assigned an ID name that a conduit can refer to for describing its cross-section geometry. A Street Section Editor is available for providing a street section's dimensions and whether it is one-sided or two-sided.
Inlets

Street inlets are curb and gutter openings that convey runoff from streets into below-ground sewers. Drop inlets serve a similar purpose for open rectangular and trapezoidal channels. SWMM can compute the amount of flow captured by inlets and sent to designated sewer nodes using the U.S. Federal Highway Administration’s HEC-22 methodology . The type, sizing, and spacing of street inlets will determine if the spread and depth of water on roadways can be maintained at acceptable levels. 

To analyze street drainage with SWMM a site is represented as a dual drainage system consisting of both street conduits along the ground surface and sewer conduits below ground (see Figure 1-10).  An inlet structure will divert some portion of the street flow it carries into a designated node of the sewer system with the rest bypassed to downstream street conduits. When an inlet’s sewer node reaches its full depth any excess sewer flow that causes it to flood is routed back into the street's downstream node rather than having it leave the system as it normally would.
 
![Figure 1-10 Representation of a dual drainage system](figures/fig3-08-dual-drainage.png)

*Figure 1-10 Representation of a dual drainage system*

As shown in Figure 1-10, inlets can be located either on a continuous sloping section of roadway (on-grade, sometimes referred to as a flow-by condition) or at a low point where flow tends to pool (on-sag, sometimes referred to as a sump condition).

SWMM’s HEC-22 inlet capture equations support the inlet types shown in Figure 1-11. Drop inlets can only be used with open rectangular or trapezoidal channels while the other curb and gutter inlets can only be placed in conduits with Street cross-sections. An additional Custom type of inlet can be used in both streets and channels. Its capture efficiency is described by either a user-supplied Diversion curve (captured flow versus approach flow) or Rating curve (captured flow versus flow depth).
 
![Figure 1-11 HEC-22 inlets supported by SWMM](figures/fig3-09-hec22-inlets.png)

*Figure 1-11 HEC-22 inlets supported by SWMM*

To add an analysis of street inlets to a SWMM project:
- Create one network layout for streets and another for sewers.
- Create a collection of street cross-section objects.
- For each street conduit, set its Shape property to one of the available street sections.
- Create a set of inlet structure design objects.
- Place a particular inlet structure design into a selected street conduit, assigning it a sewer node that receives its captured flow.
- Assign surface runoff from subcatchments or other external inflows to street conduit nodes.
A similar set of steps would be used to add drop inlets into open rectangular or trapezoidal channels. A summary of results for each street conduit (maximum flow depth and pavement spread) and for each inlet (percent capture at peak flow, frequency of bypass flow and frequency of sewer system backflow) will appear as a separate Street Flow table in SWMM's Summary Results report.
Some additional considerations when modeling inlets are:
- A conduit that carries an inlet diverts captured flow to the inlet's capture node; the two are not connected by a link.
- The rim elevations of nodes that receive captured inlet flow do not have to match the invert elevations of the end node of the conduit containing the inlet.
- Two-sided street conduits (that are symmetric about the street crown) use pairs of inlets placed on each curb side of the street.
- Multiple inlets of the same design can be assigned to a conduit (as pairs for two-sided streets). For on-grade placement the flow captured by each inlet is determined sequentially, so that the approach flow to the next inlet in line is the bypass flow from the inlet before it.
- Flow captured by inlets is limited by the amount that its sewer node can receive before it floods. If the node has no such capacity remaining then any excess flow that would cause it to flood is routed back through the inlet and onto the street.
- Users can stipulate whether an inlet operates on-grade or on-sag or have SWMM decide based on the slopes of the conduits adjoining it. (On-sag refers to a sump or low point that all adjoining conduits slope towards.)
- Inlets can have a degree of clogging and a flow capture restriction assigned to them.
- For Kinematic Wave and Steady Flow routing it is recommended that storage nodes be used at the end of inlet conduits that converge at sag points since otherwise any non-captured flow will simply exit the system. This is not necessary for Dynamic Wave routing as any non-captured water will create a backwater effect raising water levels in the adjoining street conduits.

### External Inflows

In addition to inflows originating from subcatchment runoff and groundwater, drainage system nodes can receive three other types of external inflows:
- Direct Inflows - These are user-defined time series of inflows added directly into a node. They can be used to perform flow and water quality routing in the absence of any runoff computations (as in a study area where no subcatchments are defined).
- Dry Weather Inflows - These are continuous inflows that typically reflect the contribution from sanitary sewage in sewer systems or base flows in pipes and stream channels. They are represented by an average inflow rate that can be periodically adjusted on a monthly, daily, and hourly basis by applying Time Pattern multipliers to this average value.
- Rainfall-Dependent Infiltration and Inflow (RDII) - These are stormwater flows that enter sanitary or combined sewers due to "inflow" from direct connections of downspouts, sump pumps, foundation drains, etc. as well as "infiltration" of subsurface water through cracked pipes, leaky joints, poor manhole connections, etc. RDII can be computed for a given rainfall record based on set of triangular unit hydrographs (UH) that determine a short-term, intermediate-term, and long-term inflow response for each time period of rainfall. Any number of UH sets can be supplied for different sewershed areas and different months of the year. RDII flows can also be specified in an external RDII interface file.

Direct, Dry Weather, and RDII inflows are properties associated with each type of drainage system node (junctions, outfalls, flow dividers, and storage units) and can be specified when nodes are edited. They can be used to perform flow and water quality routing in the absence of any runoff computations (as in a study area where no subcatchments are defined). It is also possible to make the outflows generated from an upstream drainage system be the inflows to a downstream system by using interface files. See @ref engine_manual_ch3_files for further details.

### Control Rules

Control Rules determine how pumps and regulators in the drainage system will be adjusted over the course of a simulation. Some examples of these rules are:

Simple time-based pump control:
RULE R1  
IF SIMULATION TIME > 8  
THEN PUMP 12 STATUS = ON  
ELSE PUMP 12 STATUS = OFF

Multiple-condition orifice gate control:
RULE R2A  
IF NODE 23 DEPTH > 12  
AND LINK 165 FLOW > 100  
THEN ORIFICE R55 SETTING = 0.5 
 
RULE R2B  
IF NODE 23 DEPTH > 12  
AND LINK 165 FLOW > 200  
THEN ORIFICE R55 SETTING = 1.0  
 

RULE R2C  
IF NODE 23 DEPTH <= 12  
OR LINK 165 FLOW <= 100  
THEN ORIFICE R55 SETTING = 0  

Pump station operation:
RULE R3A  
IF NODE N1 DEPTH > 5  
THEN PUMP N1A STATUS = ON  
 
RULE R3B  
IF NODE N1 DEPTH > 7  
THEN PUMP N1B STATUS = ON  
 
RULE R3C  
IF NODE N1 DEPTH <= 3  
THEN PUMP N1A STATUS = OFF  
AND PUMP N1B STATUS = OFF

Modulated weir height control:
RULE R4
IF NODE N2 DEPTH >= 0
THEN WEIR W25 SETTING = CURVE C25

@ref manual_hydrology describes the control rule format in more detail and the special Editor used to edit them.

### Pollutants

SWMM can simulate the generation, inflow and transport of any number of user-defined pollutants. Required information for each pollutant includes: 
- pollutant name
- concentration units (i.e., milligrams/liter, micrograms/liter, or counts/liter)
- concentration in rainfall
- concentration in groundwater
- concentration in rainfall-dependent infiltration and inflow
- concentration in dry weather flow
- initial concentration throughout the conveyance system
- first-order decay coefficient.

Co-pollutants can also be defined in SWMM. For example, pollutant X can have a co-pollutant Y, meaning that the runoff concentration of X will have some fixed fraction of the runoff concentration of Y added to it.
Pollutant buildup and washoff from subcatchment areas are determined by the land uses assigned to those areas. Input loadings of pollutants to the drainage system can also originate from external time series inflows as well as from dry weather inflows.

### Land Uses

Land Uses are categories of development activities or land surface characteristics assigned to subcatchments. Examples of land use activities are residential, commercial, industrial, and undeveloped. Land surface characteristics might include rooftops, lawns, paved roads, undisturbed soils, etc. Land uses are used solely to account for spatial variation in pollutant buildup and washoff rates within subcatchments. 

The SWMM user has many options for defining land uses and assigning them to subcatchment areas. One approach is to assign a mix of land uses for each subcatchment, which results in all land uses within the subcatchment having the same pervious and impervious characteristics. Another approach is to create subcatchments that have a single land use classification along with a distinct set of pervious and impervious characteristics that reflects the classification.

The following processes can be defined for each land use category: 
- pollutant buildup
- pollutant washoff
- street cleaning.

### Pollutant Buildup

Pollutant buildup that accumulates within a land use category is described (or “normalized”) by either a mass per unit of subcatchment area or per unit of curb length. Mass is expressed in pounds for US units and kilograms for metric units. The amount of buildup is a function of the number of preceding dry weather days and can be computed using one of the following functions: 

Power Function: Pollutant buildup (B) accumulates proportionally to time (t) raised to some power, until a maximum limit is achieved, 
B=Min(C_1,C_2 t^(C_3 ))
where C1 = maximum buildup possible (mass per unit of area or curb length), C2 = buildup rate constant, and C3 = time exponent. 

Exponential Function: Buildup follows an exponential growth curve that approaches a maximum limit asymptotically, 
B=C_1 (1-e^(-C_2 t))
where C1 = maximum buildup possible (mass per unit of area or curb length) and C2 = buildup rate constant (1/days). 

Saturation Function: Buildup begins at a linear rate that continuously declines with time until a saturation value is reached, 
B=(C_1 t)/(C_2+t)
where C1 = maximum buildup possible (mass per unit area or curb length) and C2 = half-saturation constant (days to reach half of the maximum buildup).

External Time Series: This option allows one to use a Time Series to describe the rate of buildup per day as a function of time. The values placed in the time series would have units of mass per unit area (or curb length) per day. One can also provide a maximum possible buildup (mass per unit area or curb length) with this option and a scaling factor that multiplies the time series values.
 
### Pollutant Washoff

Pollutant washoff from a given land use category occurs during wet weather periods and can be described in one of the following ways: 

Exponential Washoff: The washoff load (W) in units of mass per hour is proportional to the product of runoff raised to some power and to the amount of buildup remaining,
W=C_1 q^(C_2 ) B
where C1 = washoff coefficient, C2 = washoff exponent, q = runoff rate per unit area (inches/hour or mm/hour), and B = pollutant buildup in mass units. The buildup here is the total mass (not per area or curb length) and both buildup and washoff mass units are the same as used to express the pollutant's concentration (milligrams, micrograms, or counts).

Rating Curve Washoff: The rate of washoff W in mass per second is proportional to the runoff rate raised to some power, 
W=C_1 Q^(C_2 )
where C1 = washoff coefficient, C2 = washoff exponent, and Q = runoff rate in user-defined flow units. 
Event Mean Concentration: This is a special case of Rating Curve Washoff where the exponent is 1.0 and the coefficient C1 represents the washoff pollutant concentration in mass per liter (Note: the conversion between user-defined flow units used for runoff and liters is handled internally by SWMM). 

Note that in each case buildup is continuously depleted as washoff proceeds, and washoff ceases when there is no more buildup available.

Washoff loads for a given pollutant and land use category can be reduced by a fixed percentage by specifying a BMP Removal Efficiency that reflects the effectiveness of any BMP controls associated with the land use. It is also possible to use the Event Mean Concentration option by itself, without having to model any pollutant buildup at all. 

### Street Sweeping

Street sweeping can be used on each land use category to periodically reduce the accumulated buildup of specific pollutants. The parameters that describe street sweeping include: 
- days between sweeping
- days since the last sweeping at the start of the simulation
- the fraction of buildup of all pollutants that is available for removal by sweeping
- the fraction of available buildup for each pollutant removed by sweeping
Note that these parameters can be different for each land use, and the last parameter can vary also with pollutant.

### Treatment

Removal of pollutants from the flow streams entering any drainage system node is modeled by assigning a set of treatment functions to the node. A treatment function can be any well-formed mathematical expression involving:
- the pollutant concentration
- the removals of other pollutants
- any of several process variables, such as flow rate, depth, hydraulic residence time, etc.

The result of the treatment function can be either a concentration (denoted by the letter C) or a fractional removal (denoted by R). For example, a first-order decay expression for BOD exiting from a storage node might be expressed as:
C = BOD * exp(-0.05 * HRT)
where HRT is the reserved variable name for hydraulic residence time. The removal of some trace pollutant that is proportional to the removal of total suspended solids (TSS) could be expressed as:
 R = 0.75 * R_TSS
Section C.26 provides more details on how user-defined treatment equations are supplied to the program.

### Curves

Curve objects are used to describe a functional relationship between two quantities. The following types of curves are used in SWMM:
- Storage - describes how the surface area of a Storage Unit node varies with water depth.
- Shape - describes how the width of a customized cross-sectional shape varies with height for a Conduit link.
- Diversion - relates diverted outflow to total inflow for a Flow Divider node or a Custom inlet drain.
- Tidal - describes how the stage at an Outfall node changes by hour of the day.
- Pump - relates flow through a Pump link to the depth or volume of water at the upstream node or to the head delivered by the pump.
- Rating - relates flow through an Outlet link to the freeboard depth or head difference of water across it; relates flow captured by a Custom inlet drain to the depth of water above it.
- Control - determines how the control setting of a pump or flow regulator varies as a function of some control variable (such as water level at a particular node) as specified in a Modulated Control rule.
- Weir – allows a weir’s discharge coefficient to vary with the hydraulic head across it.
Each curve must be given a unique name and can be assigned any number of data pairs.


### Time Series

Time Series objects are used to describe how certain object properties vary with time. Time series can be used to describe: 


- temperature data
- evaporation data
- rainfall data
- water stage at outfall nodes
- external inflow hydrographs at drainage system nodes
- external inflow pollutographs at drainage system nodes
- control settings for pumps and flow regulators..
 
Each time series must be given a unique name and can be assigned any number of time-value data pairs. Time can be specified either as hours from the start of a simulation or as an absolute date and time-of-day. Time series data can either be entered directly into the program or be accessed from a user-supplied Time Series file.

- For rainfall time series, it is only necessary to enter periods with non-zero rainfall amounts. SWMM interprets the rainfall value as a constant value lasting over the recording interval specified for the rain gage that utilizes the time series. For all other types of time series, SWMM uses interpolation to estimate values at times that fall in between the recorded values.
- For times that fall outside the range of the time series, SWMM will use a value of 0 for rainfall and external inflow time series, and either the first or last series value for temperature, evaporation, and water stage time series.

### Time Patterns

Time Patterns allow external Dry Weather Flow (DWF) to vary in a periodic fashion. They consist of a set of adjustment factors applied as multipliers to a baseline DWF flow rate or pollutant concentration. The different types of time patterns include: 
Monthly     - one multiplier for each month of the year  
Daily         - one multiplier for each day of the week  
Hourly         - one multiplier for each hour from 12 AM to 11 PM  
Weekend     - hourly multipliers for weekend days  
Each Time Pattern must have a unique name and there is no limit on the number of patterns that can be created. Each dry weather inflow (either flow or quality) can have up to four patterns associated with it, one for each type listed above.
Monthly time patterns can also be used to adjust the baseline values of the following hydrological parameters:
- subcatchment depression storage
- subcatchment pervious surface roughness
- soil infiltration recovery rate
- groundwater evaporation rate.

### LID Controls

LID Controls are low impact development practices designed to capture surface runoff and provide some combination of detention, infiltration, and evapotranspiration to it. They are considered as properties of a given subcatchment, similar to how Aquifers and Snow Packs are treated. SWMM can explicitly model eight different generic types of LID controls:

- Bio-retention Cells are depressions that contain vegetation grown in an engineered soil mixture placed above a gravel drainage bed. They provide storage, infiltration and evaporation of both direct rainfall and runoff captured from surrounding areas.
- Rain Gardens are a type of bio-retention cell consisting of just the engineered soil layer with no gravel bed below it.

- Green Roofs are another variation of a bio-retention cell that have a soil layer laying atop a special drainage mat material that conveys excess percolated rainfall off of the roof.
- Infiltration Trenches are narrow ditches filled with gravel that intercept runoff from upslope impervious areas. They provide storage volume and additional time for captured runoff to infiltrate the native soil below.

- Continuous Permeable Pavement systems are excavated areas filled with gravel and paved over with a porous concrete or asphalt mix. Block Paver systems consist of impervious paver blocks placed on a sand or pea gravel bed with a gravel storage layer below.
- Rain Barrels (or Cisterns) are containers that collect roof runoff during storm events and can either release or re-use the rainwater during dry periods.

- Rooftop Disconnection has downspouts discharge to pervious landscaped areas and lawns instead of directly into storm drains. It can also model roofs with directly connected drains that overflow onto pervious areas.
- Vegetative Swales are channels or depressed areas with sloping sides covered with grass and other vegetation. They slow down the conveyance of collected runoff and allow it more time to infiltrate the native soil beneath it.
Bio-retention cells, infiltration trenches, and permeable pavement systems can contain optional drain systems in their gravel storage beds to convey excess captured runoff off of the site and prevent the unit from flooding. They can also have an impermeable floor or liner that prevents any infiltration into the native soil from occurring. Infiltration trenches and permeable pavement systems can also be subjected to a decrease in hydraulic conductivity over time due to clogging. 
LID units that contain drains can have a removal percentage assigned to each pollutant discharged through the drain. LID’s will also provide a reduction in pollutant mass load conveyed in their surface discharge due to the reduction in runoff flow volume they provide. 

There are two different approaches for placing LID controls within a subcatchment:
- place one or more controls in an existing subcatchment that will displace an equal amount of non-LID area from the subcatchment
- create a new subcatchment devoted entirely to just a single LID practice.

The first approach allows a mix of LIDs to be placed into a subcatchment, each treating a different portion of the runoff generated from the non-LID fraction of the subcatchment. Note that under this option the subcatchment's LIDs act in parallel -- it is not possible to make them act in series (i.e., have the outflow from one LID control become the inflow to another LID). Also, after LID placement the subcatchment's Percent Impervious and Width properties may require adjustment to compensate for the amount of original subcatchment area that has now been replaced by LIDs (see Figure 1-12 below). For example, suppose that a subcatchment which is 40% impervious has 75% of that area converted to a permeable pavement LID. After the LID is added the subcatchment's percent imperviousness should be changed to the percent of impervious area remaining divided by the percent of non-LID area remaining. This works out to (1 - 0.75)*40 / (100 - 0.75*40) or 14.3 %.

 
![Figure 1-12 Adjustment of subcatchment parameters after LID placement](figures/fig3-10-lid-adjustment.png)

*Figure 1-12 Adjustment of subcatchment parameters after LID placement*

Under this first approach the runoff available for capture by the subcatchment's LIDs is the runoff generated from its impervious area. If the option to re-route some fraction of this runoff to the pervious area is exercised, then only the remaining impervious runoff (if any) will be available for LID treatment. Also note that green roofs and roof disconnection only treat the precipitation that falls directly on them and do not capture runoff from other impervious areas in their subcatchment.

The second approach allows LID controls to be strung along in series and also allows runoff from several different upstream subcatchments to be routed onto the LID subcatchment. If these single-LID subcatchments are carved out of existing subcatchments, then once again some adjustment of the Percent Impervious, Width and also the Area properties of the latter may be necessary. In addition, whenever an LID occupies the entire subcatchment the values assigned to the subcatchment's standard surface properties (such as imperviousness, slope, roughness, etc.) are overridden by those that pertain to the LID unit.


## Computational Methods

SWMM is a physically based, discrete-time simulation model. It employs principles of conservation of mass, energy, and momentum wherever appropriate. This section briefly describes the methods SWMM uses to model stormwater runoff quantity and quality through the following physical processes:
- surface runoff
- infiltration
- groundwater
- snowmelt
- surface ponding
- flow routing
- water quality routing
- low impact development

The theory and numerical methods behind each are in the three OpenSWMM
reference manuals: @ref hydrology_reference_manual,
@ref hydraulics_reference_manual and @ref quality_reference_manual. Figure
1-3 shows how the processes connect, from precipitation on the subcatchments
and on the 2D mesh through the subsurface to routing, transport and the
outfalls.

![Figure 1-3 Processes modelled by OpenSWMM](figures/png/eng_process_flow.png)

*Figure 1-3 Processes modelled by OpenSWMM*

<div class="fig-hotspots" data-fig="eng_process_flow">
<span class="hs" data-box="0.0200,0.1111,0.2400,0.1389">@ref hydrology_ref_ch2_meteorology "Precipitation"</span>
<span class="hs" data-box="0.0260,0.1389,0.0915,0.1612">@ref engine_manual_sect_RAINGAGES "rain · snow"</span>
<span class="hs" data-box="0.0960,0.1389,0.1717,0.1612">@ref hydrology_ref_ch2_meteorology "scale factors"</span>
<span class="hs" data-box="0.0200,0.2153,0.2400,0.2431">@ref hydrology_ref_ch6_snowmelt "Snowmelt"</span>
<span class="hs" data-box="0.0260,0.2431,0.1787,0.2653">@ref hydrology_ref_ch6_snowmelt "degree-day · areal depletion"</span>
<span class="hs" data-box="0.0200,0.3194,0.2400,0.3472">@ref hydrology_ref_ch3_surface_runoff "Initial abstraction · evaporation"</span>
<span class="hs" data-box="0.0260,0.3472,0.1274,0.3695">@ref engine_manual_sect_SUBAREAS "depression storage"</span>
<span class="hs" data-box="0.0200,0.4236,0.2400,0.4514">@ref hydrology_ref_ch3_surface_runoff "Surface runoff"</span>
<span class="hs" data-box="0.0260,0.4514,0.1325,0.4737">@ref hydrology_ref_ch3_surface_runoff "nonlinear reservoir"</span>
<span class="hs" data-box="0.0260,0.4799,0.1582,0.5022">@ref hydrology_ref_ch4_infiltration "Horton · Green-Ampt · CN"</span>
<span class="hs" data-box="0.0200,0.5486,0.2400,0.5764">@ref quality_ref_ch6_lid_controls "LID controls"</span>
<span class="hs" data-box="0.0260,0.5764,0.1017,0.5987">@ref quality_ref_ch6_lid_controls "layered units"</span>
<span class="hs" data-box="0.1062,0.5764,0.1820,0.5987">@ref quality_ref_ch6_lid_controls "storage nodes"</span>
<span class="hs" data-box="0.0200,0.6528,0.2400,0.6806">@ref quality_ref_ch4_surface_washoff "Washoff"</span>
<span class="hs" data-box="0.0260,0.6806,0.1685,0.7028">@ref quality_ref_ch3_pollutant_buildup "EMC · exponential · rating"</span>
<span class="hs" data-box="0.2800,0.2639,0.5200,0.2917">@ref hydrology_ref_ch5_groundwater "Groundwater"</span>
<span class="hs" data-box="0.2860,0.2917,0.4644,0.3139">@ref hydrology_ref_ch5_groundwater "two-zone aquifer per subcatchment"</span>
<span class="hs" data-box="0.2860,0.3202,0.4439,0.3425">@ref hydrology_ref_ch9_mesh_groundwater "two-layer aquifer on the mesh"</span>
<span class="hs" data-box="0.2860,0.3487,0.4028,0.3710">@ref hydrology_ref_ch9_mesh_groundwater "groundwater transport"</span>
<span class="hs" data-box="0.2800,0.6528,0.4800,0.6806">@ref quality_ref_ch3_pollutant_buildup "Pollutant buildup"</span>
<span class="hs" data-box="0.2860,0.6806,0.3977,0.7028">@ref quality_ref_ch3_pollutant_buildup "land uses · sweeping"</span>
<span class="hs" data-box="0.5600,0.1111,0.7800,0.1389">@ref hydrology_ref_ch8_mesh_surface "Rain on the mesh"</span>
<span class="hs" data-box="0.5660,0.1389,0.6623,0.1612">@ref hydrology_ref_ch8_mesh_surface "natural neighbour"</span>
<span class="hs" data-box="0.5600,0.2153,0.7800,0.2431">@ref hydraulics_ref_ch9_two_dimensional "2D overland flow"</span>
<span class="hs" data-box="0.5660,0.2431,0.6469,0.2653">@ref hydraulics_ref_ch9_two_dimensional "local inertial"</span>
<span class="hs" data-box="0.6514,0.2431,0.7528,0.2653">@ref hydraulics_ref_ch9_two_dimensional "full shallow water"</span>
<span class="hs" data-box="0.5660,0.2716,0.6469,0.2939">@ref hydraulics_ref_ch9_two_dimensional "diffusive wave"</span>
<span class="hs" data-box="0.5600,0.3472,0.7800,0.3750">@ref hydrology_ref_ch8_mesh_surface "Per-cell infiltration · evaporation"</span>
<span class="hs" data-box="0.5660,0.3750,0.7598,0.3973">@ref engine_manual_sect_2D_INFILTRATION_OPTIONS "→ lost · subcatchment · mesh aquifer"</span>
<span class="hs" data-box="0.5600,0.4583,0.7800,0.4861">@ref quality_ref_ch10_mesh_quality "2D surface quality"</span>
<span class="hs" data-box="0.5660,0.4861,0.7239,0.5084">@ref quality_ref_ch10_mesh_quality "coverages · buildup · washoff"</span>
<span class="hs" data-box="0.8100,0.1111,0.9800,0.1389">@ref engine_manual_sect_INFLOWS "External inflows"</span>
<span class="hs" data-box="0.8160,0.1389,0.9071,0.1612">@ref engine_manual_sect_DWF "dry weather flow"</span>
<span class="hs" data-box="0.9116,0.1389,0.9668,0.1612">@ref hydrology_ref_ch7_rdii "RDII: RTK"</span>
<span class="hs" data-box="0.8160,0.1674,0.8969,0.1897">@ref hydrology_ref_ch7_rdii "RDII: IA decay"</span>
<span class="hs" data-box="0.8160,0.1959,0.9379,0.2182">@ref engine_manual_sect_INFLOWS "user · interface files"</span>
<span class="hs" data-box="0.8600,0.2778,0.9800,0.3056">@ref hydraulics_ref_ch7_advanced_features "Street inlets"</span>
<span class="hs" data-box="0.8660,0.3056,0.9828,0.3278">@ref hydraulics_ref_ch7_advanced_features "HEC-22 link attribute"</span>
<span class="hs" data-box="0.8660,0.3341,0.9469,0.3564">@ref hydraulics_ref_ch7_advanced_features "inlet junction"</span>
<span class="hs" data-box="0.0200,0.7639,0.9800,0.7917">@ref hydraulics_ref_ch2_hydraulic_model "Channel, pipe and storage routing"</span>
<span class="hs" data-box="0.0260,0.7917,0.0658,0.8139">@ref hydraulics_ref_ch2_hydraulic_model "steady"</span>
<span class="hs" data-box="0.0703,0.7917,0.1512,0.8139">@ref hydraulics_ref_ch4_kinematic_wave "kinematic wave"</span>
<span class="hs" data-box="0.1557,0.7917,0.2263,0.8139">@ref hydraulics_ref_ch3_dynamic_wave "dynamic wave"</span>
<span class="hs" data-box="0.2308,0.7917,0.3065,0.8139">@ref hydraulics_ref_ch8_finite_volume "finite volume"</span>
<span class="hs" data-box="0.3110,0.7917,0.4637,0.8139">@ref hydraulics_ref_ch3_dynamic_wave "EXTRAN · slot · dynamic slot"</span>
<span class="hs" data-box="0.4682,0.7917,0.4926,0.8139">@ref hydraulics_ref_ch3_dynamic_wave "TPA"</span>
<span class="hs" data-box="0.4971,0.7917,0.6345,0.8139">@ref hydraulics_ref_ch3_dynamic_wave "virtual · inlet junctions"</span>
<span class="hs" data-box="0.0200,0.8611,0.9800,0.8889">@ref quality_ref_ch5_transport_treatment "Transport and reactions"</span>
<span class="hs" data-box="0.0260,0.8889,0.0555,0.9112">@ref quality_ref_ch5_transport_treatment "CSTR"</span>
<span class="hs" data-box="0.0600,0.8889,0.0844,0.9112">@ref quality_ref_ch7_ard_transport "ARD"</span>
<span class="hs" data-box="0.0889,0.8889,0.1185,0.9112">@ref quality_ref_ch7_ard_transport "LARD"</span>
<span class="hs" data-box="0.1230,0.8889,0.2192,0.9112">@ref quality_ref_ch5_transport_treatment "first-order decay"</span>
<span class="hs" data-box="0.2237,0.8889,0.2481,0.9112">@ref quality_ref_ch8_msx_reactions "MSX"</span>
<span class="hs" data-box="0.2526,0.8889,0.3078,0.9112">@ref quality_ref_ch9_age_heat "water age"</span>
<span class="hs" data-box="0.3123,0.8889,0.3419,0.9112">@ref quality_ref_ch9_age_heat "heat"</span>
<span class="hs" data-box="0.3464,0.8889,0.3964,0.9112">@ref quality_ref_ch11_planned "sediment"</span>
<span class="hs" data-box="0.4009,0.8889,0.5177,0.9112">@ref engine_manual_sect_TREATMENT "treatment · diversion"</span>
<span class="hs" data-box="0.0200,0.9472,0.9800,0.9861">@ref engine_manual_sect_OUTFALLS "Outfalls"</span>
</div>

### Surface Runoff

The conceptual view of surface runoff used by SWMM is illustrated in Figure 1-13 below. Each subcatchment surface is treated as a nonlinear reservoir. Inflow comes from precipitation and any designated upstream subcatchments. There are several outflows, including infiltration, evaporation, and surface runoff. The capacity of this "reservoir" is the maximum depression storage, which is the maximum surface storage provided by ponding, surface wetting, and interception. Surface runoff per unit area occurs only when the depth of water in the "reservoir" exceeds the maximum depression storage, ds, in which case the outflow is given by Manning's equation. Depth of water over the subcatchment (d) is continuously updated with time by solving numerically a water balance equation over the subcatchment.


 
![Figure 1-13 Conceptual view of surface runoff](figures/png/hydrology_ch3_nonlinear_reservoir.png)

*Figure 1-13 Conceptual view of surface runoff*

### Infiltration

Infiltration is the process of rainfall penetrating the ground surface into the unsaturated soil zone of pervious subcatchments areas. SWMM offers four choices for modeling infiltration: 

Horton's Method 
This method is based on empirical observations showing that infiltration decreases exponentially from an initial maximum rate to some minimum rate over the course of a long rainfall event. Input parameters required by this method include the maximum and minimum infiltration rates, a decay coefficient that describes how fast the rate decreases over time, and a time it takes a fully saturated soil to completely dry. 

Modified Horton Method
This is a modified version of the classical Horton Method that uses the cumulative infiltration in excess of the minimum rate as its state variable (instead of time along the Horton curve),    providing a more accurate infiltration estimate when low rainfall intensities occur. It uses the same input parameters as does the traditional Horton Method.

Green-Ampt Method 
This method for modeling infiltration assumes that a sharp wetting front exists in the soil column, separating soil with some initial moisture content below from saturated soil above. The input parameters required are the initial moisture deficit of the soil, the soil's hydraulic conductivity, and the suction head at the wetting front. The recovery rate of moisture deficit during dry periods is empirically related to the hydraulic conductivity.

Modified Green-Ampt Method
This method modifies the original Green-Ampt procedure by not depleting moisture deficit in the top surface layer of soil during initial periods of low rainfall as was done in the original method. This change can produce more realistic infiltration behavior for storms with long initial periods where the rainfall intensity is below the soil’s saturated hydraulic conductivity.

Curve Number Method 
This approach is adopted from the NRCS (SCS) Curve Number method for estimating runoff. It assumes that the total infiltration capacity of a soil can be found from the soil's tabulated Curve Number. During a rain event this capacity is depleted as a function of cumulative rainfall and remaining capacity. The input parameters for this method are the curve number and the time it takes a fully saturated soil to completely dry.

SWMM also allows the infiltration recovery rate to be adjusted by a fixed amount on a monthly basis to account for seasonal variation in such factors as evaporation rates and groundwater levels. This optional monthly soil recovery pattern is specified as part of a project's Evaporation data.

### Groundwater

Figure 1-14 is a definitional sketch of the two-zone groundwater model that is used in SWMM. The upper zone is unsaturated with a variable moisture content of . The lower zone is fully saturated and therefore its moisture content is fixed at the soil porosity . The fluxes shown in the figure, expressed as volume per unit area per unit time, consist of the following:

 
![Figure 1-14 Two-zone groundwater model](figures/png/hydrology_ch5_two_zone.png)

*Figure 1-14 Two-zone groundwater model*

fI    infiltration from the surface 
fE     evapotranspiration from the upper zone which is a fixed fraction of the un-used surface evaporation
fU      percolation from the upper to lower zone which depends on the upper zone moisture content  and depth dU
fEL    evapotranspiration from the lower zone, which is a function of the depth of the upper zone dU 
fL     seepage from the lower zone to deep groundwater which depends on the lower zone depth dL
fG     lateral groundwater interflow to the drainage system, which depends on the lower zone depth dL as well as the depth in the receiving channel or node.

After computing the water fluxes that exist during a given time step, a mass balance is written for the change in water volume stored in each zone so that a new water table depth and unsaturated zone moisture content can be computed for the next time step.

### Snowmelt

The snowmelt routine in SWMM is a part of the runoff modeling process. It updates the state of the snow packs associated with each subcatchment by accounting for snow accumulation, snow redistribution by areal depletion and removal operations, and snow melt via heat budget accounting. Any snowmelt coming off the pack is treated as an additional rainfall input onto the subcatchment.

At each runoff time step the following computations are made:
- Air temperature and melt coefficients are updated according to the calendar date.
- Any precipitation that falls as snow is added to the snow pack.
- Any excess snow depth on the plowable area of the pack is redistributed according to the removal parameters established for the pack.
- Areal coverage of snow on the impervious and pervious areas of the pack is reduced according to the Areal Depletion Curves defined for the study area.
- The amount of snow in the pack that melts to liquid water is found using:
- a heat budget equation for periods with rainfall, where melt rate increases with increasing air temperature, wind speed, and rainfall intensity
- a degree-day equation for periods with no rainfall, where melt rate equals the product of a melt coefficient and the difference between the air temperature and the pack's base melt temperature.
- If no melting occurs, the pack temperature is adjusted up or down based on the product of the difference between current and past air temperatures and an adjusted melt coefficient. If melting occurs, the temperature of the pack is increased by the equivalent heat content of the melted snow, up to the base melt temperature. Any remaining melt liquid beyond this is available to runoff from the pack.
- The available snowmelt is then reduced by the amount of free water holding capacity remaining in the pack. The remaining melt is treated the same as an additional rainfall input onto the subcatchment.

### Flow Routing

Flow routing within a conduit link in SWMM is governed by the conservation of mass and momentum equations for gradually varied, unsteady flow (i.e., the Saint Venant flow equations). The SWMM user has a choice on the level of sophistication used to solve these equations: 

- Steady Flow Routing
- Kinematic Wave Routing
- Dynamic Wave Routing

Each of these routing methods employs the Manning equation to relate flow rate to flow depth and bed (or friction) slope. For user-designated Force Main conduits, either the Hazen-Williams or Darcy-Weisbach equation can be used when pressurized flow occurs.

### Steady Flow Routing

Steady Flow routing represents the simplest type of routing possible (actually no routing) by assuming that within each computational time step flow is uniform and steady. Thus it simply translates inflow hydrographs at the upstream end of the conduit to the downstream end, with no delay or change in shape. The normal flow equation is used to relate flow rate to flow area (or depth). 

This type of routing cannot account for channel storage, backwater effects, entrance/exit losses, flow reversal or pressurized flow. It can only be used with dendritic conveyance networks, where each node has only a single outflow link (unless the node is a divider in which case two outflow links are required). This form of routing is insensitive to the time step employed and is really only appropriate for preliminary analysis using long-term continuous simulations.

### Kinematic Wave Routing

This routing method solves the continuity equation along with a simplified form of the momentum equation in each conduit. The latter assumes that the slope of the water surface equal the slope of the conduit.

The maximum flow that can be conveyed through a conduit is the full normal flow value. Any flow in excess of this entering the inlet node is either lost from the system or can pond atop the inlet node and be re-introduced into the conduit as capacity becomes available.

Kinematic wave routing allows flow and area to vary both spatially and temporally within a conduit. This can result in attenuated and delayed outflow hydrographs as inflow is routed through the channel. However this form of routing cannot account for backwater effects, entrance/exit losses, flow reversal, or pressurized flow, and is also restricted to dendritic network layouts. It can usually maintain numerical stability with moderately large time steps, on the order of 1 to 5 minutes. If the aforementioned effects are not expected to be significant then this alternative can be an accurate and efficient routing method, especially for long-term simulations.

### Dynamic Wave Routing

Dynamic Wave routing solves the complete one-dimensional Saint Venant flow equations and therefore produces the most theoretically accurate results. These equations consist of the continuity and momentum equations for conduits and a volume continuity equation at nodes. 

With this form of routing it is possible to represent pressurized flow when a closed conduit becomes full, such that flows can exceed the full normal flow value. Flooding occurs when the water depth at a node exceeds the maximum available depth, and the excess flow is either lost from the system or can pond atop the node and re-enter the drainage system.

Dynamic wave routing can account for channel storage, backwater, entrance/exit losses, flow reversal, and pressurized flow. Because it couples together the solution for both water levels at nodes and flow in conduits it can be applied to any general network layout, even those containing multiple downstream diversions and loops. It is the method of choice for systems subjected to significant backwater effects due to downstream flow restrictions and with flow regulation via weirs and orifices. This generality comes at a price of having to use much smaller time steps, on the order of a thirty seconds or less (SWMM can automatically reduce the user-defined maximum time step as needed to maintain numerical stability).

### Ponding and Pressurization

Normally in flow routing, when the flow into a junction exceeds the capacity of the system to transport it further downstream, the excess volume overflows the system and is lost. An option exists to have instead the excess volume be stored atop the junction, in a ponded fashion, and be reintroduced into the system as capacity permits. Under Steady and Kinematic Wave flow routing, the ponded water is stored simply as an excess volume. For Dynamic Wave routing, which is influenced by the water depths maintained at nodes, the excess volume is assumed to pond over the node with a constant surface area. This amount of surface area is an input parameter supplied for the junction.

Alternatively, the user may wish to represent the surface overflow system explicitly. In open channel systems this can include road overflows at bridges or culvert crossings as well as additional floodplain storage areas. In closed conduit systems, surface overflows may be conveyed down streets, alleys, or other surface routes to the next available stormwater inlet or open channel. Overflows may also be impounded in surface depressions such as parking lots, back yards or other areas.

In sewer systems with pressurized pipes and force mains the hydraulic head at junction nodes can at times exceed the ground elevation under Dynamic Wave routing. This would normally result in an overflow which, as described above, can either be lost or ponded. SWMM allows the user to specify an additional "surcharge" depth for junction nodes that lets them pressurize and prevents any outflow until this additional depth is exceeded. If both ponding and pressurization are specified for a node ponding takes precedence and the surcharge depth is ignored. Ponding does not apply to storage nodes.

### Water Quality Routing

Water quality routing within conduit links assumes that the conduit behaves as a continuously stirred tank reactor (CSTR). Although a plug flow reactor assumption might be more realistic, the differences will be small if the travel time through the conduit is on the same order as the routing time step. The concentration of a constituent exiting the conduit at the end of a time step is found by integrating the conservation of mass equation, using average values for quantities that might change over the time step such as flow rate and conduit volume. 

Water quality modeling within storage unit nodes follows the same approach used for conduits. For other types of nodes that have no volume, the quality of water exiting the node is simply the mixture concentration of all water entering the node.

The pollutant concentration in both a conduit and a storage node will be reduced by a first-order decay reaction if the pollutant’s first-order decay coefficient is not zero.

### LID Representation

LID controls are represented by a combination of vertical layers whose properties are defined on a per-unit-area basis. This allows LIDs of the same design but differing area coverage to easily be placed within different subcatchments of a study area. During a simulation SWMM performs a moisture balance that keeps track of how much water moves between and is stored within each LID layer. As an example, the layers used to model a bio-retention cell and the flow pathways between them are shown in Figure 1-15. The various possible layers consist of the following:

 
![Figure 1-15 Conceptual diagram of a bio-retention cell LID](figures/png/eng_lid_bioretention_layers.png)

*Figure 1-15 Conceptual diagram of a bio-retention cell LID*

- The Surface Layer corresponds to the ground (or pavement) surface that receives direct rainfall and runon from upstream land areas, stores excess inflow in depression storage, and generates surface outflow that either enters the drainage system or flows onto downstream land areas.
- The Pavement Layer is the layer of porous concrete or asphalt used in continuous permeable pavement systems, or is the paver blocks and filler material used in modular systems.
- The Soil Layer is the engineered soil mixture used in bio-retention cells to support vegetative growth. It can also be a sand layer placed beneath a pavement layer to provide bedding and filtration.
- The Storage Layer is a bed of crushed rock or gravel that provides storage in bio-retention cells, porous pavement, and infiltration trench systems. For a rain barrel it is simply the barrel itself.
- The Drain System conveys water out of the gravel storage layer of bio-retention cells, permeable pavement systems, and infiltration trenches (typically with slotted pipes) into a common outlet pipe or chamber. For rain barrels it is simply the drain valve at the bottom of the barrel while for rooftop disconnection it is the roof gutter and downspout system.
- The Drainage Mat Layer is a mat or plate placed between the soil media and the roof in a green roof whose purpose is to convey any water that drains through the soil layer off of the roof.
Table 1-3 indicates which combination of layers applies to each type of LID (x means required, o means optional). 

Table 1-3 Layers used to model different types of LID units
LID Type    Surface    Pavement    Soil    Storage    Drain    Drainage Mat
Bio-Retention Cell    x        x    o    o    
Rain Garden    x        x            
Green Roof    x        x            x
Permeable Pavement    x    x    o    x    o    
Infiltration Trench    x            x    o    
Rain Barrel                x    x    
Roof Disconnection    x                x    
Vegetative Swale    x                    


All of the LID controls provide some amount of rainfall/runoff storage and evaporation of stored water (except for rain barrels). Infiltration into native soil occurs in vegetative swales and can also occur in bio-retention cells, rain gardens, permeable pavement systems, and infiltration trenches if those systems do not employ an optional impermeable bottom liner. Infiltration trenches and permeable pavement systems can also be subjected to clogging. This reduces their hydraulic conductivity over time proportional to the cumulative hydraulic loading they receive.

The performance of the LID controls placed in a subcatchment is reflected in the overall runoff, infiltration, and evaporation rates computed for the subcatchment as normally reported by SWMM. SWMM's Status Report also contains a section entitled LID Performance Summary that provides an overall water balance for each LID control placed in each subcatchment. The components of this water balance include total inflow, infiltration, evaporation, surface runoff, drain flow and initial and final stored volumes, all expressed as inches (or mm) over the LID's area. Optionally, the entire time series of flux rates and moisture levels for a selected LID control in a given subcatchment can be written to a tab delimited text file for easy viewing and graphing in a spreadsheet program (such as Microsoft Excel).
