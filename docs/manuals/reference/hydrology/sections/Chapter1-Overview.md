@page hydrology_ref_ch1_overview Chapter 1: Overview

@tableofcontents

## 1.1 Introduction

Urban runoff quantity and quality constitute problems of both a
historical and current nature. Cities have long assumed the
responsibility of control of stormwater flooding and treatment of point
sources (e.g., municipal sewage) of wastewater. Since the 1960s, the
severe pollution potential of urban nonpoint sources, principally
combined sewer overflows and stormwater discharges, has been recognized,
both through field observation and federal legislation. The advent of
modern computers has led to the development of complex, sophisticated
tools for analysis of both quantity and quality pollution problems in
urban areas and elsewhere (Singh, 1995). The EPA Storm Water Management
Model, SWMM, first developed in 1969-71, was one of the first such
models. It has been continually maintained and updated and is perhaps
the best known and most widely used of the available urban runoff
quantity/quality models (Huber and Roesner, 2013).

SWMM is a dynamic rainfall-runoff simulation model used for single event
or long-term (continuous) simulation of runoff quantity and quality from
primarily urban areas. The runoff component of SWMM operates on a
collection of subcatchment areas that receive precipitation and generate
runoff and pollutant loads. The routing portion of SWMM transports this
runoff through a system of pipes, channels, storage/treatment devices,
pumps, and regulators. SWMM tracks the quantity and quality of runoff
generated within each subcatchment, and the flow rate, flow depth, and
quality of water in each pipe and channel during a simulation period
comprised of multiple time steps.



Table 1-1 summarizes the development history of SWMM. The current
edition, Version 5, is a complete re-write of the previous releases. The
reference manual for this edition of SWMM is comprised of three volumes.
Volume I describes SWMM's hydrologic models, Volume II its hydraulic
models, and Volume III its water quality and low impact development
models. These manuals complement the SWMM 5 User's Manual (US EPA,
2010), which explains how to run the program, and the SWMM 5
Applications Manual (US EPA, 2009) which presents a number of worked-out
examples. The procedures described in this reference manual are based on
earlier descriptions included in the original SWMM documentation
(Metcalf and Eddy et al., 1971a, 1971b, 1971c, 1971d), intermediate
reports (Huber et al., 1975; Heaney et al., 1975; Huber et al., 1981),
plus new material. This information supersedes the Version 4.0
documentation (Huber and Dickinson, 1988; Roesner et al., 1988) and
includes descriptions of some newer procedures implemented since 1988.
More information on current documentation and the general status of the
EPA Storm Water Management Model as well as the full program and its
source code is available on the EPA SWMM web site:.
<http://www2.epa.gov/water-research/storm-water-management-model-swmm>.

**Table 1-1 Development history of SWMM**

| **Version** | **Year** | **Contributors** | **Comments** |
|-------------|----------|------------------|--------------|
| SWMM I | 1971 | Metcalf & Eddy, Inc.<br>Water Resources Engineers<br>University of Florida | First version of SWMM; focus was CSO modeling; few of its methods are still used today. |
| SWMM II | 1975 | University of Florida | First widely distributed version of SWMM. |
| SWMM 3 | 1981 | University of Florida<br>Camp Dresser & McKee | Full dynamic wave flow routine, Green-Ampt infiltration, snow melt, and continuous simulation added. |
| SWMM 3.3 | 1983 | US EPA | First PC version of SWMM. |
| SWMM 4 | 1988 | Oregon State University<br>Camp Dresser & McKee | Groundwater, RDII, irregular channel cross-sections and other refinements added over a series of updates throughout the 1990's. |
| SWMM 5 | 2005 | US EPA<br>CDM-Smith | Complete re-write of the SWMM engine in C; graphical user interface added; improved algorithms and new features (e.g., LID modeling) added. |

## 1.2 SWMM's Object Model



Figure 1-1 depicts the elements included in a typical urban drainage
system. SWMM conceptualizes this system as a series of water and
material flows between several major environmental compartments. These
compartments include:

![](hydrology/media/media/hydrology-image1.jpeg "image1")
<strong>Figure 1-1 Elements of a typical urban drainage system</strong>


- The Atmosphere compartment, which generates precipitation and deposits
  pollutants onto the Land Surface compartment.

- The Land Surface compartment receives precipitation from the
  Atmosphere compartment in the form of rain or snow. It sends outflow
  in the forms of 1) evaporation back to the Atmosphere compartment, 2)
  infiltration into the Sub-Surface compartment and 3) surface runoff
  and pollutant loadings on to the Conveyance compartment.

- The Sub-Surface compartment receives infiltration from the Land
  Surface compartment and transfers a portion of this inflow to the
  Conveyance compartment as groundwater interflow.

- The Conveyance compartment contains a network of elements (channels,
  pipes, pumps, and regulators) and storage/treatment units that convey
  water to outfalls or to treatment facilities. Inflows to this
  compartment can come from surface runoff, groundwater interflow,
  sanitary dry weather flow, or from user-defined time series.

Not all compartments need appear in a particular SWMM model. For
example, one could model just the Conveyance compartment, using
pre-defined hydrographs and pollutographs as inputs. As illustrated in
Figure 1-1, SWMM can be used to model any combination of stormwater
collection systems, both separate and combined sanitary sewer systems,
as well as natural catchment and river channel systems.



Figure 1-2 shows how SWMM conceptualizes the physical elements of the
actual system depicted in Figure 1-1 with a standard set of modeling
objects. The principal objects used to model the rainfall/runoff process
are Rain Gages and Subcatchments. Snowmelt is modeled with Snow Pack
objects placed on top of subcatchments while Aquifer objects placed
below subcatchments are used to model groundwater flow. The conveyance
portion of the drainage system is modeled with a network of Nodes and
Links. Nodes are points that represent simple junctions, flow dividers,
storage units, or outfalls. Links connect nodes to one another with
conduits (pipes and channels), pumps, or flow regulators (orifices,
weirs, or outlets). Land Use and Pollutant objects are used to describe
water quality. Finally, a group of data objects that includes Curves,
Time Series, Time Patterns, and Control Rules, are used to characterize
the inflows and operating behavior of the various physical objects in a
SWMM model. Table 1-2 provides a summary of the various objects used in
SWMM. Their properties and functions will be described in more detail
throughout the course of this manual.

![Figure 1-2 Objects of an OpenSWMM model](figures/png/eng_object_sketch.png)
<strong>Figure 1-2 Objects of an OpenSWMM model</strong>

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

**Table 1-2 SWMM's modeling objects**

| **Category** | **Object Type** | **Description** |
|--------------|-----------------|-----------------|
| **Hydrology** | Rain Gage | Source of precipitation data to one or more subcatchments. |
| | Subcatchment | A land parcel that receives precipitation associated with a rain gage and generates runoff that flows into a drainage system node or to another subcatchment. |
| | Aquifer | A subsurface area that receives infiltration from the subcatchment above it and exchanges groundwater flow with a conveyance system node. |
| | Snow Pack | Accumulated snow that covers a subcatchment. |
| | Unit Hydrograph | A response function that describes the amount of sewer inflow/infiltration generated over time per unit of instantaneous rainfall. |
| **Hydraulics** | Junction | A point in the conveyance system where conduits connect to one another with negligible storage volume (e.g., manholes, pipe fittings, or stream junctions). |
| | Outfall | An end point of the conveyance system where water is discharged to a receptor (such as a receiving stream or treatment plant) with known water surface elevation. |
| | Divider | A point in the conveyance system where the inflow splits into two outflow conduits according to a known relationship. |
| | Storage Unit | A pond, lake, impoundment, or chamber that provides water storage. |
| | Conduit | A channel or pipe that conveys water from one conveyance system node to another. |
| | Pump | A device that raises the hydraulic head of water. |
| | Regulator | A weir, orifice or outlet used to direct and regulate flow between two nodes of the conveyance system. |

**Table 1-2 SWMM's modeling objects (continued)**

| **Category** | **Object Type** | **Description** |
|--------------|-----------------|-----------------|
| **Water Quality** | Pollutant | A contaminant that can build up and be washed off of the land surface or be introduced directly into the conveyance system. |
| | Land Use | A classification used to characterize the functions that describe pollutant buildup and washoff. |
| **Treatment** | LID Control | A low impact development control, such as a bio-retention cell, porous pavement, or vegetative swale, used to reduce surface runoff through enhanced infiltration. |
| | Treatment Function | A user-defined function that describes how pollutant concentrations are reduced at a conveyance system node as a function of certain variables, such as concentration, flow rate, water depth, etc. |
| **Data Object** | Curve | A tabular function that defines the relationship between two quantities (e.g., flow rate and hydraulic head for a pump, surface area and depth for a storage node, etc.). |
| | Time Series | A tabular function that describes how a quantity varies with time (e.g., rainfall, outfall surface elevation, etc.). |
| | Time Pattern | A set of factors that repeats over a period of time (e.g., diurnal hourly pattern, weekly daily pattern, etc.). |
| | Control Rules | IF-THEN-ELSE statements that determine when specific control actions are taken (e.g., turn a pump on or off when the flow depth at a given node is above or below a certain value). |

## 1.3 SWMM's Process Models

![Figure 1-3 Processes modelled by OpenSWMM](figures/png/eng_process_flow.png)
<strong>Figure 1-3 Processes modelled by OpenSWMM</strong>

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
<span class="hs" data-box="0.2860,0.3202,0.4439,0.3425">@ref hydrology_ref_ch5_groundwater "two-layer aquifer on the mesh"</span>
<span class="hs" data-box="0.2860,0.3487,0.4028,0.3710">@ref hydrology_ref_ch5_groundwater "groundwater transport"</span>
<span class="hs" data-box="0.2800,0.6528,0.4800,0.6806">@ref quality_ref_ch3_pollutant_buildup "Pollutant buildup"</span>
<span class="hs" data-box="0.2860,0.6806,0.3977,0.7028">@ref quality_ref_ch3_pollutant_buildup "land uses · sweeping"</span>
<span class="hs" data-box="0.5600,0.1111,0.7800,0.1389">@ref hydraulics_ref_ch9_two_dimensional "Rain on the mesh"</span>
<span class="hs" data-box="0.5660,0.1389,0.6623,0.1612">@ref hydraulics_ref_ch9_two_dimensional "natural neighbour"</span>
<span class="hs" data-box="0.5600,0.2153,0.7800,0.2431">@ref hydraulics_ref_ch9_two_dimensional "2D overland flow"</span>
<span class="hs" data-box="0.5660,0.2431,0.6469,0.2653">@ref hydraulics_ref_ch9_two_dimensional "local inertial"</span>
<span class="hs" data-box="0.6514,0.2431,0.7528,0.2653">@ref hydraulics_ref_ch9_two_dimensional "full shallow water"</span>
<span class="hs" data-box="0.5660,0.2716,0.6469,0.2939">@ref hydraulics_ref_ch9_two_dimensional "diffusive wave"</span>
<span class="hs" data-box="0.5600,0.3472,0.7800,0.3750">@ref hydraulics_ref_ch9_two_dimensional "Per-cell infiltration · evaporation"</span>
<span class="hs" data-box="0.5660,0.3750,0.7598,0.3973">@ref engine_manual_sect_2D_INFILTRATION_OPTIONS "→ lost · subcatchment · mesh aquifer"</span>
<span class="hs" data-box="0.5600,0.4583,0.7800,0.4861">@ref quality_ref_ch4_surface_washoff "2D surface quality"</span>
<span class="hs" data-box="0.5660,0.4861,0.7239,0.5084">@ref quality_ref_ch4_surface_washoff "coverages · buildup · washoff"</span>
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
<span class="hs" data-box="0.3464,0.8889,0.3964,0.9112">@ref quality_ref_ch5_transport_treatment "sediment"</span>
<span class="hs" data-box="0.4009,0.8889,0.5177,0.9112">@ref engine_manual_sect_TREATMENT "treatment · diversion"</span>
<span class="hs" data-box="0.0200,0.9472,0.9800,0.9861">@ref engine_manual_sect_OUTFALLS "Outfalls"</span>
</div>

Figure 1-3 depicts the processes that SWMM models using the objects
described previously and how they are tied to one another. The
hydrological processes depicted in this diagram include:

- time-varying precipitation

- snow accumulation and melting

- rainfall interception from depression storage (initial abstraction)

- evaporation of standing surface water

- infiltration of rainfall into unsaturated soil layers

- percolation of infiltrated water into groundwater layers

- interflow between groundwater and the drainage system

- nonlinear reservoir routing of overland flow

- infiltration and evaporation of rainfall/runoff captured by Low Impact
  Development controls.

The hydraulic processes occurring within SWMM's conveyance compartment
include:

- external inflow of surface runoff, groundwater interflow,
  rainfall-dependent infiltration/inflow, dry weather sanitary flow, and
  user-defined inflows

- unsteady, non-uniform flow routing through any configuration of open
  channels, pipes and storage units

- various possible flow regimes such as backwater, surcharging, reverse
  flow, and surface ponding

- flow regulation via pumps, weirs, and orifices including time- and
  state-dependent control rules that govern their operation.

Regarding water quality, the following processes can be modeled for any
number of user-defined water quality constituents:

- dry-weather pollutant buildup over different land uses

- pollutant washoff from specific land uses during storm events

- direct contribution of rainfall deposition

- reduction in dry-weather buildup due to street cleaning

- reduction in washoff loads due to BMPs

- entry of dry weather sanitary flows and user-specified external
  inflows at any point in the drainage system

- routing of water quality constituents through the drainage system

- reduction in constituent concentration through treatment in storage
  units or by natural processes in pipes and channels.

The numerical procedures that SWMM uses to model the hydrologic
processes listed above are discussed in detail in subsequent chapters of
this volume. SWMM's hydraulic, water quality, treatment and low impact
development processes are described in subsequent volumes of this
manual.

## 1.4 Simulation Process Overview

SWMM is a distributed discrete time simulation model. It computes new
values of its state variables over a sequence of time steps, where at
each time step the system is subjected to a new set of external inputs.
As its state variables are updated, other output variables of interest
are computed and reported. This process is represented mathematically
with the following general set of equations that are solved at each time
step as the simulation proceeds:

*X*<sub>*t*</sub> = *f*(*X*<sub>*t* - 1</sub>, *I*<sub>*t*</sub>, *P*) (1-1)

*Y*<sub>*t*</sub> = *g*(*X*<sub>*t*</sub>, *P*) (1-2)

where

  *X*<sub>*t*</sub>   =   a vector of state variables at time *t*,
  *Y*<sub>*t*</sub>   =   a vector of output variables at time *t*,

  *I*<sub>*t*</sub>   =   a vector of inputs at time *t*,

  *P*      =   a vector of constant parameters,

  *f*      =   a vector-valued state transition function,

  *g*      =   a vector-valued output transform function.


![](hydrology/media/media/figure1-4.png "Block diagram of SWMM")
<strong>Figure 1-4 Block diagram of SWMM's state transition process</strong>

Figure 1-4 depicts the simulation process in block diagram fashion.



The variables that make up the state vector *X*<sub>*t*</sub> are listed in Table
1-3. This is a surprisingly small number given the comprehensive nature
of SWMM. All other quantities can be computed from these variables,
external inputs, and fixed input parameters. The meaning of some of the
less obvious state variables, such as those used for snow melt, is
discussed in later chapters.

**Table 1-3 State variables used by SWMM**

| **Process** | **Variable** | **Description** | **Initial Value** |
|-------------|--------------|-----------------|-------------------|
| **Runoff** | *d* | Depth of runoff on a subcatchment surface | 0 |
| **Infiltration** | *t*<sub>*p*</sub> | Equivalent time on the Horton curve | 0 |
| | *F*<sub>*e*</sub> | Cumulative excess infiltration volume | 0 |
| | *F*<sub>*u*</sub> | Upper zone moisture content | 0 |
| | *T* | Time until the next rainfall event | 0 |
| | *P* | Cumulative rainfall for current event | 0 |
| | *S* | Soil moisture storage capacity remaining | User supplied |
| **Groundwater** | *θ*<sub>*u*</sub> | Unsaturated zone moisture content | User supplied |
| | *d*<sub>*L*</sub> | Depth of saturated zone | User supplied |
| **Snowmelt** | *w*<sub>snow</sub> | Snow pack depth | User supplied |
| | *f*<sub>*w*</sub> | Snow pack free water depth | User supplied |
| | *a*<sub>*ti*</sub> | Snow pack surface temperature | User supplied |
| | *c*<sub>*c*</sub> | Snow pack cold content | 0 |
| **Flow Routing** | *y* | Depth of water at a node | User supplied |
| | *q* | Flow rate in a link | User supplied |
| | *a* | Flow area in a link | Inferred from *q* |
| **Water Quality** | *t*<sub>sweep</sub> | Time since a subcatchment was last swept | User supplied |
| | *m*<sub>*B*</sub> | Mass of pollutant on subcatchment surface | User supplied |
| | *m*<sub>*P*</sub> | Mass of pollutant ponded on subcatchment | 0 |
| | *c*<sub>*N*</sub> | Concentration of pollutant at a node | User supplied |
| | *c*<sub>*L*</sub> | Concentration of pollutant in a link | User supplied |

\*Only a sub-set of these variables is used, depending on the user's
choice of infiltration method.

Examples of user-supplied input variables *I*<sub>*t*</sub> that produce changes to
these state variables include:

- meteorological conditions, such as precipitation, air temperature,
  potential evaporation rate and wind speed

- externally imposed inflow hydrographs and pollutographs at specific
  nodes of the conveyance system

- dry weather sanitary inflows to specific nodes of the conveyance
  system

- water surface elevations at specific outfalls of the conveyance system

- control settings for pumps and regulators.

The output vector *Y*<sub>*t*</sub> that SWMM computes from its updated state
variables contains such reportable quantities as:

- runoff flow rate and pollutant concentrations from each subcatchment

- snow depth, infiltration rate and evaporation losses from each
  subcatchment

- groundwater table elevation and lateral groundwater outflow for each
  subcatchment

- total lateral inflow (from runoff, groundwater flow, dry weather flow,
  etc.), water depth, and pollutant concentration for each conveyance
  system node

- overflow rate and ponded volume at each flooded node

- flow rate, velocity, depth and pollutant concentration for each
  conveyance system link.

Regarding the constant parameter vector *P,* SWMM contains over 150
different user-supplied constants and coefficients within its collection
of process models. Most of these are either physical dimensions (e.g.,
land areas, pipe diameters, invert elevations) or quantities that can be
obtained from field observation (e.g., percent impervious cover),
laboratory testing (e.g., various soil properties), or previously
published data tables (e.g., pipe roughness based on pipe material). A
smaller remaining number might require some degree of model calibration
to determine their proper values. Not all parameters are required for
every project (e.g., the 14 groundwater parameters for each subcatchment
are not needed if groundwater is not being modeled). The subsequent
chapters of this manual carefully define each parameter and make
suggestions on how to estimate its value.

![](hydrology/media/media/figure1-5.png "Flow chart of SWMM")
<strong>Figure 1-5 Flow chart of SWMM's simulation procedure</strong>

A flowchart of the overall simulation process is shown in Figure 1-5.
The process begins by reading a description of each object and its
parameters from an input file whose format is described in the SWMM 5
UsersManual (US EPA, 2010). Next the values of all state variables are
initialized, as is the current simulation time (*T*), runoff time
(*T*<sub>roff</sub>), and reporting time (*T*<sub>rpt</sub>).

The program then enters a loop that first determines the time *T*<sub>1</sub> at the
end of the current routing time step (∆*T*<sub>rout</sub>). If the current runoff
time *T*<sub>roff</sub> is less than *T*<sub>1</sub>, then new runoff calculations are
repeatedly made and the runoff time updated until it equals or exceeds
time *T*<sub>1</sub>. Each set of runoff calculations accounts for any precipitation,
evaporation, snowmelt, infiltration, ground water seepage, overland
flow, and pollutant buildup and washoff that can contribute flow and
pollutant loads into the conveyance system.

Once the runoff time is current, all inflows and pollutant loads
occurring at time *T* are routed through the conveyance system over the
time interval from *T* to *T*<sub>1</sub>. This process updates the flow, depth and
velocity in each conduit, the water elevation at each node, the pumping
rate for each pump, and the water level and volume in each storage unit.
In addition, new values for the concentrations of all pollutants at each
node and within each conduit are computed. Next a check is made to see
if the current reporting time *T*<sub>rpt</sub> falls within the interval from *T* to
*T*<sub>1</sub>. If it does, then a new set of output results at time *T*<sub>rpt</sub> are
interpolated from the results at times *T* and *T*<sub>1</sub> and are saved to an
output file. The reporting time is also advanced by the reporting time
step ∆*T*<sub>rpt</sub>. The simulation time *T* is then updated to *T*<sub>1</sub> and the
process continues until *T* reaches the desired total duration. SWMM's
Windows-based user interface provides graphical tools for building the
aforementioned input file and for viewing the computed output.

## 1.5 Interpolation and Units

![](hydrology/media/media/hydrology-figure1-6.png "Interpolation of reported values from computed values")
<strong>Figure 1-6 Interpolation of reported values from computed values</strong>

SWMM uses linear interpolation to obtain values for quantities at times
that fall in between times at which input time series are recorded or at
which output results are computed. The concept is illustrated in Figure
1-6 which shows how reported flow values are derived from the computed
flow values on either side of it for the typical case where the
reporting time step is larger than the routing time step. One exception
to this convention is for precipitation and infiltration rates. These
remain constant within a runoff time step and no interpolation is made
when these values are used within SWMM's runoff algorithms or for
reporting purposes. In other words, if a reporting time falls within a
runoff time step the reported rainfall intensity is the value associated
with the start of the runoff time step.



The units of expression used by SWMM's input variables, parameters, and
output variables depend on the user's choice of flow units. If flow rate
is expressed in US customary units then so are all other quantities; if
SI metric units are used for flow rate then all other quantities use SI
metric units. Table 1-4 lists the units associated with each of SWMM's
major variables and parameters, for both US and SI systems. Internally
within the computer code all calculations are carried out using feet as
the unit of length and seconds as the unit of time and then converted
back to the user's choice of unit system.

**Table 1-4 Units of expression used by SWMM**

| **Variable or Parameter** | **US Customary Units** | **SI Metric Units** |
|---------------------------|------------------------|---------------------|
| Area (subcatchment) | acres | hectares |
| Area (storage surface area) | square feet | square meters |
| Depression Storage | inches | millimeters |
| Depth | feet | meters |
| Elevation | feet | meters |
| Evaporation | inches/day | millimeters/day |
| Flow Rate | cubic feet/sec (cfs)<br>gallons/min (gpm)<br>10<sup>6</sup> gallons/day (mgd) | cubic meters/sec (cms)<br>liters/sec (lps)<br>10<sup>6</sup> liters/day (mld) |
| Hydraulic Conductivity | inches/hour | millimeters/hour |
| Hydraulic Head | feet | meters |
| Infiltration Rate | inches/hour | millimeters/hour |
| Length | feet | meters |
| Manning's n | seconds/meter<sup>1/3</sup> | seconds/meter<sup>1/3</sup> |
| Pollutant Buildup | mass/acre | mass/hectare |
| Pollutant Concentration | milligrams/liter (mg/L)<br>micrograms/liter (μg/L)<br>organism counts/liter | milligrams/liter (mg/L)<br>micrograms/liter (μg/L)<br>organism counts/liter |
| Rainfall Intensity | inches/hour | millimeters/hour |
| Rainfall Volume | inches | millimeters |
| Storage Volume | cubic feet | cubic meters |
| Temperature | degrees Fahrenheit | degrees Celsius |
| Velocity | feet/second | meters/second |
| Width | feet | meters |
| Wind Speed | miles/hour | kilometers/hour |

## 1.6 The Formulations This Manual Covers

Figure 1-7 places the hydrology of this manual on OpenSWMM's process map: the
atmosphere, the two representations of the land surface — lumped
subcatchments and the distributed 2D mesh — and the two representations of
the subsurface. Each alternative carries its status (Implemented,
Experimental, Planned or Retired); the conveyance compartment, documented in
@ref hydraulics_reference_manual, is shown faded.

![Figure 1-7 The hydrology of this manual: atmosphere, land surface and subsurface representations](figures/png/hydrology_conceptual_map.png)

<strong>Figure 1-7 The hydrology of this manual: atmosphere, land surface and subsurface representations</strong>

<div class="fig-hotspots" data-fig="hydrology_conceptual_map">
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
<span class="hs" data-box="0.1265,0.3895,0.1920,0.4066">@ref quality_ref_ch8_msx_reactions "MSX species"</span>
<span class="hs" data-box="0.5300,0.2160,0.7005,0.2372">@ref hydraulics_ref_ch9_two_dimensional "Rain on the mesh"</span>
<span class="hs" data-box="0.5360,0.2372,0.6323,0.2543">@ref hydraulics_ref_ch9_two_dimensional "natural neighbour"</span>
<span class="hs" data-box="0.6368,0.2372,0.6766,0.2543">@ref engine_manual_sect_2D_OPTIONS "system"</span>
<span class="hs" data-box="0.7095,0.2160,0.8800,0.2372">@ref hydraulics_ref_ch9_two_dimensional "Per-cell infiltration"</span>
<span class="hs" data-box="0.7155,0.2372,0.9042,0.2543">@ref hydraulics_ref_ch9_two_dimensional "Horton · Green-Ampt · CN · constant"</span>
<span class="hs" data-box="0.7155,0.2591,0.9504,0.2762">@ref engine_manual_sect_2D_INFILTRATION_OPTIONS "→ lost · subcatchment aquifer · mesh aquifer"</span>
<span class="hs" data-box="0.5300,0.2703,0.7005,0.2915">@ref hydraulics_ref_ch9_two_dimensional "Overland flow"</span>
<span class="hs" data-box="0.5360,0.2915,0.6169,0.3086">@ref hydraulics_ref_ch9_two_dimensional "local inertial"</span>
<span class="hs" data-box="0.5360,0.3134,0.6374,0.3305">@ref hydraulics_ref_ch9_two_dimensional "full shallow water"</span>
<span class="hs" data-box="0.5360,0.3352,0.6169,0.3523">@ref hydraulics_ref_ch9_two_dimensional "diffusive wave"</span>
<span class="hs" data-box="0.6214,0.3352,0.6920,0.3523">@ref hydraulics_ref_ch9_two_dimensional "IMEX / CVODE"</span>
<span class="hs" data-box="0.7095,0.2921,0.8800,0.3134">@ref hydraulics_ref_ch9_two_dimensional "Mesh and closure"</span>
<span class="hs" data-box="0.7155,0.3134,0.7707,0.3305">@ref hydraulics_ref_ch9_two_dimensional "triangles"</span>
<span class="hs" data-box="0.7752,0.3134,0.8099,0.3305">@ref hydraulics_ref_ch9_two_dimensional "quads"</span>
<span class="hs" data-box="0.7155,0.3352,0.7758,0.3523">@ref hydraulics_ref_ch9_two_dimensional "flat cells"</span>
<span class="hs" data-box="0.7803,0.3352,0.8458,0.3523">@ref hydraulics_ref_ch9_two_dimensional "VFR closure"</span>
<span class="hs" data-box="0.5300,0.3683,0.7005,0.3895">@ref quality_ref_ch4_surface_washoff "Surface quality · transport"</span>
<span class="hs" data-box="0.5360,0.3895,0.6939,0.4066">@ref quality_ref_ch4_surface_washoff "coverages · buildup · washoff"</span>
<span class="hs" data-box="0.5360,0.4114,0.6425,0.4285">@ref quality_ref_ch7_ard_transport "species on the mesh"</span>
<span class="hs" data-box="0.7095,0.3683,0.8800,0.3895">@ref hydraulics_ref_ch9_two_dimensional "Backends"</span>
<span class="hs" data-box="0.7155,0.3895,0.7399,0.4066">@ref hydraulics_ref_ch9_two_dimensional "CPU"</span>
<span class="hs" data-box="0.7444,0.3895,0.7842,0.4066">@ref hydraulics_ref_ch9_two_dimensional "OpenMP"</span>
<span class="hs" data-box="0.7155,0.4114,0.8118,0.4285">@ref hydraulics_ref_ch9_two_dimensional "CUDA · HIP · SYCL"</span>
<span class="hs" data-box="0.8163,0.4114,0.8509,0.4285">@ref hydraulics_ref_ch9_two_dimensional "Metal"</span>
<span class="hs" data-box="0.0300,0.5032,0.2455,0.5245">@ref hydrology_ref_ch5_groundwater "Two-zone aquifer"</span>
<span class="hs" data-box="0.0360,0.5245,0.1887,0.5415">@ref hydrology_ref_ch5_groundwater "unsaturated + saturated zone"</span>
<span class="hs" data-box="0.0360,0.5463,0.2247,0.5634">@ref engine_manual_sect_AQUIFERS "percolation · deep percolation · ET"</span>
<span class="hs" data-box="0.0360,0.5682,0.1939,0.5852">@ref engine_manual_sect_GWF "lateral interflow, user [GWF]"</span>
<span class="hs" data-box="0.2545,0.5032,0.4700,0.5245">@ref hydrology_ref_ch5_groundwater "Groundwater transport"</span>
<span class="hs" data-box="0.2605,0.5245,0.4081,0.5415">@ref hydrology_ref_ch5_groundwater "advection–dispersion · heat"</span>
<span class="hs" data-box="0.5300,0.5032,0.8800,0.5245">@ref hydrology_ref_ch5_groundwater "Two-layer aquifer"</span>
<span class="hs" data-box="0.5360,0.5245,0.5809,0.5415">@ref hydrology_ref_ch5_groundwater "Gardner"</span>
<span class="hs" data-box="0.5854,0.5245,0.6201,0.5415">@ref hydrology_ref_ch5_groundwater "Russo"</span>
<span class="hs" data-box="0.6246,0.5245,0.6952,0.5415">@ref hydrology_ref_ch5_groundwater "Brooks–Corey"</span>
<span class="hs" data-box="0.6997,0.5245,0.7754,0.5415">@ref hydrology_ref_ch5_groundwater "van Genuchten"</span>
<span class="hs" data-box="0.5360,0.5463,0.7144,0.5634">@ref hydrology_ref_ch5_groundwater "closed form · enslaved · σ column"</span>
<span class="hs" data-box="0.7189,0.5463,0.8152,0.5634">@ref engine_manual_sect_2D_AQUIFER_OPTIONS "Dunne return flow"</span>
<span class="hs" data-box="0.5360,0.5682,0.6323,0.5852">@ref engine_manual_sect_2D_AQUIFER_NODE "node–bed exchange"</span>
<span class="hs" data-box="0.6368,0.5682,0.7895,0.5852">@ref engine_manual_sect_2D_AQUIFER_OPTIONS "capillary rise · boundary ET"</span>
<span class="hs" data-box="0.5360,0.5900,0.6528,0.6071">@ref hydrology_ref_ch5_groundwater "per-subcatchment mode"</span>
<span class="hs" data-box="0.0300,0.6787,0.3073,0.7000">@ref engine_manual_sect_JUNCTIONS "Nodes"</span>
<span class="hs" data-box="0.0360,0.7000,0.2401,0.7171">@ref engine_manual_sect_JUNCTIONS "junction · storage · divider · outfall"</span>
<span class="hs" data-box="0.0360,0.7219,0.1271,0.7389">@ref hydraulics_ref_ch3_dynamic_wave "virtual junction"</span>
<span class="hs" data-box="0.1316,0.7219,0.2125,0.7389">@ref hydraulics_ref_ch7_advanced_features "inlet junction"</span>
<span class="hs" data-box="0.3163,0.6787,0.5937,0.7000">@ref engine_manual_sect_CONDUITS "Links"</span>
<span class="hs" data-box="0.3223,0.7000,0.4391,0.7171">@ref hydraulics_ref_ch5_cross_section "conduits, 26 sections"</span>
<span class="hs" data-box="0.4436,0.7000,0.5245,0.7171">@ref hydraulics_ref_ch5_cross_section "street · dummy"</span>
<span class="hs" data-box="0.3223,0.7219,0.4289,0.7389">@ref hydraulics_ref_ch5_cross_section "Chebyshev irregular"</span>
<span class="hs" data-box="0.3223,0.7437,0.5623,0.7608">@ref engine_manual_sect_PUMPS "pumps · orifices · weirs · outlets · culverts"</span>
<span class="hs" data-box="0.3223,0.7656,0.4032,0.7826">@ref hydraulics_ref_ch5_cross_section "storage shapes"</span>
<span class="hs" data-box="0.4077,0.7656,0.4834,0.7826">@ref hydraulics_ref_ch7_advanced_features "HEC-22 inlets"</span>
<span class="hs" data-box="0.6027,0.6787,0.8800,0.7000">@ref hydraulics_ref_ch2_hydraulic_model "Flow routing"</span>
<span class="hs" data-box="0.6087,0.7000,0.6485,0.7171">@ref hydraulics_ref_ch2_hydraulic_model "steady"</span>
<span class="hs" data-box="0.6530,0.7000,0.7338,0.7171">@ref hydraulics_ref_ch4_kinematic_wave "kinematic wave"</span>
<span class="hs" data-box="0.7383,0.7000,0.8089,0.7171">@ref hydraulics_ref_ch3_dynamic_wave "dynamic wave"</span>
<span class="hs" data-box="0.6087,0.7219,0.6844,0.7389">@ref hydraulics_ref_ch8_finite_volume "finite volume"</span>
<span class="hs" data-box="0.6889,0.7219,0.7954,0.7389">@ref hydraulics_ref_ch8_finite_volume "local time stepping"</span>
<span class="hs" data-box="0.6087,0.7437,0.6895,0.7608">@ref hydraulics_ref_ch8_finite_volume "1D GPU backend"</span>
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
<span class="hs" data-box="0.3223,0.8417,0.4391,0.8588">@ref hydraulics_ref_ch3_dynamic_wave "Anderson acceleration"</span>
<span class="hs" data-box="0.4436,0.8417,0.5245,0.8588">@ref hydraulics_ref_ch8_finite_volume "FV_NODE_* keys"</span>
<span class="hs" data-box="0.6027,0.7767,0.8800,0.7980">@ref quality_ref_ch5_transport_treatment "Transport"</span>
<span class="hs" data-box="0.6087,0.7980,0.6382,0.8151">@ref quality_ref_ch5_transport_treatment "CSTR"</span>
<span class="hs" data-box="0.6427,0.7980,0.7133,0.8151">@ref quality_ref_ch7_ard_transport "Eulerian ARD"</span>
<span class="hs" data-box="0.7178,0.7980,0.8038,0.8151">@ref quality_ref_ch7_ard_transport "Lagrangian LARD"</span>
<span class="hs" data-box="0.6087,0.8199,0.7306,0.8369">@ref quality_ref_ch7_ard_transport "random-walk dispersion"</span>
<span class="hs" data-box="0.7351,0.8199,0.8314,0.8369">@ref quality_ref_ch5_transport_treatment "first-order decay"</span>
<span class="hs" data-box="0.6087,0.8417,0.6844,0.8588">@ref quality_ref_ch8_msx_reactions "MSX reactions"</span>
<span class="hs" data-box="0.6889,0.8417,0.7441,0.8588">@ref quality_ref_ch9_age_heat "water age"</span>
<span class="hs" data-box="0.7486,0.8417,0.7781,0.8588">@ref quality_ref_ch9_age_heat "heat"</span>
<span class="hs" data-box="0.7826,0.8417,0.8327,0.8588">@ref quality_ref_ch5_transport_treatment "sediment"</span>
<span class="hs" data-box="0.6087,0.8636,0.7152,0.8806">@ref engine_manual_sect_TREATMENT "treatment functions"</span>
<span class="hs" data-box="0.0300,0.8310,0.3073,0.8523">@ref engine_manual_sect_INFLOWS "Inflows"</span>
<span class="hs" data-box="0.0360,0.8523,0.1271,0.8694">@ref engine_manual_sect_DWF "dry weather flow"</span>
<span class="hs" data-box="0.1316,0.8523,0.1868,0.8694">@ref hydrology_ref_ch7_rdii "RDII: RTK"</span>
<span class="hs" data-box="0.1913,0.8523,0.2722,0.8694">@ref hydrology_ref_ch7_rdii "RDII: IA decay"</span>
<span class="hs" data-box="0.0360,0.8742,0.1579,0.8912">@ref engine_manual_sect_INFLOWS "user · interface files"</span>
<span class="hs" data-box="0.1624,0.8742,0.2382,0.8912">@ref engine_manual_sect_CONTROLS "control rules"</span>
</div>
