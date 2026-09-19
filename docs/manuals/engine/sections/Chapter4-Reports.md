@page engine_manual_ch4_reports CHAPTER 4 - Status Report; Summary Tables and Output

@tableofcontents

This chapter describes what the engine writes after a run: the status report, the summary tables, and the time-series variables saved to the binary output file. How a client displays them — on a map, as a plot, as a table — is covered by @ref manual_results and @ref manual_tabular_results.

## 4.1 The Status Report

A Status Report is available for viewing after each simulation. It contains:
- a summary of the main Simulation Options that are in effect
- a list of any error and warning conditions encountered during the run
- a summary listing of the project’s input data (if requested in the Simulation Options)
- a summary of the data read from each rainfall file used in the simulation
- a description of each control rule action taken during the simulation (if requested in the Simulation Options)
- the system-wide mass continuity errors for:
- runoff quantity and quality
- groundwater flow
- conveyance system flow and water quality
- the names of the nodes with the highest individual flow continuity errors
- the names of the conduits that most often determined the size of the time step used for flow routing (only when the Variable Time Step option is used)
- the names of the links with the highest Flow Instability Index values
- the names of the nodes with the highest frequency of non-convergence
- information on the range of routing time steps taken and the percentage of these that were considered steady state.

## 4.2 Summary Tables

 
The engine writes the following tables to the `.rpt` file. Items marked with an asterisk are also available as per-object time series in the binary output file, so a client can colour a map or draw a plot from them.






Table    Columns
Subcatchment Runoff    Total precipitation (in or mm)*
Total run-on from other subcatchments (in or mm)
Total evaporation (in or mm)*
Total infiltration (in or mm)*
Total runoff depth from impervious areas (in or mm)
Total runoff depth from pervious areas (in or mm)
Total runoff depth (in or mm)*
Total runoff volume (million gallons or million liters)
Peak runoff (flow units)*
Runoff coefficient (ratio of total runoff to total precipitation)*
LID Performance
- Total inflow volume
Total evaporation loss 
Total infiltration loss 
Total surface outflow 
Total underdrain outflow 
Initial storage volume 
Final storage volume
Flow continuity error (%) 
Note: all quantities are expressed as depths (in or mm) over the LID unit’s surface area.
Groundwater Summary    Total surface infiltration (in or mm)
Total evaporation (in or mm)
Total lower seepage (in or mm)
Total lateral outflow (in or mm)
Maximum lateral outflow (flow units)
Average upper zone moisture content (volume fraction)
Average water table elevation (ft or m)
Final upper zone moisture content (volume fraction)
Final water table elevation (ft or m)
Subcatchment Washoff
- Total mass of each pollutant washed off the subcatchment (lbs or kg)


Node Depth
- Average water depth (ft or m)
Maximum water depth (ft or m)*
Maximum hydraulic head (HGL) (ft or m)*
Time of maximum depth
Maximum water depth at reporting times (ft or m)
Node Inflow    Maximum lateral inflow (flow units)*
Maximum total inflow (flow units)
Time of maximum total inflow
Total lateral inflow volume (million gallons or million liters)*
Total inflow volume (million gallons or million liters)
Flow balance error (%)
Note: Total inflow consists of lateral inflow plus inflow from connecting links.
Node Surcharge    Hours surcharged
Maximum height of surcharge above node’s crown (ft or m)
Minimum depth of surcharge below node’s top rim (ft or m)
Note: surcharging occurs when water rises above the crown of the highest conduit and only those conduits that surcharge are listed.
Node Flooding    Hours flooded*
Maximum flooding rate (flow units)*
Time of maximum flooding
Total flood volume (million gallons or million liters)*
Peak depth (for dynamic wave routing in ft or m) or peak volume (1000 ft3 or 1000 m3) of ponded surface water
Note: flooding refers to all water that overflows a node, whether it ponds or not, and only those nodes that flood are listed.
Storage Volume    Average volume of water in the facility (1000 ft3 or 1000 m3)
Average percent of full storage capacity utilized
Percent of total stored volume lost to evaporation
Percent of total stored volume lost to seepage
Maximum volume of water in the facility (1000 ft3 or 1000 m3)
Maximum  percent of full storage capacity utilized
Time of maximum water stored
Maximum outflow rate from the facility (flow units)
Outfall Loading    Percent of time that outfall discharges
Average discharge flow (flow units)
Maximum discharge flow (flow units)
Total volume of flow discharged (million gallons or million liters)
Total mass discharged of each pollutant (lbs or kg)
Street Flow
(Street Conduits Only)    Peak flow (flow units)
Maximum spread from curb (ft or m)
Maximum depth at curb (ft or m)
For streets with assigned inlets
- name of inlet structure
- inlet location (on-grade or on-sag)
- peak flow capture efficiency (%)
- average flow capture efficiency (%)
- frequency of bypass flow (%)
- frequency of backflow (%)
Link Flow    Maximum flow (flow units)*
Time of maximum flow
Maximum velocity (ft/sec or m/sec)*
Ratio of maximum flow to full normal flow
Ratio of maximum flow depth to full depth*
Flow Classification
(Dynamic Wave
 Routing Only)    Ratio of adjusted conduit length to actual length
Fraction of all time steps spent in the following flow categories:
- dry on both ends
- dry on the upstream end
- dry on the downstream end
- subcritical flow
- supercritical flow
- critical flow at the upstream end
- critical flow at the downstream end
Fraction of all time steps flow is limited to normal flow
Fraction of all time steps flow is inlet controlled (for culverts only)
Conduit Surcharge    Hours that conduit is full at:
- both ends*
- upstream end
- downstream end
Hours that conduit flows above full normal flow
Hours that conduit is capacity limited*

Note: only conduits with one or more non-zero entries are listed and a conduit is considered capacity limited if its upstream end is full and the HGL slope is greater than the conduit slope. 
Link Pollutant Loads    Total mass load (in lbs or kg) of each pollutant carried by the link over the entire simulation period
Pumping    Percent of time that the pump is on line
Number of pump start-ups
Minimum flow pumped (flow units)
Average flow pumped (flow units)
Maximum flow pumped (flow units)
Total volume pumped (million gallons or million liters)
Total energy consumed assuming 100% efficiency (Kw-hrs)
Percent of time that the pump operates below its pump curve
Percent of time that the pump operates above its pump curve

- The summary results displayed in these tables are based on results found at every computational time step and not just on the results from each reporting time step.



## 4.3 Time-Series Output Variables

The engine writes a value for each variable below at every reporting time step, for every subcatchment, node and link selected for detailed reporting. That is all of them unless the [REPORT] section names a subset. The values go to the binary output file described in @ref engine_manual_ch3_files, which is what a client reads to plot, tabulate or animate results.

**Table 4-1 — Time-series output variables**
Subcatchment Variables
- rainfall rate (in/hr or mm/hr)
- snow depth (in or mm)
- evaporation loss ( in/day or mm/day)
- infiltration loss (in/hr or mm/hr)
- runoff flow (flow units)
- groundwater flow into the drainage network (flow units)
- groundwater elevation (ft or m)
- soil moisture in the unsaturated groundwater zone (volume fraction)
- washoff concentration of each pollutant (mass/liter)

Node Variables
- water depth (ft or m above the node invert elevation)
- hydraulic head (ft or m, absolute elevation per vertical datum)
- stored water volume (including ponded water, ft3 or m3)
- lateral inflow (runoff + all other external inflows, in flow units)
- total inflow (lateral inflow + upstream conduit inflows, in flow units)
- surface flooding (excess overflow when the node is at full depth, in flow units)
- concentration of each pollutant after any treatment applied at the node (mass/liter)    Link Variables
- flow rate (flow units)
- average water depth (ft or m)
- flow velocity (ft/sec or m/sec)
- volume of water (ft3 or m3)
- capacity (fraction of full area filled by flow for conduits; control setting for pumps and regulators)
- concentration of each pollutant (mass/liter)


System-Wide Variables
- air temperature (degrees F or C)
- potential evaporation (in/day or mm/day)
- actual evaporation (in/day or mm/day)
- total rainfall (in/hr or mm/hr)
- total snow depth (in or mm)
- average losses (in/hr or mm/hr)
- total runoff flow (flow units)
- total dry weather inflow (flow units)
- total groundwater inflow (flow units)
- total RDII inflow (flow units)
- total direct inflow (flow units)
- total external inflow (flow units)
- total external flooding (flow units)
- total outflow from outfalls (flow units)
- total nodal storage volume ( ft3 or m3)
