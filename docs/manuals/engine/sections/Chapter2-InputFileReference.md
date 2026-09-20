@page engine_manual_ch2_input_file CHAPTER 2 - Input File Reference

@tableofcontents

## 2.1 General Instructions

EPA SWMM can also be run as a console application from the command line within a DOS window. In this case the study area data are placed into a text file and results are written to a text file. The command line for running SWMM in this fashion is:
runswmm inpfile rptfile outfile
where inpfile is the name of the input file, rptfile is the name of the output report file, and outfile is the name of an optional binary output file. The latter stores all time series results in a special binary format that will require a separate post-processor program for viewing. If no binary output file name is supplied then all time series results will appear in the report file. As written, the above command assumes that you are working in the directory in which EPA SWMM was installed or that this directory has been added to the PATH variable in your user profile. Otherwise full pathnames for the runswmm executable and the files on the command line must be used. 

## 2.2 Input File Format

The input file for command line SWMM has the same format as the project file used by the Windows version of the program. Figure D-1 illustrates an example SWMM 5 input file. It is organized in sections, where each section begins with a keyword enclosed in brackets. The various section keywords are listed below.

[TITLE]                project title
[OPTIONS]            analysis options
[REPORT]                output reporting instructions
[FILES]                interface file options

[RAINGAGES]        rain gage information
[EVAPORATION]        evaporation data
[TEMPERATURE]        air temperature and snow melt data
[ADJUSTMENTS]        monthly adjustments applied to climate variables




[SUBCATCHMENTS]    basic subcatchment information
[SUBAREAS]            subcatchment impervious/pervious subarea data
[INFILTRATION]    subcatchment infiltration parameters
[LID_CONTROLS]    low impact development control information
[LID_USAGE]        assignment of LID controls to subcatchments

[AQUIFERS]            groundwater aquifer parameters
[GROUNDWATER]        subcatchment groundwater parameters
[GWF]                    groundwater flow expressions
[SNOWPACKS]        subcatchment snow pack parameters

[JUNCTIONS]        junction node information
[OUTFALLS]            outfall node information
[DIVIDERS]            flow divider node information
[STORAGE]            storage node information

[CONDUITS]            conduit link information
[PUMPS]                pump link information
[ORIFICES]            orifice link information
[WEIRS]                weir link information
[OUTLETS]            outlet link information

[XSECTIONS]        conduit, orifice, and weir cross-section geometry
[TRANSECTS]        transect geometry for conduits with irregular cross-sections
[STREETS]            cross-section geometry for street conduits
[INLETS]                design data for storm drain inlets
[INLET_USAGE]        assignment of inlets to street and channel conduits
[LOSSES]                conduit entrance/exit losses and flap valves
[CONTROLS]            rules that control pump and regulator operation

[POLLUTANTS]        pollutant information
[LANDUSES]            land use categories
[COVERAGES]        assignment of land uses to subcatchments
[LOADINGS]            initial pollutant loads on subcatchments
[BUILDUP]            buildup functions for pollutants and land uses
[WASHOFF]            washoff functions for pollutants and land uses
[TREATMENT]        pollutant removal functions at conveyance system nodes


[INFLOWS]            external hydrograph/pollutograph inflow at nodes
[DWF]                    baseline dry weather sanitary inflow at nodes
[RDII]                rainfall-dependent I/I information at nodes
[HYDROGRAPHS]        unit hydrograph data used to construct RDII inflows

[CURVES]                x-y tabular data referenced in other sections
[TIMESERIES]        time series data referenced in other sections
[PATTERNS]            periodic multipliers referenced in other sections


 
*Figure D-1 Example SWMM project file*

*Figure D-1 Example SWMM project file (continued from previous page).*






Section keywords can appear in mixed lower and upper case. The sections can appear in any arbitrary order in the input file, and not all sections must be present. Each section can contain one or more lines of data. Blank lines may appear anywhere in the file. A semicolon (;) can be used to indicate that what follows on the line is a comment, not data. Data items can appear in any column of a line. Observe how in Figure D-1 these features were used to create a tabular appearance for the data, complete with column headings.

An option is available in the [OPTIONS] section to choose flow units from among cubic feet per second (CFS), gallons per minute (GPM), million gallons per day (MGD), cubic meters per second (CMS), liters per second, (LPS), or million liters per day (MLD). If cubic feet or gallons are chosen for flow units, then US units must be used for all other quantities. If cubic meters or liters are chosen, then metric units must be used for all other quantities. Exceptions are pollutant concentration and Manning’s roughness coefficient (n) which are always expressed in metric units. The default flow units are CFS. @ref manual_reference_tables provides a complete listing of measurement units.

A detailed description of the data in each section of the input file will now be given. Each section description begins on a new page. When listing the format of a line of data, mandatory keywords are shown in boldface while optional items appear in parentheses. A list of keywords separated by a slash (YES/NO) means that only one of the words should appear in the data line.
 
### Section: [TITLE] {#engine_manual_sect_TITLE}

Purpose:
Attaches a descriptive title to the project being analyzed.

Format:
Any number of lines may be entered. The first line will be used as a page header in the output report.

 
### Section: [OPTIONS] {#engine_manual_sect_OPTIONS}

Purpose:
Provides values for the analysis options that select the process models, the solvers and their closures, the time steps, and reporting.

Format:
```
Keyword  Value
```

Parameters:

Every keyword below is matched case-insensitively by the parser (`OptionsHandler.cpp`, `handle_options()`); only the first value token after the keyword is read. A keyword the parser does not recognise is not an error: it is stored upper-cased in the extension-options map (`SimulationOptions::ext_options`), a non-fatal "Unknown option keyword" warning is written to the report file, and the value stays available to plugins through `swmm_options_get_ext()`. An unrecognised value of FLOW_UNITS, INFILTRATION, FLOW_ROUTING, QUALITY_SOLVER, OUTFALL_BACKFLOW_QUALITY or DISPERSION is stored the same way but without a warning; an unrecognised value of any other enumerated keyword is silently ignored and the default stands. Boolean spellings differ by key: the IGNORE_* keys, ALLOW_PONDING, SKIP_STEADY_STATE and WRITE_ABSOLUTE_PATHS take YES, TRUE or 1 as true and anything else as false; FV_COMPACTION, FV_LTS and FV_NODE_FEEDBACK_DT take NO, FALSE, 0 or OFF as false and anything else as true; the remaining switches list their accepted spellings in the Values column.

Three sections carry a keyword spelled `DISPERSION`, and they mean different things. In `[OPTIONS]`, `DISPERSION RWPT` or `OFF` is the switch for random-walk particle dispersion under `QUALITY_SOLVER LAGRANGIAN` (quality §7.4.1); it takes no coefficient and warns under any other quality solver. In `[TRANSPORT_OPTIONS]`, `DISPERSION OFF`, `FISCHER` or a number sets the longitudinal dispersion coefficient of the Eulerian ARD engine, in project length squared per second (quality §7.3.3). In `[2D_OPTIONS]`, `DISPERSION` is the isotropic species dispersion coefficient of the 2D surface in m²/s, default 0, with negative values refused. (A fourth, `[GW_TRANSPORT_OPTIONS] DISPERSION YES` or `NO`, is a switch of the groundwater-transport sections, which are authored but inert.) The table below documents only the `[OPTIONS]` key.

#### Units and process models

| Keyword | Values | Default | Description |
|---|---|---|---|
| FLOW_UNITS | CFS / GPM / MGD / CMS / LPS / MLD | CFS | Selects the flow unit; a US unit puts every other quantity in US customary units, a metric unit in SI. Concentrations and Manning's n stay metric. |
| INFILTRATION | HORTON / MOD_HORTON / MODIFIED_HORTON / GREEN_AMPT / MOD_GREEN_AMPT / MODIFIED_GREEN_AMPT / CURVE_NUMBER | HORTON | Selects the subcatchment infiltration model. MOD_HORTON and MOD_GREEN_AMPT are this engine's aliases of the legacy MODIFIED_* spellings. |
| FLOW_ROUTING | STEADY / NF / KINWAVE / XKINWAVE / KW / EKW / KINEMATIC_WAVE / DYNWAVE / DW / DYNAMIC_WAVE / FV / FINITE_VOLUME / NONE / NO_ROUTING | DYNWAVE | Selects the conveyance routing method; FV is the explicit finite-volume solver (hydraulics Chapter 8). Legacy spellings match as prefixes (STEADYFLOW is STEADY); NONE and NO_ROUTING set IGNORE_ROUTING. |
| LINK_OFFSETS | DEPTH / ELEVATION | DEPTH | Sets whether link offsets are distances above the node invert (DEPTH) or absolute elevations (ELEVATION). |
| ALLOW_PONDING | YES / NO | NO | Lets excess water pond atop nodes and re-enter as conditions permit; a node also needs a non-zero Ponded Area for ponding to occur. |
| IGNORE_RAINFALL | YES / NO | NO | Skips all rainfall and runoff computation; only user-supplied direct and dry-weather inflows are routed. |
| IGNORE_SNOWMELT | YES / NO | NO | Skips snowmelt computation even when the project defines snow packs. |
| IGNORE_GROUNDWATER | YES / NO | NO | Skips groundwater computation even when the project defines aquifers. |
| IGNORE_RDII | YES / NO | NO | Skips rainfall-dependent infiltration and inflow even when unit hydrographs and RDII inflows are supplied. |
| IGNORE_ROUTING | YES / NO | NO | Computes runoff only, even when the project contains nodes and links. |
| IGNORE_QUALITY | YES / NO | NO | Skips pollutant washoff, routing and treatment in a project that defines pollutants. |

#### Dates and times

| Keyword | Values | Default | Description |
|---|---|---|---|
| START_DATE | MM/DD/YYYY | 01/01/2004 | Date on which the simulation begins. |
| START_TIME | HH:MM:SS | 00:00:00 | Time of day on START_DATE at which the simulation begins. |
| END_DATE | MM/DD/YYYY | — | Date on which the simulation ends. Required: the engine has no fallback, and a run ending on or before its start is rejected at initialisation. |
| END_TIME | HH:MM:SS | 00:00:00 | Time of day on END_DATE at which the simulation ends. |
| REPORT_START_DATE | MM/DD/YYYY | START_DATE value | Date on which reporting of results begins; when omitted, reporting starts with the simulation. |
| REPORT_START_TIME | HH:MM:SS | 00:00:00 | Time of day on REPORT_START_DATE at which reporting begins. Give it together with REPORT_START_DATE; alone it is added to a zero date. |
| SWEEP_START | MM/DD | 1/1 | Day of the year on which street sweeping begins. |
| SWEEP_END | MM/DD | 12/31 | Day of the year on which street sweeping ends. |
| DRY_DAYS | number | 0 | Antecedent days without rainfall before the simulation starts, which seed the initial pollutant buildup. |

#### Time steps

| Keyword | Values | Default | Description |
|---|---|---|---|
| ROUTING_STEP | number or HH:MM:SS | 20 | Routing time step in seconds (fractions allowed) for flows and quality; also the ceiling of the variable dynamic-wave step. |
| VARIABLE_STEP | number | 0.75 | Courant safety factor for the variable dynamic-wave step, which never exceeds ROUTING_STEP; 0 fixes the step. |
| MINIMUM_STEP | number or HH:MM:SS | 0.5 | Smallest routing step, in seconds, the variable dynamic-wave step may take. |
| LENGTHENING_STEP | number | 0 | Time step, in seconds, to which conduits are lengthened to meet the Courant criterion under dynamic wave; 0 lengthens none. |
| WET_STEP | HH:MM:SS | 00:05:00 | Runoff time step during rainfall or while water remains ponded on subcatchments. |
| DRY_STEP | HH:MM:SS | 01:00:00 | Runoff time step (essentially pollutant buildup) during periods with no rainfall and no ponded water. |
| RULE_STEP | number or HH:MM:SS | 0 | Interval, in seconds, at which control rules are evaluated; 0 evaluates them every routing step. |

#### Reporting

| Keyword | Values | Default | Description |
|---|---|---|---|
| REPORT_STEP | HH:MM:SS | 00:15:00 | Interval at which computed results are written to the binary output file. |
| REPORT_SIGNED_HEADS | YES / TRUE / ON / 1 / NO | NO | Writes the true signed piezometric head to the output HEAD field, needed to see sub-atmospheric TPA heads; NO keeps legacy bit-parity. DEPTH stays floored either way. |

#### Dynamic wave solver

| Keyword | Values | Default | Description |
|---|---|---|---|
| MAX_TRIALS | number | 8 | Maximum trials per time step for the nodal head iteration to converge. |
| HEAD_TOLERANCE | number | 0 | Head change between trials, in project length units, below which the step has converged; 0 selects 0.005 ft (0.0015 m). |
| MIN_SURFAREA | number | 0 | Minimum node surface area, in project units, used in depth updates; 0 selects 12.566 ft² (1.167 m²), a 4-ft manhole. |
| MIN_SLOPE | number | 0 | Minimum conduit slope in percent; 0 imposes none, though the elevation drop is still floored at 0.001 ft (0.00035 m). |
| INERTIAL_DAMPING | NONE / PARTIAL / FULL | PARTIAL | Handling of the momentum inertial terms: kept in full (NONE), reduced toward critical flow and dropped when supercritical (PARTIAL), or dropped (FULL). Hydraulics §3.3.3. |
| NORMAL_FLOW_LIMITED | SLOPE / FROUDE / BOTH / NEITHER | BOTH | Test that flags supercritical flow to be limited to normal flow: water-surface slope, Froude number, both, or neither. Hydraulics §3.3.4. |
| NODE_CONTINUITY | EXPLICIT / SEMI_IMPLICIT | EXPLICIT | Node head update: the legacy two-branch formulation, or the unified semi-implicit one that folds flow gradients into the depth update. Hydraulics §3.5. |
| ANDERSON_ACCEL | YES / TRUE / 1 / NO | NO | Accelerates the Picard head iteration with depth-2 Anderson mixing, typically cutting trials by 25 to 50 percent. Hydraulics §3.6. |

#### Pressurisation and friction

| Keyword | Values | Default | Description |
|---|---|---|---|
| SURCHARGE_METHOD | EXTRAN / SLOT / DYNAMIC_SLOT / TPA | EXTRAN | Surcharge treatment under dynamic wave: the EXTRAN algorithm, a Preissmann slot, a dynamic slot with modeller-set celerity (hydraulics §3.3.9), or the two-component pressure approach (§3.3.11; TPA only) \status{Experimental} |
| TPA_CELERITY | number | 100 | Acoustic celerity, in project length units per second, that sizes the constant slot width under SURCHARGE_METHOD TPA. Hydraulics §3.3.11. \status{Experimental} |
| DPS_CELERITY | number | 25.0 | Target pressure-wave celerity in m/s of the dynamic slot; lower values allow larger steps but reduce transient fidelity. Hydraulics §3.3.9. |
| DPS_ALPHA | number | 3.0 | Surcharge shock parameter (at least 2) of the dynamic slot; larger values give a bigger celerity shock at the mixed-flow transition. Hydraulics §3.3.9. |
| DPS_DECAY_TIME | number | 0.5 | Time, in seconds, for the dynamic slot's Preissmann number to decay toward 1; shorter transitions faster but may oscillate. Hydraulics §3.3.9. |
| FORCE_MAIN_EQUATION | H-W / HW / HAZEN-WILLIAMS / D-W / DW / DARCY-WEISBACH | H-W | Friction-loss equation for pressurised flow in Circular Force Main conduits: Hazen-Williams or Darcy-Weisbach. |
| UNSTEADY_FRICTION | NONE / VITKOVSKY | NONE | Adds the Vítkovský-type unsteady friction term to the momentum equation in both the dynamic wave and finite-volume solvers. Hydraulics §3.3.12 and §8.5.4. |
| UF_K3 | number | 0.015 | Brunone-type coefficient k3 of the unsteady friction term; read only when UNSTEADY_FRICTION is not NONE. Literature range 0.005 to 0.020. |

#### Finite-volume routing (FV_*)

These keys are read only under FLOW_ROUTING FV; under any other routing model they are accepted and inert, so switching FLOW_ROUTING never invalidates a file (hydraulics §8.9).

| Keyword | Values | Default | Description |
|---|---|---|---|
| FV_CELL_LENGTH | number | 0 | Target cell length in project length units; 0 sets no target and each conduit gets FV_MIN_CELLS cells. Hydraulics §8.3. |
| FV_MIN_CELLS | number | 4 | Floor on cells per conduit, with or without a length target; a single cell under-conveys at every manhole. Hydraulics §8.3. |
| FV_CFL | number | 0.5 | Courant number bounding the explicit substep. Hydraulics §8.5.5. |
| FV_RIEMANN | HLL / HLLC | HLLC | Face flux solver; HLLC resolves the contact wave that carries advected scalars, HLL is a debugging baseline. Hydraulics §8.5.3. |
| FV_ORDER | 1 / 2 | 1 | Spatial order; 2 enables MUSCL-Hancock reconstruction with the limiter of FV_LIMITER. Hydraulics §8.5.7. |
| FV_LIMITER | MINMOD / VANLEER / SUPERBEE | MINMOD | Slope limiter used with FV_ORDER 2; MINMOD is most robust, SUPERBEE sharpest. Hydraulics §8.5.7. |
| FV_SCALAR_SCHEME | UPWIND / MUSCL / QUICKEST_ULTIMATE / QUICKEST | MUSCL | Reconstruction of the advected scalar field on the finite-volume mesh. Hydraulics §8.8. |
| FV_TIME_INTEGRATION | EULER / RK2 | EULER | Time integrator: forward Euler or SSP-RK2 (Heun); RK2 disables local time stepping. Hydraulics §8.5.5. |
| FV_SLOT_CELERITY | number | 100 | Pressurised wave celerity in project length units per second; sets the slot width and doubles as the TPA acoustic celerity. Hydraulics §8.4.1. |
| FV_PRESSURIZED_IMPLICIT | YES / TRUE / ON / 1 / NO | NO | Integrates the slot's acoustic pair implicitly on pressurised cells, removing the celerity from the time-step law; CPU only, local time stepping stands down. Hydraulics §8.4.4. \status{Experimental} |
| FV_PRESSURE_CLOSURE | SLOT / TPA | SLOT | Pressurisation closure: the Preissmann slot, or the two-component pressure approach that allows sub-atmospheric full-pipe flow. Hydraulics §8.4.5. \status{Experimental} |
| FV_DISPERSION | number | 0 | Longitudinal dispersion coefficient in project length squared per second; 0 disables it. Accepted but inert until finite-volume transport is connected; non-zero warns at open. Hydraulics §8.8. |
| FV_STRUCTURE_COUPLING | SUBSTEP / ROUTING_STEP | SUBSTEP | Cadence at which pump, orifice, weir and outlet flows and outfall stages refresh: every substep, or once per routing step. Hydraulics §8.6.3. |
| FV_COMPACTION | YES / NO / FALSE / 0 / OFF | YES | Skips dry, inactive parts of the network; results-transparent by contract. Hydraulics §8.5.9. |
| FV_LTS | YES / NO / FALSE / 0 / OFF | YES | Local time stepping: stiff cells substep at their own power-of-two tier; NO forces one global substep. Hydraulics §8.5.6. |
| FV_LTS_MAX_TIERS | number | 6 | Cap on the local-time-stepping tier spread; 6 allows a 64-fold spread, and the hard ceiling is 8. Hydraulics §8.5.6. |
| FV_NODE_FEEDBACK_DT | YES / NO / FALSE / 0 / OFF | NO | Bounds the explicit step by the algebraic-junction feedback limit at pressurised junctions; removes oscillation at 10 to 14 times the wall cost. Hydraulics §8.6.1. |
| FV_CFL_CENSUS_INTERVAL | number | 1 | Substeps between full Courant censuses; 1 recomputes every substep. Hydraulics §8.5.5. |
| FV_BACKEND | CPU / AUTO / OMP / CUDA / HIP / SYCL | AUTO | Kernel backend; AUTO tries plugins above FV_MIN_PARALLEL_CELLS and otherwise stays on the CPU. |
| FV_MIN_PARALLEL_CELLS | number | 20000 | Cell count below which FV_BACKEND AUTO stays on the CPU. |

#### Water quality and transport

| Keyword | Values | Default | Description |
|---|---|---|---|
| QUALITY_SOLVER | LEGACY / EULERIAN_ARD / ARD / LAGRANGIAN / LARD | LEGACY | Selects the transport engine: legacy tanks-in-series, Eulerian ARD on fixed cells, or Lagrangian parcels (LARD). Quality §7.1. |
| QUALITY_STEP | number or HH:MM:SS | 0 | LARD transport substep in seconds; each routing step splits into equal substeps. 0 follows ROUTING_STEP; warns under other solvers. Quality §7.4. |
| MAX_SEGMENTS_PER_LINK | number | 100 | Cap on LARD parcels per link; the oldest parcels merge when it is reached. Clamped to at least 2; LARD only. Quality §7.4. |
| DISPERSION | RWPT / OFF / NONE | OFF | Random-walk particle dispersion under QUALITY_SOLVER LAGRANGIAN; warns under other solvers. Distinct from the [TRANSPORT_OPTIONS] and [2D_OPTIONS] keys. Quality §7.4.1. |
| RWPT_SEED | number | 0 | Deterministic seed of the RWPT counter random generator; the same seed reproduces a run bit-for-bit at any thread count. Quality §7.4.1. |
| WATER_AGE | ON / YES / OFF / NO | OFF | Tracks transported water age as the reserved species `__WATER_AGE__`; per-source initial ages come from [WATER_AGE_SOURCES]. Quality §9.2. |
| OUTFALL_BACKFLOW_QUALITY | LAST / ZERO | LAST | Quality carried by reverse flow at outfalls: the held last mixed state (legacy), or a fresh zero boundary for every pollutant and water age. |

#### Heat transport

| Keyword | Values | Default | Description |
|---|---|---|---|
| HEAT_TRANSPORT | ON / YES / OFF / NO | OFF | Transports temperature as the reserved species `__TEMPERATURE__`; inlet temperatures come from [HEAT_SOURCES]. Quality §9.3. |
| WATER_DENSITY | number | 1000.0 | Water density in kg/m³, which weights surface heat fluxes against advected heat. Quality §9.3.9. |
| WATER_SPECIFIC_HEAT_CAPACITY | number | 4184.0 | Specific heat capacity of water in J/kg/°C used to convert surface heat fluxes into temperature change. Quality §9.3.9. |
| WIND_FUNC_COEFF_A | number | 1.505e-8 | Constant term a of the wind function f(w) = a + b·w in the latent-heat flux (Dunne and Leopold 1978). Quality §9.3.4. |
| WIND_FUNC_COEFF_B | number | 1.6e-8 | Wind-speed coefficient b of the wind function f(w) = a + b·w in the latent-heat flux. Quality §9.3.4. |
| PRESSURE_RATIO | number | 1.0 | Ratio of site to sea-level atmospheric pressure that corrects the Bowen ratio in the sensible-heat flux. Quality §9.3.5. |

#### 2D, coordinate reference and files

| Keyword | Values | Default | Description |
|---|---|---|---|
| IGNORE_2D | YES / NO | NO | Disables the 2D surface-routing module even when the model carries a mesh, so the model runs 1D-only without stripping its mesh sections. |
| CRS | EPSG code or quoted PROJ string | — | Coordinate reference system of the model geometry, applied to [COORDINATES], [VERTICES] and [POLYGONS]; readable through swmm_spatial_get_crs(). Empty when omitted. |
| WRITE_ABSOLUTE_PATHS | YES / NO | NO | Makes the .inp writer emit external-file paths absolute instead of rebased relative to the saved file; for tools that reject relative paths. |

#### Miscellaneous

| Keyword | Values | Default | Description |
|---|---|---|---|
| THREADS | number | 1 | OpenMP thread count for dynamic-wave and 2D loops; 0 resolves to the runtime maximum, and larger values oversubscribe with a warning. |
| SKIP_STEADY_STATE | YES / NO | NO | Skips routing during steady periods, reusing the last flows, when system flow and lateral inflows change by less than SYS_FLOW_TOL and LAT_FLOW_TOL. |
| SYS_FLOW_TOL | number | 5 | Maximum percent difference between total system inflow and outflow for SKIP_STEADY_STATE to take effect. |
| LAT_FLOW_TOL | number | 5 | Maximum percent difference between current and previous lateral inflows for SKIP_STEADY_STATE to take effect. |

#### Retired keywords

| Keyword | Status | Behaviour today |
|---|---|---|
| FV_NODE_COUPLING | \status{Retired} | Parsed; the value EXPLICIT warns and is treated as SEMI_IMPLICIT (storage nodes are always coupled semi-implicitly); any other value is silently ignored. |
| FV_NODE_DT | \status{Retired} | Parsed; the value NONE warns and is treated as STABILITY (the node accuracy bound is always armed); any other value is silently ignored. |
| FV_NODE_PICARD | \status{Retired} | Parsed; a value above 1 warns and is treated as 1 (the semi-implicit node correction is always a single sweep); 1 or less is silently accepted. |
| FV_NODE_CELL_COUPLING | \status{Retired} | Accepted and silently ignored; junctions are always algebraic interfaces (hydraulics §8.6.1). |
| FV_JUNCTION_MODEL | \status{Retired} | Accepted and silently ignored; the BUCKET junction model was removed in favour of the algebraic interface (hydraulics §8.6.1). |
| VIRTUAL_JUNCTION_MOMENTUM | \status{Retired} | Parsed; FULL warns and is treated as BASIC (its cross-junction momentum term was not mass conserving); every value maps to BASIC and the key is never written back. |
| COMPATIBILITY | \status{Retired} | Accepted and silently ignored; a legacy SWMM compatibility flag with no effect in this engine. |

### Section: [REPORT] {#engine_manual_sect_REPORT}

Purpose:
Describes the contents of the report file that is produced.

Formats:
DISABLED            YES / NO
INPUT                YES / NO
CONTINUITY        YES / NO
FLOWSTATS        YES / NO
CONTROLS            YES / NO
SUBCATCHMENTS    ALL / NONE / &lt;list of subcatchment names&gt;
NODES                ALL / NONE / &lt;list of node names&gt;
LINKS                ALL / NONE / &lt;list of link names&gt;
LID                Name  Subcatch  Fname

Remarks:
Setting DISABLED to YES disables all reporting (except for error and warning messages) regardless of what other reporting options are chosen.  The default is NO.
INPUT specifies whether or not a summary of the input data should be provided in the output report. The default is NO.
CONTINUITY specifies if continuity checks should be reported or not. The default is YES.
FLOWSTATS specifies whether summary flow statistics should be reported or not. The default is YES.
CONTROLS specifies whether all control actions taken during a simulation should be listed or not. The default is NO.
SUBCATCHMENTS gives a list of subcatchments whose results are to be reported. The default is NONE.
NODES gives a list of nodes whose results are to be reported. The default is NONE.
LINKS gives a list of links whose results are to be reported. The default is NONE.
LID specifies that the LID control Name in subcatchment Subcatch should have a detailed performance report for it written to file Fname.
The SUBCATCHMENTS, NODES, LINKS, and LID lines can be repeated multiple times. 
### Section: [FILES] {#engine_manual_sect_FILES}

Purpose:
Identifies optional interface files used or saved by a run.

Formats:
USE / SAVE    RAINFALL        Fname        
USE / SAVE    RUNOFF        Fname        
USE / SAVE    HOTSTART        Fname        
USE / SAVE    RDII            Fname
USE      INFLOWS                Fname
SAVE    OUTFLOWS                Fname

Parameters:
Fname     is the name of an interface file.

Remarks:
Refer to @ref engine_manual_ch3_files for a description of interface files. Rainfall, Runoff, and RDII files can either be used or saved in a run, but not both. A run can both use and save a Hot Start file (with different names).
Enclose the external file name in double quotes if it contains spaces and include its full path if it resides in a different directory than the SWMM input file. 

 
### Section: [RAINGAGES] {#engine_manual_sect_RAINGAGES}

Purpose:
Identifies each rain gage that provides rainfall data for the study area.

Formats:
Name Form Intvl SCF TIMESERIES Tseries
Name Form Intvl SCF FILE Fname (Sta Units)

Parameters:
Name    name assigned to rain gage.
Form     form of recorded rainfall, either INTENSITY, VOLUME or CUMULATIVE.
Intvl    time interval between gage readings in decimal hours or hours:minutes format (e.g., 0:15 for 15-minute readings).
SCF     snow catch deficiency correction factor (use 1.0 for no adjustment).
Tseries    name of a time series in the [TIMESERIES] section with rainfall data.
Fname    name of an external file with rainfall data. Rainfall files are discussed in @ref engine_manual_ch3_files.
Sta    name of the recording station in a user-prepared formatted rain file.
Units    rain depth units for the data in a user-prepared formatted rain file, either IN (inches) or MM (millimeters).

Remarks:
Enclose the external file name in double quotes if it contains spaces and include its full path if it resides in a different directory than the SWMM input file.
The station name and depth units entries are only required when using a user-prepared formatted rainfall file.

**New in OpenSWMM v6:** A multi-column rain file can be referenced by appending a colon and column name to the file path, e.g. `FILE "rain.csv:EAST_GAGE"`. The engine opens the file, locates the column whose header matches the given name (case-insensitive), and reads the rainfall values from that column; an empty column name selects the first data column. Comma- and tab-delimited files with a header row are supported, as is the PCSWMM `.tsf` format (tab-delimited, `IDs:` header row, 12-hour AM/PM date-times) — the format is detected automatically from the file's contents. This allows a single file to supply data for multiple rain gages (and named time series — see the [TIMESERIES] section); each such file is read from disk only once per model open, regardless of how many gages or series reference it.

 
### Section: [EVAPORATION] {#engine_manual_sect_EVAPORATION}

Purpose:
Specifies how daily potential evaporation rates vary with time for the study area.

Formats:
CONSTANT        evap
MONTHLY        e1 e2 e3 e4 e5 e6 e7 e8 e9 e10 e11 e12
TIMESERIES    Tseries
TEMPERATURE
FILE            (p1 p2 p3 p4 p5 p6 p7 p8 p9 p10 p11 p12)
RECOVERY        patternID
DRY_ONLY        NO / YES

Parameters:
evap        constant evaporation rate (in/day or mm/day).
e1            evaporation rate in January (in/day or mm/day).
...
e12        evaporation rate in December (in/day or mm/day).
Tseries    name of a time series in the [TIMESERIES] section with evaporation data.
p1            pan coefficient for January.
...
p12        pan coefficient for December.
patID        name of a monthly time pattern.

Remarks:
Use only one of the above formats (CONSTANT, MONTHLY, TIMESERIES, TEMPERATURE, or FILE). If no [EVAPORATION] section appears, then evaporation is assumed to be 0.
TEMPERATURE indicates that evaporation rates will be computed from the daily air temperatures contained in an external climate file whose name is provided in the [TEMPERATURE] section. This method also uses the site’s latitude, which can also be specified in the [TEMPERATURE] section.
FILE indicates that evaporation data will be read directly from the same external climate file used for air     temperatures as specified in the [TEMPERATURE] section. Supplying monthly pan coefficients for these data is optional.
RECOVERY identifies an optional monthly time pattern of multipliers used to modify infiltration recovery rates during dry periods. For example, if the normal infiltration recovery rate was 1% during a specific time period and a pattern factor of 0.8 applied to this period, then the actual recovery rate would be 0.8%.
DRY_ONLY determines if evaporation only occurs during periods with no precipitation. The default is NO.
The evaporation rates provided in this section are potential rates. The actual amount of water evaporated will depend on the amount available as a simulation progresses.
 
### Section: [TEMPERATURE] {#engine_manual_sect_TEMPERATURE}

Purpose:    
Specifies daily air temperatures, monthly wind speed, and various snowmelt parameters for the study area. Required only when snowmelt is being modeled or when evaporation rates are computed from daily temperatures or are read from an external climate file.

Formats:
TIMESERIES    Tseries
FILE    Fname (Start) (Units)
WINDSPEED     MONTHLY s1 s2 s3 s4 s5 s6 s7 s8 s9 s10 s11 s12
WINDSPEED    FILE
SNOWMELT         Stemp  ATIwt  RNM  Elev  Lat  DTLong
ADC IMPERVIOUS     f.0 f.1 f.2 f.3 f.4 f.5 f.6 f.7 f.8 f.9
ADC PERVIOUS       f.0 f.1 f.2 f.3 f.4 f.5 f.6 f.7 f.8 f.9

Parameters:
Tseries    name of a time series in the [TIMESERIES] section with temperature data.
Fname    name of an external Climate file with temperature data.
Start    date to begin reading from the file in month/day/year format (default is the beginning of the file).
Units    temperature units for GHCN files (C10 for tenths of a degree C (the default), C for degrees C or F for degrees F.
s1    average wind speed in January (mph or km/hr).
...
s12    average wind speed in December (mph or km/hr). 
Stemp    air temperature at which precipitation falls as snow (deg F or C).
ATIwt    antecedent temperature index weight (default is 0.5).
RNM    negative melt ratio (default is 0.6).
Elev    average elevation of study area above mean sea level (ft or m) (default is 0).
Lat    latitude of the study area in degrees North (default is 50).
DTLong    correction, in minutes of time, between true solar time and the standard clock time (default is 0).
f.0    fraction of area covered by snow when ratio of snow depth to depth at 100% cover is 0
...
f.9    fraction of area covered by snow when ratio of snow depth to depth at 100% cover is 0.9.

Remarks:
Use the TIMESERIES line to read air temperature from a time series or the FILE line to read it from an external Climate file. Climate files are discussed in @ref engine_manual_ch3_files. If neither format is used, then air temperature remains constant at 70 degrees F.
Enclose the Climate file name in double quotes if it contains spaces and include its full path if it resides in a different directory than the SWMM input file.
Temperatures supplied from NOAA's latest Climate Data Online GHCN files should have their units (C or F) specified. Older versions of these files listed temperatures in tenths of a degree C (C10). An asterisk can be entered for the Start date if it defaults to the beginning of the file.
Wind speed can be specified either by monthly average values or by the same Climate file used for air temperature. If neither option appears, then wind speed is assumed to be 0.
Separate Areal Depletion Curves (ADC) can be defined for impervious and pervious subareas. The ADC parameters will default to 1.0 (meaning no depletion) if no data are supplied for a particular type of subarea.

 
### Section: [ADJUSTMENTS] {#engine_manual_sect_ADJUSTMENTS}

Purpose:
Specifies optional monthly adjustments to be made to temperature, evaporation rate, rainfall intensity and hydraulic conductivity in each time period of a simulation.

Formats:
TEMPERATURE    t1 t2 t3 t4 t5 t6 t7 t8 t9 t10 t11 t12 
EVAPORATION    e1 e2 e3 e4 e5 e6 e7 e8 e9 e10 e11 e12
RAINFALL        r1 r2 r3 r4 r5 r6 r7 r8 r9 r10 r11 r12 
CONDUCTIVITY    c1 c2 c3 c4 c5 c6 c7 c8 c9 c10 c11 c12

Parameters:
  t1..t12    adjustments to temperature in January, February, etc., as plus or minus degrees F (degrees C).
e1..e12    adjustments to evaporation rate in January, February, etc., as plus or minus in/day (mm/day).
r1..r12    multipliers applied to precipitation rate in January, February, etc.
c1..c12    multipliers applied to soil hydraulic conductivity in January, February, etc. used in either Horton or Green-Ampt infiltration.
Remarks:
The same adjustment is applied for each time period within a given month and is repeated for that month in each subsequent year being simulated.



 
### Section: [SUBCATCHMENTS] {#engine_manual_sect_SUBCATCHMENTS}

Purpose:
Identifies each subcatchment within the study area. Subcatchments are land area units which generate runoff from rainfall.

Format:
Name Rgage OutID Area %Imperv Width Slope Clength (Spack)

Parameters:
Name    name assigned to the subcatchment.
Rgage    name of a rain gage in the [RAINGAGES] section assigned to the subcatchment.
OutID    name of the node or subcatchment that receives runoff from the subcatchment.
Area    area of the subcatchment (acres or hectares).
%Imperv    percentage of the subcatchment’s area that is impervious.
Width    characteristic width of the subcatchment (ft or meters).
Slope    the subcatchment’s slope (percent).
Clength    total curb length (any length units) used to describe pollutant buildup. Use 0 if not applicable.
Spack    optional name of a snow pack object (from the [SNOWPACKS] section) that characterizes snow accumulation and melting over the subcatchment.

 
### Section: [SUBAREAS] {#engine_manual_sect_SUBAREAS}

Purpose:
Supplies information about pervious and impervious areas for each subcatchment. Each subcatchment can consist of a pervious subarea, an impervious subarea with depression storage, and an impervious subarea without depression storage.

Format:
Subcat Nimp Nperv Simp Sperv %Zero RouteTo (%Routed)

Parameters:
Subcat    subcatchment name.
Nimp    Manning's coefficient (n) for overland flow over the impervious subarea.
Nperv    Manning's coefficient (n) for overland flow over the pervious subarea.
Simp    depression storage for the impervious subarea (inches or mm).
Sperv    depression storage for the pervious subarea (inches or mm).
%Zero    percent of impervious area with no depression storage.
RouteTo    IMPERVIOUS if pervious area runoff runs onto impervious area,  PERVIOUS if impervious runoff runs onto pervious area, or OUTLET if both areas drain to the subcatchment's outlet (default = OUTLET).
%Routed    percent of runoff routed from one type of area to another (default = 100).
 
### Section: [INFILTRATION] {#engine_manual_sect_INFILTRATION}

Purpose:
Supplies infiltration parameters for each subcatchment. Rainfall lost to infiltration only occurs over the pervious subarea of a subcatchment.

Format:
Subcat  p1  p2  p3  (p4  p5)  (Method)

Parameters:
Subcat    subcatchment name.
Method    either HORTON, MODIFIED_HORTON, GREEN_AMPT,   MODIFIED_GREEN_AMPT, or CURVE_NUMBER.
If not specified then the infiltration method supplied in the [OPTIONS] section is used.
For Horton and Modified Horton Infiltration:
p1    maximum infiltration rate on the Horton curve (in/hr or mm/hr).
p2    minimum infiltration rate on the Horton curve (in/hr or mm/hr).
p3    decay rate constant of the Horton curve (1/hr).
p4    time it takes for a fully saturated soil to dry  (days).
p5    maximum infiltration volume possible (0 if not applicable) (in or mm).
For Green-Ampt and Modified Green-Ampt Infiltration:
p1    soil capillary suction (in or mm).
p2    soil saturated hydraulic conductivity (in/hr or mm/hr).
p3    initial soil moisture deficit (porosity minus moisture content) (fraction).
For Curve-Number Infiltration:
p1    SCS Curve Number.
p2    no longer used.
p3    time it takes for a fully saturated soil to dry (days). 
### Section: [LID_CONTROLS] {#engine_manual_sect_LID_CONTROLS}

Purpose:
Defines scale-independent LID controls that can be deployed within subcatchments.

Formats:
Name     Type
followed by one or more of the following lines depending on Type:
Name SURFACE  StorHt VegFrac Rough Slope Xslope
Name SOIL     Thick Por FC WP Ksat Kcoeff Suct
Name PAVEMENT Thick Vratio FracImp Perm Vclog (Treg Freg)
Name STORAGE  Height Vratio Seepage Vclog (Covrd)
Name DRAIN    Coeff Expon Offset Delay (Hopen Hclose Qcrv)
Name DRAINMAT Thick Vratio Rough
Name REMOVALS Pollut Rmvl Pollut Rmvl ... 

Parameters:
Name    name assigned to LID process.
Type    BC for bio-retention cell; RG for rain garden; GR for green roof; IT for infiltration trench; PP for permeable pavement; RB for rain barrel; RD for rooftop disconnection; VS for vegetative swale.
Pollut    name of a pollutant
Rmvl    the percent removal the LID achieves for the pollutant (several pollutant removals can be placed on the same line or specified in separate REMOVALS lines).

For LIDs with Surface Layers:
StorHt    when confining walls or berms are present this is the maximum depth to which water can pond above the surface of the unit before overflow occurs (in inches or mm). For LIDs that experience overland flow it is the height of any surface depression storage. For swales, it is the height of its trapezoidal cross-section.
VegFrac    fraction of the surface storage volume that is filled with vegetation.
Rough    Manning's coefficient (n) for overland flow over surface soil cover, pavement, roof surface or a vegetative swale. Use 0 for other types of LIDs.
Slope     slope of a roof surface, pavement surface or vegetative swale (percent). Use 0 for other types of LIDs.
Xslope    slope (run over rise) of the side walls of a vegetative swale's cross-section. Use 0 for other types of LIDs.
If either Rough or Slope values are 0 then any ponded water that exceeds the surface storage depth is assumed to completely overflow the LID control within a single time step.

For LIDs with Pavement Layers:
Thick    thickness of the pavement layer (inches or mm).
Vratio    void ratio (volume of void space relative to the volume of solids in the pavement for continuous systems or for the fill material used in modular systems). Note that porosity = void ratio / (1 + void ratio).
FracImp    ratio of impervious paver material to total area for modular systems; 0 for continuous porous pavement systems.
Perm    permeability of the concrete or asphalt used in continuous systems or hydraulic conductivity of the fill material (gravel or sand) used in modular systems (in/hr or mm/hr).
Vclog    the number of pavement layer void volumes of runoff treated it takes to completely clog the pavement. Use a value of 0 to ignore clogging.
Treg    the number of days that the pavement layer is allowed to clog before its permeability is restored, typically by vacuuming its surface. A value of 0 (the default) indicates that no permeability regeneration occurs.
Freg    The fractional degree to which the pavement's permeability is restored when a regeneration interval is reached. The default is 0 (no restoration) while a value of 1 indicates complete restoration to the original permeability value. Once regeneration occurs the pavement begins to clog once again at a rate determined by Vclog.

For LIDs with Soil Layers:
Thick    thickness of the soil layer (inches or mm).
Por    soil porosity (pore space volume / total volume).
FC    soil field capacity (moisture content of a fully drained soil).
WP    soil wilting point (moisture content of a fully dried soil).
Ksat    soil’s saturated hydraulic conductivity (in/hr or mm/hr).
Kcoeff    slope of the curve of log(conductivity) versus soil moisture deficit (porosity minus soil moisture) (dimensionless).
Suct    soil capillary suction (in or mm).

For LIDs with Storage Layers:
Height    thickness of the storage layer or height of a rain barrel (inches or mm).
Vratio    void ratio (volume of void space relative to the volume of solids in the layer). Note that porosity = void ratio / (1 + void ratio). 
Seepage    the rate at which water seeps from the layer into the underlying native soil when first constructed (in/hr or mm/hr). If there is an impermeable floor or liner below the layer then use a value of 0.
Vclog    number of storage layer void volumes of runoff treated it takes to completely clog the layer. Use a value of 0 to ignore clogging.
Covrd    YES (the default) if a rain barrel is covered, NO if it is not. 
Values for Vratio, Seepage, and Vclog  are ignored for rain barrels while Covrd applies only to rain barrels.

For LIDs with Drain Systems:
Coeff    coefficient C that determines the rate of flow through the drain as a function of height of stored water above the drain bottom. For Rooftop Disconnection it is the maximum flow rate (in inches/hour or mm/hour) that the roof’s gutters and downspouts can handle before overflowing.
Expon    exponent n that determines the rate of flow through the drain as a function of height of stored water above the drain outlet.
Offset    height of the drain line above the bottom of the storage layer or rain barrel (inches or mm).
Delay     number of dry weather hours that must elapse before the drain line in a rain barrel is opened (the line is assumed to be closed once rainfall begins). A value of 0 signifies that the barrel's drain line is always open and drains continuously.  This parameter is ignored for other types of LIDs.
Hopen    The height of water  (in inches or mm) in the drain's Storage Layer that causes the drain to automatically open. Use 0 to disable this feature.
Hclose    The height of water (in inches or mm) in the drain's Storage Layer that causes the drain to automatically close. Use 0 to disable this feature.
Qcurve    The name of an optional Control Curve that adjusts the computed drain flow as a function of the head of water above the drain. Leave blank if not applicable.

For Green Roof LIDs with Drainage Mats:
Thick    thickness of the drainage mat (inches or mm).
Vratio     ratio of void volume to total volume in the mat.
Rough    Manning's coefficient (n) used to compute the horizontal flow rate of drained water through the mat.

Remarks:
The following table shows which layers are required (x) or are optional (o) for each type of LID process:

LID Type    Surface    Pavement    Soil    Storage    Drain    Drain Mat
Bio-Retention Cell    x        x    x    o    
Rain Garden    x        x            
Green Roof    x        x            x
Infiltration Trench    x            x    o    
Permeable Pavement    x    x    o    x    o    
Rain Barrel                x    x    
Rooftop Disconnection    x                x    
Vegetative Swale    x                    

The equation used to compute flow rate out of the underdrain per unit area of the LID (in in/hr or mm/hr) is   where q is outflow, h is height of stored water (inches or mm) and Hd is the drain offset height. Note that the units of C depend on the unit system being used as well as the value assigned to n.
The actual dimensions of an LID control are provided in the [LID_USAGE] section when it is placed in a particular subcatchment.


Examples:
;A street planter with no drain
Planter  BC
Planter  SURFACE   6  0.3  0    0     0
Planter  SOIL     24  0.5  0.1  0.05  1.2  2.4
Planter  STORAGE  12  0.5  0.5  0

;A green roof with impermeable bottom
GR1  BC
GR1  SURFACE  3  0    0    0     0
GR1  SOIL     3  0.5  0.1  0.05  1.2  2.4
GR1  STORAGE  3  0.5  0    0
GR1  DRAIN    5  0.5  0    0

;A rain barrel that drains 6 hours after rainfall ends
RB12  RB
RB12  STORAGE  36  0    0  0
RB12  DRAIN    10  0.5  0  6

;A grass swale 24 in. high with 5:1 side slope
Swale  VS
Swale  SURFACE  24  0  0.2  3  5

 
### Section: [LID_USAGE] {#engine_manual_sect_LID_USAGE}

Purpose:
Deploys LID controls within specific subcatchment areas.

Format:
Subcat LID Number Area Width InitSat FromImp ToPerv
(RptFile DrainTo FromPerv)                                                   
                                        
Parameters:
Subcat    name of the subcatchment using the LID process.
LID    name of an LID process defined in the [LID_CONTROLS] section.
Number    number of replicate LID units deployed.
Area    area of each replicate unit (ft2 or m2).
Width    width of the outflow face of each identical LID unit (in ft or m). This parameter applies to roofs, pavement, trenches, and swales that use overland flow to convey surface runoff off of the unit. It can be set to 0 for other LID processes, such as bio-retention cells, rain gardens, and rain barrels that simply spill any excess captured runoff over their berms.
InitSat    the percent to which the LID's soil, storage, and drain mat zones are initially filled with water. For soil zones 0 % saturation corresponds to the wilting point moisture content while 100 % saturation has the moisture content equal to the porosity.
FromImp    the percent of the impervious portion of the subcatchment’s non-LID area whose runoff is treated by the LID practice. (E.g., if rain barrels are used to capture roof runoff and roofs represent 60% of the impervious area, then the impervious area treated is 60%). If the LID unit treats only direct rainfall, such as with a green roof, then this value should be 0. If the LID takes up the entire subcatchment then this field is ignored.
ToPerv    a value of 1 indicates that the surface and drain flow from the LID unit should be routed back onto the pervious area of the subcatchment that contains it. This would be a common choice to make for rain barrels, rooftop disconnection, and possibly green roofs. The default value is 0.


RptFile    optional name of a file to which detailed time series results for the LID will be written. Enclose the name in double quotes if it contains spaces and include its full path if it resides in a different directory than the SWMM input file. Use ‘*’ if not applicable and an entry for DrainTo or FromPerv follows
DrainTo     optional name of subcatchment or node that receives flow from the unit’s drain line, if different from the outlet of the subcatchment that the LID is placed in. Use ‘*’ if not applicable and an entry for FromPerv follows.
FromPerv    optional percent of the pervious portion of the subcatchment’s non-LID area whose runoff is treated by the LID practice. The default value is 0.

Remarks:
If ToPerv is set to 1 and DrainTo set to some other outlet, then only the excess surface flow from the LID unit will be routed back to the subcatchment’s pervious area while the underdrain flow will be sent to DrainTo.
More than one type of LID process can be deployed within a subcatchment as long as their total area does not exceed that of the subcatchment and the total percent impervious area treated does not exceed 100.

Examples:
;34 rain barrels of 12 sq ft each are placed in
;subcatchment S1. They are initially empty and treat 17%
;of the runoff from the subcatchment’s impervious area.
;The outflow from the barrels is returned to the ;subcatchment’s pervious area.
S1  RB14  34  12  0  0  17  1

;Subcatchment S2 consists entirely of a single vegetative ;swale 200 ft long by 50 ft wide.
S2  Swale  1  10000  50  0  0  0  “swale.rpt”

 
### Section: [AQUIFERS] {#engine_manual_sect_AQUIFERS}

Purpose:
Supplies parameters for each unconfined groundwater aquifer in the study area. Aquifers consist of two zones – a lower saturated zone and an upper unsaturated zone with a moving boundary between the two.

Format:
Name Por WP FC Ks Kslp Tslp ETu ETs Seep Ebot Egw Umc (Epat)                      

Parameters:
Name    name assigned to aquifer.
Por    soil porosity (pore space volume / total volume).
WP    soil wilting point (moisture content of a fully dried soil).
FC    soil field capacity (moisture content of a fully drained soil).
Ks    saturated hydraulic conductivity (in/hr or mm/hr).
Kslp    slope of the logarithm of hydraulic conductivity versus moisture deficit (porosity minus moisture content) curve (dimensionless).
Tslp    slope of soil tension versus moisture content curve (inches or mm).
ETu    fraction of total evaporation available for evapotranspiration in the upper unsaturated zone.
ETs    maximum depth into the lower saturated zone over which evapotranspiration can occur (ft or m).
Seep    seepage rate from saturated zone to deep groundwater when water table is at ground surface (in/hr or mm/hr).
Ebot    elevation of the bottom of the aquifer (ft or m).
Egw    groundwater table elevation at start of simulation (ft or m).
Umc    unsaturated zone moisture content at start of simulation (volumetric fraction).
Epat    name of optional monthly time pattern used to adjust the upper zone evaporation fraction for different months of the year.
Remarks:
Local values for Ebot, Egw, and Umc can be assigned to specific subcatchments in the [GROUNDWATER] section.
 
### Section: [GROUNDWATER] {#engine_manual_sect_GROUNDWATER}

Purpose:
Supplies parameters that determine the rate of groundwater flow between the aquifer underneath a subcatchment and a node of the conveyance system.

Format:
Subcat Aquifer Node Esurf A1 B1 A2 B2 A3 Dsw (Egwt Ebot Egw Umc)

Parameters:
Subcat    subcatchment name.
Aquifer    name of groundwater aquifer underneath the subcatchment.
Node    name of a node in the conveyance system exchanging groundwater with the aquifer.
Esurf    surface elevation of the subcatchment (ft or m).
A1    groundwater flow coefficient (see below).
B1    groundwater flow exponent (see below).
A2    surface water flow coefficient (see below).
B2    surface water flow exponent (see below).
A3    surface water – groundwater interaction coefficient (see below).
Dsw    fixed depth of surface water at the receiving node (ft or m) (set to zero if surface water depth will vary as computed by flow routing).
Egwt    threshold groundwater table elevation which must be reached before any flow occurs (ft or m). Leave blank (or enter *) to use the elevation of the receiving node's invert.
The following optional parameters can be used to override the values supplied for the subcatchment’s aquifer.
Ebot    elevation of the bottom of the aquifer (ft or m).
Egw    groundwater table elevation at the start of the simulation (ft or m).
Umc    unsaturated zone moisture content at start of simulation (volumetric fraction).




Remarks:
The flow coefficients are used in the following equation that determines the lateral groundwater flow rate based on groundwater and surface water elevations:
    QL  =  A1 (Hgw  –  Hcb) B1  –  A2 (Hsw  –  Hcb) B2  +  A3 Hgw Hsw
where:
QL   =    lateral groundwater flow (cfs per acre or cms per hectare),
Hgw   =     height of saturated zone above the bottom of the aquifer (ft or m),
Hsw   =    height of surface water at the receiving node above the aquifer bottom (ft or m),
Hcb   =    height of the channel bottom above the aquifer bottom (ft or m).
 
### Section: [GWF] {#engine_manual_sect_GWF}

Purpose:
Defines custom groundwater flow equations for specific subcatchments.

Format:
Subcat  LATERAL/DEEP  Expr

Parameters:
Subcat    subcatchment name.
Expr    a math formula expressing the rate of groundwater flow (in cfs per acre or cms per hectare for lateral flow or in/hr or mm/hr for deep flow) as a function of the following variables:
Hgw    (for height of the groundwater table)
Hsw     (for height of the surface water)
Hcb     (for height of the channel bottom)
Hgs    (for height of ground surface)
where all heights are relative to the aquifer bottom and have units of either feet or meters;
Ks        (for saturated hydraulic conductivity in in/hr or mm/hr)
K        (for unsaturated hydraulic conductivity in in/hr or mm/hr)
Theta    (for moisture content of the unsaturated zone)
Phi    (for aquifer soil porosity)
Fi        (for infiltration rate from the ground surface in in/hr or mm/hr)
Fu    (for percolation rate from the upper unsaturated zone in in/hr or mm/hr)
A        (for subcatchment area in acres or hectares)

Remarks:
Use LATERAL to designate an expression for lateral groundwater flow (to a node of the conveyance network) and DEEP for vertical loss to deep groundwater.
See the [TREATMENT] section for a list of built-in math functions that can be used in Expr. In particular, the STEP(x) function is 1 when x > 0 and is 0 otherwise. 




Examples:
;Two-stage linear reservoir for lateral flow
Subcatch1 LATERAL 0.001*Hgw + 0.05*(Hgw–5)*STEP(Hgw–5)

;Constant seepage rate to deep aquifer
Subactch1  DEEP  0.002 
### Section: [SNOWPACKS] {#engine_manual_sect_SNOWPACKS}

Purpose:
Specifies parameters that govern how snowfall accumulates and melts on the plowable, impervious and pervious surfaces of subcatchments.

Formats:
Name PLOWABLE   Cmin  Cmax  Tbase  FWF  SD0  FW0  SNN0
Name IMPERVIOUS Cmin  Cmax  Tbase  FWF  SD0  FW0  SD100  
Name PERVIOUS   Cmin  Cmax  Tbase  FWF  SD0  FW0  SD100
Name REMOVAL      Dplow Fout Fimp Fperv Fimelt (Fsub Scatch)

Parameters:
Name    name assigned to snowpack parameter set .
Cmin    minimum melt coefficient (in/hr-deg F or mm/hr-deg C).
Cmax    maximum melt coefficient (in/hr-deg F or mm/hr-deg C).
Tbase    snow melt base temperature (deg F or deg C).
FWF    ratio of free water holding capacity to snow depth (fraction).
SD0    initial snow depth (in or mm water equivalent).
FW0    initial free water in pack (in or mm).
SNN0    fraction of impervious area that can be plowed.
SD100    snow depth above which there is 100% cover (in or mm water equivalent).
Dplow    depth of snow on plowable areas at which snow removal begins (in or mm).
Fout    fraction of snow on plowable area transferred out of watershed.
Fimp    fraction of snow on plowable area transferred to impervious area by plowing.
Fperv    fraction of snow on plowable area transferred to pervious area by plowing.
Fimelt    fraction of snow on plowable area converted into immediate melt.
Fsub    fraction of snow on plowable area transferred to pervious area in another subcatchment.
Scatch    name of subcatchment receiving the Fsub fraction of transferred snow.


Remarks:
Use one set of PLOWABLE, IMPERVIOUS, and PERVIOUS lines for each snow pack parameter set created. Snow pack parameter sets are assigned to specific subcatchments in the [SUBCATCHMENTS] section. Multiple subcatchments can share the same set of snow pack parameters.
The PLOWABLE line contains parameters for the impervious area of a subcatchment that is subject to snow removal by plowing but not to areal depletion. This area is the fraction SNN0 of the total impervious area. The IMPERVIOUS line contains parameter values for the remaining impervious area and the PERVIOUS line does the same for the entire pervious area. Both of the latter two areas are subject to areal depletion. 
The REMOVAL line describes how snow removed from the plowable area is transferred onto other areas. The various transfer fractions should sum to no more than 1.0. If the line is omitted then no snow removal takes place.


 
### Section: [JUNCTIONS] {#engine_manual_sect_JUNCTIONS}

Purpose:
Identifies each junction node of the drainage system.  Junctions are points in space where channels and pipes connect together. For sewer systems they can be either connection fittings or manholes.

Format:
Name  Elev  (Ymax  Y0  Ysur  Apond)

Parameters:
Name    name assigned to junction node.
Elev    elevation of the junction’s invert (ft or m).
Ymax    depth from ground to invert elevation (ft or m) (default is 0).
Y0    water depth at the start of the simulation (ft or m) (default is 0).
Ysur    maximum additional pressure head above the ground elevation that the junction can sustain under surcharge conditions (ft or m) (default is 0).
Apond    area subjected to surface ponding once water depth exceeds Ymax + Ysur  (ft2 or m2) (default is 0).

Remarks:
If Ymax is 0 then SWMM sets the junction’s maximum depth to the distance from its invert to the top of the highest connecting link.
If the junction is part of a force main section of the system then set Ysur to the maximum pressure that the system can sustain.
Surface ponding can only occur when Apond is non-zero and the ALLOW_PONDING analysis option is turned on.


 
### Section: [OUTFALLS] {#engine_manual_sect_OUTFALLS}

Purpose:
Identifies each outfall node (i.e., final downstream boundary) of the drainage system and the corresponding water stage elevation.  Only one link can be incident on an outfall node. 

Formats:
Name  Elev  FREE        (Gated)    (RouteTo)
Name  Elev  NORMAL      (Gated)    (RouteTo)
Name  Elev  FIXED       Stage   (Gated)    (RouteTo)
Name  Elev  TIDAL       Tcurve  (Gated)    (RouteTo)
Name  Elev  TIMESERIES  Tseries (Gated)    (RouteTo)

Parameters:
Name    name assigned to outfall node.
Elev    node’s invert elevation (ft or m).
Stage    elevation of a fixed stage outfall (ft or m).
Tcurve     name of a curve in the [CURVES] section containing tidal height (i.e., outfall stage) versus hour of day over a complete tidal cycle.
Tseries    name of a time series in [TIMESERIES] section that describes how outfall stage varies with time.
Gated    YES or NO depending on whether a flap gate is present that prevents reverse flow. The default is NO.
RouteTo    optional name of a subcatchment that receives the outfall's discharge. The default is not to route the outfall’s discharge.





 
### Section: [DIVIDERS] {#engine_manual_sect_DIVIDERS}

Purpose:
Identifies each flow divider node of the drainage system. Flow dividers are junctions with exactly two outflow conduits where the total outflow is divided between the two in a prescribed manner.

Formats:
Name Elev DivLink OVERFLOW (Ymax Y0 Ysur Apond)
Name Elev DivLink CUTOFF  Qmin (Ymax Y0 Ysur Apond)
Name Elev DivLink TABULAR Dcurve (Ymax Y0 Ysur Apond)
Name Elev DivLink WEIR    Qmin Ht Cd (Ymax Y0 Ysur Apond)

Parameters:
Name    name assigned to divider node.
Elev    node’s invert elevation (ft or m).
DivLink    name of the link to which flow is diverted.
Qmin    flow at which diversion begins for either a CUTOFF or WEIR divider (flow units).
Dcurve    name of a curve for a TABULAR divider that relates diverted flow to total flow.
Ht    height of a WEIR divider (ft or m).
Cd    discharge coefficient for a WEIR divider.
Ymax    depth from the ground to the node’s invert elevation (ft or m) (default is 0).
Y0    water depth at the start of the simulation (ft or m) (default is 0).
Ysur    maximum additional pressure head above the ground elevation that the node can sustain under surcharge conditions (ft or m) (default is 0).
Apond    area subjected to surface ponding once water depth exceeds Ymax + Ysur  (ft2 or m2) (default is 0).




Remarks:
If Ymax is 0 then SWMM sets the node’s maximum depth equal to the distance from its invert to the top of the highest connecting link.
Surface ponding can only occur when Apond is non-zero and the ALLOW_PONDING analysis option is turned on.
Divider nodes are only active under the Steady Flow or Kinematic Wave analysis options. For Dynamic Wave flow routing they behave the same as Junction nodes.


 
### Section: [STORAGE] {#engine_manual_sect_STORAGE}

Purpose:
Identifies each storage node of the drainage system. Storage nodes can have any shape as specified by a surface area versus water depth relation.

Formats:    
Name Elev Ymax Y0 TABULAR    Acurve   (Ysur Fevap Psi Ksat IMD)
Name Elev Ymax Y0 FUNCTIONAL A1 A2 A0 (Ysur Fevap Psi Ksat IMD)
Name Elev Ymax Y0 Shape      L  W  Z  (Ysur Fevap Psi Ksat IMD)

Parameters:
Name    name assigned to storage node.
Elev    node’s invert elevation (ft or m).
Ymax    water depth when the storage node is full (ft or m).
Y0    water depth at the start of the simulation (ft or m).
Acurve    name of a curve in the [CURVES] section that relates surface area (ft2 or m2) to depth (ft or m) for TABULAR geometry.
A1    coefficient of a FUNCTIONAL relation between surface area and depth.
A2    exponent of a FUNCTIONAL relation between surface area and depth.
A0    constant of a FUNCTIONAL relation between surface area and depth.
Shape    shape used to relate surface area to depth; choices are CYLINDRICAL, CONICAL, PARABOLOID, or PYRAMIDAL.
Ysur    maximum additional pressure head above full depth that a closed storage unit can sustain under surcharge conditions (ft or m) (default is 0).
L, W, Z    dimensions of the storage unit's shape (see table below).  
Fevap    fraction of potential evaporation from the storage unit’s water surface realized (default is 0).
Optional seepage parameters for soil surrounding the storage unit:
Psi     suction head (inches or mm).
Ksat    saturated hydraulic conductivity  (in/hr or mm/hr).
IMD    initial moisture deficit (porosity minus moisture content) (fraction).
Remarks:
A1, A2, and A0 are used in the following expression that relates surface area (ft2 or m2) to water depth (ft or m) for a storage unit with FUNCTIONAL geometry:
Area=A0+A1〖Depth〗^A2
For TABULAR geometry, the surface area curve will be extrapolated outwards to meet the unit's maximum depth if need be.
The dimensions of storage units with other shapes are defined as follows:
Shape    L    W    Z
CYLINDRICAL
      major axis length    minor axis width    not used
CONICAL
            major axis length of base    minor axis width of base    side slope (run/rise)
PARABOLOID
         major axis length at full height    minor axis width at full height    full height
PYRAMIDAL
         base length    base width    side slope (run/rise)

The parameters Psi, Ksat, and IMD need only be supplied if seepage loss through the soil at the bottom and sloped sides of the storage unit should be considered. They are the same Green-Ampt infiltration parameters described in the [INFILTRATION] section. If Ksat is zero then no seepage occurs while if IMD is zero then seepage occurs at a constant rate equal to Ksat. Otherwise seepage rate will vary with storage depth.
 
### Section: [CONDUITS] {#engine_manual_sect_CONDUITS}

Purpose:
Identifies each conduit link of the drainage system. Conduits are pipes or channels that convey water from one node to another.

Format:
Name  Node1  Node2  Length  N  Z1  Z2  (Q0  Qmax)

Parameters:
Name    name assigned to conduit link.
Node1    name of the conduit’s upstream node.
Node2    name of the conduit’s downstream node.
Length    conduit length (ft or m).
N    Manning’s roughness coefficient (n).
Z1    offset of the conduit’s upstream end above the invert of its upstream node (ft or m).
Z2    offset of the conduit’s downstream end above the invert of its downstream node (ft or m).
Q0    flow in the conduit at the start of the simulation (flow units) (default is 0).
Qmax    maximum flow allowed in the conduit (flow units) (default is no limit).

Remarks:
The figure below illustrates the meaning of the Z1 and Z2 parameters. 
     
These offsets are expressed as a relative distance above the node invert if the LINK_OFFSETS option is set to DEPTH (the default) or as an absolute elevation if it is set to ELEVATION. 


### Section: [PUMPS] {#engine_manual_sect_PUMPS}

Purpose:
Identifies each pump link of the drainage system.

Format:
Name  Node1  Node2  Pcurve  (Status  Startup  Shutoff)

Parameters:
Name        name assigned to pump link. 
Node1        name of the pump’s inlet node.
Node2        name of the pump’s outlet node.
Pcurve        name of a pump curve listed in the [CURVES] section of the input.
Status    pump’s status at the start of the simulation (either ON or OFF; default is ON).
Startup        depth at the inlet node when the pump turns on (ft or m) (default is 0).
Shutoff        depth at inlet node when the pump shuts off (ft or m) (default is 0).

Remarks:
See @ref engine_manual_ch1_conceptual_model for a description of the different types of pumps available.


 
### Section: [ORIFICES] {#engine_manual_sect_ORIFICES}

Purpose:
Identifies each orifice link of the drainage system. An orifice link serves to limit the flow exiting a node and is often used to model flow diversions and storage node outlets.

Format:
Name  Node1  Node2  Type  Offset  Cd  (Gated  Orate)

Parameters:
Name    name assigned to orifice link.
Node1    name of the orifice’s inlet node.
Node2    name of the orifice’s outlet node.
Type    the type of orifice - either SIDE if oriented in a vertical plane or BOTTOM if oriented in a horizontal plane.
Offset    amount that a Side Orifice’s bottom or the position of a Bottom Orifice is offset above the invert of inlet node (ft or m, expressed as either a depth or as an elevation, depending on the LINK_OFFSETS option setting).
Cd    discharge coefficient (unitless).
Flap    YES if a flap gate prevents reverse flow, NO if not (default is NO).
Orate    time in decimal hours to open a fully closed orifice (or close a fully open one). Use 0 if the orifice can open/close instantaneously.

Remarks:
The geometry of an orifice’s opening must be described in the [XSECTIONS] section. The only allowable shapes are CIRCULAR and RECT_CLOSED (closed rectangular).









 
### Section: [WEIRS] {#engine_manual_sect_WEIRS}

Purpose:
Identifies each weir link of the drainage system. Weirs are used to model flow diversions and storage node outlets.

Format:    
Name Node1 Node2 Type CrstHt Cd (Gated EC Cd2 Sur (Width Surf))

Parameters:
Name    name assigned to weir link.
Node1    name of the weir’s inlet node.
Node2    name of the weir’s outlet node.
Type    TRANSVERSE, SIDEFLOW, V-NOTCH, TRAPEZOIDAL or ROADWAY.
CrstHt    amount that the weir’s opening is offset above the invert of inlet node (ft or m, expressed as either a depth or as an elevation, depending on the LINK_OFFSETS option setting).
Cd    weir discharge coefficient (for CFS if using US flow units or CMS if using metric flow units).
Gated    YES if a flap gate prevents reverse flow, NO if not (default is NO).
EC    number of end contractions for a TRANSVERSE or TRAPEZOIDAL weir (default is 0).
Cd2    discharge coefficient for the triangular ends of a TRAPEZOIDAL weir (for CFS if using US flow units or CMS if using metric flow units) (default is the value of Cd).
Sur    YES if the weir can surcharge (have an upstream water level higher than the height of the weir’s opening); NO if it cannot (default is YES).
The following parameters apply only to ROADWAY weirs:
Width    width of road lanes and shoulders for a ROADWAY weir (ft or m).
Surf    type of road surface for a ROADWAY weir: PAVED or GRAVEL.

Remarks:
The geometry of a weir’s opening is described in the [XSECTIONS] section. The following shapes must be used with each type of weir:

Weir Type    Cross-Section Shape
Transverse    RECT_OPEN
Sideflow    RECT_OPEN
V-Notch    TRIANGULAR
Trapezoidal    TRAPEZOIDAL
Roadway    RECT_OPEN

The ROADWAY weir is a broad crested rectangular weir used model roadway crossings usually in conjunction with culvert-type conduits. It uses the FHWA HDS-5 method to determine a discharge coefficient as a function of flow depth and roadway width and surface. If no roadway data are provided then the weir behaves as a TRANSVERSE weir with Cd as its discharge coefficient. Note that if roadway data are provided, then values for the other optional weir parameters (NO for Gated, 0 for EC, 0 for Cd2, and NO for Sur) must be entered even though they do not apply to ROADWAY weirs.



 
### Section: [OUTLETS] {#engine_manual_sect_OUTLETS}

Purpose:
Identifies each outlet flow control device of the drainage system. These are devices used to model outflows from storage units or flow diversions that have a user-defined relation between flow rate and water depth.

Formats:
Name Node1 Node2 Offset TABULAR/DEPTH Qcurve (Gated)
Name Node1 Node2 Offset TABULAR/HEAD  Qcurve (Gated)
Name Node1 Node2 Offset FUNCTIONAL/DEPTH C1 C2 (Gated)
Name Node1 Node2 Offset FUNCTIONAL/HEAD C1 C2 (Gated)

Parameters:
Name    name assigned to outlet link.
Node1    name of the outlet’s inlet node.
Node2    name of the outlet’s outlet node.
Offset    amount that the outlet is offset above the invert of its inlet node (ft or m, expressed as either a depth or as an elevation, depending on the LINK_OFFSETS option setting).
Qcurve    name of the rating curve listed in the [CURVES] section that describes outflow rate (flow units) as a function of:
    water depth above the offset elevation at the inlet node (ft or m) for a TABULAR/DEPTH outlet
    head difference (ft or m) between the inlet and outflow nodes for a TABULAR/HEAD outlet.
C1, C2    coefficient and exponent, respectively, of a power function that relates outflow (Q) to:
    water depth (ft or m) above the offset elevation at the inlet node for a FUNCTIONAL/DEPTH outlet
    head difference  (ft or m) between the inlet and outflow nodes for a FUNCTIONAL/HEAD outlet.
(i.e.,  Q=C1H^C2 where H  is either depth or head).
Gated    YES if a flap gate prevents reverse flow, NO if not (default is NO).

### Section: [XSECTIONS] {#engine_manual_sect_XSECTIONS}

Purpose:
Provides cross-section geometric data for conduit and regulator links of the drainage system.

Formats:
Link    Shape            Geom1 Geom2 Geom3 Geom4 (Barrels Culvert)
Link    IRREGULAR    Tsect
Link    STREET          Street

Parameters:
Link    name of a conduit, orifice, or weir.
Shape    a cross-section shape (see Tables D-1 below or 3-1 for available shapes).
Geom1    full height of the cross-section (ft or m).
Geom2-4    auxiliary parameters (width, side slopes, etc.) as listed in Table D-1.
Barrels    number of barrels (i.e., number of parallel pipes of equal size, slope, and roughness) associated with a conduit (default is 1).
Culvert    code number from Table A.10 for the conduit’s inlet geometry if it is a culvert subject to possible inlet flow control (leave blank otherwise).
Curve    name of a Shape Curve in the [CURVES] section that defines how cross-section width varies with depth.
Tsect    name of an entry in the [TRANSECTS] section that describes the cross-section geometry of an irregular channel.
Street    name of an entry in the [STREETS] section that describes the cross-section geometry of a street.

Remarks:
The standard conduit shapes and their geometric parameters are listed in the following table:


Table D 2 Geometric parameters of conduit cross sections
Shape    Geom1    Geom2    Geom3    Geom4
CIRCULAR    Diameter            
FORCE_MAIN    Diameter    Roughness1        
FILLED_CIRCULAR2    Diameter    Sediment Depth        
RECT_CLOSED     Full Height    Top Width        
RECT_OPEN    Full Height    Top Width        
TRAPEZOIDAL    Full Height    Base Width    Left  Slope3    Right Slope3
TRIANGULAR    Full Height    Top Width        
HORIZ_ELLIPSE    Full Height    Max. Width    Size Code4    
VERT_ELLIPSE    Full Height    Max. Width    Size Code4    
ARCH     Full Height    Max. Width    Size Code5    
PARABOLIC    Full Height    Top Width        
POWER    Full Height    Top Width    Exponent    
RECT_TRIANGULAR     Full Height    Top Width    Triangle Height    
RECT_ROUND    Full Height    Top Width    Bottom Radius    
MODBASKETHANDLE    Full Height    Base Width    Top Radius6    
EGG    Full Height            
HORSESHOE    Full Height            
GOTHIC    Full Height            
CATENARY    Full Height            
SEMIELLIPTICAL    Full Height            
BASKETHANDLE    Full Height            
SEMICIRCULAR    Full Height            
CUSTOM    Full Height    Shape Curve        
1C-factors are used when H-W is the FORCE_MAIN_EQUATION choice in the [OPTIONS] section while roughness heights (in inches or mm) are used for D-W.
 2A circular conduit partially filled with sediment to a specified depth.
3Slopes are horizontal run / vertical rise.
4Size code of a standard shaped elliptical pipe as listed in Appendix A12. Leave blank (or 0) if the pipe has custom dimensions.
5Size code of a standard arch pipe as listed in Appendix A13. Leave blank (or 0) if the pipe has custom dimensions).
6Set to zero to use a standard modified baskethandle shape whose top radius is half the base width.
The CUSTOM shape is a closed conduit whose width versus height is described by a user-supplied Shape Curve.
An IRREGULAR cross-section is used to model an open channel whose geometry is described by a Transect object.
A STREET cross-section is used to model street conduits and inlet flow capture (see the [INLETS] and [INLETS_USAGE] sections).
The Culvert code number is used only for closed conduits acting as culverts that should be analyzed for inlet control conditions using the FHWA HDS-5 methodology.
 
### Section: [TRANSECTS] {#engine_manual_sect_TRANSECTS}

Purpose:
Describes the cross-section geometry of natural channels or conduits with irregular shapes following the HEC-2 data format.

Formats:
NC  Nleft Nright  Nchanl
X1  Name  Nsta Xleft Xright 0 0 0 Lfactor Wfactor Eoffset
GR  Elev  Station  ...  Elev  Station

Parameters:
Nleft    Manning’s roughness coefficient (n) of right overbank portion of channel (use 0 if no change from previous NC line).
Nright    Manning’s roughness coefficient (n) of right overbank portion of channel (use 0 if no change from previous NC line.
Nchanl    Manning’s roughness coefficient (n) of main channel portion of channel (use 0 if no change from previous NC line.
Name    name assigned to the transect.
Nsta    number of stations across the cross-section’s width at which elevation data is supplied.
Xleft    station position which ends the left overbank portion of the channel (ft or m).
Xright    station position which begins the right overbank portion of the channel (ft or m).
Lfactor    meander modifier that represents the ratio of the length of a meandering main channel to the length of the overbank area that surrounds it (use 0 if not applicable).
Wfactor    factor by which distances between stations should be multiplied to increase (or decrease) the width of the channel (enter 0 if not applicable).
Eoffset    amount to be added (or subtracted) from the elevation of each station (ft or m).
Elev    elevation of the channel bottom at a cross-section station relative to some fixed reference (ft or m).
Station    distance of a cross-section station from some fixed reference (ft or m).


Remarks:
Transect geometry is described as shown below, assuming that one is looking in a downstream direction:
 

The first line in this section must always be a NC line. After that, the NC line is only needed when a transect has different Manning’s n values than the previous one.
The Manning’s n values on the NC line will supersede any roughness value entered for the conduit which uses the irregular cross-section.
There should be one X1 line for each transect. Any number of GR lines may follow, and each GR line can have any number of Elevation-Station data pairs. (In HEC-2 the GR line is limited to 5 stations.)
The station that defines the left overbank boundary on the X1 line must correspond to one of the station entries on the GR lines that follow. The same holds true for the right overbank boundary. If there is no match, a warning will be issued and the program will assume that no overbank area exists.
The meander modifier is applied to all conduits that use this particular transect for their cross section. It assumes that the length supplied for these conduits is that of the longer main channel. SWMM will use the shorter overbank length in its calculations while increasing the main channel roughness to account for its longer length.
 
### Section: [STREETS] {#engine_manual_sect_STREETS}

Purpose:
Describes the cross-section geometry of conduits that represent streets.

Format:
Name Tcrown Hcurb Sx nRoad (a W)(Sides Tback Sback nBack)

Parameters:
Name     name assigned to the street cross-section
Tcrown     distance from street’s curb to its crown (ft or m)
Hcurb     curb height (ft or m)
Sx     street cross slope (%)
nRoad     Manning’s roughness coefficient (n) of the road surface
a     gutter depression height (in or mm) (default = 0)
W     depressed gutter width (ft or m) (default = 0)
Sides     1 for single sided street or 2 for two-sided street (default = 2)
Tback     street backing width (ft or m) (default = 0)
Sback     street backing slope (%) (default = 0)
nBack     street backing Manning’s roughness coefficient (n) (default = 0)

Remarks:
 

If the street has no depressed gutter (a = 0) then the gutter width entry is ignored. If the street has no backing then the three backing parameters can be omitted.
### Section: [INLETS] {#engine_manual_sect_INLETS}

Purpose:
Defines inlet structure designs used to capture street and channel flow that are sent to below ground sewers.

Format:
Name  GRATE/DROP_GRATE Length Width Type (Aopen Vsplash)
Name  CURB/DROP_CURB Length Height (Throat)
Name  SLOTTED Length Width
Name  CUSTOM Dcurve/Rcurve

Parameters:
Name     name assigned to the inlet structure.
Length     length of the inlet parallel to the street curb (ft or m).
Width     width of a GRATE or SLOTTED inlet (ft or m).
Height     height of a CURB opening inlet (ft or m).
Type     type of GRATE used (see below).
Aopen     fraction of a GENERIC grate’s area that is open.
Vsplash     splash over velocity for a GENERIC grate (ft/s or m/s).
Throat     the throat angle of a CURB opening inlet (HORIZONTAL, INCLINED or VERTICAL).
Dcurve     name of a Diversion-type curve (captured flow v. approach flow) for a CUSTOM inlet.
Rcurve     name of a Rating-type curve (captured flow v. water depth) for a CUSTOM inlet.

Remarks:
See @ref engine_manual_ch1_conceptual_model for a description of the different types of inlets that SWMM can model. 
Use one line for each inlet design except for a combination inlet where one GRATE line describes its grated inlet and a second CURB line (with the same inlet name) describes its curb opening inlet.

GRATE, CURB, and SLOTTED inlets are used with STREET conduits, DROP_GRATE and DROP_CURB inlets with open channels, and a CUSTOM inlet with any conduit.
GRATE and DROP_GRATE  types can be any of the following:
Grate Type    Sketch    Description
P_BAR-50         Parallel bar grate with bar spacing 1⅞” on center 
P_BAR-50X100         Parallel bar grate with bar spacing 1⅞” on center and ⅜” diameter lateral rods spaced at 4” on center
P_BAR-30         Parallel bar grate with 1⅛” on center bar spacing
CURVED_VANE         Curved vane grate with 3¼” longitudinal bar and 4¼” transverse bar spacing on center
TILT_BAR-45         45 degree tilt bar grate with 2¼” longitudinal bar and 4” transverse bar spacing on center
TILT_BAR-30         30 degree tilt bar grate with 3¼” and 4” on center longitudinal and lateral bar spacing respectively
RETICULINE         "Honeycomb" pattern of lateral bars and longitudinal bearing bars
GENERIC        A generic grate design.

 Only a GENERIC type grate requires that Aopen and Vsplash values be provided. The other standard grate types have predetermined values of these parameters. (Splash over velocity is the minimum velocity that will cause some water to shoot over the inlet thus reducing its capture efficiency).
 A CUSTOM inlet takes the name of either a Diversion curve or a Rating curve as its only parameter (see the [CURVES] section).  Diversion curves are best suited for on-grade inlets and Rating curves for on-sag inlets.


Examples:
; A 2-ft x 2-ft parallel bar grate
InletType1  GRATE  2  2  P-BAR-30
; A combination inlet
InletType2  GRATE  2  2  CURVED_VANE
InletType2  CURB   4  0.5  HORIZONTAL
; A custom inlet using Curve1 as its capture curve
InletType3  CUSTOM  Curve1
 
### Section: [INLET_USAGE] {#engine_manual_sect_INLET_USAGE}

Purpose:
Assigns inlet structures to specific street and open channel conduits.

Format:
Conduit Inlet Node (Number %Clogged Qmax aLocal wLocal Placement)

Parameters:
Conduit     name of a street or open channel conduit containing the inlet.
Inlet     name of an inlet structure (from the [INLETS] section) to use.
Node     name of the sewer node receiving flow captured by the inlet.
Number     number of replicate inlets placed on each side of the street.
%Clogged     degree to which inlet capacity is reduced due to clogging (%).
Qmax     maximum flow that the inlet can capture (flow units).
aLocal     height of local gutter depression (in or mm).
wLocal     width of local gutter depression (ft or m).
Placement     AUTOMATIC, ON_GRADE, or ON_SAG.

Remarks:
Only conduits with a STREET cross section can be assigned a curb and gutter inlet while drop inlets can only be assigned to conduits with a RECT_OPEN or TRAPEZOIDAL cross section.
Only the first three parameters are required. The default number of inlets is 1 (for each side of a two-sided street) while the remaining parameters have default values of 0.
A Qmax value of 0 indicates that the inlet has no flow restriction.
The local gutter depression applies only over the length of the inlet unlike the continuous depression for a STREET cross section which exists over the full curb length.
The default inlet placement is AUTOMATIC, meaning that the program uses the network topography to determine whether an inlet operates on-grade or on-sag. On-grade means the inlet is located on a continuous grade. On-sag means the inlet is located at a sag or sump point where all adjacent conduits slope towards the inlet leaving no place for water to flow except into the inlet.
### Section: [LOSSES] {#engine_manual_sect_LOSSES}

Purpose:
Specifies minor head loss coefficients, flap gates, and seepage rates for conduits.

Format:
Conduit  Kentry  Kexit  Kavg  (Flap  Seepage)

Parameters:
Conduit    name of a conduit.
Kentry    minor head loss coefficient at the conduit’s entrance.
Kexit    minor head loss coefficient at the conduit’s exit.
Kavg    average minor head loss coefficient across the length of the conduit.
Flap    YES if the conduit has a flap valve that prevents back flow, NO otherwise. (Default is NO).
Seepage    Rate of seepage loss into the surrounding soil (in/hr or mm/hr). (Default is 0.)

Remarks:
Minor losses are only computed for the Dynamic Wave flow routing option (see the [OPTIONS] section). They are computed as Kv2/2g where K = minor loss coefficient, v = velocity, and g = acceleration of gravity. Entrance losses are based on the velocity at the entrance of the conduit, exit losses on the exit velocity, and average losses on the average velocity.
Only enter data for conduits that actually have minor losses, flap valves, or seepage losses.



 
### Section: [CONTROLS] {#engine_manual_sect_CONTROLS}

Purpose:
Determines how pumps and regulators will be adjusted based on simulation time or conditions at specific nodes and links.

Formats:
Each control rule is a series of statements of the form:
RULE    ruleID
IF        condition_1
AND    condition_2
OR        condition_3
AND    condition_4
Etc.
THEN    action_1
AND    action_2
Etc.
ELSE    action_3
AND    action_4
Etc.
PRIORITY value

Parameters:
 ruleID            an ID label assigned to the rule.
condition_n    a condition clause.
action_n            an action clause.
value            a priority value (e.g., a number from 1 to 5).

Remarks:
Please refer to Section C.3 for a complete description of the control rule format plus examples of different types of rule statements.
 
### Section: [POLLUTANTS] {#engine_manual_sect_POLLUTANTS}

Purpose:
Identifies the pollutants being analyzed.

Format:
Name Units Crain Cgw Cii Kd (Sflag CoPoll CoFract Cdwf Cinit)

Parameters:
Name    name assigned to a pollutant.
Units    concentration units (MG/L for milligrams per liter, UG/L for micrograms per liter, or #/L for direct count per liter).
Crain    concentration of the pollutant in rainfall (concentration units).
Cgw    concentration of the pollutant in groundwater (concentration units).
Cii    concentration of the pollutant in rainfall-dependent infiltration and inflow (concentration units).
Kdecay    first-order decay coefficient (1/days).
Sflag    YES if pollutant buildup occurs only when there is snow cover, NO otherwise (default is NO).
CoPoll    name of a co-pollutant (default is no co-pollutant designated by a *).
CoFract    fraction of the co-pollutant’s concentration (default is 0).
Cdwf     pollutant concentration in dry weather flow (default is 0).
Cinit     pollutant concentration throughout the conveyance system at the start of the simulation (default is 0).
Remarks:
FLOW  is a reserved word and cannot be used to name a pollutant.
Parameters Sflag through Cinit can be omitted if they assume their default values. If there is no co-pollutant but non-default values for Cdwf or Cinit, then enter an asterisk (*) for the co-pollutant name.
When pollutant X has a co-pollutant Y, it means that fraction CoFract of pollutant Y’s runoff concentration is added to pollutant X’s runoff concentration when wash off from a subcatchment is computed.
The dry weather flow concentration can be overridden for any specific node of the conveyance system by editing the node’s Inflows property (see the [INFLOWS] section). 
### Section: [LANDUSES] {#engine_manual_sect_LANDUSES}

Purpose:
Identifies the various categories of land uses within the drainage area. Each subcatchment area can be assigned a different mix of land uses. Each land use can be subjected to a different street sweeping schedule. Land uses are only used in conjunction with pollutant buildup and wash off.

Format:
Name  (SweepInterval  Availability  LastSweep)

Parameters:
Name            land use name.
SweepInterval    days between street sweeping.
Availability    fraction of pollutant buildup available for removal by street sweeping.
LastSweep        days since last sweeping at the start of the simulation.
 
### Section: [COVERAGES] {#engine_manual_sect_COVERAGES}

Purpose:
Specifies the percentage of a subcatchment’s area that is covered by each category of land use.

Format:
    Subcat  Landuse  Percent  Landuse  Percent  . . .

Parameters:
Subcat    subcatchment name.
Landuse    land use name.
Percent    percent of the subcatchment’s area covered by the land use.

Remarks:
More than one pair of land use - percentage values can be entered per line. If more than one line is needed, then the subcatchment name must still be entered first on the succeeding lines.
If a land use does not pertain to a subcatchment, then it does not have to be entered.
If no land uses are associated with a subcatchment then no pollutants will appear in the runoff from the subcatchment.



 
### Section: [LOADINGS] {#engine_manual_sect_LOADINGS}

Purpose:
Specifies the pollutant buildup that exists on each subcatchment at the start of a simulation.

Format:
Subcat  Pollut  InitBuildup  Pollut  InitBuildup ...

Parameters:
Subcat            name of a subcatchment.
Pollut            name of a pollutant.
InitBuildup    initial buildup of the pollutant (lbs/acre or kg/hectare).

Remarks:
More than one pair of pollutant - buildup values can be entered per line. If more than one line is needed, then the subcatchment name must still be entered first on the succeeding lines.
If an initial buildup is not specified for a pollutant, then its initial buildup is computed by applying the DRY_DAYS option (specified in the [OPTIONS] section) to the pollutant’s buildup function for each land use in the subcatchment.


 
### Section: [BUILDUP] {#engine_manual_sect_BUILDUP}

Purpose:
Specifies the rate at which pollutants build up over different land uses between rain events.

Format:
Landuse  Pollutant  FuncType  C1  C2  C3  PerUnit

Parameters:
Landuse        land use name.
Pollutant    pollutant name.
FuncType        buildup function type: ( POW / EXP / SAT / EXT ).
C1,C2,C3        buildup function parameters (see Table D-2).
PerUnit        AREA if buildup is per unit area, CURBLENGTH if per length of curb.

Remarks:
Buildup is measured in pounds (kilograms) per unit of area (or curb length) for pollutants whose concentration units are either mg/L or ug/L. If the concentration units are counts/L, then buildup is expressed as counts per unit of area (or curb length).

Table D 3 Pollutant buildup functions
Name    Function    Equation*
POW    Power    Min (C1, C2*tC3)
EXP    Exponential    C1*(1 – exp(-C2*t))
SAT    Saturation    C1*t / (C3 + t)
EXT    External    See below
*t is antecedent dry days.

For the EXT buildup function, C1 is the maximum possible buildup (mass per area or curb length), C2 is a scaling factor, and C3 is the name of a Time Series that contains buildup rates (as mass per area or curb length per day) as a function of time.
 
### Section: [WASHOFF] {#engine_manual_sect_WASHOFF}

Purpose:
Specifies the rate at which pollutants are washed off from different land uses during rain events.

Format:
Landuse  Pollutant  FuncType  C1  C2  SweepRmvl BmpRmvl

Parameters:
Landuse        land use name.
Pollutant    pollutant name.
FuncType        washoff function type: EXP / RC / EMC.
C1, C2        washoff function coefficients(see Table D-3).
SweepRmvl    street sweeping removal efficiency (percent).        
BmpRmvl        BMP removal efficiency (percent).

Remarks:
Table D 4 Pollutant wash off functions
Name    Function    Equation    Units
EXP    Exponential    C1 (runoff)C2 (buildup)    Mass/hour
RC    Rating Curve    C1 (runoff)C2    Mass/sec
EMC    Event Mean
Concentration    C1    Mass/Liter

Each washoff function expresses its results in different units.
For the Exponential function the runoff variable is expressed in catchment depth per unit of time (inches per hour or millimeters per hour), while for the Rating Curve function it is in whatever flow units were specified in the [OPTIONS] section of the input file (e.g., CFS, CMS, etc.).

The buildup parameter in the Exponential function is the current total buildup over the subcatchment’s land use area in mass units. The units of C1 in the Exponential function are (in/hr) -C2 per hour (or (mm/hr) -C2 per hour). For the Rating Curve function, the units of C1 depend on the flow units employed. For the EMC (event mean concentration) function, C1 is always in concentration units.


 
### Section: [TREATMENT] {#engine_manual_sect_TREATMENT}

Purpose:
Specifies the degree of treatment received by pollutants at specific nodes of the drainage system.

Format:
Node  Pollut  Result = Func  

Parameters:
Node    Name of the node where treatment occurs.
Pollut    Name of pollutant receiving treatment.
Result    Result computed by treatment function. Choices are:
C  (function computes effluent concentration)
R  (function computes fractional removal).
Func    mathematical function expressing treatment result in terms of pollutant concentrations, pollutant removals, and other standard variables (see below).

Remarks:
Treatment functions can be any well-formed mathematical expression involving:
    inlet pollutant concentrations (use the pollutant name to represent a concentration)
    removal of other pollutants (use R_ pre-pended to the pollutant name to represent removal)
    process variables which include:
FLOW     for flow rate into node (user’s flow units)
DEPTH     for water depth above node invert (ft or m)
AREA     for node surface area (ft2 or m2)
DT         for routing time step (seconds)
HRT     for hydraulic residence time (hours)

Any of the following math functions can be used in a treatment function:
    abs(x) for absolute value of x
    sgn(x) which is +1 for x >= 0 or -1 otherwise
    step(x) which is 0 for x <= 0 and 1 otherwise
    sqrt(x) for the square root of x
    log(x) for logarithm base e of x
    log10(x) for logarithm base 10 of x
    exp(x) for e raised to the x power
    the standard trig functions (sin, cos, tan, and cot)
    the inverse trig functions (asin, acos, atan, and acot)
    the hyperbolic trig functions (sinh, cosh, tanh, and coth)
along with the standard operators +, -, *, /, ^ (for exponentiation ) and any level of nested parentheses.

Examples:
; 1-st order decay of BOD
Node23  BOD   C = BOD * exp(-0.05*HRT)

; lead removal is 20% of TSS removal
Node23  Lead  R = 0.2 * R_TSS     




 
### Section: [INFLOWS] {#engine_manual_sect_INFLOWS}

Purpose:
Specifies external hydrographs and pollutographs that enter the drainage system at specific nodes.

Formats:
Node FLOW   Tseries  (FLOW (1.0     Sfactor Base Pat)) 
Node Pollut Tseries  (Type (Mfactor Sfactor Base Pat))                     

Parameters:
Node    name of the node where external inflow enters.
Pollut    name of a pollutant.
Tseries    name of a time series in the [TIMESERIES] section describing how external flow or pollutant loading varies with time.
Type    CONCEN if pollutant inflow is described as a concentration, MASS if it is described as a mass flow rate (default is CONCEN). 
Mfactor    the factor that converts the inflow’s mass flow rate units into the project’s mass units per second, where the project’s mass units are those specified for the pollutant in the [POLLUTANTS] section (default is 1.0 - see example below).
Sfactor    a scaling factor that multiplies the recorded time series values (default is 1.0).
Base    a constant baseline value added to the time series value (default is 0.0).
Pat    name of an optional time pattern in the [PATTERNS] section used to adjust the baseline value on a periodic basis.

Remarks:
External inflows are represented by both a constant and time varying component as follows:
Inflow = (Baseline value)*(Pattern factor) +
(Scaling factor)*(Time series value)
If an external inflow of a pollutant concentration is specified for a node, then there must also be an external inflow of FLOW provided for the same node, unless the node is an Outfall. In that case a pollutant can enter the system during periods when the outfall is submerged and reverse flow occurs. External pollutant mass inflows do not require a FLOW inflow.

Examples:
; NODE2 receives flow inflow from time series N2FLOW
; and TSS concentration from time series N2TSS
NODE2   FLOW  N2FLOW
NODE2   TSS   N33TSS  CONCEN

; NODE65 has a mass inflow of BOD from time series N65BOD
; listed in lbs/hr (126 converts lbs/hr to mg/sec)
NODE65  BOD  N65BOD  MASS  126

; Flow inflow to Node N176 consists of the flow time series
; FLOW_176 scaled at 0.5 plus a baseline flow of 12.7
; adjusted by pattern FlowPat
N176  FLOW  FLOW_176  FLOW  1.0  0.5  12.7  FlowPat 
### Section: [DWF] {#engine_manual_sect_DWF}

Purpose:
Specifies dry weather flow and its quality entering the drainage system at specific nodes.

Format:
Node  Type  Base  (Pat1  Pat2  Pat3  Pat4)

Parameters:
Node    name of a node where dry weather flow enters.
Type    keyword FLOW for flow or a pollutant name for a quality constituent.
Base    average baseline value for corresponding constituent  (flow or concentration units).
Pat1,    
Pat2,
etc.    names of up to four time patterns appearing in the [PATTERNS] section.

Remarks:
The actual dry weather input will equal the product of the baseline value and any adjustment factors supplied by the specified patterns. (If not supplied, an adjustment factor defaults to 1.0.)
The patterns can be any combination of monthly, daily, hourly and weekend hourly patterns, listed in any order. See the [PATTERNS] section for more details.


 
### Section: [RDII] {#engine_manual_sect_RDII}

Purpose:
Specifies the parameters that describe rainfall-dependent infiltration and inflow (RDII) entering the drainage system at specific nodes.

Format:
    Node  UHgroup  SewerArea

Parameters:
Node    name of a node receiving RDII flow.
UHgroup    name of an RDII unit hydrograph group appearing in the [HYDROGRAPHS] section.
SewerArea    area of the sewershed that contributes RDII to the node (acres or hectares).


 
### Section: [HYDROGRAPHS] {#engine_manual_sect_HYDROGRAPHS}

Purpose:
Specifies the shapes of the triangular unit hydrographs that determine the amount of rainfall-dependent infiltration and inflow (RDII) entering the drainage system.

Format:
Name  Raingage
Name  Month  SHORT/MEDIUM/LONG  R  T  K (Dmax Drec D0)

Remarks:
Name    name assigned to a unit hydrograph group.
Raingage    name of the rain gage used by the unit hydrograph group.
Month    month of the year (e.g., JAN, FEB, etc. or ALL for all months).
R    response ratio for the unit hydrograph.
T    time to peak (hours) for the unit hydrograph.
K    recession limb ratio for the unit hydrograph.
Dmax    maximum initial abstraction depth available (in rain depth units).
Drec    initial abstraction recovery rate (in rain depth units per day)
D0    initial abstraction depth already filled at the start of the simulation (in rain depth units).

Remarks:
For each group of unit hydrographs, use one line to specify its rain gage followed by as many lines as are needed to define each unit hydrograph used by the group throughout the year. Three separate unit hydrographs, that represent the short-term, medium-term, and long-term RDII responses, can be defined for each month (or all months taken together). Months not listed are assumed to have no RDII.
The response ratio (R) is the fraction of a unit of rainfall depth that becomes RDII. The sum of the ratios for a set of three hydrographs does not have to equal 1.0.
The recession limb ratio (K) is the ratio of the duration of the hydrograph’s recession limb to the time to peak (T) making the hydrograph time base equal to T*(1+K) hours. The area under each unit hydrograph is 1 inch (or mm).

The optional initial abstraction parameters determine how much rainfall is lost at the start of a storm to interception and depression storage. If not supplied then the default is no initial abstraction.

Lines are processed in the order they appear, and an entry for ALL assigns its values to every month of the year. An ALL entry therefore overrides any month-specific values entered on earlier lines (e.g., if entries appear in the order JAN through SEP, ALL, OCT, NOV, DEC, the ALL entry replaces the values entered for JAN through SEP, leaving only OCT, NOV, and DEC with their own values). Whenever parameters for the same month and response type are supplied more than once, the values entered last are used and WARNING 13 is written to the status report.

Example:
;  All three unit hydrographs in this group have the same shapes except those in July,
;  which have only a short- and medium-term response and a different shape.
UH101  RG1
UH101  ALL SHORT  0.033 1.0  2.0
UH101  ALL MEDIUM 0.300 3.0  2.0
UH101  ALL LONG   0.033 10.0 2.0
UH101  JUL SHORT  0.033 0.5  2.0
UH101  JUL MEDIUM 0.011 2.0  2.0
 
### Section: [CURVES] {#engine_manual_sect_CURVES}

Purpose:
Describes a relationship between two variables in tabular format. 

Format:
Name  Type
Name  X-value  Y-value  ...

Parameters:
Name    name assigned to the curve.
Type    the type of curve being defined:
STORAGE / SHAPE / DIVERSION / TIDAL / PUMP1 / PUMP2 / PUMP3 / PUMP4 / PUMP5 / RATING / CONTROL / WEIR.
X-value    an X (independent variable) value.
Y-value    the Y (dependent variable) value corresponding to X.

Remarks:
Each curve should have its name and type on the first line with its data points entered on subsequent lines.
Multiple pairs of x-y values can appear on a line. If more than one line is needed, repeat the curve's name on subsequent lines.
X-values must be entered in increasing order.
Choices for curve type have the following meanings (flows are expressed in the user’s choice of flow units set in the [OPTIONS] section):
STORAGE    surface area in ft2 (m2) versus depth in ft (m) for a storage unit node
SHAPE    width versus depth for a custom closed cross-section, both normalized with respect to full depth
DIVERSION    diverted outflow versus total inflow for a flow divider node or a Custom inlet
TIDAL    water surface elevation in ft (m) versus hour of the day for an outfall node
PUMP1    pump outflow versus increment of inlet node volume in ft3 (m3)
PUMP2    pump outflow versus increment of inlet node depth in ft (m)
PUMP3    pump outflow versus head difference between outlet and inlet nodes in ft (m) that has decreasing flow with increasing head
PUMP4    pump outflow versus continuous inlet node depth in ft (m)
PUMP5    pump outflow versus head difference between outlet and inlet nodes in ft (m) that has decreasing flow with increasing head
RATING    flow versus head in ft (m) for an Outlet link or a Custom inlet
CONTROL    control setting for a pump or flow regulator versus a controller variable (such as a node water level) in a modulated control; flow adjustment setting versus head for an LID unit’s underdrain
WEIR    discharge coefficient for flow in CFS (CMS) versus head in ft (m)

Remarks:
See @ref engine_manual_ch1_conceptual_model for illustrations of the different types of pump curves.

Examples:
; Storage curve (x = depth, y = surface area)
AC1  STORAGE
AC1  0  1000  2  2000  4  3500  6  4200  8  5000

; Type 1 pump curve (x = inlet wet well volume, y = flow)
PC1  PUMP1
PC1  100  5  300  10  500  20

; Type 5 pump curve (x = pump head, y = pump flow)
PC2  PUMP5
PC2  0  4
PC2  4  2
PC2  6  0


 
### Section: [TIMESERIES] {#engine_manual_sect_TIMESERIES}

Purpose:
Describes how a quantity varies over time.

Formats:
Name  ( Date )  Hour  Value  ...
Name  Time  Value  ...
Name  FILE  Fname

Parameters:
Name    name assigned to the time series.
Date    date in Month/Day/Year format (e.g., June 15, 2001 would be 6/15/2001).
Hour    24-hour military time (e.g., 8:40 pm would be 20:40) relative to the last date specified (or to midnight of the starting date of the simulation if no previous date was specified).
Time    hours since the start of the simulation, expressed as a decimal number or as hours:minutes (where hours can be greater than 24).
Value    a value corresponding to the specified date and time.
Fname    the name of a file in which the time series data are stored

Remarks:
There are two options for supplying the data for a time series:
    directly within this input file section as described by the first two formats
    through an external data file named with the third format.
When direct data entry is used, multiple date-time-value or time-value entries can appear on a line. If more than one line is needed, the table's name must be repeated as the first entry on subsequent lines.
When an external file is used, each line in the file must use the same formats listed above, except that only one date-time-value (or time-value) entry is allowed per line. Any line that begins with a semicolon is considered a comment line and is ignored. Blank lines are also permitted. Enclose the external file name in double quotes if it contains spaces and include its full path if it resides in a different directory than the SWMM input file. 

**New in OpenSWMM v6:** The external file may also be a multi-column series file — a comma- or tab-delimited file with a header row whose first column holds a full date-time, or a PCSWMM `.tsf` file (tab-delimited with an `IDs:` header and 12-hour AM/PM date-times). Select a column by appending a colon and the column's header name to the file path, e.g. `TS_EAST FILE "rain_2024.csv:EAST_GAGE"`. Without a column name the first data column is used. A single multi-column file can supply any number of time series and rain gages and is read from disk only once per model open. The format (CSV/TSV/TSF) is detected automatically from the file's contents.

There are two options for describing the occurrence time of time series data:  
    as calendar date plus time of day (which requires that at least one date, at the start of the series, be entered)  
    as elapsed hours since the start of the simulation.
For the first method, dates need only be entered at points in time when a new day occurs.
For rainfall time series, it is only necessary to enter periods with non-zero rainfall amounts. SWMM interprets the rainfall value as a constant value lasting over the recording interval specified for the rain gage which utilizes the time series. For all other types of time series, SWMM uses interpolation to estimate values at times that fall in between the recorded values.

Examples:
; Hourly rainfall time series with dates specified using
; one data point per line to emphasize when dates change
TS1 6-15-2001 7:00  0.1
TS1           8:00  0.2
TS1           9:00  0.05
TS1           10:00 0
TS1 6-21-2001 4:00  0.2
TS2           5:00  0
TS2           14:00 0.1
TS2           15:00 0

;Inflow hydrograph - time relative to start of simulation
HY1  0  0  1.25 100  2:30 150  3.0 120  4.5 0
HY1  32:10 0  34.0 57  35.33 85  48.67 24  50 0



      
 
### Section: [PATTERNS] {#engine_manual_sect_PATTERNS}

Purpose:
Specifies time patterns of dry weather flow or quality in the form of adjustment factors applied as multipliers to baseline values.

Format:
Name  MONTHLY        Factor1  Factor2  ...  Factor12
Name  DAILY        Factor1  Factor2  ...  Factor7
Name  HOURLY        Factor1  Factor2  ...  Factor24
Name  WEEKEND        Factor1  Factor2  ...  Factor24

Parameters:
Name    name used to identify the pattern.
Factor1,
Factor2,
etc.    multiplier values.

Remarks:
The MONTHLY format is used to set monthly pattern factors for dry weather flow constituents.
The DAILY format is used to set dry weather pattern factors for each day of the week, where Sunday is day 1.
The HOURLY format is used to set dry weather factors for each hour of the day starting from midnight. If these factors are different for weekend days than for weekday days then the WEEKEND format can be used to specify hourly adjustment factors just for weekends.
More than one line can be used to enter a pattern’s factors by repeating the pattern’s name (but not the pattern type) at the beginning of each additional line.
The pattern factors are applied as multipliers to any baseline dry weather flows or quality concentrations supplied in the [DWF] section. 

Examples:
; Day of week adjustment factors
D1  DAILY  0.5  1.0  1.0  1.0  1.0  1.0  0.5
D2  DAILY  0.8  0.9  1.0  1.1  1.0  0.9  0.8
; Hourly adjustment factors 
H1 HOURLY  0.5 0.6 0.7 0.8 0.8 0.9
H1         1.1 1.2 1.3 1.5 1.1 1.0
H1         0.9 0.8 0.7 0.6 0.5 0.5
H1         0.5 0.5 0.5 0.5 0.5 0.5  

 
### Section: [VIRTUAL_JUNCTIONS] {#engine_manual_sect_VIRTUAL_JUNCTIONS}

Purpose:
Declares junction nodes that exist only to join two conduits, with no physical structure of their own.

Format:
    Name

Parameters:
Name    name of a junction declared in the [JUNCTIONS] section.

Remarks:
One token per line. Invert elevation and maximum depth are not read here — they are derived from the two attached conduits after parsing, so a virtual junction never imposes a node geometry of its own on the solution. Supplying any extra token is a parse error (ERROR 609).

This section has no counterpart in SWMM 5.

 
### Section: [INLET_JUNCTIONS] {#engine_manual_sect_INLET_JUNCTIONS}

Purpose:
Declares a virtual junction that also carries a street inlet, combining the [VIRTUAL_JUNCTIONS] and [INLET_USAGE] roles in one row.

Format:
    Name  Elev  MaxDepth  Inlet  CaptureNode  (Number  %Clogged  Qmax  aLocal  wLocal  Placement)

Parameters:
Name    name of the node.
Elev    invert elevation (ft or m).
MaxDepth    maximum depth, i.e. the street's flood depth (ft or m).
Inlet    name of an inlet design in the [INLETS] section.
CaptureNode    name of the node that receives the captured flow.
Number    number of inlets placed (default 1).
%Clogged    degree of clogging as a percent (default 0).
Qmax    maximum flow the inlet can capture (flow units; 0 or blank = no limit).
aLocal    height of a local gutter depression (in or mm).
wLocal    width of a local gutter depression (ft or m).
Placement    ON_GRADE, ON_SAG or AUTOMATIC (default AUTOMATIC).

Remarks:
The node is marked both virtual and inlet-bearing. More than eleven tokens is a parse error (ERROR 623).

This section has no counterpart in SWMM 5.

 
### Section: [INITIAL_QUALITY] {#engine_manual_sect_INITIAL_QUALITY}

Purpose:
Sets the initial concentration of a constituent in a node or a link at the start of the simulation.

Format:
    NODE  Name  Constituent  Value
    LINK  Name  Constituent  Value
    FILE  Fname

Parameters:
Name    name of the node or link.
Constituent    name of a pollutant, or a constituent of a process component.
Value    initial concentration (concentration units).
Fname    name of a CSV file holding the same four fields per line.

Remarks:
The scope keyword must be NODE or LINK; anything else is a parse error. A row with fewer than four tokens is a parse error.

The FILE form names a sidecar whose lines are `scope,element,constituent,value`, with an optional header row. It is resolved relative to the input file's directory and read when the model is opened, so it may be written by a pre-processor that does not have to rewrite the `.inp`.

 
### Section: [RDII_DECAY] {#engine_manual_sect_RDII_DECAY}

Purpose:
Applies a seasonal decay to an RDII unit hydrograph group, so that the same hydrograph produces less inflow as antecedent conditions dry out.

Format:
    UHgroup  Response  Kdep  K0  KT  Tref  ThetaRec  Tfreeze

Parameters:
UHgroup    name of a unit hydrograph group in the [HYDROGRAPHS] section.
Response    SHORT, MEDIUM or LONG — which of the group's three response terms this row applies to.
Kdep    depletion rate coefficient (1/day).
K0    base recovery rate coefficient (1/day).
KT    temperature coefficient of the recovery rate (1/day per degree).
Tref    reference temperature for KT (deg F or deg C).
ThetaRec    recovery threshold as a fraction.
Tfreeze    temperature below which recovery stops (deg F or deg C).

Remarks:
A row with fewer than eight tokens is ignored, as is a row whose Response keyword is not one of the three accepted values or whose Kdep, K0 or KT is negative.

This section has no counterpart in SWMM 5.

 
### Section: [EVENTS] {#engine_manual_sect_EVENTS}

Purpose:
Restricts reporting to one or more time windows, so that a long continuous run writes output only for the periods of interest.

Format:
    StartDate  StartTime  EndDate  EndTime

Parameters:
StartDate    date on which the event begins (MM/DD/YYYY).
StartTime    time of day at which it begins (HH:MM:SS).
EndDate    date on which it ends.
EndTime    time of day at which it ends.

Remarks:
A row with fewer than four tokens is ignored, as is any row whose start is not earlier than its end. Events do not change the simulation period in the [OPTIONS] section; the model still runs continuously.

 
### Section: [TAGS] {#engine_manual_sect_TAGS}

Purpose:
Attaches a free-text category label to an object, for grouping and filtering.

Format:
    ObjectType  Name  Tag

Parameters:
ObjectType    NODE, LINK or SUBCATCH.
Name    name of the object.
Tag    the label. Use double quotes if it contains spaces.

Remarks:
A row with fewer than three tokens is ignored, as is a row naming an object that does not exist. Tags are stored against the object's position rather than its name, so renaming the object keeps its tag.

 
### Section: [PROFILE] {#engine_manual_sect_PROFILE}

Purpose:
Names a sequence of links that form a profile path, for a graphical client to plot.

Format:
    Name  Link1  Link2  ...

Remarks:
The engine parses and discards this section; it exists so that a profile defined in a client survives a round trip through the engine's writer. It has no effect on the simulation.

 
### Section: [USER_FLAGS] {#engine_manual_sect_USER_FLAGS}

Purpose:
Declares a user-defined attribute that can then be given per-object values in the [USER_FLAG_VALUES] section.

Format:
    Name  Type  (Description)

Parameters:
Name    name of the flag. Stored upper-case, so flag names are case-insensitive.
Type    BOOLEAN, INTEGER, REAL or STRING.
Description    optional free text; quote it if it contains spaces.

Remarks:
A row with fewer than two tokens is ignored. An unrecognised Type is treated as STRING and raises WARNING 102.

This section has no counterpart in SWMM 5.

 
### Section: [USER_FLAG_VALUES] {#engine_manual_sect_USER_FLAG_VALUES}

Purpose:
Assigns a value for a user-defined flag to one object.

Format:
    ObjectType  Name  FlagName  Value

Parameters:
ObjectType    NODE, LINK or SUBCATCH.
Name    name of the object; case is preserved.
FlagName    name of a flag declared in [USER_FLAGS]; matched case-insensitively.
Value    the value, parsed according to the flag's declared type.

Remarks:
A row with fewer than four tokens is ignored. Assigning a value for a flag that was never declared is accepted, treated as STRING, and raises WARNING 103 — so ordering the two sections the wrong way round degrades the type rather than failing the run.

This section has no counterpart in SWMM 5.

 
### Section: [PLUGINS] {#engine_manual_sect_PLUGINS}

Purpose:
Loads a plugin shared library at start-up.

Format:
    Path  (Arg1  Arg2  ...)

Parameters:
Path    path to the plugin library. Relative paths resolve against the plugin search path.
Arg     optional initialisation arguments passed to the plugin verbatim.

Remarks:
See @ref engine_manual_ch5_api for what a plugin can do and where libraries are discovered.

This section has no counterpart in SWMM 5.

 
### Section: [PROCESS_COMPONENTS] {#engine_manual_sect_PROCESS_COMPONENTS}

Purpose:
Attaches a process component — an advection-reaction-dispersion solver, a reaction system, water age or heat transport — to the simulation.

Format:
    Id  key="value"  key="value"  ...

Parameters:
Id    identifier of the component.
key="value"    configuration arguments. The reserved key CONFIG names a configuration file for the component; every other key is passed through to it.

Remarks:
An argument that is not a `key="value"` pair is a parse error naming the component and the offending token.

This section has no counterpart in SWMM 5.

## 2.3 Map Data Section

SWMM’s graphical user interface (GUI) can display a schematic map of the drainage area being analyzed. This map displays subcatchments as polygons, nodes as circles, links as polylines, and rain gages as bitmap symbols. In addition it can display text labels and a backdrop image, such as a street map. The GUI has tools for drawing, editing, moving, and displaying these map elements.

 The map’s coordinate data are stored in the format described below. Normally these data are simply appended to the SWMM input file by the GUI so users do not have to concern themselves with it. However it is sometimes more convenient to import map data from some other source, such as a CAD or GIS file, rather than drawing a map from scratch using the GUI. In this case the data can be added to the SWMM project file using any text editor or spreadsheet program. SWMM does not provide any automated facility for converting coordinate data from other file formats into the SWMM map data format. 

SWMM's map data are organized into the following seven sections:
[MAP]    X,Y coordinates of the map’s bounding rectangle
[POLYGONS]    X,Y coordinates for each vertex of subcatchment polygons
[COORDINATES]    X,Y coordinates for nodes
[VERTICES]    X,Y coordinates for each interior vertex of polyline links
[LABELS]    X,Y coordinates and text of labels
[SYMBOLS]    X,Y coordinates for rain gages
[BACKDROP]    X,Y coordinates of the bounding rectangle and file name of the backdrop image.
Figure D-2 displays a sample map and Figure D-3 the data that describes it. Note that only one link, 3, has interior vertices which give it a curved shape. Also observe that this map’s coordinate system has no units, so that the positions of its objects may not necessarily coincide to their real-world locations. 

 
*Figure D-2 Example study area map*

*Figure D-3 Data for example study area map*
A detailed description of each map data section will now be given. Remember that map data are only used as a visualization aid for SWMM’s GUI and they play no role in any of the runoff or routing computations. Map data are not needed for running the command line version of SWMM.
---

### Section: [MAP] {#engine_manual_sect_MAP}

Purpose:
    Provides dimensions and distance units for the map.

Formats:
DIMENSIONS    X1 Y1 X2 Y2
UNITS           FEET / METERS / DEGREES / NONE

Parameters:
X1    lower-left X coordinate of full map extent
Y1    lower-left  Y coordinate of full map extent
X2    upper-right X coordinate of full map extent
Y2    upper-right Y coordinate of full map extent
---

### Section: [COORDINATES] {#engine_manual_sect_COORDINATES}

Purpose:
    Assigns X,Y coordinates to drainage system nodes.

Format:
    Node  Xcoord  Ycoord

Parameters:
Node    name of node.
Xcoord    horizontal coordinate relative to origin in lower left of map.
Ycoord    vertical coordinate relative to origin in lower left of map.
---





### Section: [VERTICES] {#engine_manual_sect_VERTICES}

Purpose:
    Assigns X,Y coordinates to interior vertex points of curved drainage system links.

Format:
    Link  Xcoord  Ycoord

Parameters:
Link    name of link.
Xcoord    horizontal coordinate of vertex relative to origin in lower left of map.
Ycoord    vertical coordinate of vertex relative to origin in lower left of map.

Remarks:
Include a separate line for each interior vertex of the link, ordered from the inlet node to the outlet node.

Straight-line links have no interior vertices and therefore are not listed in this section.
---

### Section: [POLYGONS] {#engine_manual_sect_POLYGONS}

Purpose:
Assigns X,Y coordinates to  vertex points of polygons that define a subcatchment boundary.

Format:
    Subcat  Xcoord  Ycoord

Parameters:
Subcat    name of subcatchment.
Xcoord    horizontal coordinate of vertex relative to origin in lower left of map.
Ycoord    vertical coordinate of vertex relative to origin in lower left of map.

Remarks:
Include a separate line for each vertex of the subcatchment polygon, ordered in a consistent clockwise or counter-clockwise sequence.
---



### Section: [SYMBOLS] {#engine_manual_sect_SYMBOLS}

Purpose:
    Assigns X,Y coordinates to rain gage symbols.

Format:
    Gage  Xcoord  Ycoord

Remarks:
Gage    name of rain gage.
Xcoord    horizontal coordinate relative to origin in lower left of map.
Ycoord    vertical coordinate relative to origin in lower left of map.
---

### Section: [LABELS] {#engine_manual_sect_LABELS}

Purpose:
    Assigns X,Y coordinates to user-defined map labels.

Format:
    Xcoord  Ycoord  Label (Anchor  Font  Size  Bold  Italic)

Parameters:
Xcoord    horizontal coordinate relative to origin in lower left of map.
Ycoord    vertical coordinate relative to origin in lower left of map.
Label    text of label surrounded by double quotes.
Anchor    name of node or subcatchment that anchors the label on zoom-ins (use an empty pair of double quotes if there is no anchor).
Font    name of label’s font (surround by double quotes if the font name includes spaces).
Size    font size in points.
Bold    YES for bold font, NO otherwise.
Italic    YES for italic font, NO otherwise.

Remarks:
Use of the anchor node feature will prevent the label from moving outside the viewing area when the map is zoomed in on. 

If no font information is provided then a default font is used to draw the label.
---
### Section: [BACKDROP] {#engine_manual_sect_BACKDROP}

Purpose:
    Specifies file name and coordinates of map’s backdrop image.

Formats:
FILE           Fname
DIMENSIONS    X1 Y1 X2 Y2

Parameters:
Fname    name of file containing backdrop image
X1    lower-left X coordinate of backdrop image
Y1    lower-left  Y coordinate of backdrop image
X2    upper-right X coordinate of backdrop image
Y2    upper-right Y coordinate of backdrop image

 
## 2.4 Two-Dimensional Surface and Groundwater Sections

The twenty-one `[2D_*]` sections configure the two-dimensional overland
surface of @ref hydraulics_ref_ch9_two_dimensional — its mesh, coupling to the
network, boundary conditions, per-cell infiltration and evaporation, the
aquifer beneath it, and surface quality — and the six `[GW_*]` sections carry
the groundwater-transport grammar, which this release parses, validates and
writes back but does not solve. Every row grammar below follows the parser;
where the design notes in `plans/2D_INPUT_FORMAT_SPEC.md` and the parser
disagree, the parser wins and the entry says so.

### Section: [2D_OPTIONS] {#engine_manual_sect_2D_OPTIONS}

Purpose:
Sets the options of the two-dimensional overland-flow module: the momentum closure and marcher controls, the wetting and drying thresholds, the coupling to the one-dimensional network, how rainfall, evaporation and infiltration reach the mesh, the groundwater and transport process enables, and the 2D results file.

Format:
```
Keyword  Value
```

One keyword per line. Keywords are matched case-insensitively; an unknown keyword is a parse error (`Unknown 2D_OPTIONS parameter`). The value of `REPORT_2D_VARIABLES` and `REPORT_2D_SPECIES` may span several tokens; every other keyword reads its first value token only.

Parameters:

Solver

| Keyword | Values | Default | Description |
|---|---|---|---|
| MOMENTUM_EQUATION | LOCAL_INERTIAL (LI) / FULL_SWE (SWE, FULL) / DIFFUSIVE_WAVE (DW, DIFFUSIVE) | LOCAL_INERTIAL | Face law of the explicit marcher. LOCAL_INERTIAL is the de Almeida and Bates update; FULL_SWE is the conservative shallow-water form with the convective term (hydrostatic reconstruction, rotated HLLC flux, shock capturing); DIFFUSIVE_WAVE is the Manning quasi-steady flux with no inertia. |
| INTEGRATOR | EXPLICIT | EXPLICIT | The explicit marcher is the only 2D integrator. Any other value is a retired selection (see the Retired keys table). |
| RECONSTRUCTION_ORDER | 1 / 2 | 1 | FULL_SWE only. 1 is piecewise constant (first-order Godunov, forward Euler); 2 is MUSCL on (eta, u, v) with the Barth-Jespersen-limited Green-Gauss gradient and SSP-RK2, run in global-time-step mode (LTS_TIERS is reduced to 1 with a warning). |
| THETA | (0, 1] | 0.8 | Face-update weighting of the local-inertial law: 1 is the pure Bates 2010 update, below 1 blends the reconstructed neighbour discharge to damp thin-film checkerboarding. |
| FROUDE_MAX | > 0 | 1.5 | Clamp on the face Froude number of the local-inertial law. |
| CELL_CLOSURE | FLAT / VFR | FLAT | Volume-to-free-surface closure of a cell. FLAT is the flat-cell relation; VFR is the planar-bed volume/free-surface relationship, which restores the lake-at-rest property at shorelines at the cost of more substeps. |
| FACE_RECONSTRUCTION | MEAN / VFR_FACE | MEAN | Conveyance depth at a shared edge. MEAN uses the upwind cell-mean depth; VFR_FACE reconstructs the wetted depth over the edge's own endpoint elevations, so a crest resolved as a line of high vertices blocks flow until the water reaches it. |
| VFR_MIN_WET_FRAC | (0, 0.5] | 0.01 | Wetted-area-fraction floor of the regularised VFR closure. Used only under CELL_CLOSURE VFR. |
| FLUX_DH_EPS | number, metres | 0.004 | Head-difference floor below which the diffusive flux is linearised. 0 restores the bare square root. Always metres. |
| LIMITER_EPSILON | number | 1.0e-6 | Regularisation of the slope limiter applied to the reported gradient fields. |
| ADVECTION | YES / ON / TRUE / NO / OFF / FALSE | NO | Deprecated spelling of MOMENTUM_EQUATION FULL_SWE (2026-09-06). Still honoured so an old deck keeps its physics; a file load warns and names the replacement. |
| BACKEND | AUTO / CPU / OMP / CUDA / HIP / SYCL | AUTO | Which marcher implementation runs the mesh. CPU is the built-in host marcher; OMP, CUDA, HIP and SYCL name a Kokkos plugin and bypass the mesh-size floors (an absent plugin or device falls back to CPU with a notice). AUTO picks a device plugin above the device floor, the OpenMP plugin above its own floor, else CPU. The OPENSWMM_2D_BACKEND environment variable overrides the key. |

Time control

| Keyword | Values | Default | Description |
|---|---|---|---|
| MAX_TIMESTEP | number, seconds | 10.0 | Cap on the marcher step. Also caps the spread of the local-time-stepping tiers and the co-advance batch span. |
| CFL_NUMBER | (0, 1] | 0.7 | Courant fraction of the marcher step. 1.0 is the linear stability limit; the default keeps a 30 percent margin. |
| LTS_TIERS | 1 .. 8 | 4 | Number of local-time-stepping tiers. 1 forces one global step for every cell. |

Wetting and drying

| Keyword | Values | Default | Description |
|---|---|---|---|
| DRY_DEPTH | number, metres | 0.001 | Depth below which a cell is dry. Also drives the coupling and outfall wet ramps and the evaporation taper. Always metres. |
| H_MOVE | >= 0, metres | 0.003 | Flux-activation depth: a cell below it is source-only (rain accumulates, no face flux). The on/off hysteresis band is the smaller of 1 mm and half of H_MOVE. Always metres. |
| FRONT_REBUILD | AUTO / YES / ON / NO / OFF | AUTO | Rebuild the flux-active set as soon as a wetting front reaches the edge of the active halo instead of waiting for the fixed rebuild cadence. AUTO is YES for FULL_SWE and DIFFUSIVE_WAVE and NO for LOCAL_INERTIAL. |

Coupling to the 1D network

| Keyword | Values | Default | Description |
|---|---|---|---|
| COUPLING_CD | number | 0.65 | Discharge coefficient of every coupling row that does not author its own CD. |
| COUPLING_AREA | AUTO / DEFAULT | DEFAULT | AUTO derives the exchange area of each coupling row that did not author an AREA from the largest connected conduit (1.25 times its full-flow area, clamped to 0.05 .. 2.0 square metres). DEFAULT keeps the authored or default area. |
| COUPLING_SYNC | >= 0, seconds | 0 | 0 co-advances the 2D domain every routing step. A positive value batches the 2D advance over roughly that span (clamped at run time to the routing step .. 60 s); the held-exchange error grows with the span. |
| COUPLING_IN_FLOODING | YES / TRUE / NO / FALSE | NO | YES books the 1D-to-2D spill into the 1D report's Flooding Loss row instead of its own 2D Coupling Outflow row. Changes the report only; routed volumes are identical. |

Rainfall, evaporation and infiltration

| Keyword | Values | Default | Description |
|---|---|---|---|
| RAINFALL_MODE | NATURAL_NEIGHBOUR (NATURAL_NEIGHBOR) / SYSTEM / NONE | NATURAL_NEIGHBOUR | How rain gage rainfall is mapped onto the cells: natural-neighbour interpolation of the located gages (inverse-distance outside their hull), the uniform mean of all gages, or no rain on the mesh. Use NONE when subcatchments already capture the storm. The OPENSWMM_2D_RAINFALL_MODE environment variable overrides the key. |
| EVAPORATION | NO / OFF / YES / ON / FORCING / CLIMATE | YES | NO zeroes the mesh evaporation sink. YES applies only per-cell forcing supplied through the API. CLIMATE evaporates every unforced cell at the project [EVAPORATION] rate; forcing still overrides it. |
| INFILTRATION | YES / ON / NO / OFF / AUTO | AUTO | Process enable for mesh infiltration. AUTO runs it when any [2D_INFILTRATION_DEFAULTS] or [2D_INFILTRATION] row resolves; NO keeps the rows but deactivates them for the run; YES warns when no row resolves. |
| INFIL_STEP | HH:MM:SS, HH:MM or seconds | 0 (unset) | Infiltration update cadence. Unset falls back to [2D_INFILTRATION_OPTIONS] INFIL_STEP, then to the project WET_STEP. When both are present this key wins. |
| INFIL_DEFAULT_METHOD | NONE / HORTON / MOD_HORTON / GREEN_AMPT / MOD_GREEN_AMPT / CURVE_NUMBER / CONSTANT (the legacy spellings MODIFIED_HORTON, MODIFIED_GREEN_AMPT and CURVE_NUM are accepted) | unset | Method of the mesh-wide `*` row of [2D_INFILTRATION_DEFAULTS]. Unset lets that row govern; NONE drops the `*` row for the run; a method must match the `*` row (its parameters live there) - a mismatch is an initialise error, a missing row a warning. |
| INFIL_DESTINATION | LOST / SUBCATCH_AQUIFER / AQUIFER_2D | LOST | Destination applied to every infiltration row that did not spell its own DEST column. SUBCATCH_AQUIFER recharges the legacy aquifer of the subcatchment containing the cell; AQUIFER_2D recharges the integrated two-zone aquifer and requires a resolved [2D_AQUIFER]. |

Groundwater

| Keyword | Values | Default | Description |
|---|---|---|---|
| GROUNDWATER | YES / ON / NO / OFF / AUTO | AUTO | Process enable for the integrated 2D subsurface. AUTO runs the kernel when any [2D_AQUIFER_OPTIONS], [2D_AQUIFER] or [2D_AQUIFER_NODE] row was authored; NO keeps the rows and runs without the subsurface; YES with no rows warns. |
| GW_ET | NONE / CAPILLARY_RISE / BOUNDARY_ET / BOTH | NONE | Alias of the [2D_AQUIFER_OPTIONS] key of the same name. A value spelled here is folded into [2D_AQUIFER_OPTIONS] when the model opens and is never stored in two places. |

Transport

| Keyword | Values | Default | Description |
|---|---|---|---|
| TRANSPORT_POLLUTANTS | YES / ON / NO / OFF | YES | Carry the [POLLUTANTS] species on the 2D surface. NO drops their rows from the surface transport state; the 1D side is unaffected. |
| TRANSPORT_MSX | YES / ON / NO / OFF | YES | Carry the reactions component's species on the 2D surface. |
| TRANSPORT_AGE | YES / ON / NO / OFF | YES | Carry water age on the 2D surface. |
| TRANSPORT_TEMPERATURE | YES / ON / NO / OFF | YES | Carry temperature on the 2D surface. |
| DISPERSION | >= 0, square metres per second | 0 | Isotropic species dispersion coefficient on the surface. 0 never enters the dispersive face term; a negative value is refused. Always SI. |

Output

| Keyword | Values | Default | Description |
|---|---|---|---|
| REPORT_2D | YES / 1 / NO / 0 | YES | NO skips the recomputation of the reported gradient fields at each report step. It does not suppress the 2D results file. |
| OUTPUT_FILE | path (one token; quote a path containing spaces) | none | 2D results file (HDF5, CF/UGRID), resolved relative to the input file's directory. When absent no 2D results file is written. |
| OUTPUT_PRECISION | FLOAT32 (F32, SINGLE) / FLOAT64 (F64, DOUBLE) | FLOAT32 | Storage type of the time-varying datasets and envelopes of the results file. Mesh geometry always stays float64. |
| OUTPUT_COMPRESSION | 0 .. 9 | 4 | zlib level of the chunked datasets; the byte-shuffle filter precedes it whenever the level is above 0. 0 is none. |
| REPORT_2D_VARIABLES | DEFAULT / MINIMAL / ALL / NONE, or any list of DEPTH VELOCITY EDGE_FLUX NODE_HEAD SPECIES RAINFALL INFILTRATION COUPLING GRADIENTS CONTINUITY ENVELOPES BUILDUP GROUNDWATER GW_DETAILED | DEFAULT | Dataset groups written to the results file (space- or comma-separated; presets and tokens may be mixed). DEPTH is always included. DEFAULT is DEPTH VELOCITY EDGE_FLUX NODE_HEAD SPECIES RAINFALL INFILTRATION ENVELOPES BUILDUP GROUNDWATER; MINIMAL is DEPTH NODE_HEAD ENVELOPES. Unselected groups are not created in the file. |
| REPORT_2D_SPECIES | ALL, or a list of species names | ALL | Species rows written to the results file. ALL writes every row the transport layout carries. |
| REPORT_2D_STEP | HH:MM:SS, HH:MM or seconds | 0 | 2D-only report interval. 0 follows [OPTIONS] REPORT_STEP; otherwise it must be a positive multiple of REPORT_STEP (checked at start). |

Retired keys

| Keyword | Status | Behaviour today |
|---|---|---|
| MIN_TIMESTEP | \status{Retired} | Ignored on file load with WARNING 104; a hard error on the programmatic option-set path. |
| REL_TOLERANCE | \status{Retired} | Ignored on file load with WARNING 104; a hard error on the programmatic option-set path. |
| ABS_TOLERANCE | \status{Retired} | Ignored on file load with WARNING 104; a hard error on the programmatic option-set path. |
| MAX_CVODE_STEPS | \status{Retired} | Ignored on file load with WARNING 104; a hard error on the programmatic option-set path. |
| MAX_KRYLOV_DIM | \status{Retired} | Ignored on file load with WARNING 104; a hard error on the programmatic option-set path. |
| LINEAR_SOLVER | \status{Retired} | Ignored on file load with WARNING 104; a hard error on the programmatic option-set path. |
| PRECONDITIONER | \status{Retired} | Ignored on file load with WARNING 104; a hard error on the programmatic option-set path. |
| JACOBIAN | \status{Retired} | Ignored on file load with WARNING 104; a hard error on the programmatic option-set path. |
| ATOL_AREA_REF | \status{Retired} | Ignored on file load with WARNING 104; a hard error on the programmatic option-set path. |
| COUPLING_INTERVAL | \status{Retired} | Ignored on file load with WARNING 104; a hard error on the programmatic option-set path. Superseded by COUPLING_SYNC. |
| COUPLING_WINDOW | \status{Retired} | Ignored on file load with WARNING 104; a hard error on the programmatic option-set path. Superseded by COUPLING_SYNC. |
| ACTIVE_SET | \status{Retired} | Ignored on file load with WARNING 104; a hard error on the programmatic option-set path. The active set is always on. |
| ACTIVE_SET_HALO | \status{Retired} | Ignored on file load with WARNING 104; a hard error on the programmatic option-set path. |
| MOMENTUM | \status{Retired} | Ignored on file load with WARNING 104; a hard error on the programmatic option-set path. Superseded by MOMENTUM_EQUATION. |
| INTEGRATOR with any value other than EXPLICIT (CVODE, ARKODE) | \status{Retired} | The value is ignored on file load with WARNING 104 and the marcher runs; a hard error on the programmatic option-set path. |
| ADVECTION YES | \status{Retired} | Deprecated spelling of MOMENTUM_EQUATION FULL_SWE. Accepted and honoured; a file load warns and names the replacement. |

Remarks:
The keys marked "always metres" or "always SI" (DRY_DEPTH, H_MOVE, FLUX_DH_EPS, DISPERSION) do not follow FLOW_UNITS or the mesh's `;; UNITS:` header; everything the mesh sections carry does (see [2D_VERTICES]).

WARNING 104 reads: "[2D_OPTIONS] %s was retired with the CVODE/ARKODE 2D solvers and was ignored; the explicit local-inertial marcher is the only 2D integrator."

The section may also appear in the external mesh file named by [2D_MESH_FILE]; values read there are applied after the inline ones and override them.

`OUTPUT_FILE` reads a single token, so a path containing spaces must be double-quoted; [2D_MESH_FILE] is the one section that rejoins an unquoted path.

`INFIL_STEP` here is the canonical home of the [2D_INFILTRATION_OPTIONS] value and `GW_ET` of the [2D_AQUIFER_OPTIONS] value; the writer emits each in its canonical section only.

The theory behind the solver, wetting and drying, coupling and reporting keys is in @ref hydraulics_ref_ch9_two_dimensional (sections 9.4, 9.5, 9.7, 9.8, 9.9 and 9.11); the groundwater enables refer to hydrology Chapter 9 (mesh groundwater).

This section has no counterpart in SWMM 5.

<!-- source: src/engine/2d/input/SectionHandlers2D.cpp:102-429 (parser 102-390, allow-lists 393-429; registration 1225-1240); src/engine/2d/data/SolverOptions2D.hpp:59-336; src/engine/2d/data/Report2DVars.hpp:28-36 -->

### Section: [2D_VERTICES] {#engine_manual_sect_2D_VERTICES}

Purpose:
Lists the vertices of the two-dimensional mesh: their planimetric coordinates and bed elevation.

Format:
```
X  Y  Z  (Tag)
```

Parameters:
X    horizontal coordinate of the vertex, in the mesh's length units (ft or m).
Y    vertical map coordinate of the vertex, in the mesh's length units (ft or m).
Z    bed elevation of the vertex, in the mesh's length units (ft or m).
Tag    optional name of the vertex, usable instead of its index in [2D_VERTEX_NODE_MAP].

Remarks:
Vertices are numbered 0, 1, 2, ... in file order; that 0-based index is what [2D_TRIANGLES], [2D_QUADS], [2D_EDGE_CONVEYANCE] and [2D_VERTEX_NODE_MAP] refer to. A row with fewer than three numeric tokens is a parse error.

The mesh's length unit follows [OPTIONS] FLOW_UNITS (feet for CFS, GPM and MGD; metres otherwise) unless the file carries a comment line `;; UNITS: SI (m)` (also accepted: `m`, `metre`, `metres`, `meter`, `meters`, case-insensitive), which declares the coordinates, elevations, initial depths, coupling areas and constant stages already metric and suppresses the feet-to-metres scaling at initialise. The header is looked for over the whole file, last match wins; the engine's own writer emits it under this section.

A mesh needs at least three vertices; the mesh is validated when the model initialises (see @ref hydraulics_ref_ch9_two_dimensional, section 9.3).

This section has no counterpart in SWMM 5.

<!-- source: src/engine/2d/input/SectionHandlers2D.cpp:574-599 (registration 1242-1245; UNITS header 1455-1508) -->

### Section: [2D_TRIANGLES] {#engine_manual_sect_2D_TRIANGLES}

Purpose:
Lists the triangular cells of the mesh, each with its Manning roughness and, optionally, an initial water depth and a classification tag.

Format:
```
V1  V2  V3  ManningsN  (InitDepth)  (Tag)
```

Parameters:
V1, V2, V3    0-based indices of the three vertices, from [2D_VERTICES].
ManningsN    Manning's roughness coefficient (n) of the cell.
InitDepth    initial water depth in the mesh's length units (ft or m); must be >= 0 (default 0 = dry).
Tag    optional classification name of the cell, matched by [2D_INFILTRATION_DEFAULTS] and by the TAG scope of the aquifer, quality and groundwater sections, and usable instead of the index in [2D_TRIANGLE_NODE_MAP].

Remarks:
Cells are numbered 0, 1, 2, ... in file order, triangles first and then every [2D_QUADS] row; that unified 0-based index is the TRI column of [2D_INITIAL_VELOCITY], [2D_BOUNDARY_CONDITIONS] and [2D_BOUNDARY_QUALITY], and the CELL column (1-based) of the other cell-addressed sections. A triangle row that follows a quad row is a parse error.

The fifth column is ambiguous by design: when it parses as a number it is InitDepth and the sixth column is Tag; otherwise it is Tag. The engine's writer emits InitDepth on every row whenever any cell carries a depth or a tag, so files it wrote are unambiguous.

Edge k of a cell joins vertices V[(k+1) mod 3] and V[(k+2) mod 3]: edge 0 is opposite V1, edge 1 opposite V2 and edge 2 opposite V3. This is the EDGE numbering of [2D_BOUNDARY_CONDITIONS] and [2D_BOUNDARY_QUALITY].

Vertex indices must be in range and distinct, the area positive and ManningsN positive; these are checked when the model initialises, not at parse.

This section has no counterpart in SWMM 5.

<!-- source: src/engine/2d/input/SectionHandlers2D.cpp:602-653 (registration 1247-1250) -->

### Section: [2D_QUADS] {#engine_manual_sect_2D_QUADS}

Purpose:
Lists the convex quadrilateral cells of the mesh, appended after every triangle.

Format:
```
V1  V2  V3  V4  ManningsN  (InitDepth)  (Tag)
```

Parameters:
V1, V2, V3, V4    0-based indices of the four vertices in cyclic order (either orientation).
ManningsN    Manning's roughness coefficient (n) of the cell.
InitDepth    initial water depth in the mesh's length units (ft or m); must be >= 0 (default 0 = dry).
Tag    optional classification name of the cell (see [2D_TRIANGLES]).

Remarks:
The j-th quad row is cell number (number of triangles + j) in the unified cell index used by every cell-addressed section. Column six is InitDepth when numeric, otherwise Tag; column seven is Tag when InitDepth is present, exactly as for [2D_TRIANGLES].

Edge k of a quad joins V[(k+1) mod 4] and V[(k+2) mod 4]: edge 0 = (V2, V3), edge 1 = (V3, V4), edge 2 = (V4, V1), edge 3 = (V1, V2). Non-convex or self-intersecting quads fail validation at initialise.

The section is written only when the mesh holds quads; an all-triangle file is unchanged. See @ref hydraulics_ref_ch9_two_dimensional, section 9.11b.

This section has no counterpart in SWMM 5.

<!-- source: src/engine/2d/input/SectionHandlers2D.cpp:656-699 (registration 1252-1255) -->

### Section: [2D_MESH_FILE] {#engine_manual_sect_2D_MESH_FILE}

Purpose:
Names an external file that holds the mesh and the other 2D sections instead of, or in addition to, the main input file.

Format:
```
FILE  Path
```

Parameters:
Path    absolute path, or a path relative to the directory of the input file. Surrounding double quotes are stripped; an unquoted path containing spaces is rejoined from its tokens.

Remarks:
Only the first FILE line of the section is read. The conventional extension is `.2dm`, but the file is plain input-file text: any of [2D_OPTIONS], [2D_VERTICES], [2D_TRIANGLES], [2D_QUADS], [2D_INITIAL_VELOCITY], [2D_VERTEX_NODE_MAP], [2D_TRIANGLE_NODE_MAP], [2D_BOUNDARY_CONDITIONS], [2D_EDGE_CONVEYANCE], [2D_INITIAL_QUALITY], [2D_BOUNDARY_QUALITY], [2D_INFILTRATION_OPTIONS], [2D_INFILTRATION_DEFAULTS] and [2D_INFILTRATION], with the same grammar as here. It may not contain [2D_MESH_FILE] itself, and it does not carry the aquifer, coverage, loading, curb-length or [GW_*] sections, which stay in the main file.

The external file is parsed after the main file. Per section, the external file wins: a section present in both replaces the inline rows rather than adding to them, and its [2D_OPTIONS] values override inline ones. Its own `;; UNITS:` header is honoured (see [2D_VERTICES]). A missing or unreadable file is fatal when the model is opened for a run.

When a mesh file is set, the writer emits only [2D_OPTIONS] and this section into the input file and rewrites the current mesh to the external file. The file slot is described in @ref engine_manual_ch3_files.

This section has no counterpart in SWMM 5.

<!-- source: src/engine/2d/input/SectionHandlers2D.cpp:1299-1319 (external load 1327-1449) -->

### Section: [2D_VERTEX_NODE_MAP] {#engine_manual_sect_2D_VERTEX_NODE_MAP}

Purpose:
Couples a mesh vertex to a node of the one-dimensional network, so that water is exchanged between the cells around the vertex and the node.

Format:
```
Vertex  Node  (Cd)  (Area)
```

Parameters:
Vertex    0-based index of a vertex, or its Tag from [2D_VERTICES].
Node    name of a junction, storage unit or outfall node.
Cd    discharge coefficient of the exchange (default: [2D_OPTIONS] COUPLING_CD, 0.65).
Area    exchange area, in the mesh's length units squared (sq ft or sq m; default 1.0).

Remarks:
The first token is tried as a numeric index first; a number that is not a valid vertex index is looked up as a tag, so purely numeric tags work. An unknown index or tag is a parse error, so this section must follow [2D_VERTICES] in the same file. An unknown node name is fatal when the model initialises.

The exchange is a bidirectional orifice: positive when the surface drains into the node, negative when the node surcharges onto the surface. It opens once the higher of the two heads reaches the node's rim (invert plus maximum depth), so the node's Elevation + MaxDepth should equal the ground elevation at the inlet. The exchange is spread over the cells incident to the vertex, weighted by the water-surface slope.

One row per vertex; a repeated vertex overwrites the earlier row. A Cd or Area token that does not parse as a number is ignored, not reported. An Area that was authored explicitly is never replaced by COUPLING_AREA AUTO.

Naming an outfall couples the outfall's discharge onto the surface and makes the surface stage its dynamic tailwater. See @ref hydraulics_ref_ch9_two_dimensional, section 9.7.

This section has no counterpart in SWMM 5.

<!-- source: src/engine/2d/input/SectionHandlers2D.cpp:728-766 (registration 1262-1265); src/engine/2d/data/MeshData.hpp:158-165,263-265 -->

### Section: [2D_TRIANGLE_NODE_MAP] {#engine_manual_sect_2D_TRIANGLE_NODE_MAP}

Purpose:
Couples a mesh cell (triangle or quad) to a node of the one-dimensional network; the whole exchange lands on that cell.

Format:
```
Cell  Node  (Cd)  (Area)
```

Parameters:
Cell    0-based unified cell index, or the cell's Tag from [2D_TRIANGLES] or [2D_QUADS].
Node    name of a junction, storage unit or outfall node.
Cd    discharge coefficient of the exchange (default: [2D_OPTIONS] COUPLING_CD, 0.65).
Area    exchange area, in the mesh's length units squared (sq ft or sq m; default 1.0).

Remarks:
The first token is tried as a numeric index first and falls back to a tag lookup when the number is not a valid cell index. An unknown index or tag is a parse error, so this section must follow the cell sections in the same file.

Unlike [2D_VERTEX_NODE_MAP], every row appends a coupling: several nodes may couple to one cell. A Cd or Area token that does not parse as a number is ignored.

The exchange law, the rim gate and the outfall behaviour are those of [2D_VERTEX_NODE_MAP]; see @ref hydraulics_ref_ch9_two_dimensional, section 9.7.

This section has no counterpart in SWMM 5.

<!-- source: src/engine/2d/input/SectionHandlers2D.cpp:769-820 (registration 1267-1270) -->

### Section: [2D_BOUNDARY_CONDITIONS] {#engine_manual_sect_2D_BOUNDARY_CONDITIONS}

Purpose:
Assigns a boundary condition to an edge of a cell that lies on the mesh boundary.

Format:
```
Cell  Edge  WALL
Cell  Edge  NORMAL_FLOW      Slope     (*)  (Group)
Cell  Edge  SPECIFIED_STAGE  Head      (*)  (Group)
Cell  Edge  TS_STAGE         Tseries   (*)  (Group)
Cell  Edge  SPECIFIED_FLOW   Flow      (*)  (Group)
Cell  Edge  TS_FLOW          Tseries   (*)  (Group)
Cell  Edge  RATING_CURVE     Curve     (*)  (Group)
```

Parameters:
Cell    0-based unified cell index (the TRI column of files the engine writes).
Edge    local edge of that cell: 0 .. 2 for a triangle, 0 .. 3 for a quad (see [2D_TRIANGLES] and [2D_QUADS] for the numbering).
Slope    bed slope used by the Manning outflow of a NORMAL_FLOW edge (dimensionless).
Head    fixed water-surface elevation outside a SPECIFIED_STAGE edge, in the mesh's length units (ft or m).
Flow    fixed discharge per metre of edge for a SPECIFIED_FLOW edge, in the project's flow units per metre; outward positive, so an inflow is negative.
Tseries    name of a [TIMESERIES] giving the stage (TS_STAGE, project length units) or the per-metre flow (TS_FLOW, project flow units per metre) against time.
Curve    name of a [CURVES] table giving discharge per metre of edge (project flow units per metre, outward positive) against stage (project length units) for a RATING_CURVE edge.
*    placeholder for the fifth column, which is reserved and ignored.
Group    optional label of the edge; stored and written back, never acted on.

Remarks:
Every boundary edge not listed is a WALL (zero flux). A row needs at least Cell, Edge and the type; the fourth token may be `*` or absent to mean "no value". The fourth token must be numeric for NORMAL_FLOW, SPECIFIED_STAGE and SPECIFIED_FLOW and is a name for the others. The sixth token is Group and is read only when a fifth token is present, so write `*` in the fifth column whenever a Group is given. Edge is checked against the widest cell (0 .. 3) at parse and against the cell's own vertex count at initialise; the Cell index is checked at initialise.

TS_STAGE and SPECIFIED_STAGE share one internal type, as do TS_FLOW and SPECIFIED_FLOW; the TS_ spelling only signals that the parameter is a name. A NORMAL_FLOW edge with a zero slope behaves as a wall (a warning is raised; the slope is never derived from the geometry). Under FULL_SWE a NORMAL_FLOW edge is transmissive.

Series and curve names are resolved once, at the first advance, and evaluated every routing step at the absolute date-time; a name that resolves to nothing is treated as a constant. A constant stage follows the mesh's length unit, including the `;; UNITS:` header; a series stage is in the project's display length unit whatever the header says; flows follow FLOW_UNITS (CFS, GPM, MGD, CMS, LPS, MLD). The cumulative volume through each edge, outward positive, enters the 2D mass balance.

The section may also be placed in the external mesh file. See @ref hydraulics_ref_ch9_two_dimensional, section 9.6.

This section has no counterpart in SWMM 5.

<!-- source: src/engine/2d/input/SectionHandlers2D.cpp:861-919 (registration 1274-1277); src/engine/2d/data/BoundaryData.hpp:56-62; src/engine/2d/SurfaceRouter2D.cpp:245-275,1922-1938 -->

### Section: [2D_EDGE_CONVEYANCE] {#engine_manual_sect_2D_EDGE_CONVEYANCE}

Purpose:
Partially or fully blocks an interior edge of the mesh, to represent a leaky berm, a fence or a partly blocked wall between two cells.

Format:
```
FromVertex  ToVertex  Conveyance
```

Parameters:
FromVertex    0-based index of one end vertex of the edge.
ToVertex    0-based index of the other end vertex; must differ from FromVertex.
Conveyance    fraction of the edge's flux that passes, in [0, 1]: 0 blocks the edge, 1 leaves it unobstructed.

Remarks:
Exactly three tokens. The vertex pair is undirected; a repeated pair takes the last row. A conveyance outside [0, 1] is a parse error; a pair that is not an edge of the mesh is fatal when the model initialises. Edges not listed have a conveyance of 1.

For a feature that must hold water until a true crest elevation, resolve the crest as a line of high vertices and use [2D_OPTIONS] FACE_RECONSTRUCTION VFR_FACE; edge conveyance is the complementary control for partial blockage. The section may also be placed in the external mesh file.

This section has no counterpart in SWMM 5.

<!-- source: src/engine/2d/input/SectionHandlers2D.cpp:926-961 (registration 1282-1285) -->

### Section: [2D_INITIAL_VELOCITY] {#engine_manual_sect_2D_INITIAL_VELOCITY}

Purpose:
Gives a non-zero initial velocity to selected cells, for problems whose initial state is not at rest.

Format:
```
Cell  U  V
```

Parameters:
Cell    0-based unified cell index (the TRI column of files the engine writes).
U    initial velocity component along the map x-axis (m/s, always SI).
V    initial velocity component along the map y-axis (m/s, always SI).

Remarks:
Rows may cover any subset of cells; unlisted cells start at rest. The index is checked at parse against the cells already read, so the section must follow [2D_TRIANGLES] and [2D_QUADS] in the same file. At initialise the marcher projects (h u, h v) onto its face normals to seed the face discharges, which a depth-only initial condition cannot represent (the Thacker planar oscillation of the verification suite, for instance). See @ref hydraulics_ref_ch9_two_dimensional, section 9.10.

This section has no counterpart in SWMM 5.

<!-- source: src/engine/2d/input/SectionHandlers2D.cpp:702-725 (registration 1257-1260) -->

### Section: [2D_INFILTRATION_OPTIONS] {#engine_manual_sect_2D_INFILTRATION_OPTIONS}

Purpose:
Sets the update cadence of infiltration on the mesh.

Format:
```
INFIL_STEP  Step
```

Parameters:
Step    infiltration update interval as HH:MM:SS, HH:MM or seconds; must be >= 0. 0 (the default) uses the project's WET_STEP.

Remarks:
INFIL_STEP is the only keyword accepted; any other is a parse error. [2D_OPTIONS] INFIL_STEP is the canonical spelling of the same value and wins when both are present; the writer emits the key there. The section may also be placed in the external mesh file, where it replaces the inline value.

This section has no counterpart in SWMM 5.

<!-- source: src/engine/2d/input/SectionHandlers2D.cpp:1053-1071 (registration 1290-1291) -->

### Section: [2D_INFILTRATION_DEFAULTS] {#engine_manual_sect_2D_INFILTRATION_DEFAULTS}

Purpose:
Assigns an infiltration method and its parameters to every cell that carries a given tag, or to the whole mesh.

Format:
```
Tag  NONE
Tag  HORTON               f0  fmin  Decay  DryTime  Fmax  (Dest)
Tag  MODIFIED_HORTON      f0  fmin  Decay  DryTime  Fmax  (Dest)
Tag  GREEN_AMPT           S   Ks    IMD                   (Dest)
Tag  MODIFIED_GREEN_AMPT  S   Ks    IMD                   (Dest)
Tag  CURVE_NUMBER         CN  -     DryTime               (Dest)
Tag  CONSTANT             Rate                            (Dest)
```

Parameters:
Tag    a cell tag from [2D_TRIANGLES] or [2D_QUADS], or `*` for every cell.
f0    maximum infiltration rate on the Horton curve (in/hr or mm/hr).
fmin    minimum infiltration rate on the Horton curve (in/hr or mm/hr).
Decay    decay rate constant of the Horton curve (1/hr).
DryTime    time for a fully saturated soil to dry completely (days).
Fmax    maximum infiltration volume possible (in or mm; 0 if not applicable).
S    soil capillary suction head (in or mm).
Ks    soil saturated hydraulic conductivity (in/hr or mm/hr).
IMD    initial soil moisture deficit (volume of voids / total volume).
CN    SCS curve number.
Rate    constant infiltration rate (in/hr or mm/hr).
Dest    destination of the infiltrated water: LOST (default), SUBCATCH_AQUIFER or AQUIFER_2D.

Remarks:
The first token is the tag itself (not the keyword TAG); `*` is the mesh-wide fallback and may appear on any row. The parameters are positional, in the project's rainfall units, at most five; a `-` leaves a slot unset (0) and trailing unused columns may be omitted. Dest is recognised as the final token when it is neither numeric nor `-`. NONE assigns no infiltration to the matching cells. The method spellings are those listed above; the [2D_OPTIONS] INFIL_DEFAULT_METHOD short forms (MOD_HORTON, MOD_GREEN_AMPT, CURVE_NUM) are not accepted on a row.

Precedence at initialise is [2D_INFILTRATION] cell override, then the tag match, then the `*` row. A row's own Dest overrides [2D_OPTIONS] INFIL_DESTINATION. AQUIFER_2D needs a resolved [2D_AQUIFER]; SUBCATCH_AQUIFER needs the cell to lie inside a subcatchment that has a [GROUNDWATER] aquifer, and is refused when the integrated groundwater kernel is also on (the water would be delivered twice).

The section may also be placed in the external mesh file, where it replaces the inline rows. The formulas are those of the [INFILTRATION] section; the mesh application is in hydrology Chapter 9 (mesh groundwater) and @ref hydraulics_ref_ch9_two_dimensional.

This section has no counterpart in SWMM 5.

<!-- source: src/engine/2d/input/SectionHandlers2D.cpp:982-1019,1074-1090 (registration 1293-1294); src/engine/2d/infil/Infil2D.hpp:91-126; src/engine/2d/infil/Infil2D.cpp:141-200 -->

### Section: [2D_INFILTRATION] {#engine_manual_sect_2D_INFILTRATION}

Purpose:
Overrides the infiltration method and parameters of a single cell.

Format:
```
Cell  Method  (P1  P2  P3  P4  P5)  (Dest)
```

Parameters:
Cell    1-based unified cell index (the first triangle is 1; the first quad is the number of triangles + 1).
Method    NONE, HORTON, MODIFIED_HORTON, GREEN_AMPT, MODIFIED_GREEN_AMPT, CURVE_NUMBER or CONSTANT.
P1 .. P5    the method's positional parameters, laid out exactly as in [2D_INFILTRATION_DEFAULTS] (project rainfall units; `-` = unset).
Dest    LOST (default), SUBCATCH_AQUIFER or AQUIFER_2D.

Remarks:
Among the [2D_*] sections that address a cell by a bare index column, this is the only 1-based one; the sections that spell CELL as a scope keyword ([2D_INITIAL_QUALITY], [2D_AQUIFER], the quality and [GW_*] sections) are 1-based as well, whereas the TRI columns of [2D_INITIAL_VELOCITY], [2D_BOUNDARY_CONDITIONS] and [2D_BOUNDARY_QUALITY] are 0-based. Only the lower bound is checked at parse; a cell beyond the mesh is reported at initialise. A cell row wins over any tag or `*` row of [2D_INFILTRATION_DEFAULTS].

The section may also be placed in the external mesh file, where it replaces the inline rows.

This section has no counterpart in SWMM 5.

<!-- source: src/engine/2d/input/SectionHandlers2D.cpp:982-1019,1093-1117 (registration 1296-1297) -->

### Section: [2D_AQUIFER_OPTIONS] {#engine_manual_sect_2D_AQUIFER_OPTIONS}

Purpose:
Sets the global options of the integrated two-zone groundwater kernel that runs beneath the mesh.

Format:
```
Keyword  Value
```

Parameters:

| Keyword | Values | Default | Description |
|---|---|---|---|
| SOIL_CHAR | RUSSO / GARDNER / BROOKS_COREY / VAN_GENUCHTEN | RUSSO | Soil-characteristic law shaping conductivity and water content against pressure head, for rows that do not set their own. |
| CLOSURE | AUTO / CLOSED_FORM / ENSLAVED / SIGMA | AUTO | Unsaturated-zone closure: a bulk store with a quasi-steady recharge (CLOSED_FORM), that store made algebraic in the water table (ENSLAVED), or an explicit column of sigma layers (SIGMA). AUTO selects per cell at initialise. |
| M_LAYERS | 2 .. 128 | 8 | Number of sigma layers of a SIGMA column, for rows that do not set their own. |
| CAPILLARY_DIFF | YES / TRUE / 1 / NO / FALSE / 0 | NO | Optional diffusive term of the SIGMA column. |
| C_GW | (0, 1] | 0.5 | Stability safety factor of the saturated-zone step. |
| C_COL | (0, 1] | 0.9 | Stability safety factor of the unsaturated column step. |
| FORCE_CLOSED_FORM | YES / TRUE / 1 / NO / FALSE / 0 | NO | Suppress the automatic switch away from CLOSED_FORM on strongly capillary soils. |
| MODE | MESH / PER_SUBCATCH | MESH | MESH runs one aquifer cell under every mesh cell. PER_SUBCATCH runs one degenerate cell per subcatchment with no lateral flow, exchanging with the outlet node. |
| DUNNE | YES / TRUE / 1 / NO / FALSE / 0 | YES | Saturation-excess transfer from the aquifer to the surface cell above it. Exists for comparison runs; NO breaks the mass balance between the two domains. |
| GW_ET | NONE / CAPILLARY_RISE / BOUNDARY_ET / BOTH | NONE | Subsurface evapotranspiration: capillary rise from the water table, removal at the top of the unsaturated column, both, or neither. |

Remarks:
Authoring any [2D_AQUIFER_OPTIONS], [2D_AQUIFER] or [2D_AQUIFER_NODE] row turns the kernel on unless [2D_OPTIONS] GROUNDWATER NO is set. A value of GW_ET spelled in [2D_OPTIONS] is folded into this section when the model opens. The writer emits only the keys that differ from their defaults.

The kernel is described in hydrology Chapter 9 (mesh groundwater).

This section has no counterpart in SWMM 5.

<!-- source: src/engine/2d/subsurface/SubsurfaceSections.cpp:109-167 (registration 285-289); src/engine/2d/subsurface/SubsurfaceData.hpp:58-107 -->

### Section: [2D_AQUIFER] {#engine_manual_sect_2D_AQUIFER}

Purpose:
Gives the soil properties of the aquifer beneath the whole mesh, beneath every cell carrying a tag, or beneath one cell.

Format:
```
*         Ks  Zs  ThetaS  ThetaR  Alpha  (Key value) ...
TAG name  Ks  Zs  ThetaS  ThetaR  Alpha  (Key value) ...
CELL n    Ks  Zs  ThetaS  ThetaR  Alpha  (Key value) ...
```

Parameters:
name    a cell tag from [2D_TRIANGLES] or [2D_QUADS].
n    1-based unified cell index.
Ks    saturated hydraulic conductivity (in/hr or mm/hr); > 0.
Zs    thickness of the soil column from the aquifer bottom to the surface (ft or m); > 0.
ThetaS    porosity, i.e. saturated water content (fraction); in (0, 1].
ThetaR    residual water content (fraction); in [0, ThetaS).
Alpha    sorptive number of the Gardner and Russo laws (1/ft or 1/m); > 0.

Keyword pairs, in any order after the five positional columns:

| Keyword | Value | Default | Description |
|---|---|---|---|
| PSI_B | >= 0 (ft or m) | 0.20 | Air-entry pressure head of the Brooks-Corey law. |
| LAMBDA | > 0 | 0.40 | Pore-size distribution index of the Brooks-Corey law. |
| N | > 1 | 1.6 | Shape parameter n of the van Genuchten law (m = 1 - 1/n). |
| L | number | 0.5 | Mualem tortuosity exponent of the van Genuchten law. |
| C_LOSS | >= 0 (in/hr or mm/hr) | 0 | Deep-loss coefficient: the percolation rate out of the aquifer bottom at full saturation, scaled by the saturated fraction of the column. |
| HG0 | >= 0 (ft or m) | unset | Initial saturated thickness above the aquifer bottom, clamped to Zs. Unset leaves the water table at the aquifer bottom. |
| SOIL_CHAR | RUSSO / GARDNER / BROOKS_COREY / VAN_GENUCHTEN | from [2D_AQUIFER_OPTIONS] | Soil-characteristic law of these cells. |
| CLOSURE | AUTO / CLOSED_FORM / ENSLAVED / SIGMA | from [2D_AQUIFER_OPTIONS] | Unsaturated closure of these cells. |
| M_LAYERS | 2 .. 128 | from [2D_AQUIFER_OPTIONS] | Sigma layers of these cells' columns. |

Remarks:
Rows are resolved in the order `*`, then TAG, then CELL, each pass overwriting the previous, so the most specific scope wins. All five positional columns are required. Values are authored in the project's units and converted to SI once at initialise; the writer emits the rows as authored, so a load-and-save is byte-identical. A CELL index beyond the mesh is reported at initialise.

The theory of the two zones, the soil laws and the closures is in hydrology Chapter 9 (mesh groundwater).

This section has no counterpart in SWMM 5.

<!-- source: src/engine/2d/subsurface/SubsurfaceSections.cpp:173-241 (registration 290-294; unit factors 306-321); src/engine/2d/subsurface/SubsurfaceData.hpp:109-136; src/engine/2d/subsurface/SubsurfaceSolver.cpp:128-156 -->

### Section: [2D_AQUIFER_NODE] {#engine_manual_sect_2D_AQUIFER_NODE}

Purpose:
Couples a node of the one-dimensional network to the aquifer cell beneath it, optionally through a semi-confining bed layer.

Format:
```
Node  Cell  (KC k)  (DC d)  (AREA a)
```

Parameters:
Node    name of a node.
Cell    1-based unified index of the mesh cell containing the node.
k    hydraulic conductivity of the bed layer (in/hr or mm/hr); >= 0. 0 (default) means a direct Darcy exchange with no bed.
d    thickness of the bed layer (ft or m); >= 0; must be > 0 whenever k > 0.
a    exchange area (sq ft or sq m); >= 0. 0 (default) uses the cell's own area.

Remarks:
The three keyword pairs may appear in any order and any may be omitted. Each node may have one row only; a second row for the same node is an error at initialise, as is an unknown node or a cell beyond the mesh. Values are in the project's units and converted at initialise.

The exchange law is described in hydrology Chapter 9 (mesh groundwater).

This section has no counterpart in SWMM 5.

<!-- source: src/engine/2d/subsurface/SubsurfaceSections.cpp:247-276 (registration 295-299; resolution 337-360); src/engine/2d/subsurface/SubsurfaceData.hpp:76-84 -->

### Section: [2D_INITIAL_QUALITY] {#engine_manual_sect_2D_INITIAL_QUALITY}

Purpose:
Sets the initial concentration of a transported species in the water standing on the mesh at the start of the simulation.

Format:
```
*          Species  Conc
TAG name   Species  Conc
CELL n     Species  Conc
```

Parameters:
name    a cell tag from [2D_TRIANGLES] or [2D_QUADS].
n    1-based unified cell index.
Species    name of a pollutant, of a species of the reactions component, or `__WATER_AGE__` or `__TEMPERATURE__` when that transport is on.
Conc    initial value: concentration in the species' units, water age in hours, temperature in degrees C. Must be finite and >= 0.

Remarks:
A `*` row has exactly three tokens and a TAG or CELL row exactly four. Rows are applied in the order `*`, then TAG, then CELL, later writes winning. The seeded mass is the concentration times the cell's initial volume, so a dry cell receives nothing. An unknown species, a tag that matches no cell, a cell beyond the mesh, or rows in a model that carries no transported species are fatal at initialise.

The section may also be placed in the external mesh file. The surface transport scheme is in @ref hydraulics_ref_ch9_two_dimensional.

This section has no counterpart in SWMM 5.

<!-- source: src/engine/2d/input/SectionHandlers2D.cpp:1124-1167 (registration 1213-1216); src/engine/2d/SurfaceRouter2D.cpp:1104-1160 -->

### Section: [2D_BOUNDARY_QUALITY] {#engine_manual_sect_2D_BOUNDARY_QUALITY}

Purpose:
Sets the concentration of a species carried by the water that enters the mesh through a non-wall boundary edge.

Format:
```
Cell  Edge  Species  Conc
```

Parameters:
Cell    0-based unified cell index (the TRI column of files the engine writes).
Edge    local edge of that cell (0 .. 2 for a triangle, 0 .. 3 for a quad), one that carries a [2D_BOUNDARY_CONDITIONS] row.
Species    name of a pollutant, of a species of the reactions component, or `__WATER_AGE__` or `__TEMPERATURE__`.
Conc    inflow value: concentration in the species' units, water age in hours, temperature in degrees C. Must be finite and >= 0.

Remarks:
Exactly four tokens. The value applies whenever the edge's boundary condition brings water in; outflow carries the cell's own concentration. A cell beyond the mesh, an unknown species, or rows in a model that carries no transported species are fatal at initialise.

The section may also be placed in the external mesh file.

This section has no counterpart in SWMM 5.

<!-- source: src/engine/2d/input/SectionHandlers2D.cpp:1173-1197 (registration 1218-1221); src/engine/2d/SurfaceRouter2D.cpp:796-819 -->

### Section: [2D_COVERAGES] {#engine_manual_sect_2D_COVERAGES}

Purpose:
Assigns land uses, and the percentage of area each covers, to the cells of the mesh, so that pollutant buildup, washoff and street sweeping run on the overland surface as they do on subcatchments.

Format:
```
*          LandUse  Percent  (LandUse  Percent) ...
TAG name   LandUse  Percent  (LandUse  Percent) ...
CELL n     LandUse  Percent  (LandUse  Percent) ...
```

Parameters:
name    a cell tag from [2D_TRIANGLES] or [2D_QUADS].
n    1-based unified cell index.
LandUse    name of a land use from the [LANDUSES] section.
Percent    percent of the cell's area covered by that land use; >= 0, and the percents of one row sum to at most 100.

Remarks:
A row carries the whole coverage set of its scope: a later row with the same scope replaces the earlier one rather than adding to it, and the scopes resolve `*`, then TAG, then CELL, the most specific winning. An unknown land use, a tag that matches no cell or a cell beyond the mesh is fatal at initialise, as is a model with no [LANDUSES] or no transported species.

Buildup accrues per cell, land use and species with the land use's [BUILDUP] function, is swept on the [LANDUSES] schedule and washed off by the cell's own runoff (its net outflow per runoff step) into the cell's species row. Under [2D_OPTIONS] RAINFALL_MODE NONE the rows are inert (the subcatchments own the surface); a covered cell that lies inside a subcatchment which itself has [COVERAGES] raises a double-counting warning. A land use whose buildup is normalised per curb length needs a [2D_CURB_LENGTH] on every cell it covers.

This section has no counterpart in SWMM 5.

<!-- source: src/engine/2d/quality/SurfaceQuality2D.cpp:93-137 (registration 166-188; resolution 260-330); src/engine/2d/quality/SurfaceQuality2D.hpp:24-60 -->

### Section: [2D_LOADINGS] {#engine_manual_sect_2D_LOADINGS}

Purpose:
Sets the initial pollutant buildup on the cells of the mesh at the start of the simulation.

Format:
```
*          Species  Buildup
TAG name   Species  Buildup
CELL n     Species  Buildup
```

Parameters:
name    a cell tag from [2D_TRIANGLES] or [2D_QUADS].
n    1-based unified cell index.
Species    name of a pollutant or of a species of the reactions component (water age and temperature do not build up).
Buildup    initial buildup per unit land area (lbs/acre or kg/ha); >= 0.

Remarks:
Exactly one species and value after the scope. Scopes resolve `*`, then TAG, then CELL. The value is divided among the cell's covered land uses; for a land use normalised per curb length it is converted with the cell's [2D_CURB_LENGTH]. Cells without a row start with the buildup their [BUILDUP] functions reach after [OPTIONS] DRY_DAYS, as subcatchments do. Rows without any [2D_COVERAGES] raise a warning and build up nothing.

This section has no counterpart in SWMM 5.

<!-- source: src/engine/2d/quality/SurfaceQuality2D.cpp:139-151 (registration 166-188; resolution 360-376, initial state 445-471) -->

### Section: [2D_CURB_LENGTH] {#engine_manual_sect_2D_CURB_LENGTH}

Purpose:
Gives the length of curb on a mesh cell, for land uses whose pollutant buildup is normalised per unit curb length.

Format:
```
*          Length
TAG name   Length
CELL n     Length
```

Parameters:
name    a cell tag from [2D_TRIANGLES] or [2D_QUADS].
n    1-based unified cell index.
Length    curb length on the cell (ft or m); >= 0.

Remarks:
Exactly one value after the scope. Scopes resolve `*`, then TAG, then CELL. A cell covered by a land use with a per-curb buildup function and no positive curb length is an error at initialise.

This section has no counterpart in SWMM 5.

<!-- source: src/engine/2d/quality/SurfaceQuality2D.cpp:153-164 (registration 166-188; resolution 333-357) -->

### Section: [GW_TRANSPORT_OPTIONS] {#engine_manual_sect_GW_TRANSPORT_OPTIONS}

Purpose:
\status{Planned} Sets the process switches and thermal boundary conditions of solute and heat transport in the integrated groundwater kernel. The rows are parsed, validated, kept and written back unchanged, but no kernel consumes them in this release.

Format:
```
TRANSPORT_POLLUTANTS   YES / NO
TRANSPORT_MSX          YES / NO
TRANSPORT_AGE          YES / NO
TRANSPORT_TEMPERATURE  YES / NO
DISPERSION             YES / NO
CONDUCTION             YES / NO
SURFACE_THERMAL_BC     AIR
SURFACE_THERMAL_BC     SURFACE_WATER
SURFACE_THERMAL_BC     FIXED  Temp
SURFACE_THERMAL_BC     TIMESERIES  Tseries
DEEP_THERMAL_BC        GEOTHERMAL_FLUX  Flux   (DEPTH z)
DEEP_THERMAL_BC        FIXED_TEMP       Temp   (DEPTH z)
DEEP_THERMAL_BC        TIMESERIES       Tseries  (DEPTH z)
THERMAL_MIXING         ARITHMETIC / GEOMETRIC
C_DIFF                 value
```

Parameters:

| Keyword | Values | Default | Description |
|---|---|---|---|
| TRANSPORT_POLLUTANTS | YES / ON / NO / OFF | YES | Carry the [POLLUTANTS] species in the aquifer. |
| TRANSPORT_MSX | YES / ON / NO / OFF | YES | Carry the reactions component's species in the aquifer. |
| TRANSPORT_AGE | YES / ON / NO / OFF | YES | Carry water age in the aquifer. |
| TRANSPORT_TEMPERATURE | YES / ON / NO / OFF | YES | Carry temperature in the aquifer. |
| DISPERSION | YES / ON / NO / OFF | YES | Activate the dispersivities and molecular diffusivity of [GW_TRANSPORT_PARAMS]. |
| CONDUCTION | YES / ON / NO / OFF | YES | Activate vertical and lateral heat conduction. |
| SURFACE_THERMAL_BC | AIR / SURFACE_WATER / FIXED Temp / TIMESERIES Tseries | AIR | Temperature at the top of the column when it is not inundated: the air temperature, the surface water, a fixed value (degrees C) or a [TIMESERIES]. |
| DEEP_THERMAL_BC | GEOTHERMAL_FLUX Flux / FIXED_TEMP Temp / TIMESERIES Tseries, each optionally followed by DEPTH z | GEOTHERMAL_FLUX | Condition at the bottom of the column: a geothermal heat flux (W per sq m), a fixed temperature (degrees C) or a [TIMESERIES]; DEPTH z (>= 0, metres) places it. |
| THERMAL_MIXING | ARITHMETIC / GEOMETRIC | ARITHMETIC | How the effective thermal conductivity of the soil-water mixture is averaged. |
| C_DIFF | (0, 1] | 0.4 | Explicit diffusion Courant number bounding the transport step. |

Remarks:
FIXED and FIXED_TEMP require a number and GEOTHERMAL_FLUX a number; TIMESERIES requires a series name. DEPTH is read only as the fourth and fifth tokens of a DEEP_THERMAL_BC line. Naming any [GW_*] section marks subsurface transport as authored, which is what the run-time warning and the writer key on.

At run time the engine emits one warning: "[GW_*] subsurface transport is AUTHORED but INERT this run: the integrated 2D groundwater component (org.hydrocouple.openswmm.integrated2d) provides the two-zone kernel these sections configure and is not available in this release. The rows are validated, kept and written back unchanged."

The intended formulation is described in hydrology Chapter 9 (mesh groundwater).

This section has no counterpart in SWMM 5.

<!-- source: src/engine/2d/gw/GwTransportSections.cpp:152-243 (registration 452-455; warning 632-639); src/engine/2d/gw/GwTransportData.hpp:73-101 -->

### Section: [GW_TRANSPORT_PARAMS] {#engine_manual_sect_GW_TRANSPORT_PARAMS}

Purpose:
\status{Planned} Gives the solute and thermal properties of the aquifer matrix beneath the whole mesh, beneath tagged cells, or beneath one cell. The rows are parsed, validated, kept and written back unchanged, but no kernel consumes them in this release.

Format:
```
*          (RhoS  Cs  LambdaS  As  AlphaL  AlphaT  Dm  Dv  GeoFlux)
TAG name   (RhoS  Cs  LambdaS  As  AlphaL  AlphaT  Dm  Dv  GeoFlux)
CELL n     (RhoS  Cs  LambdaS  As  AlphaL  AlphaT  Dm  Dv  GeoFlux)
```

Parameters:
name    a cell tag from [2D_TRIANGLES] or [2D_QUADS].
n    1-based unified cell index.
RhoS    grain density (kg per cu m; default 2650).
Cs    grain specific heat (J per kg per K; default 880).
LambdaS    grain thermal conductivity (W per m per K; default 2.0).
As    specific surface area of the grains (sq m per kg; default 0).
AlphaL    longitudinal dispersivity (m; default 1.0).
AlphaT    transverse dispersivity (m; default 0.1).
Dm    molecular diffusivity (sq m per s; default 1.0e-9).
Dv    vapour diffusivity (sq m per s; default 1.0e-9).
GeoFlux    geothermal flux (W per sq m), or the deep temperature (degrees C) when DEEP_THERMAL_BC is FIXED_TEMP; default 0.065. The only signed column.

Remarks:
All nine columns are positional and optional; an omitted column or a `-` keeps the default. More than nine columns is a parse error, and every column but GeoFlux must be >= 0. Values are SI. Scopes resolve `*`, then TAG, then CELL by order of the rows; a tag that matches no cell or a cell beyond the mesh is reported when the model initialises.

At run time the engine emits one warning: "[GW_*] subsurface transport is AUTHORED but INERT this run: the integrated 2D groundwater component (org.hydrocouple.openswmm.integrated2d) provides the two-zone kernel these sections configure and is not available in this release. The rows are validated, kept and written back unchanged."

This section has no counterpart in SWMM 5.

<!-- source: src/engine/2d/gw/GwTransportSections.cpp:66-89,249-282 (registration 456-459; resolution 559-562; warning 632-639); src/engine/2d/gw/GwTransportData.hpp:103-117 -->

### Section: [GW_SORPTION] {#engine_manual_sect_GW_SORPTION}

Purpose:
\status{Planned} Gives the sorption partition coefficient and, optionally, the first-order decay of a species in the aquifer, by scope. The rows are parsed, validated, kept and written back unchanged, but no kernel consumes them in this release.

Format:
```
*          Species  Kd  (Decay)
TAG name   Species  Kd  (Decay)
CELL n     Species  Kd  (Decay)
```

Parameters:
name    a cell tag from [2D_TRIANGLES] or [2D_QUADS].
n    1-based unified cell index.
Species    name of a pollutant, of a species of the reactions component, or `__WATER_AGE__` or `__TEMPERATURE__`.
Kd    linear partition coefficient (L per kg); >= 0.
Decay    first-order decay rate (1/day); >= 0. Absent or `-`: a pollutant uses its [POLLUTANTS] decay coefficient.

Remarks:
Scopes resolve `*`, then TAG, then CELL. An unknown species, a tag that matches no cell or a cell beyond the mesh is reported when the model initialises.

At run time the engine emits one warning: "[GW_*] subsurface transport is AUTHORED but INERT this run: the integrated 2D groundwater component (org.hydrocouple.openswmm.integrated2d) provides the two-zone kernel these sections configure and is not available in this release. The rows are validated, kept and written back unchanged."

This section has no counterpart in SWMM 5.

<!-- source: src/engine/2d/gw/GwTransportSections.cpp:66-89,288-309 (registration 460-463; resolution 563-567; warning 632-639); src/engine/2d/gw/GwTransportData.hpp:119-127 -->

### Section: [GW_INITIAL_QUALITY] {#engine_manual_sect_GW_INITIAL_QUALITY}

Purpose:
\status{Planned} Seeds the initial concentration, water age or temperature of a zone of the aquifer column. The rows are parsed, validated, kept and written back unchanged, but no kernel consumes them in this release.

Format:
```
*          SAT        Species  Value
*          UNSAT      Species  Value
*          LAYER j    Species  Value
TAG name   Zone       Species  Value
CELL n     Zone       Species  Value
FILE  Fname
```

Parameters:
name    a cell tag from [2D_TRIANGLES] or [2D_QUADS].
n    1-based unified cell index.
Zone    SAT (the saturated zone), UNSAT (the unsaturated zone) or LAYER j (layer j of a SIGMA column, j >= 1).
Species    name of a pollutant, of a species of the reactions component, or `__WATER_AGE__` or `__TEMPERATURE__`.
Value    initial value: concentration in the species' units, water age in hours, temperature in degrees C. Concentrations must be >= 0; age and temperature may be negative.
Fname    name of a CSV file holding the same tokens per line (scope, zone, species, value).

Remarks:
The FILE form names a sidecar resolved relative to the input file's directory and read when the model initialises. Its fields may be separated by commas, semicolons, tabs or spaces, and a first line that does not parse is taken as a header. A missing file is reported at initialise, as is an unknown species, a tag that matches no cell, a cell beyond the mesh, or a negative concentration.

At run time the engine emits one warning: "[GW_*] subsurface transport is AUTHORED but INERT this run: the integrated 2D groundwater component (org.hydrocouple.openswmm.integrated2d) provides the two-zone kernel these sections configure and is not available in this release. The rows are validated, kept and written back unchanged."

This section has no counterpart in SWMM 5.

<!-- source: src/engine/2d/gw/GwTransportSections.cpp:66-89,123-137,315-352 (registration 464-468; FILE resolution 491-529, checks 568-577; warning 632-639); src/engine/2d/gw/GwTransportData.hpp:129-138,185-186 -->

### Section: [GW_BOUNDARY_QUALITY] {#engine_manual_sect_GW_BOUNDARY_QUALITY}

Purpose:
\status{Planned} Sets the quality of water crossing a lateral boundary edge of the aquifer. The rows are parsed, validated, kept and written back unchanged, but no kernel consumes them in this release.

Format:
```
Cell  Edge  Species  CONC      Value
Cell  Edge  Species  TS        Tseries
Cell  Edge  Species  MASSFLUX  Value
Cell  Edge  Species  HEATFLUX  Value
```

Parameters:
Cell    1-based unified cell index.
Edge    local edge of that cell, 0-based (0 .. 2 for a triangle, 0 .. 3 for a quad).
Species    name of a pollutant, of a species of the reactions component, or `__WATER_AGE__` or `__TEMPERATURE__`.
CONC    the value is a concentration in the species' units (temperature in degrees C).
TS    the value is the name of a [TIMESERIES] of that quantity.
MASSFLUX    the value is a mass flux per metre of edge.
HEATFLUX    the value is a heat flux per metre of edge.
Value    a number, or the name of a [TIMESERIES] in its place.
Tseries    name of a [TIMESERIES].

Remarks:
Exactly five tokens. For CONC, MASSFLUX and HEATFLUX a non-numeric fifth token is taken as a series name. The edge is checked against the cell's own vertex count at initialise, as are the cell index, the species name and any series name.

At run time the engine emits one warning: "[GW_*] subsurface transport is AUTHORED but INERT this run: the integrated 2D groundwater component (org.hydrocouple.openswmm.integrated2d) provides the two-zone kernel these sections configure and is not available in this release. The rows are validated, kept and written back unchanged."

This section has no counterpart in SWMM 5.

<!-- source: src/engine/2d/gw/GwTransportSections.cpp:91-96,358-384 (registration 469-472; resolution 578-593; warning 632-639); src/engine/2d/gw/GwTransportData.hpp:140-149 -->

### Section: [GW_SOURCES] {#engine_manual_sect_GW_SOURCES}

Purpose:
\status{Planned} Declares wells and point sources or sinks in the aquifer, with their flow and the quality of injected water. The rows are parsed, validated, kept and written back unchanged, but no kernel consumes them in this release.

Format:
```
Name  CELL n    FLOW  Flow  (Species  CONC  Value) (Species  MASS  Value) ...
Name  TAG tag   FLOW  Flow  (Species  CONC  Value) ...
Name  XY x y    FLOW  Flow  (Species  CONC  Value) ...
```

Parameters:
Name    name of the source; unique within the section.
n    1-based unified cell index of the cell holding the source.
tag    a cell tag from [2D_TRIANGLES] or [2D_QUADS]; the source applies to every cell carrying it.
x, y    map coordinates of the source; it is placed in the cell whose centroid is nearest.
Flow    flow rate (cu m per s), positive to inject and negative to extract, or the name of a [TIMESERIES] of the flow.
Species    name of a pollutant, of a species of the reactions component, or `__WATER_AGE__` or `__TEMPERATURE__`.
CONC    the term's value is a concentration in the injected water.
MASS    the term's value is a mass rate.
Value    a number, or the name of a [TIMESERIES] in its place.

Remarks:
A row needs at least Name, a location, FLOW and its value; species terms follow in groups of three and may repeat. A duplicate Name is an error at initialise, not a last-wins overwrite; so are a cell beyond the mesh, a tag that matches no cell, an unknown species and an unknown series. The XY form keeps its coordinates for the writer and resolves to a cell at initialise.

At run time the engine emits one warning: "[GW_*] subsurface transport is AUTHORED but INERT this run: the integrated 2D groundwater component (org.hydrocouple.openswmm.integrated2d) provides the two-zone kernel these sections configure and is not available in this release. The rows are validated, kept and written back unchanged."

This section has no counterpart in SWMM 5.

<!-- source: src/engine/2d/gw/GwTransportSections.cpp:91-96,390-444 (registration 473-476; resolution 594-627; warning 632-639); src/engine/2d/gw/GwTransportData.hpp:151-170 -->

## 2.5 Process Component Configuration Sections

The sections in this part of the chapter are **not** read by the input parser's
section registry. Each belongs to a *process component* — a reaction system,
the Eulerian transport engine, the heat module or the water-age tracker — and
is read by that component from the file named in its
`[PROCESS_COMPONENTS]` row (@ref engine_manual_sect_PROCESS_COMPONENTS):

```
[PROCESS_COMPONENTS]
org.hydrocouple.openswmm.reactions       config="model.rxn"
org.hydrocouple.openswmm.transport.ard   config="model.ard"
org.hydrocouple.openswmm.waterage        config="model.age"
org.hydrocouple.openswmm.heat            config="model.heat"
```

**Dispatch.** Every row is `Id key="value" ...`. The first token is the
component id; every further token must be a `key="value"` pair (the quotes are
optional and are stripped), and one that is not is a parse error naming the
component and the token. The key `CONFIG` (case-insensitive) names the
component's configuration file; any other key is passed to the component
unchanged. At open, after the whole `.inp` has been read, the engine resolves
the rows in file order: an id that appears twice is refused; an id that looks
like a shared-library path (contains a path separator or ends in `.so`,
`.dylib` or `.dll`) is refused until HydroCouple library loading arrives
(plan phase HC2); an unknown id is refused with the list of known ids; a
known id whose implementation has not landed is refused with the plan phase
that delivers it; a row without `CONFIG` is refused. Otherwise the file is
read — a relative path resolves against the directory of the `.inp` — and
handed to the component, which parses its own sections and reports every
error it finds. Errors from any component fail the open unless the engine was
opened leniently, in which case they are recorded and the configuration that
failed is discarded wholesale (no component ever half-applies).

**File dialect.** A component file uses the bracketed-section dialect of the
`.inp`: `[SECTION]` headers (case-insensitive), `;` comments, blank lines
ignored, one record per line. Content before the first header is an error, and
a component file may not itself contain a `[PROCESS_COMPONENTS]` section (no
recursion). The conventional extensions are `.rxn` (reactions), `.ard`
(Eulerian transport), `.age` (water age) and `.heat` (heat); the engine does
not check the extension. On save, every implemented component renders its
configuration back to its file from the live model state, and a save-as
carries the file alongside the written `.inp`.

**Embedded form.** The twelve `[REACTION_*]` sections may also be written
directly in the `.inp`. They are captured verbatim by the input parser and
applied after component resolution: when no external reactions component is
registered they take effect with a style warning; when one is registered they
are **ignored** with a warning and the external file wins wholesale. Embedded
sections are not written back into the `.inp` on save — the writer warns that
they are lost from the saved deck — so the external file is the intended
layout. The heat, water-age and transport sections have no embedded form.

**Cross-component references.** `[TRANSPORT_BOUNDARIES]` and
`[TRANSPORT_SOURCES]` rows name species declared in `[REACTION_SPECIES]`, and
the per-element rows of `[RADIATIVE_FLUXES]` and `[SEDIMENT_EXCHANGE]` name
links, nodes and `[TAGS]` values. These are resolved once, after every
component has applied, so the order of the `[PROCESS_COMPONENTS]` rows does not
matter.

| Component id | Configuration file | Sections | Status |
|---|---|---|---|
| `org.hydrocouple.openswmm.reactions` | `model.rxn` (or embedded in the `.inp`) | `[REACTION_OPTIONS]` `[REACTION_SPECIES]` `[REACTION_COEFFICIENTS]` `[REACTION_TERMS]` `[REACTION_PIPES]` `[REACTION_TANKS]` `[REACTION_QUALITY]`; `[REACTION_SOURCES]` `[REACTION_PARAMETERS]` `[REACTION_PATTERNS]` `[REACTION_REPORT]` `[REACTION_SUBCATCHMENTS]` are recognised and refused until their phases | \status{Implemented} |
| `org.hydrocouple.openswmm.transport.ard` | `model.ard` | `[TRANSPORT_OPTIONS]` `[CONDUIT_DISPERSION]` `[TRANSPORT_BOUNDARIES]` `[TRANSPORT_SOURCES]`; `[STORAGE_MIXING]` is refused until phase E2b | \status{Implemented} |
| `org.hydrocouple.openswmm.transport.lard` | none | none — the Lagrangian engine is selected by `[OPTIONS] QUALITY_SOLVER LAGRANGIAN` (alias `LARD`) and reads no component file; registering this id is refused with "arrives with plan phase T5" | \status{Planned} |
| `org.hydrocouple.openswmm.heat` | `model.heat` | `[HEAT_SOURCES]` `[HEAT_FLUXES]` `[RADIATIVE_FLUXES]` `[SOLAR_RADIATION]` `[CLOUD_COVER]` `[SEDIMENT_EXCHANGE]` | \status{Implemented} |
| `org.hydrocouple.openswmm.waterage` | `model.age` | `[WATER_AGE_SOURCES]` | \status{Implemented} |
| `org.hydrocouple.openswmm.integrated2d` | none | none — unified 2D surface and two-zone groundwater component; registering this id is refused with "arrives with plan phase S1/G1" | \status{Planned} |

The four implemented components are registered once, at the first open, so
the id list above is the complete set a `[PROCESS_COMPONENTS]` row may name.

<!-- source: src/engine/input/handlers/ProcessComponentsHandler.cpp:53-79; src/engine/plugins/ProcessComponentRegistry.cpp:63-108,119-140,146-168,201-268; src/engine/plugins/DefaultInputPlugin.cpp:163-170; src/engine/core/SWMMEngine.cpp:353-400; src/engine/core/InpWriter.cpp:3284-3299 -->

### Component section: [REACTION_OPTIONS] {#engine_manual_sect_REACTION_OPTIONS}

Purpose: \status{Implemented}
Sets the integrator, coupling, units, tolerances and reference temperature of
the reaction system.

Format:
```
SOLVER        EUL / RK5 / ROS2 / BDF2
COUPLING      NONE / FULL
RATE_UNITS    SEC / MIN / HR / DAY
AREA_UNITS    FT2 / M2 / CM2
TIMESTEP      value
ATOL          value
RTOL          value
TEMPERATURE   value
```

Parameters:

| Parameter | Meaning | Units / values | Default |
|---|---|---|---|
| `SOLVER` | Integrator for the RATE species. RK5 is explicit and cheapest for non-stiff kinetics; ROS2 and BDF2 are implicit and are the remedy when RK5 reports its substep cap. | EUL, RK5, ROS2 or BDF2 | RK5 |
| `COUPLING` | NONE integrates one RATE species at a time with the others frozen at their start-of-step values (EPANET-MSX semantics); FULL integrates all RATE species simultaneously. A system with a single RATE species is always coupled. | NONE or FULL | NONE |
| `RATE_UNITS` | Time unit of every RATE expression. The evaluated rate is multiplied by 1, 1/60, 1/3600 or 1/86400 to give a per-second rate. | SEC, MIN, HR or DAY | HR |
| `AREA_UNITS` | Area unit recorded for wall-species expressions. | FT2, M2 or CM2 | FT2 |
| `TIMESTEP` | Reaction step; 0 means follow the quality step. | value, 0 or greater | 0 |
| `ATOL` | Absolute tolerance of the integrator, used for every species that does not carry its own in `[REACTION_SPECIES]`. | value, greater than 0 | 1.0e-6 |
| `RTOL` | Relative tolerance of the integrator, likewise. | value, greater than 0 | 1.0e-4 |
| `TEMPERATURE` | Value of the expression variable `TEMP` for an element that has no heat-transport temperature (`HEAT_TRANSPORT OFF`, or before the heat state is seeded). | degrees C, any finite value | 20 |

Remarks:
Keys are case-insensitive and may appear in any order; a line with fewer than
two tokens is skipped and an unrecognised key is an error. A bad value is an
error naming the key. `TEMPERATURE` has no range gate — any finite number is
accepted. `AREA_UNITS` and `TIMESTEP` are parsed, stored and written back on
save; see the audit note at the end of this part for their consumers.

<!-- source: src/engine/transport/components/ReactionModule/ReactionsComponent.cpp:82-135,396,418; src/engine/data/ReactionData.hpp:52-55,87-97; src/engine/transport/components/ReactionModule/ReactionIntegrator.cpp:58-65,101-103,239 -->

### Component section: [REACTION_SPECIES] {#engine_manual_sect_REACTION_SPECIES}

Purpose: \status{Implemented}
Declares the species of the reaction system. At least one species is required.

Format:
```
BULK / WALL   Name   Units   (Atol   Rtol)
```

Parameters:

| Parameter | Meaning | Units / values | Default |
|---|---|---|---|
| Kind | BULK: a dissolved species carried with the water. WALL: a species attached to the wetted wall. | BULK or WALL | required |
| `Name` | Species name, unique among species, coefficients, terms, `[POLLUTANTS]` names and the reserved names. | text | required |
| `Units` | Concentration unit label recorded with the species (for example MG or UG). It is carried to the species registry and the reports; no conversion is applied. | text | required |
| `Atol` | Absolute tolerance for this species; 0 means use the global `ATOL`. | value | 0 |
| `Rtol` | Relative tolerance for this species; 0 means use the global `RTOL`. | value | 0 |

Remarks:
A row with fewer than three tokens, a kind other than BULK or WALL, a
duplicate species name, or a name that collides with an existing pollutant or
reserved species is an error. A configuration that declares no species is
refused as a whole. Species are entered into the engine's species registry
only after the complete file has validated and every expression has compiled,
so a rejected file leaves no species behind.

<!-- source: src/engine/transport/components/ReactionModule/ReactionsComponent.cpp:137-182,396,420-426,495-506; src/engine/data/ReactionData.hpp:101-106 -->

### Component section: [REACTION_COEFFICIENTS] {#engine_manual_sect_REACTION_COEFFICIENTS}

Purpose: \status{Implemented}
Declares named numeric coefficients that expressions may reference.

Format:
```
PARAMETER / CONSTANT   Name   Value
```

Parameters:

| Parameter | Meaning | Units / values | Default |
|---|---|---|---|
| Kind | PARAMETER: a coefficient that may later be overridden per element. CONSTANT: a fixed value. | PARAMETER or CONSTANT | required |
| `Name` | Coefficient name, unique among coefficients and species. | text | required |
| `Value` | Numeric value. | value | required |

Remarks:
A row with fewer than three tokens, a kind other than PARAMETER or CONSTANT, a
name that duplicates a coefficient or species, or a non-numeric value is an
error. The kind keyword must be spelled in full — `PARAM` is not accepted.
Both kinds evaluate identically in this release; the per-element override
table that would distinguish them is `[REACTION_PARAMETERS]`, which is not yet
supported.

<!-- source: src/engine/transport/components/ReactionModule/ReactionsComponent.cpp:184-217,397,427; src/engine/data/ReactionData.hpp:108-110 -->

### Component section: [REACTION_TERMS] {#engine_manual_sect_REACTION_TERMS}

Purpose: \status{Implemented}
Declares named intermediate expressions that pipe and tank expressions may
reference, so a closure is defined once rather than copied.

Format:
```
Name   Expression
```

Parameters:

| Parameter | Meaning | Units / values | Default |
|---|---|---|---|
| `Name` | Term name, unique among terms, species and coefficients. | text | required |
| `Expression` | The remainder of the row, taken verbatim, including any unquoted commas. A remainder wrapped in double quotes is unwrapped. | expression | required |

Remarks:
A row with fewer than two tokens or a duplicate name is an error. Terms are
compiled in file order and a term may reference only the terms declared before
it (forward references are compile errors, reported with the term name and
column). Expressions may reference species, coefficients, earlier terms, the
hydraulic variables listed under `[REACTION_PIPES]`, and `[POLLUTANTS]` names
read-only.

<!-- source: src/engine/transport/components/ReactionModule/ReactionsComponent.cpp:50-74,219-239,397,429,594-599 -->

### Component section: [REACTION_PIPES] {#engine_manual_sect_REACTION_PIPES}

Purpose: \status{Implemented}
Gives each species its kinetic expression in the pipe (conduit) scope.

Format:
```
RATE / EQUIL / FORMULA   Species   Expression
```

Parameters:

| Parameter | Meaning | Units / values | Default |
|---|---|---|---|
| Form | RATE: the expression is the time derivative of the species, in `RATE_UNITS`. EQUIL: the expression is an algebraic residual driven to zero. FORMULA: the expression is the species value itself. | RATE, EQUIL or FORMULA | required |
| `Species` | A species declared in `[REACTION_SPECIES]`. | text | required |
| `Expression` | The remainder of the row, taken verbatim (a double-quoted remainder is unwrapped). | expression | required |

Remarks:
A row with fewer than three tokens, an unknown form, an undeclared species, or
a second expression for the same species in this scope is an error. Naming a
`[POLLUTANTS]` row here is refused: pollutant kinetics arrive with plan phase
R4b, and until then pollutants may only be referenced read-only inside
expressions (their `[POLLUTANTS]` decay coefficient applies under every
engine). A species with no row in this scope has no reaction in pipes. The
hydraulic variables available to every expression, in the engine's internal
units, are `D` depth (ft), `Q` flow (cfs), `U` velocity (ft/s), `RE` Reynolds
number, `US` shear velocity (ft/s), `FF` Darcy-Weisbach friction factor, `AV`
wetted surface area per volume (1/ft), `HRT` hydraulic residence time (s), `DT`
reaction step (s) and `TEMP` water temperature (degrees C, falling back to
`[REACTION_OPTIONS] TEMPERATURE` when heat transport is off). Variable names
are case-insensitive. If the section is absent every species is given no pipe
expression.

<!-- source: src/engine/transport/components/ReactionModule/ReactionsComponent.cpp:241-295,398,431-439; src/engine/transport/components/ReactionModule/ReactionExpression.cpp:125-137,146-150 -->

### Component section: [REACTION_TANKS] {#engine_manual_sect_REACTION_TANKS}

Purpose: \status{Implemented}
Gives each species its kinetic expression in the tank (storage node) scope.

Format:
```
RATE / EQUIL / FORMULA   Species   Expression
```

Parameters:

| Parameter | Meaning | Units / values | Default |
|---|---|---|---|
| Form | As for `[REACTION_PIPES]`. | RATE, EQUIL or FORMULA | required |
| `Species` | A species declared in `[REACTION_SPECIES]`. | text | required |
| `Expression` | The remainder of the row, taken verbatim. | expression | required |

Remarks:
The row grammar, validation rules and available variables are exactly those
of `[REACTION_PIPES]`; the two sections share one parser and differ only in
the scope they fill. A species may have a different form in each scope (for
example a wall-demand term in pipes and none in tanks), and one expression per
species per scope is the limit. If the section is absent every species is
given no tank expression.

<!-- source: src/engine/transport/components/ReactionModule/ReactionsComponent.cpp:241-295,398,440-448 -->

### Component section: [REACTION_QUALITY] {#engine_manual_sect_REACTION_QUALITY}

Purpose: \status{Implemented}
Sets the initial concentration of each species, globally and per element.

Format:
```
GLOBAL   Species   Value
NODE     Node      Species   Value
LINK     Link      Species   Value
```

Parameters:

| Parameter | Meaning | Units / values | Default |
|---|---|---|---|
| Scope | GLOBAL applies to every element; NODE and LINK apply to one named element and win over GLOBAL there. | GLOBAL, NODE or LINK | required |
| `Node`, `Link` | Name of an existing node or link (NODE and LINK rows only). | text | required |
| `Species` | A species declared in `[REACTION_SPECIES]`. | text | required |
| `Value` | Initial concentration in the species' own units. | value, 0 or greater | 0 |

Remarks:
A scope other than GLOBAL, NODE or LINK, a row short of its fields, an unknown
element, an undeclared species, a negative value, or a second NODE or LINK row
for the same element and species is an error. Every species starts at 0 when
it has no GLOBAL row, and when the section is absent altogether. Element names
are resolved at open, after the full `.inp` has been read. Rows of
`[INITIAL_QUALITY]` (@ref engine_manual_sect_INITIAL_QUALITY) that name a
reaction species are mirrored into this table, so an element may be seeded
from either place; the same element and species in both is a duplicate error,
and on save a mirrored row is written once, in `[INITIAL_QUALITY]`. The name
REACTION_INITIAL that appears in some older prose is not recognised and is
refused as an unknown section.

<!-- source: src/engine/transport/components/ReactionModule/ReactionsComponent.cpp:297-390,399,449-453; src/engine/transport/MsxInitialQuality.hpp:17-40 -->

### Component section: [REACTION_SOURCES] {#engine_manual_sect_REACTION_SOURCES}

Purpose: \status{Planned}
Reserved for per-element species sources of the reaction system. The tag is
recognised so that a typo cannot hide behind it, but no row is parsed.

Format:
```
[REACTION_SOURCES]
;; no row grammar is defined in this release
```

Parameters:
None. No row is read.

Remarks:
Presence of the section in a `.rxn` file, or embedded in the `.inp`, is an
error — "recognised but not yet supported — arrives with plan phase
R-sources (post-R3)" — and the whole reactions configuration is rejected.
Until it lands, species mass enters through the transport component's
`[TRANSPORT_BOUNDARIES]` and `[TRANSPORT_SOURCES]`.

<!-- source: src/engine/transport/components/ReactionModule/ReactionsComponent.cpp:399,455-468 -->

### Component section: [REACTION_PARAMETERS] {#engine_manual_sect_REACTION_PARAMETERS}

Purpose: \status{Planned}
Reserved for per-element overrides of PARAMETER coefficients. The tag is
recognised; no row is parsed.

Format:
```
[REACTION_PARAMETERS]
;; no row grammar is defined in this release
```

Parameters:
None. No row is read.

Remarks:
Presence of the section is an error — "arrives with plan phase R-parameters
(post-R3)" — and the whole reactions configuration is rejected. Until it
lands, a `[REACTION_COEFFICIENTS]` PARAMETER behaves exactly like a CONSTANT.

<!-- source: src/engine/transport/components/ReactionModule/ReactionsComponent.cpp:400,455-468 -->

### Component section: [REACTION_PATTERNS] {#engine_manual_sect_REACTION_PATTERNS}

Purpose: \status{Planned}
Reserved for time patterns applied to reaction sources. The tag is
recognised; no row is parsed.

Format:
```
[REACTION_PATTERNS]
;; no row grammar is defined in this release
```

Parameters:
None. No row is read.

Remarks:
Presence of the section is an error — "arrives with plan phase R-sources
(post-R3)" — and the whole reactions configuration is rejected. A
time-varying boundary or source is available today as a `TIMESERIES` row of
`[TRANSPORT_BOUNDARIES]` or `[TRANSPORT_SOURCES]`.

<!-- source: src/engine/transport/components/ReactionModule/ReactionsComponent.cpp:400,455-468 -->

### Component section: [REACTION_REPORT] {#engine_manual_sect_REACTION_REPORT}

Purpose: \status{Planned}
Reserved for reaction-system reporting options. The tag is recognised; no row
is parsed.

Format:
```
[REACTION_REPORT]
;; no row grammar is defined in this release
```

Parameters:
None. No row is read.

Remarks:
Presence of the section is an error — "arrives with plan phase R5" — and the
whole reactions configuration is rejected. Species results are written with
the pollutant results under the ordinary `[REPORT]` options.

<!-- source: src/engine/transport/components/ReactionModule/ReactionsComponent.cpp:401,455-468 -->

### Component section: [REACTION_SUBCATCHMENTS] {#engine_manual_sect_REACTION_SUBCATCHMENTS}

Purpose: \status{Planned}
Reserved for reaction expressions in the subcatchment (runoff) scope. The tag
is recognised; no row is parsed.

Format:
```
[REACTION_SUBCATCHMENTS]
;; no row grammar is defined in this release
```

Parameters:
None. No row is read.

Remarks:
Presence of the section is an error — "arrives with plan phase R6" — and the
whole reactions configuration is rejected. Reactions run in the pipe and tank
scopes only.

<!-- source: src/engine/transport/components/ReactionModule/ReactionsComponent.cpp:401,455-468 -->

### Component section: [HEAT_SOURCES] {#engine_manual_sect_HEAT_SOURCES}

Purpose: \status{Implemented}
Sets the temperature carried by water entering the network through each
source pathway, globally and per node.

Format:
```
Source   GLOBAL   Temperature
Source   NODE     Node   Temperature
```

Parameters:

| Parameter | Meaning | Units / values | Default |
|---|---|---|---|
| `Source` | The inflow pathway: RAINFALL (runoff), DWF (dry-weather inflow), GW (groundwater), RDII, EXTERNAL_INFLOW (`[INFLOWS]` rows), IFACE (interface-file inflow) or INITIAL_STATE (water in the network at the start). | keyword | required |
| Scope | GLOBAL sets the pathway everywhere; NODE sets it at one node and wins there. | GLOBAL or NODE | required |
| `Node` | Name of an existing node (NODE rows only). | text | required |
| `Temperature` | Inlet temperature. | degrees C, from -50 to 100 | 20 |

Remarks:
Every pathway without a row enters at 20 degrees C. NODE scope is available for
DWF and EXTERNAL_INFLOW only; any other source takes GLOBAL. A row with fewer
than three tokens, an unknown source, an unknown node, a temperature outside
the range, or a second NODE row for the same source and node is an error.
SUBCATCH scope ("arrives with plan phase H5") and EDGE_BC scope ("arrives with
phase T6") are refused, and a `TIMESERIES` in place of the value is refused —
this table takes constant temperatures. Any error discards the whole heat
configuration. The component warns when `[OPTIONS] HEAT_TRANSPORT` is OFF,
because then no temperature is tracked.

<!-- source: src/engine/transport/components/HeatModule/HeatComponent.cpp:49-50,93-108,877-989,1079-1084; src/engine/data/HeatData.hpp:70-81,379-388 -->

### Component section: [HEAT_FLUXES] {#engine_manual_sect_HEAT_FLUXES}

Purpose: \status{Implemented}
Switches the energy-flux modules on or off and chooses what a dry element
reports. With every module off, temperature is carried and mixed as a
conservative tracer.

Format:
```
SURFACE_EXCHANGE          ON / OFF
RADIATIVE_EXCHANGE        ON / OFF
LAYER_CONDUCTION          ON / OFF
SEDIMENT_EXCHANGE         ON / OFF
DRY_ELEMENT_TEMPERATURE   HOLD / AIR / DEFAULT
```

Parameters:

| Parameter | Meaning | Units / values | Default |
|---|---|---|---|
| `SURFACE_EXCHANGE` | Latent and sensible exchange at the free surface, driven by the `[TEMPERATURE]` climate data. | ON or OFF | OFF |
| `RADIATIVE_EXCHANGE` | Shortwave and longwave exchange, parameterised by `[RADIATIVE_FLUXES]`, `[SOLAR_RADIATION]` and `[CLOUD_COVER]`. | ON or OFF | OFF |
| `LAYER_CONDUCTION` | Vertical conduction between LID layers. Its material constants are built in; no section configures them. | ON or OFF | OFF |
| `SEDIMENT_EXCHANGE` | The bed zone beneath conduits — conduction to the bed and deep ground plus hyporheic exchange — parameterised by `[SEDIMENT_EXCHANGE]`. | ON or OFF | OFF |
| `DRY_ELEMENT_TEMPERATURE` | What a dry element reports: HOLD freezes the last wet value; AIR tracks air temperature; DEFAULT falls to 20 degrees C. | HOLD, AIR or DEFAULT | HOLD |

Remarks:
Each row is exactly two tokens; `YES` and `NO` are accepted for `ON` and
`OFF`. An unknown module name, a toggle value other than these four, or a
`DRY_ELEMENT_TEMPERATURE` value other than the three policies is an error.
Every module defaults OFF so that a deck gets pure transport unless it asks
for physics. The parameter sections may be present while their module is OFF;
they are then parsed and validated but have no effect. `SEDIMENT_EXCHANGE`
here is the toggle; the section of the same name holds its parameters.

<!-- source: src/engine/transport/components/HeatModule/HeatComponent.cpp:818-876; src/engine/data/HeatData.hpp:103-113,321-358 -->

### Component section: [RADIATIVE_FLUXES] {#engine_manual_sect_RADIATIVE_FLUXES}

Purpose: \status{Implemented}
Supplies the incoming shortwave resource and the optical properties of the
water surface and its surroundings for `RADIATIVE_EXCHANGE`.

Format:
```
SHORTWAVE            GLOBAL   Value
SHORTWAVE            GLOBAL   TIMESERIES   Name
SHORTWAVE            GLOBAL   COMPUTED
ATM_EMISS_COEFF      GLOBAL   Value
ATM_LW_REFLECTION    GLOBAL   Value
Parameter            GLOBAL   Value
Parameter            TAG / LINK / NODE   Name   Value
```

Parameters:

| Parameter | Meaning | Units / values | Default |
|---|---|---|---|
| `SHORTWAVE` | Incoming solar radiation: a constant, a `[TIMESERIES]` of measured values, or COMPUTED from solar position and a clear-sky atmosphere using `[SOLAR_RADIATION]`. GLOBAL only. | W/m2, 0 or greater; or TIMESERIES name; or COMPUTED | 0 (constant) |
| `ALBEDO` | Shortwave reflectance of the water surface. | fraction 0 to 1 | 0 |
| `SHADE_FACTOR` | Fraction of insolation blocked by shading. | fraction 0 to 1 | 0 |
| `SKY_VIEW` | Fraction of the hemisphere above the water that is sky rather than land cover. | fraction 0 to 1 | 1 |
| `EMISS_WATER` | Longwave emissivity of the water surface. | fraction 0 to 1 | 0.97 |
| `EMISS_LANDCOVER` | Longwave emissivity of the surrounding land cover. | fraction 0 to 1 | 0.97 |
| `LANDCOVER_TEMPERATURE` | Radiating temperature of the land cover. When unset, air temperature is used. | degrees C, any finite value | air temperature |
| `ATM_EMISS_COEFF` | Brunt clear-sky atmospheric emissivity coefficient. GLOBAL only. | fraction 0 to 1 | 0.5 |
| `ATM_LW_REFLECTION` | Longwave reflectance of the water surface. GLOBAL only. | fraction 0 to 1 | 0.03 |

Remarks:
Every row carries a scope token. `SHORTWAVE`, `ATM_EMISS_COEFF` and
`ATM_LW_REFLECTION` describe the incident resource or the atmosphere and are
refused at any scope but GLOBAL; the six surface parameters accept GLOBAL,
TAG, LINK or NODE, where TAG matches every link and node carrying that value in
`[TAGS]` (@ref engine_manual_sect_TAGS). Precedence is GLOBAL, then TAG, then
the named element. A TAG that matches nothing, an unknown link or node, or two
rows for the same parameter at the same scope and name are errors. The three
`SHORTWAVE` spellings are mutually exclusive: a second `SHORTWAVE` row is an
error, not an override. `COMPUTED` requires `[SOLAR_RADIATION]` `LATITUDE` and
`LONGITUDE` (they are never taken from the `[TEMPERATURE]` snowmelt line) and
takes no further token; `TIMESERIES` names an existing `[TIMESERIES]`. Fractions
are refused outside 0 to 1 rather than clamped, and a non-finite value is
refused. The section has no effect unless `[HEAT_FLUXES] RADIATIVE_EXCHANGE`
is ON.

<!-- source: src/engine/transport/components/HeatModule/HeatComponent.cpp:145-258,263-280,313-472,1004-1012; src/engine/transport/components/HeatFluxModules/HeatOverrides.cpp:85-175; src/engine/data/HeatData.hpp:152-156,243-277 -->

### Component section: [SOLAR_RADIATION] {#engine_manual_sect_SOLAR_RADIATION}

Purpose: \status{Implemented}
Gives the site position and the clear-sky atmosphere used when
`[RADIATIVE_FLUXES] SHORTWAVE` is `COMPUTED`.

Format:
```
LATITUDE        GLOBAL   Value
LONGITUDE       GLOBAL   Value
TIMEZONE        GLOBAL   Value
ELEVATION       GLOBAL   Value
TURBIDITY_380   GLOBAL   Value
TURBIDITY_500   GLOBAL   Value
PRECIP_WATER    GLOBAL   Value
OZONE           GLOBAL   Value
GROUND_ALBEDO   GLOBAL   Value
```

Parameters:

| Parameter | Meaning | Units / values | Default |
|---|---|---|---|
| `LATITUDE` | Site latitude, positive north. Required under COMPUTED. | degrees, -90 to 90 | none |
| `LONGITUDE` | Site longitude, positive east. Required under COMPUTED. | degrees, -180 to 180 | none |
| `TIMEZONE` | Offset of the deck's clock from UTC, positive east (for example -7 for MST). | hours, -14 to 14 | 0 (UTC) |
| `ELEVATION` | Site elevation for the atmospheric pressure term. When unset the `[TEMPERATURE]` climate elevation is used. | metres, -500 to 9000 | climate elevation |
| `TURBIDITY_380` | Aerosol optical depth at 380 nm. | value, 0 or greater | 0.30 |
| `TURBIDITY_500` | Aerosol optical depth at 500 nm. | value, 0 or greater | 0.20 |
| `PRECIP_WATER` | Precipitable water vapour column. | cm, 0 or greater | 1.42 |
| `OZONE` | Ozone column at NTP. | cm, 0 or greater | 0.34 |
| `GROUND_ALBEDO` | Albedo of the ground around the site, for the sky-ground multiple-reflection term. This is not the water's `ALBEDO`. | fraction 0 to 1 | 0.20 |

Remarks:
Every row is exactly `Parameter GLOBAL Value`; there is no per-element scope.
Ranges are refused rather than clamped (a latitude of 100 is a typo, not the
pole), and a non-finite value is refused. The atmosphere defaults are the
Bird and Hulstrom standard atmosphere. The section is consulted only under
`SHORTWAVE GLOBAL COMPUTED`: when coordinates are given under another
`SHORTWAVE` spelling the component warns that they are unused, and when
COMPUTED is in force without a `TIMEZONE` it warns that local time is taken as
UTC, which can shift the computed solar day by up to 12 hours.

<!-- source: src/engine/transport/components/HeatModule/HeatComponent.cpp:476-561,1013-1022,1053-1060; src/engine/data/HeatData.hpp:174-208 -->

### Component section: [CLOUD_COVER] {#engine_manual_sect_CLOUD_COVER}

Purpose: \status{Implemented}
Gives one cloud fraction that attenuates the shortwave resource and raises the
atmospheric longwave emissivity, with the coefficients of both corrections.

Format:
```
FRACTION     GLOBAL   Value
FRACTION     GLOBAL   TIMESERIES   Name
SW_ATTEN_K   GLOBAL   Value
SW_ATTEN_N   GLOBAL   Value
LW_CLOUD_K   GLOBAL   Value
```

Parameters:

| Parameter | Meaning | Units / values | Default |
|---|---|---|---|
| `FRACTION` | Cloud fraction, constant or from a `[TIMESERIES]`. | fraction 0 to 1; or TIMESERIES name | 0 (clear sky) |
| `SW_ATTEN_K` | Kasten-Czeplak shortwave attenuation coefficient k. | value, 0 or greater | 0.75 |
| `SW_ATTEN_N` | Kasten-Czeplak shortwave attenuation exponent n. | value, 0 or greater | 3.4 |
| `LW_CLOUD_K` | Bolz longwave cloud coefficient. | value, 0 or greater | 0.17 |

Remarks:
Every row is `Parameter GLOBAL Value`; a constant `FRACTION` outside 0 to 1 is
refused (it is a fraction, not a percent), while a `TIMESERIES` value is
clamped at run time. An unknown parameter, a negative coefficient or a
non-finite value is an error. Both corrections are the identity at a fraction
of 0, so a section holding coefficients but no `FRACTION` row has no effect
and the component warns. It also warns when `[CLOUD_COVER]` is combined with
`SHORTWAVE GLOBAL TIMESERIES`, because a measured record already contains its
clouds and would be attenuated twice; the longwave correction still applies.
The section has no effect unless `RADIATIVE_EXCHANGE` is ON.

<!-- source: src/engine/transport/components/HeatModule/HeatComponent.cpp:714-816,1028-1046; src/engine/data/HeatData.hpp:223-232 -->

### Component section: [SEDIMENT_EXCHANGE] {#engine_manual_sect_SEDIMENT_EXCHANGE}

Purpose: \status{Implemented}
Parameterises the bed zone beneath conduits for `[HEAT_FLUXES]
SEDIMENT_EXCHANGE`: conduction to the bed and the deep ground, and hyporheic
exchange of heat and dissolved species.

Format:
```
Parameter             GLOBAL   Value
Parameter             TAG / LINK   Name   Value
GROUND_TEMPERATURE    GLOBAL   TIMESERIES   Name
INITIAL_TEMPERATURE   GLOBAL   Value
```

Parameters:

| Parameter | Meaning | Units / values | Default |
|---|---|---|---|
| `THERMAL_DIFFUSIVITY` | Bed thermal diffusivity. | m2/s, greater than 0 | 1.0e-6 |
| `SOLUTE_DIFFUSIVITY` | Bed effective solute diffusivity, one value for every species; 0 selects advection-only exchange. | m2/s, 0 or greater | 1.0e-9 |
| `BED_THICKNESS` | Thickness of the bed layer — the conduction length and the bed's heat capacity. | m, greater than 0 | 0.20 |
| `GROUND_DEPTH` | Depth from the bed to the deep-ground boundary. | m, greater than 0 | 2.0 |
| `GROUND_TEMPERATURE` | Deep-ground temperature, constant or (GLOBAL only) from a `[TIMESERIES]`. | degrees C, any finite value; or TIMESERIES name | 12.0 |
| `HYPORHEIC_VELOCITY` | Exchange velocity across the bed interface; 0 means conduction only. Direction follows the gradient, so a negative value is refused. | m/s, 0 or greater | 0 |
| `SEDIMENT_DENSITY` | Bed bulk density. | kg/m3, greater than 0 | 1670 |
| `SEDIMENT_SPECIFIC_HEAT` | Bed specific heat. | J/kg/K, greater than 0 | 1807 |
| `INITIAL_TEMPERATURE` | Initial bed temperature. GLOBAL only. | degrees C, any finite value | the ground temperature |

Remarks:
The bed exists beneath conduits only, so NODE scope is refused; the first
eight parameters accept GLOBAL, TAG or LINK, with TAG matching every link
carrying that value in `[TAGS]` and precedence GLOBAL, then TAG, then LINK.
`INITIAL_TEMPERATURE` has no per-element form. Lengths, diffusivities and
material properties are refused at or below zero rather than clamped, because
each is a divisor or a capacity in the exchange kernel. A TAG that matches
nothing, an unknown link, a duplicate row at the same scope and name, or a
non-finite value is an error. `GROUND_TEMPERATURE` has a default but is worth
stating, since the ground term is often the largest in a buried pipe. Under
the Eulerian engine every transport cell carries its own bed slice; under the
other engines the bed exchanges with the link's mean temperature. The section
has no effect unless `[HEAT_FLUXES] SEDIMENT_EXCHANGE` is ON.

<!-- source: src/engine/transport/components/HeatModule/HeatComponent.cpp:159-166,570-712; src/engine/data/BedZoneData.hpp:88-146 -->

### Component section: [WATER_AGE_SOURCES] {#engine_manual_sect_WATER_AGE_SOURCES}

Purpose: \status{Implemented}
Sets the age carried by water entering the network through each source
pathway, globally and per node, for `[OPTIONS] WATER_AGE ON`.

Format:
```
Source   GLOBAL   Hours
Source   NODE     Node   Hours
```

Parameters:

| Parameter | Meaning | Units / values | Default |
|---|---|---|---|
| `Source` | The inflow pathway: RAINFALL (runoff), DWF, GW, RDII, EXTERNAL_INFLOW (`[INFLOWS]` rows), IFACE (interface-file inflow) or INITIAL_STATE (water in the network at the start). | keyword | required |
| Scope | GLOBAL sets the pathway everywhere; NODE sets it at one node and wins there. | GLOBAL or NODE | required |
| `Node` | Name of an existing node (NODE rows only). | text | required |
| `Hours` | Age of the arriving water. A negative age extracts age-volume (the receiving water reads younger) and is warned; extraction clamps so age never falls below zero. | hours, any number | 0 (fresh) |

Remarks:
Ages are entered in hours and held internally in seconds. Every pathway
without a row enters fresh. NODE scope is available for DWF and
EXTERNAL_INFLOW only; any other source takes GLOBAL. A row with fewer than
three tokens, an unknown source, an unknown node, a non-numeric age, or a
second NODE row for the same source and node is an error, as is any section
in the file other than this one. SUBCATCH scope ("arrives with plan phase A3")
and EDGE_BC scope ("arrives with phase T6") are refused. A `TIMESERIES` in
place of the value is refused with a redirect: a time-varying inflow age is
written as an `[INFLOWS]` row naming the reserved constituent `__WATER_AGE__`
(@ref engine_manual_sect_INFLOWS), and such a row wins over this table's
EXTERNAL_INFLOW entry at that node. Any error discards the whole water-age
configuration. The component warns when `[OPTIONS] WATER_AGE` is OFF, because
then no age is tracked. A zero-valued GLOBAL row is not written back on save,
since zero is the default.

<!-- source: src/engine/transport/components/WaterAgeModule/WaterAgeComponent.cpp:65-72,99-108,110-261,265-296; src/engine/data/WaterAgeData.hpp:50-61,72-91,167-173 -->

### Component section: [TRANSPORT_OPTIONS] {#engine_manual_sect_TRANSPORT_OPTIONS}

Purpose: \status{Implemented}
Configures the Eulerian advection-reaction-dispersion engine selected by
`[OPTIONS] QUALITY_SOLVER EULERIAN_ARD`: the dispersion model, the scalar
advection scheme, the transport mesh and a per-cell diagnostic output.

Format:
```
DISPERSION        OFF / FISCHER / Value
SCALAR_SCHEME     UPWIND / MUSCL / QUICKEST_ULTIMATE
LIMITER           MINMOD / VANLEER / SUPERBEE
TARGET_DX         Value
DETAILED_OUTPUT   Path
```

Parameters:

| Parameter | Meaning | Units / values | Default |
|---|---|---|---|
| `DISPERSION` | Global longitudinal dispersion model. OFF applies none; FISCHER computes \f$D = 0.011\,v^{2} B^{2} / (Y\,U_{*})\f$ with \f$U_{*} = \sqrt{g Y S}\f$ per cell; a number applies one uniform coefficient. | OFF, FISCHER, or a coefficient in display units of length squared per second (m2/s under SI flow units, ft2/s under US), 0 or greater | OFF |
| `SCALAR_SCHEME` | Face reconstruction of the transported scalars. An alias of `[OPTIONS] FV_SCALAR_SCHEME`; this file wins because the component applies after the options. | UPWIND, MUSCL or QUICKEST_ULTIMATE | MUSCL |
| `LIMITER` | Slope limiter for the higher-order schemes. An alias of `[OPTIONS] FV_LIMITER`. | MINMOD, VANLEER or SUPERBEE | MINMOD |
| `TARGET_DX` | Transport-mesh cell length under non-FV hydraulic routing. Under `FLOW_ROUTING FV` the solver mesh governs and this key is ignored with a warning. | display length units, greater than 0 | 0 (the `FV_CELL_LENGTH` rules) |
| `DETAILED_OUTPUT` | Path of a per-cell CSV sidecar written during the run. A relative path resolves against the directory of the component file, not the `.inp`. | path | none |

Remarks:
Keys are case-insensitive and each takes exactly one argument; an unknown key
or a malformed value is an error, and any error discards the whole transport
configuration. Per-conduit dispersion overrides are given in the companion
section `[CONDUIT_DISPERSION]` of the same file, one `Conduit Value` row each
(non-conduit links and duplicate rows are refused); an override wins over the
global model in its conduit, including when `DISPERSION` is OFF. A
`[STORAGE_MIXING]` section is recognised and refused ("arrives with plan phase
E2b"). Because `SCALAR_SCHEME` and `LIMITER` write through to the `[OPTIONS]`
values and the `.inp` writer emits `FV_*` keys only under `FLOW_ROUTING FV`,
this file is the carrier of those choices across a save on a non-FV deck; a
file that never spelled the alias never gains one. `[OPTIONS] FV_DISPERSION`
does not configure this engine and is warned about when set. The component
warns at open whenever its content can reach nothing: `IGNORE_QUALITY YES`, a
`QUALITY_SOLVER` other than `EULERIAN_ARD`, or dispersion configured with no
`[POLLUTANTS]` and no reactions component.

<!-- source: src/engine/transport/components/EulerianArdComponent/ArdConfig.cpp:75-203,204-246,295-305,308-353,358-367; src/engine/data/ArdConfigData.hpp:81-86,97-104,133-153; src/engine/hydraulics/fv/FvOptions.hpp:177-178 -->

### Component section: [CONDUIT_DISPERSION] {#engine_manual_sect_CONDUIT_DISPERSION}

Purpose:
\status{Implemented} Overrides, for individual conduits, the longitudinal
dispersion coefficient the Eulerian ARD engine takes from
`[TRANSPORT_OPTIONS] DISPERSION`.

Format:
```
Conduit  Value
```

Parameters:

| Parameter | Meaning | Units / values | Default |
|---|---|---|---|
| Conduit | Name of an existing conduit. A pump, orifice, weir or outlet is refused: structures are zero-volume pass-throughs and carry no dispersion. | link name | — |
| Value | Longitudinal dispersion coefficient for that conduit. | project length squared per second, finite and non-negative | the `[TRANSPORT_OPTIONS] DISPERSION` value |

Remarks:
One row per conduit; a second row for the same conduit is an error. Rows are
validated when the component configuration is applied, so an unknown link
name fails the open (or, under a lenient open, discards the whole
configuration). See quality §7.3.3 for how the coefficient enters the
implicit dispersion update.

<!-- source: src/engine/transport/components/EulerianArdComponent/ArdConfig.cpp:204-245 -->

### Component section: [STORAGE_MIXING] {#engine_manual_sect_STORAGE_MIXING}

Purpose:
\status{Planned} Per-storage-node mixing models for the Eulerian ARD engine.
The section name is recognised by the parser, and any content under it is
refused with an error naming the plan phase that delivers it (E2b); leave the
section out of a configuration file.

Format:
```
(no rows are accepted this release)
```

Remarks:
Storage nodes are complete-mix reactors under every transport engine in this
release (quality §7.3). The section is listed here so that a file carrying it
is understood rather than mistaken for a typo.

<!-- source: src/engine/transport/components/EulerianArdComponent/ArdConfig.cpp:295-302 -->

### Component section: [TRANSPORT_BOUNDARIES] {#engine_manual_sect_TRANSPORT_BOUNDARIES}

Purpose: \status{Implemented}
Sets the concentration of a reaction species carried by the external inflow
water entering at a node, under the Eulerian engine.

Format:
```
Node   Species   VALUE        Concentration
Node   Species   TIMESERIES   Name
```

Parameters:

| Parameter | Meaning | Units / values | Default |
|---|---|---|---|
| `Node` | Name of an existing node. | text | required |
| `Species` | A species declared in the reactions component's `[REACTION_SPECIES]`. | text | required |
| Mode | VALUE gives a constant; TIMESERIES names a `[TIMESERIES]` of concentrations. | VALUE or TIMESERIES | required |
| `Concentration`, `Name` | The constant concentration in the species' own units, or the series name. | value, 0 or greater; or text | required |

Remarks:
Every row is exactly four tokens; a mode other than VALUE or TIMESERIES, or a
negative or non-numeric VALUE, is an error at parse. The node, the species and
the series are resolved after every component has applied, so the reactions
row may precede or follow the transport row in `[PROCESS_COMPONENTS]`; an
unknown node, an unknown series, an undeclared species, or a second row for
the same node and species is then an error. A `[POLLUTANTS]` name is refused:
pollutants keep their `[INFLOWS]`, dry-weather and washoff loading pathways,
and allowing them here would double-count. The concentration applies to the
node's external inflow water only; it is not a fixed concentration at the
node. Rows are inert, with a warning, unless `QUALITY_SOLVER` is
`EULERIAN_ARD` and `IGNORE_QUALITY` is NO.

<!-- source: src/engine/transport/components/EulerianArdComponent/ArdConfig.cpp:247-294,369-373,375-446; src/engine/data/ArdConfigData.hpp:61-79,112-125 -->

### Component section: [TRANSPORT_SOURCES] {#engine_manual_sect_TRANSPORT_SOURCES}

Purpose: \status{Implemented}
Applies a distributed mass rate of a reaction species along a conduit,
independent of its flow, under the Eulerian engine. A negative rate extracts.

Format:
```
Conduit   Species   VALUE        Rate
Conduit   Species   TIMESERIES   Name
```

Parameters:

| Parameter | Meaning | Units / values | Default |
|---|---|---|---|
| `Conduit` | Name of an existing conduit. Other link types are refused. | text | required |
| `Species` | A species declared in the reactions component's `[REACTION_SPECIES]`. | text | required |
| Mode | VALUE gives a constant; TIMESERIES names a `[TIMESERIES]` of rates. | VALUE or TIMESERIES | required |
| `Rate`, `Name` | The constant mass rate in the species' mass units per second, or the series name. | value, any sign; or text | required |

Remarks:
Every row is exactly four tokens; a mode other than VALUE or TIMESERIES, or a
non-numeric VALUE, is an error at parse. The conduit, the species and the
series are resolved after every component has applied; a link that is not a
conduit, an unknown series, an undeclared species, a `[POLLUTANTS]` name, or a
second row for the same conduit and species is then an error. The rate is
spread over the conduit's cells in proportion to cell length. A negative VALUE
is accepted as extraction and warned once at open; a series that dips negative
extracts likewise. Extraction is clamped to the mass the cells actually hold,
and the unmet portion is counted and summarised at the end of the run. The
mass rate is converted internally by dividing by 28.316846592 litres per cubic
foot, since the engine's store is concentration times cubic feet. Rows are
inert, with a warning, unless `QUALITY_SOLVER` is `EULERIAN_ARD` and
`IGNORE_QUALITY` is NO.

<!-- source: src/engine/transport/components/EulerianArdComponent/ArdConfig.cpp:247-294,448-496; src/engine/data/ArdConfigData.hpp:61-79,112-131,171-173 -->
