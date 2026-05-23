# Appendix B VISUAL OBJECT PROPERTIES

## B.1 Rain Gage Properties

| Name | Description |
| :--- | :--- |
| **Name** | User-assigned rain gage name. |
| **X-Coordinate** | Horizontal location of the rain gage on the Study Area Map. If left blank then the rain gage will not appear on the map. |
| **Y-Coordinate** | Vertical location of the rain gage on the Study Area Map. If left blank then the rain gage will not appear on the map. |
| **Description** | Click the ellipsis button (or press Enter) to edit an optional description of the rain gage. |
| **Tag** | Optional label used to categorize or classify the rain gage. |
| **Rain Format** | Format in which the rain data are supplied:<br>**INTENSITY:** each rainfall value is an average rate in inches/hour (or mm/hour) over the recording interval,<br>**VOLUME:** each rainfall value is the volume of rain that fell in the recording interval (in inches or millimeters),<br>**CUMULATIVE:** each rainfall value represents the cumulative rainfall that has occurred since the start of the last series of non-zero values (in inches or millimeters). |
| **Time Interval** | Recording time interval between gage readings in either decimal hours or hours:minutes format. |
| **Snow Catch Factor** | Factor that corrects gage readings for snowfall. |
| **Data Source** | Source of rainfall data; either **TIMESERIES** for user-supplied time series data or **FILE** for an external data file. |
| **TIME SERIES** | |
| - *Series Name* | Name of time series with rainfall data if Data Source selection was **TIMESERIES**; leave blank otherwise (double-click to edit the series). |
| **DATA FILE** | |
| - *File Name* | Name of external file containing rainfall data (see Section 11.3). |
| - *Station ID* | Recording gage station identifier. |
| - *Rain Units* | Depth units (**IN** or **MM**) for rainfall values in user-prepared files (other standard file formats have fixed units depending on the format). |

## B.2 Subcatchment Properties

| Name | Description |
| :--- | :--- |
| **Name** | User-assigned subcatchment name. |
| **X-Coordinate** | Horizontal location of the subcatchment's centroid on the Study Area Map. If left blank then the subcatchment will not appear on the map. |
| **Y-Coordinate** | Vertical location of the subcatchment's centroid on the Study Area Map. If left blank then the subcatchment will not appear on the map. |
| **Description** | Click the ellipsis button (or press Enter) to edit an optional description of the subcatchment. |
| **Tag** | Optional label used to categorize or classify the subcatchment. |
| **Rain Gage** | Name of the rain gage associated with the subcatchment. |
| **Outlet** | Name of the node or subcatchment which receives the subcatchment's runoff. |
| **Area** | Area of the subcatchment, including any LID controls (acres or hectares). |
| **Width¹** | Characteristic width of the overland flow path for sheet flow runoff (feet or meters). |
| **% Slope** | Average percent slope of the subcatchment. |
| **% Imperv** | Percent of land area (excluding the area used for LID controls) which is impervious. |
| **N-Imperv** | Manning's coefficient (n) for overland flow over the impervious portion of the subcatchment (see Section A.6 for typical values). |
| **N-Perv** | Manning's coefficient (n) for overland flow over the pervious portion of the subcatchment (see Section A.6 for typical values). |
| **Dstore-Imperv** | Depth of depression storage on the impervious portion of the subcatchment (inches or millimeters) (see Section A.5 for typical values). |
| **Dstore-Perv** | Depth of depression storage on the pervious portion of the subcatchment (inches or millimeters) (see Section A.5 for typical values). |
| **% Zero-Imperv** | Percent of the impervious area with no depression storage. |
| **Subarea Routing**| Choice of internal routing of runoff between pervious and impervious areas:<br>**IMPERV:** runoff from pervious area flows to impervious area,<br>**PERV:** runoff from impervious area flows to pervious area,<br>**OUTLET:** runoff from both areas flows directly to outlet. |
| **Percent Routed** | Percent of runoff routed between subareas. |
| **Infiltration Data** | Click the ellipsis button (or press Enter) to edit infiltration parameters for the subcatchment. |
| **Groundwater** | Click the ellipsis button (or press Enter) to edit groundwater flow parameters for the subcatchment. |
| **Snow Pack** | Name of snow pack parameter set (if any) assigned to the subcatchment. |
| **LID Controls** | Click the ellipsis button (or press Enter) to edit the use of low impact development controls in the subcatchment. |
| **Land Uses** | Click the ellipsis button (or press Enter) to assign land uses to the subcatchment. Only needed if pollutant buildup/washoff modeled. |
| **Initial Buildup** | Click the ellipsis button (or press Enter) to specify initial quantities of pollutant buildup over the subcatchment. |
| **Curb Length** | Total length of curbs in the subcatchment (any length units). Used only when pollutant buildup is normalized to curb length. |
| **N-Perv Pattern** | Name of optional monthly pattern that adjusts pervious Manning's n. |
| **Dstore Pattern**| Name of optional monthly pattern that adjusts depression storage. |
| **Infil. Pattern**| Name of optional monthly pattern that adjusts infiltration rate. |

¹ An initial estimate of the characteristic width is given by the subcatchment area divided by the average maximum overland flow length. The maximum overland flow length is the length of the flow path from the furthest drainage point of the subcatchment before the flow becomes channelized. Maximum lengths from several different possible flow paths should be averaged. These paths should reflect slow flow, such as over pervious surfaces, more than rapid flow over pavement, for example. Adjustments should be made to the width parameter to produce good fits to measured runoff hydrographs.

## B.3 Junction Properties

| Name | Description |
| :--- | :--- |
| **Name** | User-assigned junction name. |
| **X-Coordinate** | Horizontal location of the junction on the Study Area Map. If left blank then the junction will not appear on the map. |
| **Y-Coordinate** | Vertical location of the junction on the Study Area Map. If left blank then the junction will not appear on the map. |
| **Description** | Click the ellipsis button (or press Enter) to edit an optional description of the junction. |
| **Tag** | Optional label used to categorize or classify the junction. |
| **Inflows** | Click the ellipsis button (or press Enter) to assign external direct, dry weather or RDII inflows to the junction. |
| **Treatment** | Click the ellipsis button (or press Enter) to edit a set of treatment functions for pollutants entering the node. |
| **Invert El.** | Invert elevation of the junction (feet or meters). |
| **Max. Depth** | Maximum depth of junction (i.e., from ground surface to invert) (feet or meters). If zero, then the distance from the invert to the top of the highest connecting link will be used. |
| **Initial Depth**| Depth of water at the junction at the start of the simulation (feet or meters). |
| **Surcharge Depth** | Additional depth of water beyond the maximum depth that the junction can sustain before overflowing (feet or meters). This parameter can be used to simulate bolted manhole covers or force main connections. |
| **Ponded Area** | Area occupied by ponded water atop the junction after flooding occurs (sq. feet or sq. meters). If the **Allow Ponding** simulation option is turned on, a non-zero value of this parameter will allow ponded water to be stored and subsequently returned to the conveyance system when capacity exists. |

## B.4 Outfall Properties

| Name | Description |
| :--- | :--- |
| **Name** | User-assigned outfall name. |
| **X-Coordinate** | Horizontal location of the outfall on the Study Area Map. If left blank then the outfall will not appear on the map. |
| **Y-Coordinate** | Vertical location of the outfall on the Study Area Map. If left blank then the outfall will not appear on the map. |
| **Description** | Click the ellipsis button (or press Enter) to edit an optional description of the outfall. |
| **Tag** | Optional label used to categorize or classify the outfall. |
| **Inflows** | Click the ellipsis button (or press Enter) to assign external direct, dry weather or RDII inflows to the outfall. |
| **Treatment** | Click the ellipsis button (or press Enter) to edit a set of treatment functions for pollutants entering the node. |
| **Invert El.** | Invert elevation of the outfall (feet or meters). |
| **Tide Gate** | **YES** - tide gate present to prevent backflow<br>**NO** - no tide gate present |
| **Route To** | Optional name of a subcatchment that receives the outfall's discharge. |
| **Type** | Type of outfall boundary condition:<br>**FREE:** outfall stage determined by minimum of critical flow depth and normal flow depth in the connecting conduit<br>**NORMAL:** outfall stage based on normal flow depth in connecting conduit<br>**FIXED:** outfall stage set to a fixed value<br>**TIDAL:** outfall stage given by a table of tide elevation versus time of day<br>**TIMESERIES:** outfall stage supplied from a time series of elevations. |
| **Fixed Stage** | Water elevation for a **FIXED** type of outfall (feet or meters). |
| **Tidal Curve Name** | Name of the Tidal Curve relating water elevation to hour of the day for a **TIDAL** outfall (double-click to edit the curve). |
| **Time Series Name** | Name of time series containing time history of outfall elevations for a **TIMESERIES** outfall (double-click to edit the series). |

## B.5 Flow Divider Properties

| Name | Description |
| :--- | :--- |
| **Name** | User-assigned divider name. |
| **X-Coordinate** | Horizontal location of the divider on the Study Area Map. If left blank then the divider will not appear on the map. |
| **Y-Coordinate** | Vertical location of the divider on the Study Area Map. If left blank then the divider will not appear on the map. |
| **Description** | Click the ellipsis button (or press Enter) to edit an optional description of the divider. |
| **Tag** | Optional label used to categorize or classify the divider. |
| **Inflows** | Click the ellipsis button (or press Enter) to assign external direct, dry weather or RDII inflows to the divider. |
| **Treatment** | Click the ellipsis button (or press Enter) to edit a set of treatment functions for pollutants entering the node. |
| **Invert El.** | Invert elevation of the divider (feet or meters). |
| **Max. Depth** | Maximum depth of divider (i.e., from ground surface to invert) (feet or meters). See description for **Junctions**. |
| **Initial Depth**| Depth of water at the divider at the start of the simulation (feet or meters). |
| **Surcharge Depth** | Additional depth of water beyond the maximum depth that the divider can sustain before overflowing (feet or meters). |
| **Ponded Area** | Area occupied by ponded water atop the junction after flooding occurs (sq. feet or sq. meters). See description for **Junctions**. |
| **Diverted Link**| Name of link which receives the diverted flow. |
| **Type** | Type of flow divider. Choices are:<br>**CUTOFF** (diverts all inflow above a defined cutoff value),<br>**OVERFLOW** (diverts all inflow above the flow capacity of the non-diverted link),<br>**TABULAR** (uses a Diversion Curve to express diverted flow as a function of the total inflow),<br>**WEIR** (uses a weir equation to compute diverted flow). |
| **CUTOFF DIVIDER**| |
| - *Cutoff Flow* | Cutoff flow value used for a **CUTOFF** divider (flow units). |
| **TABULAR DIVIDER** | |
| - *Curve Name* | Name of Diversion Curve for a **TABULAR** divider (double-click to edit). |
| **WEIR DIVIDER** | |
| - *Min. Flow* | Minimum flow at which diversion begins for a **WEIR** divider (flow units). |
| - *Max. Depth* | Vertical height of **WEIR** opening (feet or meters) |
| - *Coefficient* | Product of **WEIR's** discharge coefficient and its length. Weir coefficients are typically in the range of 2.65 to 3.10 per foot, for flows in CFS. |

**Note:** Flow dividers are operational only for Steady Flow and Kinematic Wave flow routing. For Dynamic Wave flow routing they behave as Junction nodes.

## B.6 Storage Unit Properties

| Name | Description |
| :--- | :--- |
| **Name** | User-assigned storage unit name. |
| **X-Coordinate** | Horizontal location of the storage unit on the Study Area Map. If left blank then the storage unit will not appear on the map. |
| **Y-Coordinate** | Vertical location of the storage unit on the Study Area Map. If left blank then the storage unit will not appear on the map. |
| **Description** | Click the ellipsis button (or press Enter) to edit an optional description of the storage unit. |
| **Tag** | Optional label used to categorize or classify the storage unit. |
| **Inflows** | Click the ellipsis button (or press Enter) to assign external direct, dry weather or RDII inflows to the storage unit. |
| **Treatment** | Click the ellipsis button (or press Enter) to edit a set of treatment functions for pollutants within the storage unit. |
| **Invert El.** | Elevation of the bottom of the storage unit (feet or meters). |
| **Max. Depth** | Maximum depth of the storage unit (feet or meters). |
| **Initial Depth**| Initial depth of water in the storage unit at the start of the simulation (feet or meters). |
| **Surcharge Depth** | Additional depth of water above full depth that a storage unit can sustain before overflowing (feet or meters). Only used for covered units. |
| **Evap. Factor** | The fraction of the potential evaporation from the storage unit's water surface that is actually realized. |
| **Seepage Loss** | Click the ellipsis button (or press Enter) to specify optional soil properties that determine seepage loss through the bottom and sloped sides of the storage unit. |
| **Storage Shape** | Click the ellipsis button (or press Enter) to specify the shape of the storage unit by relating surface area to depth. |

## B.7 Conduit Properties

| Name | Description |
| :--- | :--- |
| **Name** | User-assigned conduit name. |
| **Inlet Node** | Name of node on the inlet end of the conduit (normally the end at higher elevation). |
| **Outlet Node** | Name of node on the outlet end of the conduit (normally the end at lower elevation). |
| **Description** | Click the ellipsis button (or press Enter) to edit an optional description of the conduit. |
| **Tag** | Optional label used to categorize or classify the conduit. |
| **Shape** | Click the ellipsis button (or press Enter) to edit the geometric properties of the conduit's cross-section. |
| **Max. Depth** | Maximum depth of the conduit's cross-section (feet or meters). |
| **Length** | Conduit length (feet or meters). |
| **Roughness** | Manning's roughness coefficient (n) (see Section A.7 for closed conduit values or Section A.8 for open channel values). |
| **Inlet Offset** | Depth or elevation of the conduit invert above the node invert at the upstream end of the conduit (feet or meters). See note below. |
| **Outlet Offset** | Depth or elevation of the conduit invert above the node invert at the downstream end of the conduit (feet or meters). See note below. |
| **Initial Flow** | Initial flow in the conduit (flow units). |
| **Maximum Flow** | Maximum flow allowed in the conduit (flow units) – use 0 or leave blank if not applicable. |
| **Entry Loss Coeff.** | Head loss coefficient associated with energy losses at the entrance of the conduit. For culverts, refer to Table A11. |
| **Exit Loss Coeff.** | Head loss coefficient associated with energy losses at the exit of the conduit. For culverts, use a value of 1.0 |
| **Avg. Loss Coeff.** | Head loss coefficient associated with energy losses along the length of the conduit. |
| **Seepage Loss Rate**| Rate of seepage loss into surrounding soil (inches or millimeters per hour). |
| **Flap Gate** | **YES** if a flap gate exists that prevents backflow through the conduit, or **NO** if no flap gate exists. |
| **Culvert Code** | If the conduit is a culvert subject to possible inlet flow control click the ellipsis button (or press Enter) to select a code number for its inlet geometry from those listed in Appendix A10 |
| **Inlets** | Click the ellipsis button (or press Enter) to assign a storm drain inlet to a street or open channel conduit. |

**NOTE:** Conduits and flow regulators (orifices, weirs, and outlets) can be offset some distance above the invert of their connecting end nodes. There are two different conventions available for specifying the location of these offsets. The Depth convention uses the offset distance from the node's invert (distance between ① and ② in the figure on the right). The Elevation convention uses the absolute elevation of the offset location (the elevation of point ① in the figure). The choice of convention can be made on the Status Bar of SWMM's main window or on the Node/Link Properties page of the Project Defaults dialog.

![Conduit offset diagram](../../Manual/images/conduit-offset.png)

## B.8 Pump Properties

| Name | Description |
| :--- | :--- |
| **Name** | User-assigned pump name. |
| **Inlet Node** | Name of node on the inlet side of the pump. |
| **Outlet Node** | Name of node on the outlet side of the pump. |
| **Description** | Click the ellipsis button (or press Enter) to edit an optional description of the pump. |
| **Tag** | Optional label used to categorize or classify the pump. |
| **Pump Curve** | Name of the Pump Curve which contains the pump's operating data (double-click to edit the curve). Enter * for an Ideal pump. |
| **Initial Status** | Status of the pump (**ON** or **OFF**) at the start of the simulation. |
| **Startup Depth** | Depth at inlet node when pump turns on (feet or meters). Enter 0 if not applicable. |
| **Shutoff Depth** | Depth at inlet node when pump shuts off (feet or meters). Must be lower than the Startup Depth. Enter 0 if not applicable. |

## B.9 Orifice Properties

| Name | Description |
| :--- | :--- |
| **Name** | User-assigned orifice name. |
| **Inlet Node** | Name of node on the inlet side of the orifice. |
| **Outlet Node** | Name of node on the outlet side of the orifice. |
| **Description** | Click the ellipsis button (or press Enter) to edit an optional description of the orifice. |
| **Tag** | Optional label used to categorize or classify the orifice. |
| **Type** | Type of orifice (**SIDE** or **BOTTOM**). |
| **Shape** | Orifice shape (**CIRCULAR** or **RECT_CLOSED**). |
| **Height** | Height of orifice opening when fully open (feet or meters). Corresponds to the diameter of a circular orifice or the height of a rectangular orifice. |
| **Width** | Width of rectangular orifice when fully opened (feet or meters). |
| **Inlet Offset** | Depth or elevation of bottom of orifice above invert of inlet node (feet or meters – see note below table of Conduit Properties). |
| **Discharge Coeff.** | Discharge coefficient (unitless). A typical value is 0.65. |
| **Flap Gate** | **YES** if the orifice has a flap gate that prevents backflow, **NO** otherwise. |
| **Time to Open / Close** | The time it takes to open a closed (or close an open) gated orifice in decimal hours. Use 0 or leave blank if timed openings/closings do not apply. Use Control Rules to adjust gate position. |

## B.10 Weir Properties

| Name | Description |
| :--- | :--- |
| **Name** | User-assigned weir name. |
| **Inlet Node** | Name of node on inlet side of weir. |
| **Outlet Node** | Name of node on outlet side of weir. |
| **Description** | Click the ellipsis button (or press Enter) to edit an optional description of the weir. |
| **Tag** | Optional label used to categorize or classify the weir. |
| **Type** | Weir type: **TRANSVERSE**, **SIDEFLOW**, **V-NOTCH**, **TRAPEZOIDAL** or **ROADWAY**. |
| **Height** | Vertical height of weir opening (feet or meters). |
| **Length** | Horizontal length of weir opening (feet or meters). |
| **Side Slope** | Slope (run/rise) of side walls for a **V-NOTCH** or **TRAPEZOIDAL** weir. |
| **Inlet Offset** | Depth or elevation of bottom of weir opening from invert of inlet node (feet or meters – see note below table of Conduit Properties). |
| **Discharge Coeff.¹** | Discharge coefficient for flow through the central portion of the weir (for flow in CFS when using US units or CMS when using SI units). |
| **Flap Gate** | **YES** if the weir has a flap gate that prevents backflow, **NO** otherwise. |
| **End Contractions** | Number of end contractions for a **TRANSVERSE** or **TRAPEZOIDAL** weir whose length is shorter than the channel it is placed in. Values will be either **0**, **1**, or **2** depending if no ends, one end, or both ends are beveled in from the side walls. |
| **End Coeff.** | Discharge coefficient for flow through the triangular ends of a **TRAPEZOIDAL** weir. See the recommended values for V-notch weirs. |
| **Can Surcharge** | **YES** if the weir can surcharge (have an upstream water level higher than the height of the opening) or **NO** if it cannot. |
| **Coeff. Curve** | Name of an optional Weir Curve that allows the central Discharge Coeff. to vary with head (ft or m) across the weir. Does not apply to Roadway weirs. |
| **ROADWAY WEIR** | (used only for Roadway weirs) |
| **Road Width** | Width of roadway and shoulders (feet or meters) |
| **Road Surface** | Type of road surface: **PAVED** or **GRAVEL**. |

¹ Typical values are: 3.33 US (1.84 SI) for sharp crested transverse weirs, 2.5 - 3.3 US (1.38 - 1.83 SI) for broad crested rectangular weirs, 2.4 - 2.8 US (1.35 - 1.55 SI) for V-notch (triangular) weirs.

## B.11 Outlet Properties

| Name | Description |
| :--- | :--- |
| **Name** | User-assigned outlet name. |
| **Inlet Node** | Name of node on inflow side of outlet. |
| **Outlet Node** | Name of node on discharge side of outlet. |
| **Description** | Click the ellipsis button (or press Enter) to edit an optional description of the outlet. |
| **Tag** | Optional label used to categorize or classify the outlet. |
| **Inlet Offset** | Depth or elevation of outlet above inlet node invert (feet or meters – see note below table of Conduit Properties). |
| **Flap Gate** | **YES** if the outlet has a flap gate that prevents backflow, **NO** otherwise. |
| **Rating Curve** | Method of defining flow (Q) as a function of depth or head (y) across the outlet.<br>**FUNCTIONAL/DEPTH** uses a power function (Q = Ay<sup>B</sup>) to describe this relation where y is the depth of water above the outlet's opening at the inlet node.<br>**FUNCTIONAL/HEAD** uses the same power function except that y is the difference in head across the outlet's nodes.<br>**TABULAR/DEPTH** uses a tabulated curve of flow versus depth of water above the outlet's opening at the inlet node.<br>**TABULAR/HEAD** uses a tabulated curve of flow versus difference in head across the outlet's nodes. |
| **FUNCTIONAL** | (used only for a functional rating curve) |
| - *Coefficient* | Coefficient (A) for the functional relationship between depth or head and flow rate. |
| - *Exponent* | Exponent (B) used for the functional relationship between depth or head and flow rate. |
| **TABULAR** | (used only for a tabular rating curve) |
| - *Curve Name* | Name of Rating Curve containing the relationship between depth or head and flow rate (double-click to edit the curve). |

## B.12 Map Label Properties

| Name | Description |
| :--- | :--- |
| **Text** | Text of label. |
| **X-Coordinate** | Horizontal location of the upper-left corner of the label on the Study Area Map. |
| **Y-Coordinate** | Vertical location of the upper-left corner of the label on the Study Area Map. |
| **Anchor Node** | Name of node (or subcatchment) that anchors the label's position when the map is zoomed in (i.e., the pixel distance between the node and the label remains constant). Leave blank if anchoring is not used. |
| **Font** | Click the ellipsis button (or press Enter) to modify the font used to draw the label. |