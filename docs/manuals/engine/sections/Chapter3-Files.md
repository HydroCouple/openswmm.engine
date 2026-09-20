@page engine_manual_ch3_files CHAPTER 3 - Files and Command-Line Operation

@tableofcontents

This section describes the various files that SWMM can utilize. They include: the project file, the report and output files, rainfall files, the climate file, calibration data files, time series files, and interface files. The only file required to run SWMM is the project file; the others are optional.

## Project Files

A SWMM project file is a plain text file that contains all of the data used to describe a study area and the options used to analyze it. The file is organized into sections, where each section generally corresponds to a particular category of object used by SWMM. Its full format is given in @ref engine_manual_ch2_input_file.

Normally a SWMM user would not edit the project file directly, since SWMM's graphical user interface can add, delete, or modify a project's data and control settings. However, for large projects where data currently reside in other electronic formats, such as CAD or GIS files, it may be more expeditious to extract data from these sources and save it to a formatted project file before running SWMM. The format of the project file is described in detail in Appendix D of this manual.

After a project file is saved to disk, a settings file will automatically be saved with it. This file has the same name as the project file except that its extension is .ini (e.g., if the project file were named project1.inp then its settings file would have the name project1.ini). It contains various settings used by SWMM’s graphical user interface, such as map display options, legend colors and intervals, object default values, and calibration file information. Users should not edit this file. A SWMM project will still load and run even if the settings file is missing.

## Report and Output Files

The report file is a plain text file created after every SWMM run that contains the contents of both the Status Report and all of the tables included in the Summary Results report. Refer to Sections 9.1 and 9.2 to review their contents.

The output file is a binary file that contains the numerical results from a successful SWMM run. This file is used by SWMM’s user interface to interactively create time series plots and tables, profile plots, and statistical analyses of a simulation's results.

Whenever a successfully run project is either saved or closed, the report and output files are saved with the same name as the project file, but with extensions of .rpt and .out.  This will happen automatically if the program preference Prompt to Save Results is turned off (see @ref manual_preferences). Otherwise the user is asked if the current results should be saved or not. If results are saved then the next time the project is opened, the results from these files will automatically be available for viewing.

## Rainfall Files

SWMM’s rain gage objects can utilize rainfall data stored in external rainfall files. The program currently recognizes the following formats for storing such data:
- Hourly and fifteen minute precipitation data from over 5,500 reporting stations retrieved using NOAA’s National Centers for Environmental Information (NCEI) Climate Data Online service (www.ncdc.noaa.gov/cdo-web) (space-delimited text format only).
- The older DS-3240 and related formats used for hourly precipitation by NCEI.
- The older DS-3260 and related formats used for fifteen minute precipitation by NCEI.
- HLY03 and HLY21 formats for hourly rainfall at Canadian stations, available from Environment Canada at www.climate.weather.gc.ca.
- FIF21 format for fifteen minute rainfall at Canadian stations, also available online from Environment Canada.
- a standard user-prepared format where each line of the file contains the station ID, year, month, day, hour, minute, and non-zero precipitation reading, all separated by one or more spaces.

When requesting data from NCEI’s online service, be sure to specify the TEXT format option,   make sure that the data flags are included, and, for 15-minute data, select the QPCP option and not the QGAG one.

An excerpt from a sample user-prepared Rainfall file is as follows:
STA01  2004  6  12  00  00  0.12
STA01  2004  6  12  01  00  0.04
STA01  2004  6  22  16  00  0.07
This format can also accept multiple stations within the same file.
When a rain gage is designated as receiving its rainfall data from a standard user-prepared format, the following properties must be supplied for it: the name of the recording station referenced in the file, the rainfall type (e.g., intensity or volume), the recording time interval, and rainfall depth units. For the other file types these properties are defined by their respective file format and are automatically recognized by SWMM.

## Climate Files

SWMM can use an external climate file that contains daily air temperature, evaporation, and wind speed data. The program currently recognizes the following formats:
- Global Historical Climatology Network - Daily (GHCN-D) files (TEXT output format) available from NOAA's National Climatic Data Center (NCDC) Climate Data Online service at www.ncdc.noaa.gov/cdo-web.
- Older NCDC DS-3200 or DS-3210 files.
- Canadian climate files available from Environment Canada at www.climate.weather.gc.ca.
- A user-prepared climate file where each line contains a recording station name, the year, month, day, maximum temperature, minimum temperature, and optionally, evaporation rate, and wind speed. If no data are available for any of these items on a given date, then an asterisk should be entered as its value.
When a climate file has days with missing values, SWMM will use the value from the most recent previous day with a recorded value. 

- For a user-prepared climate file, the data must be in the same units as the project being analyzed. For US units, temperature is in degrees F, evaporation is in inches/day, and wind speed is in miles/hour. For metric units, temperature is in degrees C, evaporation is in mm/day, and wind speed is in km/hour.

## Calibration Files

Calibration files contain measurements of variables at one or more locations that can be compared with simulated values in Time Series Plots. Separate files can be used for each of the following:
- Subcatchment Runoff
- Subcatchment Groundwater Flow
- Subcatchment Groundwater Elevation
- Subcatchment Snow Pack Depth
- Subcatchment Pollutant Washoff
- Node Depth
- Node Lateral Inflow
- Node Flooding
- Node Water Quality
- Link Flow
- Link Velocity
- Link Depth
Calibration files are named by the client that loads them; the engine itself neither reads nor writes them. 

The format of the file is as follows:
- The name of the first object with calibration data is entered on a single line.
- Subsequent lines contain the following recorded measurements for the object:
- measurement date (month/day/year, e.g., 6/21/2004) or number of whole days since the start of the simulation
- measurement time (hours:minutes) on the measurement date or relative to the number of elapsed days
- measurement value (for pollutants, a value is required for each pollutant).
- Follow the same sequence for any additional objects.
An excerpt from an example calibration file is shown below. It contains flow values for two conduits: 1030 and 1602. Note that a semicolon can be used to begin a comment. In this example, elapsed time rather than the actual measurement date was used.









;Flows for Selected Conduits    
;Conduit  Days    Time  Flow
;-----------------------------
 1030            
- 0     0:15  0
- 0     0:30  0
- 0     0:45  23.88
- 0     1:00  94.58
- 0     1:15  115.37
 1602            
- 0     0:15  5.76
- 0     0:30  38.51
- 0     1:00  67.93
- 0     1:15  68.01



## Time Series Files

Time series files are external text files that contain data for SWMM's time series objects. Examples of time series data include rainfall, evaporation, inflows to nodes of the drainage system, and water stage at outfall boundary nodes. The file must be created and edited outside of SWMM, using a text editor or spreadsheet program. A time series file can be linked to a specific time series object using SWMM's Time Series Editor (see @ref manual_hydraulics). 

The format of a time series file consists of one time series value per line. Comment lines can be inserted anywhere in the file as long as they begin with a semicolon. Time series values can either be in date / time / value format or in time / value format, where each entry is separated by one or more spaces or tab characters. For the date / time / value format, dates are entered as month/day/year (e.g., 7/21/2004) and times as 24-hour military time (e.g., 8:30 pm is 20:30). After the first date, additional dates need only be entered whenever a new day occurs. For the time / value format, time can either be decimal hours or military time since the start of a simulation (e.g., 2 days, 4 hours and 20 minutes can be entered as either 52.333 or 52:20). An example of a time series file is shown below:






;Rainfall Data for Gage G1
07/01/2003  00:00 0.00000
- 00:15 0.03200
- 00:30 0.04800
- 00:45 0.02400
- 01:00 0.0100
07/06/2003  14:30 0.05100
- 14:45 0.04800
- 15:00 0.03000
- 18:15 0.01000
- 18:30 0.00800


- In earlier releases of SWMM 5, a time series file was required to have two header lines of descriptive text at the start of the file that did not have to begin with the semicolon comment character. These files can still be used as long as they are modified by inserting a semicolon at the front of the first two lines.
- When preparing rainfall time series files, it is only necessary to enter periods with non-zero rainfall amounts. SWMM interprets the rainfall value as a constant value lasting over the recording interval specified for the rain gage which utilizes the time series. For all other types of time series, SWMM uses interpolation to estimate values at times that fall in between the recorded values.

## Interface Files

SWMM can use several different kinds of interface files that contain either externally imposed inputs (e.g., rainfall or RDII hydrographs) or the results of previously run analyses (e.g., runoff or routing results). These files can help speed up simulations, simplify comparisons of different loading scenarios, and allow large study areas to be broken up into smaller areas that can be analyzed individually. The different types of interface files that are currently available include:
- rainfall interface file
- runoff interface file
- hot start file
- RDII interface file
- routing interface files
Consult @ref engine_manual_sect_OPTIONS for instructions on how to specify interface files for use as input and/or output in a simulation.

## Rainfall and Runoff Files

The rainfall and runoff interface files are binary files created internally by SWMM that can be saved and reused from one analysis to the next.

The rainfall interface file collates a series of separate rain gage files into a single rainfall data file. Normally a temporary file of this type is created for every SWMM analysis that uses external rainfall data files and is then deleted after the analysis is completed. However, if the same rainfall data are being used with many different analyses, requesting SWMM to save the rainfall interface file after the first run and then reusing this file in subsequent runs can save computation time.

- The rainfall interface file should not be confused with a rainfall data file. The latter is an external text file that provides rainfall time series data for a single rain gage. The former is a binary file created internally by SWMM that processes all of the rainfall data files used by a project.

The runoff interface file can be used to save the runoff results generated from a simulation run. If runoff is not affected in future runs, the user can request that SWMM use this interface file to supply runoff results without having to repeat the runoff calculations again.

## Hot Start Files

Hot start files are binary files created by SWMM that contain the full hydrologic, hydraulic and water quality state of the study area at the end of a run. The following information is saved to the file:
- the ponded depth and its water quality for each subcatchment
- the pollutant mass buildup on each subcatchment
- the infiltration state of each subcatchment
- the conditions of any snowpack on each subcatchment
- the unsaturated zone moisture content, water table elevation, and groundwater outflow for each subcatchment that has a groundwater zone defined for it
- the water depth, lateral inflow, and water quality at each node of the system
- the flow rate, water depth, control setting and water quality in each link of the system.
The hydrologic state of any LID units is not saved. The hot start file saved after a run can be used to define the initial conditions for a subsequent run.

Hot start files can be used to avoid the initial numerical instabilities that sometimes occur under Dynamic Wave routing. For this purpose they are typically generated by imposing a constant set of base flows (for a natural channel network) or set of dry weather sanitary flows (for a sewer network) over some startup period of time. The resulting hot start file from this run is then used to initialize a subsequent run where the inflows of real interest are imposed.

It is also possible to both use and save a hot start file in a single run, starting off the run with one file and saving the ending results to another. The resulting file can then serve as the initial conditions for a subsequent run if need be. This technique can be used to divide up extremely long continuous simulations into more manageable pieces.

Hot start files are named in the [FILES] section — see @ref engine_manual_sect_FILES. A file saved mid-run rather than at the end holds only node, link and groundwater state.

## RDII Files

The RDII interface file contains a time series of rainfall-dependent infiltration and inflow flows for a specified set of drainage system nodes. This file can be generated from a previous SWMM run when Unit Hydrographs and nodal RDII inflow data have been defined for the project, or it can be created outside of SWMM using some other source of RDII data (e.g., through measurements or output from a different computer program). RDII files generated by SWMM are saved in a binary format. RDII files created outside of SWMM are text files with the same format used for routing interface files discussed below, where Flow is the only variable contained in the file.

## Routing Files

A routing interface file stores a time series of flows and pollutant concentrations that are discharged from the outfall nodes of drainage system model. This file can serve as the source of inflow to another drainage system model that is connected at the outfalls of the first system. A Combine utility is available on the File menu that will combine pairs of routing interface files into a single interface file. This allows very large systems to be broken into smaller sub-systems that can be analyzed separately and linked together through the routing interface file. Figure 11.1 below illustrates this concept.


















<!-- FIGURE: eng_ch3_routing_interface_files — combining routing interface files -->

A single SWMM run can utilize an outflows routing file to save results generated at a system's outfalls, an inflows routing file to supply hydrograph and pollutograph inflows at selected nodes, or both. 

## RDII / Routing File Format

RDII interface files and routing interface files have the same text format:
- the first line contains the keyword "SWMM5" (without the quotes)
- a line of text that describes the file (can be blank)
- the time step used for all inflow records (integer seconds)
- the number of variables stored in the file, where the first variable must always be flow rate
- the name and units of each variable (one per line), where flow rate is the first variable listed and is always named FLOW
- the number of nodes with recorded inflow data
- the name of each node (one per line)
- a line of text that provides column headings for the data to follow (can be blank)
- for each node at each time step, a line with:
- the name of the node
- the date (year, month, and day separated by spaces)
- the time of day (hours, minutes, and seconds separated by spaces)
- the flow rate followed by the concentration of each quality constituent
Time periods with no values at any node can be skipped. An excerpt from an RDII / routing interface file is shown below.


SWMM5
Example File
300
1
FLOW CFS
2
N1
N2
Node Year Mon Day Hr Min Sec Flow
N1   2002 04  01  00 20  00  0.000000
N2   2002 04  01  00 20  00  0.002549
N1   2002 04  01  00 25  00  0.000000
N2   2002 04  01  00 25  00  0.002549

## 2D Mesh Files

A two-dimensional surface is described by the `[2D_*]` sections of
@ref engine_manual_ch2_input_file §2.4. Because a mesh can run to hundreds of
thousands of cells, those sections may live in a separate file that the
project references with a single row:

```
[2D_MESH_FILE]
FILE  meshes/city_basin.2dm
```

The path is absolute or relative to the directory of the project file. The
mesh file uses the same bracketed-section dialect as the project file, and the
sections it carries are read as if they appeared in the project itself, so the
same mesh can be shared by several projects. Grammar and rules are in
@ref engine_manual_sect_2D_MESH_FILE.

## 2D Results Files

When a project carries a mesh, the two-dimensional results are written to a
separate HDF5 file named by `[2D_OPTIONS] OUTPUT_FILE` rather than to the
binary output file. The file follows the CF-1.11 and UGRID-1.0 conventions
(its `Conventions` attribute says so), so it opens in any UGRID-aware reader
and in `h5py`:

- **Mesh topology**, written once: `Mesh2_node_x`, `Mesh2_node_y`,
  `Mesh2_node_z`, the face–node connectivity `Mesh2_face_nodes` with the
  vertex count per face `Mesh2_face_nv` (3 or 4), the face centroids
  `Mesh2_face_x`, `Mesh2_face_y`, `Mesh2_face_z`, the per-face roughness
  `Mesh2_face_mannings_n` and area `Mesh2_face_area`, and the edge geometry
  `Mesh2_edge_length`, `Mesh2_edge_nx`, `Mesh2_edge_ny`.
- **Time**, an unlimited `time` dataset, advanced every `REPORT_2D_STEP`
  (or every report step when that key is absent).
- **Per-face series**, one frame per time: `Mesh2_face_depth` and
  `Mesh2_face_head` always; `Mesh2_face_vx`, `Mesh2_face_vy`,
  `Mesh2_face_rainfall`, `Mesh2_face_rain_cum`, `Mesh2_face_infil_rate`,
  `Mesh2_face_infil_cum`, `Mesh2_face_coupling_flux`, `Mesh2_face_net_source`,
  the gradient fields `Mesh2_face_grad_hx`, `Mesh2_face_grad_hy` and their
  limited counterparts, `Mesh2_face_continuity_err`, and the species field
  `Mesh2_face_species_conc` (faces × species) — each present only when its
  group is selected by `REPORT_2D_VARIABLES`.
- **Groundwater**, when the mesh carries an aquifer and the `GROUNDWATER`
  group is selected: `Mesh2_face_gw_table_elev`, `Mesh2_face_gw_hg`
  (saturated thickness), `Mesh2_face_gw_hu` (unsaturated storage),
  `Mesh2_face_gw_recharge`, `Mesh2_face_gw_lateral`,
  `Mesh2_face_gw_node_exchange`, `Mesh2_face_gw_deep`, `Mesh2_face_gw_et`,
  `Mesh2_face_gw_dunne` and `Mesh2_face_gw_infil_in`, with the static
  `Mesh2_face_gw_bed_elev` and `Mesh2_face_gw_closure`; the `GW_DETAILED`
  group adds the σ-column moisture profile `Mesh2_face_gw_theta_sigma`
  (faces × layers). Each dataset carries its meaning and unit as attributes.

`OUTPUT_PRECISION FLOAT32` halves the file at the cost of single-precision
series; `OUTPUT_COMPRESSION 1`–`9` enables gzip chunk compression. The file is
written with a single writer and is not flushed until the run ends, so it
cannot be read while the simulation is still running. The keys are documented
in @ref engine_manual_sect_2D_OPTIONS; the meaning of each series is in
@ref hydraulics_ref_ch9_two_dimensional §9.9.

## Process Component Configuration Files

Reaction systems, the Eulerian transport engine, heat transport and water-age
tracking are configured in their own files, named in the project's
`[PROCESS_COMPONENTS]` section and conventionally suffixed `.rxn`, `.ard`,
`.heat` and `.age`. Each file uses the bracketed-section dialect of the
project file; the sections each component accepts are documented in
@ref engine_manual_ch2_input_file §2.5. On save, every component writes its
configuration back to its file, and a save-as carries the files alongside
the written project.
