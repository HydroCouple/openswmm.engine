# Appendix D COMMAND LINE SWMM

## D.1 General Instructions

EPA SWMM can also be run as a console application from the command line within a DOS window. In this case the study area data are placed into a text file and results are written to a text file. The command line for running SWMM in this fashion is:

`runswmm inpfile rptfile outfile`

where `inpfile` is the name of the input file, `rptfile` is the name of the output report file, and `outfile` is the name of an optional binary output file. The latter stores all time series results in a special binary format that will require a separate post-processor program for viewing. If no binary output file name is supplied then all time series results will appear in the report file. As written, the above command assumes that you are working in the directory in which EPA SWMM was installed or that this directory has been added to the PATH variable in your user profile. Otherwise full pathnames for the `runswmm` executable and the files on the command line must be used.

## D.2 Input File Format

The input file for command line SWMM has the same format as the project file used by the Windows version of the program. Figure D-1 illustrates an example SWMM 5 input file. It is organized in sections, where each section begins with a keyword enclosed in brackets. The various section keywords are listed below.

| Section Keyword | Description |
| :--- | :--- |
| **[TITLE]** | project title |
| **[OPTIONS]** | analysis options |
| **[REPORT]** | output reporting instructions |
| **[FILES]** | interface file options |
| **[RAINGAGES]** | rain gage information |
| **[EVAPORATION]**| evaporation data |
| **[TEMPERATURE]**| air temperature and snow melt data |
| **[ADJUSTMENTS]**| monthly adjustments applied to climate variables |
| **[SUBCATCHMENTS]**| basic subcatchment information |
| **[SUBAREAS]** | subcatchment impervious/pervious subarea data |
| **[INFILTRATION]**| subcatchment infiltration parameters |
| **[LID_CONTROLS]**| low impact development control information |
| **[LID\_USAGE]** | assignment of LID controls to subcatchments |
| **[AQUIFERS]** | groundwater aquifer parameters |
| **[GROUNDWATER]**| subcatchment groundwater parameters |
| **[GWF]** | groundwater flow expressions |
| **[SNOWPACKS]** | subcatchment snow pack parameters |
| **[JUNCTIONS]** | junction node information |
| **[OUTFALLS]** | outfall node information |
| **[DIVIDERS]** | flow divider node information |
| **[STORAGE]** | storage node information |
| **[CONDUITS]** | conduit link information |
| **[PUMPS]** | pump link information |
| **[ORIFICES]** | orifice link information |
| **[WEIRS]** | weir link information |
| **[OUTLETS]** | outlet link information |
| **[XSECTIONS]** | conduit, orifice, and weir cross-section geometry |
| **[TRANSECTS]** | transect geometry for conduits with irregular cross-sections |
| **[STREETS]** | cross-section geometry for street conduits |
| **[INLETS]** | design data for storm drain inlets |
| **[INLET\_USAGE]**| assignment of inlets to street and channel conduits |
| **[LOSSES]** | conduit entrance/exit losses and flap valves |
| **[CONTROLS]** | rules that control pump and regulator operation |
| **[POLLUTANTS]** | pollutant information |
| **[LANDUSES]** | land use categories |
| **[COVERAGES]** | assignment of land uses to subcatchments |
| **[LOADINGS]** | initial pollutant loads on subcatchments |
| **[BUILDUP]** | buildup functions for pollutants and land uses |
| **[WASHOFF]** | washoff functions for pollutants and land uses |
| **[TREATMENT]** | pollutant removal functions at conveyance system nodes |
| **[INFLOWS]** | external hydrograph/pollutograph inflow at nodes |
| **[DWF]** | baseline dry weather sanitary inflow at nodes |
| **[RDII]** | rainfall-dependent I/I information at nodes |
| **[HYDROGRAPHS]**| unit hydrograph data used to construct RDII inflows |
| **[CURVES]** | x-y tabular data referenced in other sections |
| **[TIMESERIES]** | time series data referenced in other sections |
| **[PATTERNS]** | periodic multipliers referenced in other sections |
| **[TAGS]** | optional tags assigned to objects |
| **[EVENTS]** | optional event periods for adjusted analysis |
| **[USER_FLAGS]** | user-defined metadata flag definitions *(new in v6)* |
| **[USER_FLAG_VALUES]** | user-defined flag values assigned to objects *(new in v6)* |
| **[PLUGINS]** | plugin shared-library specifications *(new in v6)* |

```
[TITLE]
Example SWMM Project
[OPTIONS]
FLOW_UNITS CFS
INFILTRATION GREEN_AMPT
FLOW_ROUTING KINWAVE
START_DATE 8/6/2002
START_TIME 10:00
END_TIME 18:00
WET_STEP 00:15:00
DRY_STEP 01:00:00
ROUTING_STEP 00:05:00
[RAINGAGES]
;;Name Format Interval SCF DataSource SourceName
;;============== ========== ======== ==== ========== ==========
GAGE1 INTENSITY 0:15 1.0 TIMESERIES SERIES1
[EVAPORATION]
CONSTANT 0.02
[SUBCATCHMENTS]
;;Name Raingage Outlet Area %Imperv Width Slope
;;========= ========== ========== ====== ======== ======= ======
AREA1 GAGE1 NODE1 2 80.0 800.0 1.0
AREA2 GAGE1 NODE2 2 75.0 50.0 1.0
[INFILTRATION]
;;Subcatch Suction Conduct InitDef
;;=========== ========== ========== ==========
AREA1 4.0 1.0 0.34
AREA2 4.0 1.0 0.34
[JUNCTIONS]
;;Name Elev
;;========== ========
NODE1 10.0
NODE2 10.0
NODE3 5.0
NODE4 5.0
NODE6 1.0
NODE7 2.0
```

*Figure D-1 Example SWMM project file*

```
[DIVIDERS]
;;Name Elev Link Type Parameters
;;========== ======== ========= ========= ==========
NODE5 3.0 C6 CUTOFF 1.0
[CONDUITS]
;;Name Node1 Node2 Length N Z1 Z2 Q0
;;========== ========== ========== ========= ========= ========= ========= =========
C1 NODE1 NODE3 800 0.01 0 0 0
C2 NODE2 NODE4 800 0.01 0 0 0
C3 NODE3 NODE5 400 0.01 0 0 0
C4 NODE4 NODE5 400 0.01 0 0 0
C5 NODE5 NODE6 600 0.01 0 0 0
C6 NODE5 NODE7 400 0.01 0 0 0
[XSECTIONS]
;;Link Type G1 G2 G3 G4
;;========== =========== ========= ========= ========= =========
C1 RECT_OPEN 0.5 1 0 0
C2 RECT_OPEN 0.5 1 0 0
C3 CIRCULAR 1.0 0 0 0
C4 RECT_OPEN 1.0 1.0 0 0
C5 PARABOLIC 1.5 2.0 0 0
C6 PARABOLIC 1.5 2.0 0 0
[POLLUTANTS]
;;Name Units Cppt Cgw Cii Kd Snow CoPollut CoFract
;;========== ========= ==== ==== ==== ==== ==== ========= =======
TSS MG/L 0 0 0 0
Lead UG/L 0 0 0 0 NO TSS 0.20
[LANDUSES]
RESIDENTIAL
UNDEVELOPED
[WASHOFF]
;;Landuse Pollutant Type Coeff Expon SweepEff BMPEff
;;============= =========== ========= ========= ========= ========= =========
RESIDENTIAL TSS EMC 23.4 0 0 0
UNDEVELOPED TSS EMC 12.1 0 0 0
[COVERAGES]
;;Subcatch Landuse Pcnt Landuse Pcnt
;;============= ============= ========= ============= =========
AREA1 RESIDENTIAL 80 UNDEVELOPED 20
AREA2 RESIDENTIAL 55 UNDEVELOPED 45
[TIMESERIES]
; Rainfall time series
SERIES1 0:0 0.1 0:15 1.0 0:30 0.5
SERIES1 0:45 0.1 1:00 0.0 2:00 0.0
```

*Figure D-1 Example SWMM project file (continued from previous page).*

Section keywords can appear in mixed lower and upper case. The sections can appear in any arbitrary order in the input file, and not all sections must be present. Each section can contain one or more lines of data. Blank lines may appear anywhere in the file. A semicolon (;) can be used to indicate that what follows on the line is a comment, not data. Data items can appear in any column of a line. Observe how in Figure D-1 these features were used to create a tabular appearance for the data, complete with column headings.

An option is available in the `[OPTIONS]` section to choose flow units from among cubic feet per second (CFS), gallons per minute (GPM), million gallons per day (MGD), cubic meters per second (CMS), liters per second, (LPS), or million liters per day (MLD). If cubic feet or gallons are chosen for flow units, then US units must be used for all other quantities. If cubic meters or liters are chosen, then metric units must be used for all other quantities. Exceptions are pollutant concentration and Manning's roughness coefficient (n) which are always expressed in metric units. The default flow units are CFS. Appendix A.1 provides a complete listing of measurement units.

A detailed description of the data in each section of the input file will now be given. Each section description begins on a new page. When listing the format of a line of data, mandatory keywords are shown in **boldface** while optional items appear in parentheses. A list of keywords separated by a slash (YES/NO) means that only one of the words should appear in the data line.

---

**Section: [TITLE]**
*   **Purpose:** Attaches a descriptive title to the project being analyzed.
*   **Format:** Any number of lines may be entered. The first line will be used as a page header in the output report.

---

**Section: [OPTIONS]**
*   **Purpose:** Provides values for various analysis options.
*   **Format:**

| Option | Values |
| :--- | :--- |
| FLOW_UNITS | CFS / GPM / MGD / CMS / LPS / MLD |
| INFILTRATION | HORTON / MODIFIED_HORTON / GREEN_AMPT / MODIFIED_GREEN_AMPT / CURVE_NUMBER |
| FLOW_ROUTING | STEADY / KINWAVE / DYNWAVE |
| LINK_OFFSETS | DEPTH / ELEVATION |
| FORCE_MAIN_EQUATION | H-W / D-W |
| IGNORE_RAINFALL | YES / NO |
| IGNORE_SNOWMELT | YES / NO |
| IGNORE_GROUNDWATER | YES / NO |
| IGNORE_RDII | YES / NO |
| IGNORE_ROUTING | YES / NO |
| IGNORE_QUALITY | YES / NO |
| ALLOW_PONDING | YES / NO |
| SKIP_STEADY_STATE | YES / NO |
| SYS_FLOW_TOL | value |
| LAT_FLOW_TOL | value |
| START_DATE | month/day/year |
| START_TIME | hours:minutes |
| END_DATE | month/day/year |
| END_TIME | hours:minutes |
| REPORT_START_DATE | month/day/year |
| REPORT_START_TIME | hours:minutes |
| SWEEP_START | month/day |
| SWEEP_END | month/day |
| DRY_DAYS | days |
| REPORT_STEP | hours:minutes:seconds |
| WET_STEP | hours:minutes:seconds |
| DRY_STEP | hours:minutes:seconds |
| ROUTING_STEP | seconds |
| LENGTHENING_STEP | seconds |
| VARIABLE_STEP | value |
| MINIMUM_STEP | seconds |
| INERTIAL_DAMPING | NONE / PARTIAL / FULL |
| NORMAL_FLOW_LIMITED | SLOPE / FROUDE / BOTH |
| SURCHARGE_METHOD | EXTRAN / SLOT |
| MIN_SURFAREA | value |
| MIN_SLOPE | value |
| MAX_TRIALS | value |
| HEAD_TOLERANCE | value |
| THREADS | value |

*   **Remarks:**
    *   **FLOW_UNITS** makes a choice of flow units. Selecting a US flow unit means that all other quantities will be expressed in US customary units, while choosing a metric flow unit will force all quantities to be expressed in SI metric units. The default is CFS.
    *   **INFILTRATION** selects a model for computing infiltration of rainfall into the upper soil zone of subcatchments. The default model is HORTON.
    *   **FLOW_ROUTING** determines which method is used to route flows through the drainage system. STEADY refers to sequential steady state routing, KINWAVE to kinematic wave routing, DYNWAVE to dynamic wave routing. The default routing method is DYNWAVE.
    *   **LINK_OFFSETS** determines the convention used to specify the position of a link offset above the invert of its connecting node. DEPTH indicates offsets are expressed as a distance; ELEVATION uses the absolute elevation. The default is DEPTH.
    *   **FORCE_MAIN_EQUATION** establishes whether the Hazen-Williams (H-W) or the Darcy-Weisbach (D-W) equation will be used for pressurized flow in force main conduits. The default is H-W.
    *   **IGNORE_RAINFALL** set to YES to ignore all rainfall and runoff. The default is NO.
    *   **IGNORE_SNOWMELT** set to YES to ignore snowmelt calculations. The default is NO.
    *   **IGNORE_GROUNDWATER** set to YES to ignore groundwater calculations. The default is NO.
    *   **IGNORE_RDII** set to YES to ignore rainfall-dependent I/I. The default is NO.
    *   **IGNORE_ROUTING** set to YES to compute only runoff. The default is NO.
    *   **IGNORE_QUALITY** set to YES to ignore pollutant washoff, routing, and treatment. The default is NO.
    *   **ALLOW_PONDING** determines whether excess water can collect atop nodes. The default is NO.
    *   **SKIP_STEADY_STATE** set to YES to skip routing computations during steady state periods. The default is NO.
    *   **SYS_FLOW_TOL** is the maximum percent difference between total system inflow and outflow for steady state detection. Default is 5%.
    *   **LAT_FLOW_TOL** is the maximum percent difference between current and previous lateral inflows for steady state detection. Default is 5%.
    *   **START_DATE** is the date when the simulation begins. Default is 1/1/2004.
    *   **START_TIME** is the time of day on the starting date. Default is 0:00:00.
    *   **END_DATE** is the date when the simulation ends. Default is the start date.
    *   **END_TIME** is the time of day on the ending date. Default is 24:00:00.
    *   **REPORT_START_DATE** is the date when reporting begins. Default is the simulation start date.
    *   **REPORT_START_TIME** is the time of day when reporting begins. Default is the simulation start time.
    *   **SWEEP_START** is the day of the year when street sweeping begins. Default is 1/1.
    *   **SWEEP_END** is the day of the year when street sweeping ends. Default is 12/31.
    *   **DRY_DAYS** is the number of days with no rainfall prior to the start. Default is 0.
    *   **REPORT_STEP** is the time interval for reporting results. Default is 0:15:00.
    *   **WET_STEP** is the time step for runoff during rainfall. Default is 0:05:00.
    *   **DRY_STEP** is the time step for runoff during dry periods. Default is 1:00:00.
    *   **ROUTING_STEP** is the time step for flow routing in seconds. Default is 20 sec. Fractional values and hours:minutes:seconds format are allowed.
    *   **LENGTHENING_STEP** is a time step in seconds used to lengthen conduits to meet the Courant criterion. Default is 0 (no lengthening).
    *   **VARIABLE_STEP** is a safety factor applied to a variable time step computed for dynamic wave routing. Default is 0 (no variable step).
    *   **MINIMUM_STEP** is the smallest time step allowed when variable time steps are used. Default is 0.5 seconds.
    *   **INERTIAL_DAMPING** controls how inertial terms are handled in dynamic wave routing. NONE maintains full terms, PARTIAL (default) reduces them near critical flow, FULL drops them entirely.
    *   **NORMAL_FLOW_LIMITED** specifies the condition checked for supercritical flow limiting. Default is BOTH.
    *   **SURCHARGE_METHOD** selects EXTRAN (variation of Surcharge Algorithm) or SLOT (Preissmann Slot) for handling surcharge. Default is EXTRAN.
    *   **MIN_SURFAREA** is the minimum node surface area for dynamic wave routing. Default is 12.566 ft2 (1.167 m2).
    *   **MIN_SLOPE** is the minimum conduit slope (%). Default is 0.
    *   **MAX_TRIALS** is the maximum number of convergence trials per time step. Default is 8.
    *   **HEAD_TOLERANCE** is the convergence tolerance for nodal heads. Default is 0.005 ft (0.0015 m).
    *   **THREADS** is the number of parallel computing threads. Default is 1.

*   **New in OpenSWMM v6:**
    *   **CRS** specifies a Coordinate Reference System for the model geometry, given as an EPSG code (e.g. `EPSG:4326`) or a PROJ string. When set, all coordinate data are assumed to be in that reference system.
    *   **Extension options:** Any option keyword not recognised by the parser is stored in an extension-options map as a key-value string pair (the key is upper-cased). A non-fatal warning is issued. This allows plugins and coupled models to receive configuration through the `[OPTIONS]` section.

---

**Section: [REPORT]**
*   **Purpose:** Describes the contents of the report file that is produced.
*   **Formats:**
    *   `DISABLED YES / NO`
    *   `INPUT YES / NO`
    *   `CONTINUITY YES / NO`
    *   `FLOWSTATS YES / NO`
    *   `CONTROLS YES / NO`
    *   `SUBCATCHMENTS ALL / NONE / <list of subcatchment names>`
    *   `NODES ALL / NONE / <list of node names>`
    *   `LINKS ALL / NONE / <list of link names>`
    *   `LID Name Subcatch Fname`
*   **Remarks:**
    *   Setting **DISABLED** to YES disables all reporting except errors and warnings. Default is NO.
    *   **INPUT** specifies whether an input summary should appear. Default is NO.
    *   **CONTINUITY** specifies if continuity checks are reported. Default is YES.
    *   **FLOWSTATS** specifies whether summary flow statistics are reported. Default is YES.
    *   **CONTROLS** specifies whether all control actions are listed. Default is NO.
    *   **SUBCATCHMENTS**, **NODES**, **LINKS** give lists of objects whose results are reported. Default is NONE.
    *   **LID** specifies that a detailed performance report for LID control *Name* in subcatchment *Subcatch* be written to file *Fname*.
    *   The SUBCATCHMENTS, NODES, LINKS, and LID lines can be repeated multiple times.

---

**Section: [FILES]**
*   **Purpose:** Identifies optional interface files used or saved by a run.
*   **Formats:**
    *   `USE / SAVE RAINFALL Fname`
    *   `USE / SAVE RUNOFF Fname`
    *   `USE / SAVE HOTSTART Fname`
    *   `USE / SAVE RDII Fname`
    *   `USE INFLOWS Fname`
    *   `SAVE OUTFLOWS Fname`
*   **Parameters:**
    *   **Fname**: name of an interface file.
*   **Remarks:**
    *   Rainfall, Runoff, and RDII files can either be used or saved, but not both. A run can both use and save a Hot Start file (with different names).
    *   Enclose file names in double quotes if they contain spaces; include the full path if the file resides in a different directory.

---

**Section: [RAINGAGES]**
*   **Purpose:** Identifies each rain gage that provides rainfall data for the study area.
*   **Formats:**
    *   `Name Form Intvl SCF TIMESERIES Tseries`
    *   `Name Form Intvl SCF FILE Fname (Sta Units)`
*   **Parameters:**
    *   **Name**: name assigned to rain gage.
    *   **Form**: form of recorded rainfall — INTENSITY, VOLUME or CUMULATIVE.
    *   **Intvl**: time interval between gage readings in decimal hours or hours:minutes format.
    *   **SCF**: snow catch deficiency correction factor (use 1.0 for no adjustment).
    *   **Tseries**: name of a time series in the [TIMESERIES] section with rainfall data.
    *   **Fname**: name of an external file with rainfall data.
    *   **Sta**: name of the recording station in a user-prepared formatted rain file.
    *   **Units**: rain depth units — IN (inches) or MM (millimeters).
*   **Remarks:**
    *   Enclose file names in double quotes if they contain spaces.
    *   Station name and depth units are only required when using a user-prepared formatted rainfall file.
*   **New in OpenSWMM v6:** A multi-column CSV rain file can be referenced by appending a colon and column name to the file path, e.g. `FILE "rain.csv:EAST_GAGE"`. The engine opens the CSV, locates the column whose header matches the given name, and reads the rainfall values from that column. This allows a single CSV file to supply data for multiple rain gages.

---

**Section: [EVAPORATION]**
*   **Purpose:** Specifies how daily potential evaporation rates vary with time.
*   **Formats:**
    *   `CONSTANT evap`
    *   `MONTHLY e1 e2 e3 e4 e5 e6 e7 e8 e9 e10 e11 e12`
    *   `TIMESERIES Tseries`
    *   `TEMPERATURE`
    *   `FILE (p1 p2 p3 p4 p5 p6 p7 p8 p9 p10 p11 p12)`
    *   `RECOVERY patternID`
    *   `DRY_ONLY NO / YES`
*   **Parameters:**
    *   **evap**: constant evaporation rate (in/day or mm/day).
    *   **e1..e12**: monthly evaporation rates (in/day or mm/day).
    *   **Tseries**: name of a time series with evaporation data.
    *   **p1..p12**: monthly pan coefficients.
    *   **patternID**: name of a monthly time pattern.
*   **Remarks:**
    *   Use only one of the above formats. If no [EVAPORATION] section appears, evaporation is assumed to be 0.
    *   TEMPERATURE computes evaporation from daily air temperatures in an external climate file.
    *   FILE reads evaporation directly from the external climate file.
    *   RECOVERY identifies an optional pattern of multipliers to modify infiltration recovery rates during dry periods.
    *   DRY_ONLY determines if evaporation only occurs during periods with no precipitation. Default is NO.

---

**Section: [TEMPERATURE]**
*   **Purpose:** Specifies daily air temperatures, monthly wind speed, and snowmelt parameters. Required only for snowmelt modeling or temperature-based evaporation.
*   **Formats:**
    *   `TIMESERIES Tseries`
    *   `FILE Fname (Start) (Units)`
    *   `WINDSPEED MONTHLY s1 s2 s3 s4 s5 s6 s7 s8 s9 s10 s11 s12`
    *   `WINDSPEED FILE`
    *   `SNOWMELT Stemp ATIwt RNM Elev Lat DTLong`
    *   `ADC IMPERVIOUS f.0 f.1 f.2 f.3 f.4 f.5 f.6 f.7 f.8 f.9`
    *   `ADC PERVIOUS f.0 f.1 f.2 f.3 f.4 f.5 f.6 f.7 f.8 f.9`
*   **Parameters:**
    *   **Tseries**: name of a time series with temperature data.
    *   **Fname**: name of an external Climate file.
    *   **Start**: date to begin reading (month/day/year format).
    *   **Units**: temperature units for GHCN files (C10, C, or F).
    *   **s1..s12**: average monthly wind speeds (mph or km/hr).
    *   **Stemp**: air temperature at which precipitation falls as snow.
    *   **ATIwt**: antecedent temperature index weight (default 0.5).
    *   **RNM**: negative melt ratio (default 0.6).
    *   **Elev**: average elevation above mean sea level (default 0).
    *   **Lat**: latitude in degrees North (default 50).
    *   **DTLong**: correction in minutes between true solar time and standard clock time (default 0).
    *   **f.0..f.9**: areal depletion curve fractions.

---

**Section: [ADJUSTMENTS]**
*   **Purpose:** Specifies optional monthly adjustments to temperature, evaporation rate, rainfall intensity, and hydraulic conductivity.
*   **Formats:**
    *   `TEMPERATURE t1 t2 t3 t4 t5 t6 t7 t8 t9 t10 t11 t12`
    *   `EVAPORATION e1 e2 e3 e4 e5 e6 e7 e8 e9 e10 e11 e12`
    *   `RAINFALL r1 r2 r3 r4 r5 r6 r7 r8 r9 r10 r11 r12`
    *   `CONDUCTIVITY c1 c2 c3 c4 c5 c6 c7 c8 c9 c10 c11 c12`
*   **Parameters:**
    *   **t1..t12**: temperature adjustments as plus or minus degrees F (or C).
    *   **e1..e12**: evaporation adjustments as plus or minus in/day (or mm/day).
    *   **r1..r12**: multipliers applied to precipitation rate.
    *   **c1..c12**: multipliers applied to soil hydraulic conductivity.
*   **Remarks:** The same adjustment is applied for each time period within a given month and is repeated for that month in each subsequent year.

---

**Section: [SUBCATCHMENTS]**
*   **Purpose:** Identifies each subcatchment within the study area.
*   **Format:** `Name Rgage OutID Area %Imperv Width Slope Clength (Spack)`
*   **Parameters:**
    *   **Name**: name assigned to the subcatchment.
    *   **Rgage**: name of a rain gage assigned to the subcatchment.
    *   **OutID**: name of the node or subcatchment that receives runoff.
    *   **Area**: area of the subcatchment (acres or hectares).
    *   **%Imperv**: percentage of impervious area.
    *   **Width**: characteristic width (ft or m).
    *   **Slope**: subcatchment slope (percent).
    *   **Clength**: total curb length used for pollutant buildup. Use 0 if not applicable.
    *   **Spack**: optional name of a snow pack object from the [SNOWPACKS] section.

---

**Section: [SUBAREAS]**
*   **Purpose:** Supplies information about pervious and impervious areas for each subcatchment.
*   **Format:** `Subcat Nimp Nperv Simp Sperv %Zero RouteTo (%Routed)`
*   **Parameters:**
    *   **Subcat**: subcatchment name.
    *   **Nimp**: Manning's n for the impervious subarea.
    *   **Nperv**: Manning's n for the pervious subarea.
    *   **Simp**: depression storage for the impervious subarea (inches or mm).
    *   **Sperv**: depression storage for the pervious subarea (inches or mm).
    *   **%Zero**: percent of impervious area with no depression storage.
    *   **RouteTo**: IMPERVIOUS, PERVIOUS, or OUTLET (default is OUTLET).
    *   **%Routed**: percent of runoff routed from one type of area to another (default is 100).

---

**Section: [INFILTRATION]**
*   **Purpose:** Supplies infiltration parameters for each subcatchment.
*   **Format:** `Subcat p1 p2 p3 (p4 p5) (Method)`
*   **Parameters:**
    *   **Subcat**: subcatchment name.
    *   **Method**: HORTON, MODIFIED_HORTON, GREEN_AMPT, MODIFIED_GREEN_AMPT, or CURVE_NUMBER. If not specified, the method from [OPTIONS] is used.
    *   For **Horton / Modified Horton**: p1 = max rate (in/hr or mm/hr), p2 = min rate, p3 = decay constant (1/hr), p4 = drying time (days), p5 = max volume (in or mm).
    *   For **Green-Ampt / Modified Green-Ampt**: p1 = suction (in or mm), p2 = conductivity (in/hr or mm/hr), p3 = initial moisture deficit (fraction).
    *   For **Curve-Number**: p1 = SCS Curve Number, p2 = no longer used, p3 = drying time (days).

---

**Section: [LID_CONTROLS]**
*   **Purpose:** Defines scale-independent LID controls that can be deployed within subcatchments.
*   **Formats:**
    *   `Name Type`

    followed by one or more of:
    *   `Name SURFACE StorHt VegFrac Rough Slope Xslope`
    *   `Name SOIL Thick Por FC WP Ksat Kcoeff Suct`
    *   `Name PAVEMENT Thick Vratio FracImp Perm Vclog (Treg Freg)`
    *   `Name STORAGE Height Vratio Seepage Vclog (Covrd)`
    *   `Name DRAIN Coeff Expon Offset Delay (Hopen Hclose Qcrv)`
    *   `Name DRAINMAT Thick Vratio Rough`
    *   `Name REMOVALS Pollut Rmvl Pollut Rmvl ...`
*   **Parameters:**
    *   **Name**: name assigned to LID process.
    *   **Type**: BC (bio-retention cell), RG (rain garden), GR (green roof), IT (infiltration trench), PP (permeable pavement), RB (rain barrel), RD (rooftop disconnection), VS (vegetative swale).
*   **Remarks:** See user manual Section 3.2 for details on each layer type and required/optional layers per LID type.

---

**Section: [LID_USAGE]**
*   **Purpose:** Deploys LID controls within specific subcatchment areas.
*   **Format:** `Subcat LID Number Area Width InitSat FromImp ToPerv (RptFile DrainTo FromPerv)`
*   **Parameters:**
    *   **Subcat**: subcatchment name.
    *   **LID**: name of an LID process from [LID_CONTROLS].
    *   **Number**: number of replicate LID units.
    *   **Area**: area of each replicate unit (ft2 or m2).
    *   **Width**: width of each unit's outflow face (ft or m).
    *   **InitSat**: initial percent saturation of soil/storage/drain mat zones.
    *   **FromImp**: percent of impervious non-LID area whose runoff is treated.
    *   **ToPerv**: 1 to route LID outflow back onto pervious area, 0 otherwise (default 0).
    *   **RptFile**: optional file name for detailed time series output.
    *   **DrainTo**: optional node or subcatchment receiving drain flow.
    *   **FromPerv**: optional percent of pervious non-LID area whose runoff is treated (default 0).

---

**Section: [AQUIFERS]**
*   **Purpose:** Supplies parameters for each unconfined groundwater aquifer.
*   **Format:** `Name Por WP FC Ks Kslp Tslp ETu ETs Seep Ebot Egw Umc (Epat)`
*   **Parameters:**
    *   **Name**: aquifer name.
    *   **Por**: soil porosity.
    *   **WP**: wilting point.
    *   **FC**: field capacity.
    *   **Ks**: saturated hydraulic conductivity (in/hr or mm/hr).
    *   **Kslp**: slope of log(conductivity) vs moisture deficit curve.
    *   **Tslp**: slope of soil tension vs moisture content curve (in or mm).
    *   **ETu**: fraction of total evaporation available for ET in upper zone.
    *   **ETs**: maximum depth into the saturated zone over which ET can occur (ft or m).
    *   **Seep**: seepage rate from saturated zone to deep groundwater (in/hr or mm/hr).
    *   **Ebot**: elevation of the bottom of the aquifer (ft or m).
    *   **Egw**: initial groundwater table elevation (ft or m).
    *   **Umc**: initial unsaturated zone moisture content (fraction).
    *   **Epat**: optional monthly time pattern for upper zone ET fraction.

---

**Section: [GROUNDWATER]**
*   **Purpose:** Supplies parameters for groundwater flow between an aquifer and a conveyance system node.
*   **Format:** `Subcat Aquifer Node Esurf A1 B1 A2 B2 A3 Dsw (Egwt Ebot Egw Umc)`
*   **Parameters:**
    *   **Subcat**: subcatchment name.
    *   **Aquifer**: name of groundwater aquifer.
    *   **Node**: name of a node exchanging groundwater with the aquifer.
    *   **Esurf**: surface elevation (ft or m).
    *   **A1, B1**: groundwater flow coefficient and exponent.
    *   **A2, B2**: surface water flow coefficient and exponent.
    *   **A3**: surface water-groundwater interaction coefficient.
    *   **Dsw**: fixed depth of surface water at the receiving node (ft or m). Use 0 for varying depth.
    *   **Egwt**: threshold groundwater table elevation for flow (ft or m). Use * for node invert.
    *   **Ebot, Egw, Umc**: optional overrides for aquifer parameters.
*   **Remarks:** The lateral groundwater flow equation is: `QL = A1*(Hgw - Hcb)^B1 - A2*(Hsw - Hcb)^B2 + A3*Hgw*Hsw`

---

**Section: [GWF]**
*   **Purpose:** Defines custom groundwater flow equations for specific subcatchments.
*   **Format:** `Subcat LATERAL/DEEP Expr`
*   **Parameters:**
    *   **Subcat**: subcatchment name.
    *   **Expr**: a math formula for the groundwater flow rate using variables: Hgw, Hsw, Hcb, Hgs (heights relative to aquifer bottom), Ks, K, Theta, Phi, Fi, Fu, A.
*   **Remarks:** Use LATERAL for lateral flow to a conveyance node and DEEP for vertical loss to deep groundwater.

---

**Section: [SNOWPACKS]**
*   **Purpose:** Specifies parameters for snowfall accumulation and melting.
*   **Formats:**
    *   `Name PLOWABLE Cmin Cmax Tbase FWF SD0 FW0 SNN0`
    *   `Name IMPERVIOUS Cmin Cmax Tbase FWF SD0 FW0 SD100`
    *   `Name PERVIOUS Cmin Cmax Tbase FWF SD0 FW0 SD100`
    *   `Name REMOVAL Dplow Fout Fimp Fperv Fimelt (Fsub Scatch)`
*   **Parameters:**
    *   **Name**: name assigned to snowpack parameter set.
    *   **Cmin, Cmax**: minimum and maximum melt coefficients.
    *   **Tbase**: snow melt base temperature.
    *   **FWF**: ratio of free water holding capacity to snow depth.
    *   **SD0**: initial snow depth (in or mm water equivalent).
    *   **FW0**: initial free water in pack (in or mm).
    *   **SNN0**: fraction of impervious area that can be plowed.
    *   **SD100**: snow depth above which there is 100% cover.
    *   **Dplow**: depth of snow when removal begins.
    *   **Fout, Fimp, Fperv, Fimelt**: fractions transferred out, to impervious, to pervious, and to immediate melt.
    *   **Fsub**: fraction transferred to pervious area in another subcatchment.
    *   **Scatch**: name of the receiving subcatchment.

---

**Section: [JUNCTIONS]**
*   **Purpose:** Identifies each junction node of the drainage system.
*   **Format:** `Name Elev (Ymax Y0 Ysur Apond)`
*   **Parameters:**
    *   **Name**: junction node name.
    *   **Elev**: invert elevation (ft or m).
    *   **Ymax**: depth from ground to invert (ft or m). Default 0 (auto-computed from highest connecting link).
    *   **Y0**: initial water depth (ft or m). Default 0.
    *   **Ysur**: maximum additional pressure head above ground under surcharge (ft or m). Default 0.
    *   **Apond**: ponding area once depth exceeds Ymax + Ysur (ft2 or m2). Default 0.
*   **Remarks:** Surface ponding only occurs when Apond is non-zero and ALLOW_PONDING is turned on.

---

**Section: [OUTFALLS]**
*   **Purpose:** Identifies each outfall node (final downstream boundary).
*   **Formats:**
    *   `Name Elev FREE (Gated) (RouteTo)`
    *   `Name Elev NORMAL (Gated) (RouteTo)`
    *   `Name Elev FIXED Stage (Gated) (RouteTo)`
    *   `Name Elev TIDAL Tcurve (Gated) (RouteTo)`
    *   `Name Elev TIMESERIES Tseries (Gated) (RouteTo)`
*   **Parameters:**
    *   **Name**: outfall node name.
    *   **Elev**: invert elevation (ft or m).
    *   **Stage**: elevation of a fixed stage outfall (ft or m).
    *   **Tcurve**: name of a tidal curve in [CURVES].
    *   **Tseries**: name of a time series describing outfall stage.
    *   **Gated**: YES or NO for flap gate. Default is NO.
    *   **RouteTo**: optional subcatchment that receives the outfall's discharge.

---

**Section: [DIVIDERS]**
*   **Purpose:** Identifies each flow divider node with exactly two outflow conduits.
*   **Formats:**
    *   `Name Elev DivLink OVERFLOW (Ymax Y0 Ysur Apond)`
    *   `Name Elev DivLink CUTOFF Qmin (Ymax Y0 Ysur Apond)`
    *   `Name Elev DivLink TABULAR Dcurve (Ymax Y0 Ysur Apond)`
    *   `Name Elev DivLink WEIR Qmin Ht Cd (Ymax Y0 Ysur Apond)`
*   **Parameters:**
    *   **Name**: divider node name.
    *   **Elev**: invert elevation (ft or m).
    *   **DivLink**: name of the link to which flow is diverted.
    *   **Qmin**: flow at which diversion begins (flow units).
    *   **Dcurve**: name of a curve relating diverted flow to total flow.
    *   **Ht**: height of a WEIR divider (ft or m).
    *   **Cd**: discharge coefficient for a WEIR divider.
    *   **Ymax, Y0, Ysur, Apond**: same as for [JUNCTIONS].
*   **Remarks:** Divider nodes are only active under Steady Flow or Kinematic Wave routing. For Dynamic Wave they behave as Junctions.

---

**Section: [STORAGE]**
*   **Purpose:** Identifies each storage node. Storage nodes can have any shape.
*   **Formats:**
    *   `Name Elev Ymax Y0 TABULAR Acurve (Ysur Fevap Psi Ksat IMD)`
    *   `Name Elev Ymax Y0 FUNCTIONAL A1 A2 A0 (Ysur Fevap Psi Ksat IMD)`
    *   `Name Elev Ymax Y0 Shape L W Z (Ysur Fevap Psi Ksat IMD)`
*   **Parameters:**
    *   **Name**: storage node name.
    *   **Elev**: invert elevation (ft or m).
    *   **Ymax**: water depth when full (ft or m).
    *   **Y0**: initial water depth (ft or m).
    *   **Acurve**: name of a storage curve (area vs. depth).
    *   **A1, A2, A0**: coefficients for FUNCTIONAL geometry: `Area = A0 + A1 * Depth^A2`.
    *   **Shape**: CYLINDRICAL, CONICAL, PARABOLOID, or PYRAMIDAL.
    *   **L, W, Z**: dimensions of the storage unit's shape.
    *   **Ysur**: maximum surcharge head (ft or m). Default 0.
    *   **Fevap**: fraction of potential evaporation realized. Default 0.
    *   **Psi, Ksat, IMD**: optional Green-Ampt seepage parameters.

---

**Section: [CONDUITS]**
*   **Purpose:** Identifies each conduit link (pipe or channel).
*   **Format:** `Name Node1 Node2 Length N Z1 Z2 (Q0 Qmax)`
*   **Parameters:**
    *   **Name**: conduit link name.
    *   **Node1**: upstream node name.
    *   **Node2**: downstream node name.
    *   **Length**: conduit length (ft or m).
    *   **N**: Manning's roughness coefficient (n).
    *   **Z1**: upstream offset above inlet node invert (ft or m).
    *   **Z2**: downstream offset above outlet node invert (ft or m).
    *   **Q0**: initial flow (flow units). Default 0.
    *   **Qmax**: maximum flow allowed (flow units). Default no limit.
*   **Remarks:** Offsets are relative distances when LINK_OFFSETS is DEPTH, or absolute elevations when ELEVATION.

---

**Section: [PUMPS]**
*   **Purpose:** Identifies each pump link.
*   **Format:** `Name Node1 Node2 Pcurve (Status Startup Shutoff)`
*   **Parameters:**
    *   **Name**: pump link name.
    *   **Node1**: inlet node name.
    *   **Node2**: outlet node name.
    *   **Pcurve**: name of a pump curve in [CURVES].
    *   **Status**: ON or OFF at start. Default ON.
    *   **Startup**: depth at inlet when pump turns on (ft or m). Default 0.
    *   **Shutoff**: depth at inlet when pump shuts off (ft or m). Default 0.

---

**Section: [ORIFICES]**
*   **Purpose:** Identifies each orifice link.
*   **Format:** `Name Node1 Node2 Type Offset Cd (Gated Orate)`
*   **Parameters:**
    *   **Name**: orifice link name.
    *   **Node1**: inlet node name.
    *   **Node2**: outlet node name.
    *   **Type**: SIDE (vertical plane) or BOTTOM (horizontal plane).
    *   **Offset**: offset above the inlet node invert (ft or m).
    *   **Cd**: discharge coefficient.
    *   **Gated**: YES or NO for flap gate. Default NO.
    *   **Orate**: time in decimal hours to open/close. Use 0 for instantaneous.
*   **Remarks:** Orifice geometry must be described in [XSECTIONS] as CIRCULAR or RECT_CLOSED.

---

**Section: [WEIRS]**
*   **Purpose:** Identifies each weir link.
*   **Format:** `Name Node1 Node2 Type CrstHt Cd (Gated EC Cd2 Sur (Width Surf))`
*   **Parameters:**
    *   **Name**: weir link name.
    *   **Node1**: inlet node name.
    *   **Node2**: outlet node name.
    *   **Type**: TRANSVERSE, SIDEFLOW, V-NOTCH, TRAPEZOIDAL, or ROADWAY.
    *   **CrstHt**: crest offset above inlet node invert (ft or m).
    *   **Cd**: discharge coefficient.
    *   **Gated**: YES or NO for flap gate. Default NO.
    *   **EC**: number of end contractions. Default 0.
    *   **Cd2**: discharge coefficient for trapezoidal ends. Default equals Cd.
    *   **Sur**: YES if the weir can surcharge. Default YES.
    *   **Width**: width of road lanes (for ROADWAY weirs).
    *   **Surf**: road surface type — PAVED or GRAVEL (for ROADWAY weirs).

---

**Section: [OUTLETS]**
*   **Purpose:** Identifies each outlet flow control device.
*   **Formats:**
    *   `Name Node1 Node2 Offset TABULAR/DEPTH Qcurve (Gated)`
    *   `Name Node1 Node2 Offset TABULAR/HEAD Qcurve (Gated)`
    *   `Name Node1 Node2 Offset FUNCTIONAL/DEPTH C1 C2 (Gated)`
    *   `Name Node1 Node2 Offset FUNCTIONAL/HEAD C1 C2 (Gated)`
*   **Parameters:**
    *   **Name**: outlet link name.
    *   **Node1**: inlet node name.
    *   **Node2**: outlet node name.
    *   **Offset**: offset above inlet node invert (ft or m).
    *   **Qcurve**: name of a rating curve in [CURVES].
    *   **C1, C2**: coefficient and exponent of a power function (`Q = C1 * H^C2`).
    *   **Gated**: YES or NO for flap gate. Default NO.

---

**Section: [XSECTIONS]**
*   **Purpose:** Provides cross-section geometric data for conduit and regulator links.
*   **Formats:**
    *   `Link Shape Geom1 Geom2 Geom3 Geom4 (Barrels Culvert)`
    *   `Link IRREGULAR Tsect`
    *   `Link STREET Street`
*   **Parameters:**
    *   **Link**: name of a conduit, orifice, or weir.
    *   **Shape**: a cross-section shape (CIRCULAR, FORCE_MAIN, FILLED_CIRCULAR, RECT_CLOSED, RECT_OPEN, TRAPEZOIDAL, TRIANGULAR, HORIZ_ELLIPSE, VERT_ELLIPSE, ARCH, PARABOLIC, POWER, RECT_TRIANGULAR, RECT_ROUND, MODBASKETHANDLE, EGG, HORSESHOE, GOTHIC, CATENARY, SEMIELLIPTICAL, BASKETHANDLE, SEMICIRCULAR, CUSTOM, IRREGULAR, STREET).
    *   **Geom1**: full height of the cross-section (ft or m).
    *   **Geom2-4**: auxiliary parameters (width, side slopes, etc.).
    *   **Barrels**: number of barrels (default 1).
    *   **Culvert**: code number for inlet flow control analysis (leave blank if not a culvert).
    *   **Tsect**: name of a transect in [TRANSECTS] for IRREGULAR shapes.
    *   **Street**: name of an entry in [STREETS] for STREET shapes.

---

**Section: [TRANSECTS]**
*   **Purpose:** Describes cross-section geometry of natural channels or irregular conduits in HEC-2 format.
*   **Formats:**
    *   `NC Nleft Nright Nchanl`
    *   `X1 Name Nsta Xleft Xright 0 0 0 Lfactor Wfactor Eoffset`
    *   `GR Elev Station ... Elev Station`
*   **Parameters:**
    *   **Nleft, Nright, Nchanl**: Manning's n for left overbank, right overbank, and main channel.
    *   **Name**: transect name.
    *   **Nsta**: number of stations.
    *   **Xleft, Xright**: station positions defining overbank boundaries.
    *   **Lfactor**: meander modifier ratio. Use 0 if not applicable.
    *   **Wfactor**: width multiplier. Use 0 if not applicable.
    *   **Eoffset**: elevation offset for all stations (ft or m).
    *   **Elev, Station**: elevation-station data pairs.
*   **Remarks:** The first line must always be an NC line. One X1 line per transect followed by any number of GR lines.

---

**Section: [STREETS]**
*   **Purpose:** Describes the cross-section geometry of street conduits.
*   **Format:** `Name Tcrown Hcurb Sx nRoad (a W) (Sides Tback Sback nBack)`
*   **Parameters:**
    *   **Name**: street cross-section name.
    *   **Tcrown**: distance from curb to crown (ft or m).
    *   **Hcurb**: curb height (ft or m).
    *   **Sx**: street cross slope (%).
    *   **nRoad**: Manning's n for the road surface.
    *   **a**: gutter depression height (in or mm). Default 0.
    *   **W**: depressed gutter width (ft or m). Default 0.
    *   **Sides**: 1 for single sided, 2 for two-sided. Default 2.
    *   **Tback**: street backing width (ft or m). Default 0.
    *   **Sback**: street backing slope (%). Default 0.
    *   **nBack**: Manning's n for street backing. Default 0.

---

**Section: [INLETS]**
*   **Purpose:** Defines inlet structure designs used to capture street and channel flow.
*   **Formats:**
    *   `Name GRATE/DROP_GRATE Length Width Type (Aopen Vsplash)`
    *   `Name CURB/DROP_CURB Length Height (Throat)`
    *   `Name SLOTTED Length Width`
    *   `Name CUSTOM Dcurve/Rcurve`
*   **Parameters:**
    *   **Name**: inlet structure name.
    *   **Length**: length parallel to the curb (ft or m).
    *   **Width**: width of a GRATE or SLOTTED inlet (ft or m).
    *   **Height**: height of a CURB opening (ft or m).
    *   **Type**: grate type (P_BAR-50, P_BAR-50X100, P_BAR-30, CURVED_VANE, TILT_BAR-45, TILT_BAR-30, RETICULINE, or GENERIC).
    *   **Aopen**: fraction of a GENERIC grate's area that is open.
    *   **Vsplash**: splash over velocity for a GENERIC grate (ft/s or m/s).
    *   **Throat**: HORIZONTAL, INCLINED, or VERTICAL for curb inlets.
    *   **Dcurve/Rcurve**: name of a Diversion or Rating curve for CUSTOM inlets.
*   **Remarks:** Use one line per inlet except for combination inlets (one GRATE line + one CURB line with the same name).

---

**Section: [INLET_USAGE]**
*   **Purpose:** Assigns inlet structures to specific conduits.
*   **Format:** `Conduit Inlet Node (Number %Clogged Qmax aLocal wLocal Placement)`
*   **Parameters:**
    *   **Conduit**: name of a street or open channel conduit.
    *   **Inlet**: name of an inlet structure from [INLETS].
    *   **Node**: name of the sewer node receiving captured flow.
    *   **Number**: number of replicate inlets per side. Default 1.
    *   **%Clogged**: percent capacity reduction due to clogging. Default 0.
    *   **Qmax**: maximum capturable flow (flow units). 0 = no limit.
    *   **aLocal**: height of local gutter depression (in or mm). Default 0.
    *   **wLocal**: width of local gutter depression (ft or m). Default 0.
    *   **Placement**: AUTOMATIC, ON_GRADE, or ON_SAG. Default AUTOMATIC.

---

**Section: [LOSSES]**
*   **Purpose:** Specifies minor head loss coefficients, flap gates, and seepage rates for conduits.
*   **Format:** `Conduit Kentry Kexit Kavg (Flap Seepage)`
*   **Parameters:**
    *   **Conduit**: conduit name.
    *   **Kentry**: entrance minor loss coefficient.
    *   **Kexit**: exit minor loss coefficient.
    *   **Kavg**: average minor loss coefficient.
    *   **Flap**: YES or NO for flap valve. Default NO.
    *   **Seepage**: seepage rate into surrounding soil (in/hr or mm/hr). Default 0.
*   **Remarks:** Minor losses are only computed for Dynamic Wave routing as `K * v^2 / 2g`.

---

**Section: [CONTROLS]**
*   **Purpose:** Determines how pumps and regulators are adjusted based on simulation time or conditions.
*   **Format:**

```
RULE    ruleID
IF      condition_1
AND     condition_2
OR      condition_3
THEN    action_1
AND     action_2
ELSE    action_3
AND     action_4
PRIORITY value
```

*   **Parameters:**
    *   **ruleID**: an ID label assigned to the rule.
    *   **condition_n**: a condition clause.
    *   **action_n**: an action clause.
    *   **value**: a priority value (e.g., 1 to 5).
*   **Remarks:** See Appendix C for a complete description of the control rule format.

---

**Section: [POLLUTANTS]**
*   **Purpose:** Identifies the pollutants being analyzed.
*   **Format:** `Name Units Crain Cgw Cii Kd (Sflag CoPoll CoFract Cdwf Cinit)`
*   **Parameters:**
    *   **Name**: pollutant name. (FLOW is a reserved word.)
    *   **Units**: MG/L, UG/L, or #/L.
    *   **Crain**: concentration in rainfall.
    *   **Cgw**: concentration in groundwater.
    *   **Cii**: concentration in rainfall-dependent I/I.
    *   **Kd**: first-order decay coefficient (1/days).
    *   **Sflag**: YES if buildup requires snow cover. Default NO.
    *   **CoPoll**: name of a co-pollutant. Default * (none).
    *   **CoFract**: fraction of co-pollutant's concentration. Default 0.
    *   **Cdwf**: concentration in dry weather flow. Default 0.
    *   **Cinit**: initial concentration throughout the system. Default 0.

---

**Section: [LANDUSES]**
*   **Purpose:** Identifies the various categories of land uses.
*   **Format:** `Name (SweepInterval Availability LastSweep)`
*   **Parameters:**
    *   **Name**: land use name.
    *   **SweepInterval**: days between street sweeping.
    *   **Availability**: fraction of buildup available for sweeping removal.
    *   **LastSweep**: days since last sweeping at start.

---

**Section: [COVERAGES]**
*   **Purpose:** Specifies the percentage of a subcatchment's area covered by each land use.
*   **Format:** `Subcat Landuse Percent Landuse Percent ...`
*   **Remarks:** More than one land use - percentage pair can appear per line. Subcatchments without land uses produce no pollutant runoff.

---

**Section: [LOADINGS]**
*   **Purpose:** Specifies the initial pollutant buildup on each subcatchment.
*   **Format:** `Subcat Pollut InitBuildup Pollut InitBuildup ...`
*   **Remarks:** If not specified, initial buildup is computed using the DRY_DAYS option.

---

**Section: [BUILDUP]**
*   **Purpose:** Specifies the rate at which pollutants build up over different land uses.
*   **Format:** `Landuse Pollutant FuncType C1 C2 C3 PerUnit`
*   **Parameters:**
    *   **FuncType**: POW (Power), EXP (Exponential), SAT (Saturation), or EXT (External time series).
    *   **C1, C2, C3**: function parameters.
    *   **PerUnit**: AREA or CURBLENGTH.
*   **Remarks:**
    *   POW: `Min(C1, C2 * t^C3)`
    *   EXP: `C1 * (1 - exp(-C2 * t))`
    *   SAT: `C1 * t / (C3 + t)`
    *   EXT: C1 = max buildup, C2 = scaling factor, C3 = time series name.

---

**Section: [WASHOFF]**
*   **Purpose:** Specifies the rate at which pollutants are washed off during rain events.
*   **Format:** `Landuse Pollutant FuncType C1 C2 SweepRmvl BmpRmvl`
*   **Parameters:**
    *   **FuncType**: EXP (Exponential), RC (Rating Curve), or EMC (Event Mean Concentration).
    *   **C1, C2**: function coefficients.
    *   **SweepRmvl**: street sweeping removal efficiency (%).
    *   **BmpRmvl**: BMP removal efficiency (%).

---

**Section: [TREATMENT]**
*   **Purpose:** Specifies pollutant treatment at specific nodes.
*   **Format:** `Node Pollut Result = Func`
*   **Parameters:**
    *   **Node**: node name.
    *   **Pollut**: pollutant name.
    *   **Result**: C (effluent concentration) or R (fractional removal).
    *   **Func**: mathematical function using pollutant concentrations, removals (R_pollutname), and process variables (FLOW, DEPTH, AREA, DT, HRT).

---

**Section: [INFLOWS]**
*   **Purpose:** Specifies external hydrographs and pollutographs entering at specific nodes.
*   **Formats:**
    *   `Node FLOW Tseries (FLOW (1.0 Sfactor Base Pat))`
    *   `Node Pollut Tseries (Type (Mfactor Sfactor Base Pat))`
*   **Parameters:**
    *   **Node**: node name.
    *   **Pollut**: pollutant name.
    *   **Tseries**: time series name.
    *   **Type**: CONCEN or MASS. Default CONCEN.
    *   **Mfactor**: mass flow rate conversion factor. Default 1.0.
    *   **Sfactor**: scaling factor for time series values. Default 1.0.
    *   **Base**: constant baseline value. Default 0.
    *   **Pat**: optional time pattern for adjusting the baseline.
*   **Remarks:** Inflow = `(Baseline * Pattern factor) + (Scaling factor * Time series value)`

---

**Section: [DWF]**
*   **Purpose:** Specifies dry weather flow and quality entering at specific nodes.
*   **Format:** `Node Type Base (Pat1 Pat2 Pat3 Pat4)`
*   **Parameters:**
    *   **Node**: node name.
    *   **Type**: FLOW or a pollutant name.
    *   **Base**: average baseline value.
    *   **Pat1..Pat4**: names of up to four time patterns from [PATTERNS].
*   **Remarks:** The actual dry weather input equals the product of the baseline value and any pattern adjustment factors. Patterns can be any combination of monthly, daily, hourly, and weekend hourly.

---

**Section: [RDII]**
*   **Purpose:** Specifies rainfall-dependent infiltration and inflow (RDII) at specific nodes.
*   **Format:** `Node UHgroup SewerArea`
*   **Parameters:**
    *   **Node**: node receiving RDII flow.
    *   **UHgroup**: name of a unit hydrograph group from [HYDROGRAPHS].
    *   **SewerArea**: contributing sewershed area (acres or hectares).

---

**Section: [HYDROGRAPHS]**
*   **Purpose:** Specifies the shapes of triangular unit hydrographs for RDII.
*   **Formats:**
    *   `Name Raingage`
    *   `Name Month SHORT/MEDIUM/LONG R T K (Dmax Drec D0)`
*   **Parameters:**
    *   **Name**: unit hydrograph group name.
    *   **Raingage**: name of the rain gage used.
    *   **Month**: month of the year (JAN, FEB, etc.) or ALL.
    *   **R**: response ratio (fraction of rainfall that becomes RDII).
    *   **T**: time to peak (hours).
    *   **K**: recession limb ratio (recession duration / time to peak).
    *   **Dmax**: maximum initial abstraction depth.
    *   **Drec**: initial abstraction recovery rate (per day).
    *   **D0**: initial abstraction depth already filled at start.

---

**Section: [CURVES]**
*   **Purpose:** Describes a relationship between two variables in tabular format.
*   **Formats:**
    *   `Name Type`
    *   `Name X-value Y-value ...`
*   **Parameters:**
    *   **Name**: curve name.
    *   **Type**: STORAGE, SHAPE, DIVERSION, TIDAL, PUMP1, PUMP2, PUMP3, PUMP4, PUMP5, RATING, CONTROL, or WEIR.
    *   **X-value, Y-value**: data pairs (X-values must be in increasing order).
*   **Remarks:** Multiple x-y pairs can appear on a line. Repeat the curve name on subsequent lines.

---

**Section: [TIMESERIES]**
*   **Purpose:** Describes how a quantity varies over time.
*   **Formats:**
    *   `Name (Date) Hour Value ...`
    *   `Name Time Value ...`
    *   `Name FILE Fname`
*   **Parameters:**
    *   **Name**: time series name.
    *   **Date**: date in Month/Day/Year format.
    *   **Hour**: 24-hour clock time relative to the last date.
    *   **Time**: hours since start of simulation (decimal or hours:minutes).
    *   **Value**: a value corresponding to the date and time.
    *   **Fname**: name of an external data file.
*   **Remarks:**
    *   For rainfall time series, only non-zero periods need be entered.
    *   For all other time series, interpolation is used between recorded values.
    *   Enclose file names in double quotes if they contain spaces.

---

**Section: [PATTERNS]**
*   **Purpose:** Specifies time patterns of dry weather flow or quality as adjustment multipliers.
*   **Formats:**
    *   `Name MONTHLY Factor1 Factor2 ... Factor12`
    *   `Name DAILY Factor1 Factor2 ... Factor7`
    *   `Name HOURLY Factor1 Factor2 ... Factor24`
    *   `Name WEEKEND Factor1 Factor2 ... Factor24`
*   **Remarks:**
    *   MONTHLY sets monthly factors.
    *   DAILY sets factors for each day of the week (Sunday = day 1).
    *   HOURLY sets factors for each hour starting from midnight.
    *   WEEKEND overrides hourly factors for weekend days.
    *   Multiple lines can be used by repeating the name (but not the type).

---

**Section: [TAGS]**
*   **Purpose:** Assigns optional category tags to objects for identification and filtering.
*   **Format:** `ObjectType Name Tag`
*   **Parameters:**
    *   **ObjectType**: NODE, LINK, or SUBCATCH.
    *   **Name**: name of the object.
    *   **Tag**: tag text assigned to the object.

---

**Section: [EVENTS]**
*   **Purpose:** Specifies event periods during which adjusted climate parameters or special processing should apply.
*   **Format:** `StartDate StartTime EndDate EndTime`
*   **Parameters:**
    *   **StartDate**: start date of the event (month/day/year).
    *   **StartTime**: start time of the event (hours:minutes).
    *   **EndDate**: end date of the event (month/day/year).
    *   **EndTime**: end time of the event (hours:minutes).

---

### D.2.1 OpenSWMM v6 Extension Sections

The following sections are new in OpenSWMM Engine v6 and are not present in standard EPA SWMM 5.x input files. They are optional.

---

**Section: [USER_FLAGS]**
*   **Purpose:** Defines user-defined metadata flags that can be attached to any model object (nodes, links, or subcatchments). Flags provide a schema for custom attributes such as inspection status, maintenance priority, or external asset IDs.
*   **Format:** `Name Type Description`
*   **Parameters:**
    *   **Name**: name assigned to the flag definition.
    *   **Type**: data type — one of BOOLEAN, INTEGER, REAL, or STRING.
    *   **Description**: optional quoted description of the flag's purpose.
*   **Remarks:**
    *   Supported types accept the following values:
        *   BOOLEAN: YES / NO / TRUE / FALSE / 1 / 0
        *   INTEGER: signed integer values
        *   REAL: double-precision floating-point values
        *   STRING: arbitrary text (enclose in double quotes if it contains spaces)
    *   Flag definitions create a schema; actual values are assigned in the [USER_FLAG_VALUES] section.

*   **Example:**

```
[USER_FLAGS]
;;Name            Type      Description
INSPECTED         BOOLEAN   "Has the object been field-inspected?"
PRIORITY          INTEGER   "Maintenance priority (1 = highest)"
ROUGHNESS_ADJ     REAL      "Site-specific roughness multiplier"
ASSET_ID          STRING    "External asset-management system ID"
```

---

**Section: [USER_FLAG_VALUES]**
*   **Purpose:** Assigns concrete values to user-defined flags for specific model objects.
*   **Format:** `ObjectType ObjectName FlagName Value`
*   **Parameters:**
    *   **ObjectType**: the type of object — NODE, LINK, or SUBCATCHMENT.
    *   **ObjectName**: name of the specific object.
    *   **FlagName**: name of a flag defined in the [USER_FLAGS] section.
    *   **Value**: the value to assign (must match the flag's declared type).
*   **Remarks:**
    *   Multiple flag values can be assigned to the same object using separate lines.
    *   Flag values can be queried and modified at runtime through the C API (`swmm_userflag_get_*` / `swmm_userflag_set_*`).

*   **Example:**

```
[USER_FLAG_VALUES]
;;ObjectType   ObjectName   FlagName        Value
NODE           J1           INSPECTED       YES
NODE           J1           PRIORITY        2
LINK           C_MAIN       ROUGHNESS_ADJ   1.05
LINK           C_MAIN       ASSET_ID        "AM-00341"
SUBCATCHMENT   S_WEST       INSPECTED       NO
```

---

**Section: [PLUGINS]**
*   **Purpose:** Specifies shared-library plugins to load for custom output and reporting.
*   **Format:** `Path Args`
*   **Parameters:**
    *   **Path**: path to the shared library file (.so / .dylib / .dll).
    *   **Args**: optional key=value initialisation arguments forwarded to the plugin's `initialize()` method.
*   **Remarks:**
    *   Each plugin must export a C factory function: `extern "C" openswmm::IPluginComponentInfo* openswmm_plugin_info(void);`
    *   Two plugin interfaces are supported:
        *   **IOutputPlugin** — writes time-series results at each output time step.
        *   **IReportPlugin** — writes summary statistics at simulation end.
    *   See `include/openswmm/plugin_sdk/` for full API details.

*   **Example:**

```
[PLUGINS]
./plugins/hdf5_output.dylib     file="results.h5"  compress=9
./plugins/csv_report.dylib      file="report.csv"   delimiter=","
```

---

### D.3 Map Data Section

SWMM's graphical user interface (GUI) can display a schematic map of the drainage area being analyzed. This map displays subcatchments as polygons, nodes as circles, links as polylines, and rain gages as bitmap symbols. In addition it can display text labels and a backdrop image, such as a street map. The GUI has tools for drawing, editing, moving, and displaying these map elements.

The map's coordinate data are stored in the format described below. Normally these data are simply appended to the SWMM input file by the GUI so users do not have to concern themselves with it. However it is sometimes more convenient to import map data from some other source, such as a CAD or GIS file, rather than drawing a map from scratch using the GUI. In this case the data can be added to the SWMM project file using any text editor or spreadsheet program. SWMM does not provide any automated facility for converting coordinate data from other file formats into the SWMM map data format.

SWMM's map data are organized into the following seven sections:

| Section | Description |
| :--- | :--- |
| **[MAP]** | X,Y coordinates of the map's bounding rectangle |
| **[POLYGONS]** | X,Y coordinates for each vertex of subcatchment polygons |
| **[COORDINATES]**| X,Y coordinates for nodes |
| **[VERTICES]** | X,Y coordinates for each interior vertex of polyline links |
| **[LABELS]** | X,Y coordinates and text of labels |
| **[SYMBOLS]** | X,Y coordinates for rain gages |
| **[BACKDROP]** | X,Y coordinates of the bounding rectangle and file name of the backdrop image. |

Figure D-2 displays a sample map and Figure D-3 the data that describes it. Note that only one link, 3, has interior vertices which give it a curved shape. Also observe that this map's coordinate system has no units, so that the positions of its objects may not necessarily coincide to their real-world locations.

![Example study area map showing two subcatchments, a rain gage, four nodes, and three links.](../../Manual/images/figure-d-2.png)
*Figure D-2 Example study area map*

```
[MAP]
DIMENSIONS 0.00 0.00 10000.00 10000.00
UNITS None
[COORDINATES]
;;Node X-Coord Y-Coord
N1 4006.62 5463.58
N2 6953.64 4768.21
N3 4635.76 3443.71
N4 8509.93 827.81
[VERTICES]
;;Link X-Coord Y-Coord
3 5430.46 2019.87
3 7251.66 927.15
[SYMBOLS]
;;Gage X-Coord Y-Coord
G1 5298.01 9139.07
[Polygons]
;;Subcatchment X-Coord Y-Coord
S1 3708.61 8543.05
S1 4834.44 7019.87
S1 3675.50 4834.44
< additional vertices not listed >
S2 6523.18 8079.47
S2 8112.58 8841.06
```

*Figure D-3 Data for example study area map*

A detailed description of each map data section will now be given. Remember that map data are only used as a visualization aid for SWMM's GUI and they play no role in any of the runoff or routing computations. Map data are not needed for running the command line version of SWMM.

**Section: [MAP]**
*   **Purpose:** Provides dimensions and distance units for the map.
*   **Formats:**
    *   `DIMENSIONS X1 Y1 X2 Y2`
    *   `UNITS FEET / METERS / DEGREES / NONE`
*   **Parameters:**
    *   **X1**: lower-left X coordinate of full map extent
    *   **Y1**: lower-left Y coordinate of full map extent
    *   **X2**: upper-right X coordinate of full map extent
    *   **Y2**: upper-right Y coordinate of full map extent

**Section: [COORDINATES]**
*   **Purpose:** Assigns X,Y coordinates to drainage system nodes.
*   **Format:** `Node Xcoord Ycoord`
*   **Parameters:**
    *   **Node**: name of node.
    *   **Xcoord**: horizontal coordinate relative to origin in lower left of map.
    *   **Ycoord**: vertical coordinate relative to origin in lower left of map.

**Section: [VERTICES]**
*   **Purpose:** Assigns X,Y coordinates to interior vertex points of curved drainage system links.
*   **Format:** `Link Xcoord Ycoord`
*   **Parameters:**
    *   **Link**: name of link.
    *   **Xcoord**: horizontal coordinate of vertex relative to origin in lower left of map.
    *   **Ycoord**: vertical coordinate of vertex relative to origin in lower left of map.
*   **Remarks:**
    *   Include a separate line for each interior vertex of the link, ordered from the inlet node to the outlet node.
    *   Straight-line links have no interior vertices and therefore are not listed in this section.

**Section: [POLYGONS]**
*   **Purpose:** Assigns X,Y coordinates to vertex points of polygons that define a subcatchment boundary.
*   **Format:** `Subcat Xcoord Ycoord`
*   **Parameters:**
    *   **Subcat**: name of subcatchment.
    *   **Xcoord**: horizontal coordinate of vertex relative to origin in lower left of map.
    *   **Ycoord**: vertical coordinate of vertex relative to origin in lower left of map.
*   **Remarks:**
    *   Include a separate line for each vertex of the subcatchment polygon, ordered in a consistent clockwise or counter-clockwise sequence.

**Section: [SYMBOLS]**
*   **Purpose:** Assigns X,Y coordinates to rain gage symbols.
*   **Format:** `Gage Xcoord Ycoord`
*   **Remarks:**
    *   **Gage**: name of rain gage.
    *   **Xcoord**: horizontal coordinate relative to origin in lower left of map.
    *   **Ycoord**: vertical coordinate relative to origin in lower left of map.

**Section: [LABELS]**
*   **Purpose:** Assigns X,Y coordinates to user-defined map labels.
*   **Format:** `Xcoord Ycoord Label (Anchor Font Size Bold Italic)`
*   **Parameters:**
    *   **Xcoord**: horizontal coordinate relative to origin in lower left of map.
    *   **Ycoord**: vertical coordinate relative to origin in lower left of map.
    *   **Label**: text of label surrounded by double quotes.
    *   **Anchor**: name of node or subcatchment that anchors the label on zoom-ins (use an empty pair of double quotes if there is no anchor).
    *   **Font**: name of label's font (surround by double quotes if the font name includes spaces).
    *   **Size**: font size in points.
    *   **Bold**: **YES** for bold font, **NO** otherwise.
    *   **Italic**: **YES** for italic font, **NO** otherwise.
*   **Remarks:**
    *   Use of the anchor node feature will prevent the label from moving outside the viewing area when the map is zoomed in on.
    *   If no font information is provided then a default font is used to draw the label.

**Section: [BACKDROP]**
*   **Purpose:** Specifies file name and coordinates of map's backdrop image.
*   **Formats:**
    *   `FILE Fname`
    *   `DIMENSIONS X1 Y1 X2 Y2`
*   **Parameters:**
    *   **Fname**: name of file containing backdrop image
    *   **X1**: lower-left X coordinate of backdrop image
    *   **Y1**: lower-left Y coordinate of backdrop image
    *   **X2**: upper-right X coordinate of backdrop image
    *   **Y2**: upper-right Y coordinate of backdrop image





