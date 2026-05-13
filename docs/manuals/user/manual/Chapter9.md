# Chapter 9 VIEWING RESULTS

This chapter describes the different ways in which the results of a simulation can be viewed. These include a status report, a summary report, various map views, graphs, tables, and a statistical frequency report.

## 9.1 Viewing a Status Report

A Status Report is available for viewing after each simulation. It contains:

*   a summary of the main Simulation Options that are in effect
*   a list of any error and warning conditions encountered during the run
*   a summary listing of the project's input data (if requested in the Simulation Options)
*   a summary of the data read from each rainfall file used in the simulation
*   a description of each control rule action taken during the simulation (if requested in the Simulation Options)
*   the system-wide mass continuity errors for:
    *   runoff quantity and quality
    *   groundwater flow
    *   conveyance system flow and water quality
*   the names of the nodes with the highest individual flow continuity errors
*   the names of the conduits that most often determined the size of the time step used for flow routing (only when the Variable Time Step option is used)
*   the names of the links with the highest Flow Instability Index values
*   the names of the nodes with the highest frequency of non-convergence
*   information on the range of routing time steps taken and the percentage of these that were considered steady state.

To view the Status Report select **Report >> Status** from the Main Menu or click the report button on the Main Toolbar and select **Status Report** from the drop-down menu that appears.

To copy selected text from the Status Report to a file or to the Windows Clipboard, first select the text to copy with the mouse and then choose **Edit >> Copy To** from the Main Menu (or press the copy button on the Main Toolbar).

To save both the entire Status Report and Summary Report (discussed next) to file, select **File >> Export >> Status/Summary Report** from the Main Menu.

## 9.2 Viewing Summary Results

SWMM's Summary Results report lists summary results for each subcatchment, node, and link in the project through a selectable list of tables. To view the various summary results tables, select **Report >> Summary** from the Main Menu or click the report button on the Main Toolbar and select **Summary Results** from the drop-down menu that appears. The Summary Results window looks as follows:

![Summary Results window showing Subcatchment Runoff.](../../Manual/images/summary-results.png)

The drop-down box at the upper left allows you to choose the type of results to view. The selection of tables and the results they display are as follows (items marked with an asterisk can also be viewed as color coded themes on the Study Area Map by selecting them from the Map Browser – see Section 7.1):

| Table | Columns |
| :--- | :--- |
| **Subcatchment Runoff** | Total precipitation (in or mm)\*<br>Total run-on from other subcatchments (in or mm)<br>Total evaporation (in or mm)\*<br>Total infiltration (in or mm)\*<br>Total runoff depth from impervious areas (in or mm)<br>Total runoff depth from pervious areas (in or mm)<br>Total runoff depth (in or mm)\*<br>Total runoff volume (million gallons or million liters)<br>Peak runoff (flow units)\*<br>Runoff coefficient (ratio of total runoff to total precipitation)\* |
| **LID Performance** | Total inflow volume<br>Total evaporation loss<br>Total infiltration loss<br>Total surface outflow<br>Total underdrain outflow<br>Initial storage volume<br>Final storage volume<br>Flow continuity error (%)<br>*Note: all quantities are expressed as depths (in or mm) over the LID unit's surface area.* |
| **Groundwater Summary** | Total surface infiltration (in or mm)<br>Total evaporation (in or mm)<br>Total lower seepage (in or mm)<br>Total lateral outflow (in or mm)<br>Maximum lateral outflow (flow units)<br>Average upper zone moisture content (volume fraction)<br>Average water table elevation (ft or m)<br>Final upper zone moisture content (volume fraction)<br>Final water table elevation (ft or m) |
| **Subcatchment Washoff**| Total mass of each pollutant washed off the subcatchment (lbs or kg) |
| **Node Depth** | Average water depth (ft or m)<br>Maximum water depth (ft or m)\*<br>Maximum hydraulic head (HGL) (ft or m)\*<br>Time of maximum depth<br>Maximum water depth at reporting times (ft or m) |
| **Node Inflow** | Maximum lateral inflow (flow units)\*<br>Maximum total inflow (flow units)<br>Time of maximum total inflow<br>Total lateral inflow volume (million gallons or million liters)\*<br>Total inflow volume (million gallons or million liters)<br>Flow balance error (%)<br>*Note: Total inflow consists of lateral inflow plus inflow from connecting links.* |
| **Node Surcharge** | Hours surcharged<br>Maximum height of surcharge above node's crown (ft or m)<br>Minimum depth of surcharge below node's top rim (ft or m)<br>*Note: surcharging occurs when water rises above the crown of the highest conduit and only those conduits that surcharge are listed.* |
| **Node Flooding** | Hours flooded\*<br>Maximum flooding rate (flow units)\*<br>Time of maximum flooding<br>Total flood volume (million gallons or million liters)\*<br>Peak depth (for dynamic wave routing in ft or m) or peak volume (1000 ft³ or 1000 m³) of ponded surface water<br>*Note: flooding refers to all water that overflows a node, whether it ponds or not, and only those nodes that flood are listed.* |
| **Storage Volume** | Average volume of water in the facility (1000 ft³ or 1000 m³)<br>Average percent of full storage capacity utilized<br>Percent of total stored volume lost to evaporation<br>Percent of total stored volume lost to seepage<br>Maximum volume of water in the facility (1000 ft³ or 1000 m³)<br>Maximum percent of full storage capacity utilized<br>Time of maximum water stored<br>Maximum outflow rate from the facility (flow units) |
| **Outfall Loading** | Percent of time that outfall discharges<br>Average discharge flow (flow units)<br>Maximum discharge flow (flow units)<br>Total volume of flow discharged (million gallons or million liters)<br>Total mass discharged of each pollutant (lbs or kg) |
| **Street Flow (Street Conduits Only)** | Peak flow (flow units)<br>Maximum spread from curb (ft or m)<br>Maximum depth at curb (ft or m)<br>For streets with assigned inlets:<ul><li>name of inlet structure</li><li>inlet location (on-grade or on-sag)</li><li>peak flow capture efficiency (%)</li><li>average flow capture efficiency (%)</li><li>frequency of bypass flow (%)</li><li>frequency of backflow (%)</li></ul> |
| **Link Flow** | Maximum flow (flow units)\*<br>Time of maximum flow<br>Maximum velocity (ft/sec or m/sec)\*<br>Ratio of maximum flow to full normal flow<br>Ratio of maximum flow depth to full depth\* |
| **Flow Classification (Dynamic Wave Routing Only)** | Ratio of adjusted conduit length to actual length<br>Fraction of all time steps spent in the following flow categories:<ul><li>dry on both ends</li><li>dry on the upstream end</li><li>dry on the downstream end</li><li>subcritical flow</li><li>supercritical flow</li><li>critical flow at the upstream end</li><li>critical flow at the downstream end</li></ul>Fraction of all time steps flow is limited to normal flow<br>Fraction of all time steps flow is inlet controlled (for culverts only) |
| **Conduit Surcharge** | Hours that conduit is full at:<ul><li>both ends\*</li><li>upstream end</li><li>downstream end</li></ul>Hours that conduit flows above full normal flow<br>Hours that conduit is capacity limited\*<br>*Note: only conduits with one or more non-zero entries are listed and a conduit is considered capacity limited if its upstream end is full and the HGL slope is greater than the conduit slope.* |
| **Link Pollutant Loads**| Total mass load (in lbs or kg) of each pollutant carried by the link over the entire simulation period |
| **Pumping** | Percent of time that the pump is on line<br>Number of pump start-ups<br>Minimum flow pumped (flow units)<br>Average flow pumped (flow units)<br>Maximum flow pumped (flow units)<br>Total volume pumped (million gallons or million liters)<br>Total energy consumed assuming 100% efficiency (Kw-hrs)<br>Percent of time that the pump operates below its pump curve<br>Percent of time that the pump operates above its pump curve |

> **Note:** The summary results displayed in these tables are based on results found at every computational time step and not just on the results from each reporting time step.

Clicking on the name of an object in the first column of the table will locate that object both in the Project Browser and on the Study Area Map. Clicking on a column heading will sort the entries in the table by the values in that column (alternating between ascending and descending order with each click).

Selecting **Edit >> Copy To** from the Main Menu or clicking the copy icon on the Main Toolbar will allow you to copy the contents of the table to either the Windows Clipboard or to a file. To save both the entire Status Report and all tables of the Summary Report to a file select **File >> Export >> Status/Summary Report** from the Main Menu.

## 9.3 Time Series Results

Computed results at each reporting time step for the variables listed in Table 9-1 are available for viewing on the map and can be plotted, tabulated, and statistically analyzed. These variables can be viewed only for those subcatchments, nodes, and links that were selected to have detailed time series results saved for them. This normally includes all objects in the project unless the Reporting option (under the Options category in the Project Browser) was used to select specific objects to report on.

**Table 9-1 Time series variables available for viewing**

**Subcatchment Variables**
*   rainfall rate (in/hr or mm/hr)
*   snow depth (in or mm)
*   evaporation loss (in/day or mm/day)
*   infiltration loss (in/hr or mm/hr)
*   runoff flow (flow units)
*   groundwater flow into the drainage network (flow units)
*   groundwater elevation (ft or m)
*   soil moisture in the unsaturated groundwater zone (volume fraction)
*   washoff concentration of each pollutant (mass/liter)

**Node Variables**
*   water depth (ft or m above the node invert elevation)
*   hydraulic head (ft or m, absolute elevation per vertical datum)
*   stored water volume (including ponded water, ft³ or m³)
*   lateral inflow (runoff + all other external inflows, in flow units)
*   total inflow (lateral inflow + upstream conduit inflows, in flow units)
*   surface flooding (excess overflow when the node is at full depth, in flow units)
*   concentration of each pollutant after any treatment applied at the node (mass/liter)

**Link Variables**
*   flow rate (flow units)
*   average water depth (ft or m)
*   flow velocity (ft/sec or m/sec)
*   volume of water (ft³ or m³)
*   capacity (fraction of full area filled by flow for conduits; control setting for pumps and regulators)
*   concentration of each pollutant (mass/liter)

**System-Wide Variables**
*   air temperature (degrees F or C)
*   potential evaporation (in/day or mm/day)
*   actual evaporation (in/day or mm/day)
*   total rainfall (in/hr or mm/hr)
*   total snow depth (in or mm)
*   average losses (in/hr or mm/hr)
*   total runoff flow (flow units)
*   total dry weather inflow (flow units)
*   total groundwater inflow (flow units)
*   total RDII inflow (flow units)
*   total direct inflow (flow units)
*   total external inflow (flow units)
*   total external flooding (flow units)
*   total outflow from outfalls (flow units)
*   total nodal storage volume (ft³ or m³)

## 9.4 Viewing Results on the Map

There are several ways to view the values of certain input parameters and simulation results directly on the Study Area Map:
*   For the current settings on the Map Browser, the subcatchments, nodes and links of the map will be colored according to their respective Map Legends. The map's color coding will be updated as a new time period is selected in the Map Browser.
*   When the Flyover Map Labeling program preference is selected (see Section 4.10), moving the mouse over any map object will display its ID name and the value of its current theme parameter in a hint-style box.
*   ID names and parameter values can be displayed next to all subcatchments, nodes and/or links by selecting the appropriate options on the Annotation page of the Map Options dialog (see Section 7.12).
*   Subcatchments, nodes or links meeting a specific criterion can be identified by submitting a Map Query (see Section 7.9).
*   One can animate the display of results on the network map either forward or backward in time by using the controls on the Animator panel of the Map Browser (see Section 4.8).
*   The map can be printed, copied to the Windows clipboard, or saved as a DXF file or Windows metafile (see Section 7.13).

## 9.5 Viewing Results with a Graph

Analysis results can be viewed using several different types of graphs. Graphs can be printed, copied to the Windows clipboard, or saved to a text file or to a Windows metafile. The following types of graphs can be created from available simulation results:

*   **Time Series Plot:**
    ![Time Series Plot example showing flow over elapsed time.](../../Manual/images/time-series-plot.png)

*   **Profile Plot:**
    ![Profile Plot example showing water elevation over distance.](../../Manual/images/profile-plot.png)

*   **Scatter Plot:**
    ![Scatter Plot example showing Link Flow versus Node Depth.](../../Manual/images/scatter-plot.png)

You can zoom in or out of any graph by holding down the `<Shift>` key while drawing a zoom rectangle with the mouse's left button held down. Drawing the rectangle from left to right zooms in, drawing it from right to left zooms out. The plot can also be panned in any direction by moving the mouse across the plot with the left button held down.

An opened graph will normally be redrawn when a new simulation is run. To prevent the automatic updating of a graph once a new set of results is computed you can lock the current graph by clicking the lock icon in the upper left corner of the graph. To unlock the graph, click the icon again.

### 9.5.1 Time Series Plots

A Time Series Plot graphs the values over time of specific combinations of objects and variables. Up to six time series can be plotted on the same graph. When only a single time series is plotted, and that item has calibration data registered for the plotted variable, then the calibration data will be plotted along with the simulated results (see Section 5.7.2 for instructions on how to register calibration data with a project).

To create a Time Series Plot, select **Report >> Graph >> Time Series** from the Main Menu or click the time series graph icon on the Main Toolbar. A Time Series Plot Selection dialog will appear. Use it to describe what objects and quantities should be plotted.

![Time Series Plot Selection dialog box.](../../Manual/images/time-series-plot-selection.png)

The Time Series Plot Selection dialog selects a set of objects and their variables whose computed time series will be graphed in a Time Series Plot. The dialog is used as follows:

1.  Select a Start Date and End Date for the plot (the default is the entire simulation period).
2.  Choose whether to show time as Elapsed Time or as Date/Time values.
3.  Add up to six different data series to the plot by clicking the **Add** button above the data series list box.
4.  Use the **Edit** button to make changes to a selected data series or the **Delete** button to delete a data series.
5.  Use the **Up** and **Down** buttons to change the order in which the data series will be plotted.
6.  Click the **OK** button to create the plot.

When you click the **Add** or **Edit** buttons a Data Series Selection dialog will be displayed for selecting a particular object and variable to plot. It contains the following data fields:

![Data Series Selection dialog box.](../../Manual/images/data-series-selection.png)

*   **Object Type:** The type of object to plot (Subcatchment, Node, Link or System).
*   **Object Name:** The ID name of the object to be plotted. (This field is disabled for System variables).
*   **Variable:** The variable whose time series will be plotted (choices vary by object type).
*   **Legend Label:** The text to use in the legend for the data series. If left blank, a default label made up of the object type, name, variable and units will be used (e.g. Link C16 Flow (CFS)).
*   **Axis:** Whether to use the left or right vertical axis to plot the data series.

> As you select objects on the Study Area Map or in the Project Browser their types and ID names will automatically appear in this dialog.

Click the **Accept** button to add/update the data series into the plot or click the **Cancel** button to disregard your edits. You will then be returned to the Time Series Plot Selection dialog where you can add or edit another data series.

> To make a precipitation time series display in inverted fashion on a plot, assign it to the right axis and after the plot is displayed, use the Graph Options Dialog (see Section 9.6) to invert the right axis and expand the scales of both the left and right axes (so it doesn't overlap another data series).

### 9.5.2 Profile Plots

A Profile Plot displays the variation in simulated water depth with distance over a connected path of drainage system links and nodes at a particular point in time. Once the plot has been created it will be automatically updated as a new time period is selected using the Map Browser.

To create a Profile Plot:

1.  Select **Report >> Graph >> Profile** from the main menu or press the profile plot icon on the Main Toolbar.
2.  A Profile Plot Selection dialog will appear (see below). Use it to identify the path along which the profile plot is to be drawn.

![Profile Plot Selection dialog box.](../../Manual/images/profile-plot-selection.png)

The Profile Plot Selection dialog is used to select a path of connected conveyance system links along which a water depth profile versus distance should be drawn. To define a path using the dialog:

1.  Enter the ID of the upstream node of the first link in the path in the **Start Node** edit field (or click on the node on the Study Area Map and then on the **+** button next to the edit field).
2.  Enter the ID of the downstream node of the last link in the path in the **End Node** edit field (or click the node on the map and then click the **+** button next to the edit field).
3.  Click the **Find Path** button to have the program automatically identify the path with the smallest number of links between the start and end nodes. These will be listed in the **Links in Profile** box.
4.  You can insert a new link into the Links in Profile list by selecting the new link either on the Study Area Map or in the Project Browser and then clicking the **+** button underneath the Links in Profile list box.
5.  Entries in the Links in Profile list can be deleted or rearranged by using the **-**, **up arrow**, and **down arrow** buttons underneath the list box.
6.  Click the **OK** button to view the profile plot.

To save the current set of links listed in the dialog for future use:

1.  Click the **Save Current Profile** button.
2.  Supply a name for the profile when prompted.

To use a previously saved profile:

1.  Click the **Use Saved Profile** button.
2.  Select the profile to use from the Profile Selection dialog that appears.

Profile plots can also be created before any simulation results are available, to help visualize and verify the vertical layout of a drainage system. Plots created in this manner will contain a refresh button in the upper left corner that can be used to redraw the plot after edits are made to any elevation data appearing in the plot.

### 9.5.3 Scatter Plots

A Scatter Plot displays the relationship between a pair of variables, such as flow rate in a pipe versus water depth at a node. To create a Scatter Plot, select **Report >> Graph >> Scatter** from the main menu or press the scatter plot icon on the Main Toolbar. Then use the Scatter Plot Selection dialog that appears (see below) to specify what time interval and what pair of objects and their variables to plot.

![Scatter Plot Selection dialog box.](../../Manual/images/scatter-plot-selection.png)

The Scatter Plot Selection dialog is used to select the objects and variables to be graphed against one another in a scatter plot. Use the dialog as follows:

1.  Select a **Start Date** and **End Date** for the plot (the default is the entire simulation period).
2.  Select the following choices for the **X-variable** (the quantity plotted along the horizontal axis):
    a.  Object Category (Subcatchment, Node or Link)
    b.  Object ID (enter a value or click on the object either on the Study Area Map or in the Project Browser and then click the **+** button on the dialog)
    c.  Variable to plot (choices depend on the category of object selected).
3.  Do the same for the **Y-variable** (the quantity plotted along the vertical axis).
4.  Click the **OK** button to create the plot.

## 9.6 Customizing a Graph's Appearance

To customize the appearance of a graph:

1.  Make the graph the active window (click on its title bar).
2.  Select **Report >> Customize** from the Main Menu, or click the customize icon on the Main Toolbar, or right-click on the graph.
3.  Use the **Graph Options** dialog that appears to customize the appearance of a Time Series or Scatter Plot, or use the **Profile Plot Options** dialog for a Profile Plot.

The Graph Options dialog is used to customize the appearance of a time series plot, a scatter plot, or a frequency plot (described in Section 9.8). To use the dialog:

1.  Select from among the four tabbed pages that cover the following categories of options: General, Axes, Legend, and Styles.
2.  Check the **Default** box to use the current settings as defaults for all new graphs as well.
3.  Select **OK** to accept your selections.

### 9.6.1 Graph Options - General

![Graph Options dialog, General tab.](../../Manual/images/graph-options-general.png)

The following options can be set on the General page of the Graph Options dialog box:

| Option | Description |
| :--- | :--- |
| **Panel Color** | Color of the panel that contains the graph |
| **Start Background Color** | Starting gradient color of graph's plotting area |
| **End Background Color** | Ending gradient color of graph's plotting area |
| **View in 3D** | Check if graph should be drawn in 3D |
| **3D Effect Percent** | Degree to which 3D effect is drawn |
| **Main Title** | Text of graph's main title |
| **Font** | Click to set the font used for the main title |

The figure below shows a 3D graph with White as the *Start Background Color* and Sky Blue as the *End Background Color*.

![3D graph of Link C2 Flow.](../../Manual/images/3d-graph-example.png)

### 9.6.2 Graph Options - Axes

The Axes page of the Graph Options dialog box adjust the way that the axes are drawn on a graph. One first selects an axis (Bottom, Left or Right (if present)) to work with and then selects from the following options:

| Option | Description |
| :--- | :--- |
| **Gridlines** | Displays grid lines on the graph. |
| **Inverted** | Inverts the scale of the right vertical axis. |
| **Auto Scale** | Fills in the Minimum, Maximum and Increment boxes with an automatic axis scaling. |
| **Minimum** | Sets the minimum axis value (the minimum data value is shown in parentheses). Can be left blank. |
| **Maximum** | Sets the maximum axis value (the maximum data value is shown in parentheses). Can be left blank. |
| **Increment** | Sets the increment between axis labels. If left blank or set to zero the program will automatically select an increment. |
| **Axis Title** | Text of axis title. |
| **Font** | Click to select a font for the axis title. |

### 9.6.3 Graph Options - Legend

The Legend page of the Graph Options dialog box controls how the legend is displayed on the graph.

| Option | Description |
| :--- | :--- |
| **Position** | Selects where to place the legend. |
| **Color** | Selects color to use for legend background. |
| **Check Boxes**| If selected, check boxes will appear next to each legend entry, allowing one to make the data series visible or invisible on the graph. |
| **Framed** | Places a frame around the legend. |
| **Shadowed** | Places a shadow behind the legend's text. |
| **Transparent**| Makes the legend background transparent. |
| **Visible** | Makes the legend visible. |
| **Symbol Width** | Selects the width used to draw the symbol portion of a legend item, as a percentage of the length of the longest legend label. |

### 9.6.4 Graph Options - Styles

The Styles page of the Graph Options dialog box controls how individual data series (or curves) are displayed on a graph. To use this page:

1.  Select a data series to work with from the **Series** combo box.
2.  Edit the title used to identify this series in the legend.
3.  Click the **Font** button to change the font used for the legend. (Other legend properties are selected on the **Legend** page of the dialog.)
4.  Select a property of the data series you would like to modify (not all properties are available for some types of graphs). The choices are:
    *   Lines
    *   Markers
    *   Patterns
    *   Labels

### 9.6.5 Profile Plot Options Dialog

The Profile Plot Options dialog is used to customize the appearance of a Profile Plot. The dialog contains five pages:

![Profile Plot Options dialog.](../../Manual/images/profile-plot-options.png)

*   **Colors:**
    *   Selects the color to use for the plot window panel, the plot background, a conduit's interior, and the depth of filled water.
*   **Styles:**
    *   Selects to use thick lines or not when drawing conduits and the ground profile.
    *   Selects to display the ground profile or not.
    *   Includes a "Display Conduits Only" check box that provides a closer look at the water levels within conduits by removing all other details from the plot.
*   **Axes:**
    *   Edits the main and axis titles, including their fonts.
    *   Selects to display horizontal and vertical axis grid lines.
*   **Vertical Scale:**
    *   Lets one choose the minimum, maximum, and increment values for the vertical axis scale, or have SWMM set the scale automatically. If the increment field contains 0 or is left blank the program will automatically select an increment to use.
*   **Node Labels:**
    *   Selects to display node ID labels either along the plot's top axis, directly on the plot above the node's crown height, or both.
    *   Selects the length of arrow to draw between the node label and the node's crown on the plot (use 0 for no arrows).
    *   Selects the font size of the node ID labels.

Check the **Default** box to have these options (except the Vertical Scale) apply to all new profile plots when they are first created.

## 9.7 Viewing Results with a Table

Time series results for selected variables and objects can also be viewed in a tabular format. There are two types of formats available:

*   **Table by Object** - tabulates the time series of several variables for a single object (e.g., flow and water depth for a conduit).

    ![Table by Object - Link 7.](../../Manual/images/table-by-object.png)

*   **Table by Variable** - tabulates the time series of a single variable for several objects of the same type (e.g., runoff for a group of subcatchments).

    ![Table by Variable - Subcatchment Runoff.](../../Manual/images/table-by-variable.png)

To create a tabular report:

1.  Select **Report >> Table** from the Main Menu or click the table icon on the Main Toolbar.
2.  Choose the table format (either **By Object** or **By Variable**) from the sub-menu that appears.
3.  Fill in the Table by Object or Table by Variable dialogs to specify what information the table should contain.

The **Table by Object** dialog is used when creating a time series table of several variables for a single object. Use the dialog as follows:

![Table by Object Selection dialog.](../../Manual/images/table-by-object-selection.png)

1.  Select a **Start Date** and **End Date** for the table (the default is the entire simulation period).
2.  Choose whether to show time as **Elapsed Time** or as **Date/Time** values.
3.  Choose an **Object Category** (Subcatchment, Node, Link, or System).
4.  Identify a specific object in the category by clicking the object either on the Study Area Map or in the Project Browser and then clicking the **+** button on the dialog. Only a single object can be selected for this type of table.
5.  Check off the variables to be tabulated for the selected object. The available choices depend on the category of object selected.
6.  Click the **OK** button to create the table.

The **Table by Variable** dialog is used when creating a time series table of a single variable for one or more objects. Use the dialog as follows:

![Table by Variable Selection dialog.](../../Manual/images/table-by-variable-selection.png)

1.  Select a **Start Date** and **End Date** for the table (the default is the entire simulation period).
2.  Choose whether to show time as **Elapsed Time** or as **Date/Time** values.
3.  Choose an **Object Category** (Subcatchment, Node or Link).
4.  Select a simulated variable to be tabulated. The available choices depend on the category of object selected.
5.  Identify one or more objects in the category by successively clicking the object either on the Study Area Map or in the Project Browser and then clicking the **+** button on the dialog.
6.  Click the **OK** button to create the table.

Objects already selected can be deleted, moved up in the order or moved down in the order by clicking the **-**, **up arrow**, and **down arrow** buttons, respectively.

## 9.8 Viewing a Statistics Report

A Statistics Report can be generated from the time series of simulation results. For a given object and variable this report will do the following:

*   segregate the simulation period into a sequence of non-overlapping events, either by day, month, or by flow (or volume) above some minimum threshold value,
*   compute a statistical value that characterizes each event, such as the mean, maximum, or total sum of the variable over the event's time period,
*   compute summary statistics for the entire set of event values (mean, standard deviation and skewness),
*   perform a frequency analysis on the set of event values.

The frequency analysis of event values will determine the frequency at which a particular event value has occurred and will also estimate a return period for each event value. Statistical analyses of this nature are most suitable for long-term continuous simulation runs.

To generate a Statistics Report:

1.  Select **Report >> Statistics** from the Main Menu or click the sigma icon on the Main Toolbar.
2.  Fill in the Statistics Report Selection dialog that appears, specifying the object, variable, and event definition to be analyzed.

![Statistics Report Selection dialog.](../../Manual/images/stats-report-selection.png)

*   **Object Category**
    Select the category of object to analyze (Subcatchment, Node, Link or System).
*   **Object Name**
    Enter the ID name of the object to analyze. Instead of typing in an ID name, you can select the object on the Study Area Map or in the Project Browser and then click the **+** button to select it into the Object Name field.
*   **Variable Analyzed**
    Select the variable to be analyzed. The available choices depend on the object category selected (e.g., rainfall, losses, or runoff for subcatchments; depth, inflow, or flooding for nodes; depth, flow, velocity, or capacity for links; water quality for all categories).
*   **Event Time Period**
    Select the length of the time period that defines an event. The choices are daily, monthly, or event-dependent. In the latter case, the event period depends on the number of consecutive reporting periods where simulation results are above the threshold values defined below.
*   **Statistic**
    Choose an event statistic to be analyzed. The available choices depend on the choice of variable to be analyzed and include such quantities as mean value, peak value, event total, event duration, and inter-event time (i.e., the time interval between the midpoints of successive events). For water quality variables the choices include mean concentration, peak concentration, mean loading, peak loading, and event total load.
*   **Event Thresholds**
    These define minimum values that must be exceeded for an event to occur:
    *   The **Analysis Variable** threshold specifies the minimum value of the variable being analyzed that must be exceeded for a time period to be included in an event.
    *   The **Event Volume** threshold specifies a minimum flow volume (or rainfall volume) that must be exceeded for a result to be counted as part of an event.
    *   **Separation Time** sets the minimum number of hours that must occur between the end of one event and the start of the next event. Events with fewer hours are combined together. This value applies only to event-dependent time periods (not to daily or monthly event periods).
    If a particular type of threshold does not apply, then leave the field blank.

After the choices made on the Statistics Selection dialog form are processed, a Statistics Report is produced as shown below. It consists of four tabbed pages that contain:
*   a table of event summary statistics
*   a table of rank-ordered event periods, including their date, duration, and magnitude
*   a histogram plot of the chosen event statistic
*   an exceedance frequency plot of the event values.

The exceedance frequencies included in the Statistics Report are computed with respect to the number of events that occur, not the total number of reporting periods.

![Statistics report for Subcatchment 1 Precipitation.](../../Manual/images/stats-report.png)