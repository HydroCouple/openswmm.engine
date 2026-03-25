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

...

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





