# Chapter 2 QUICK START TUTORIAL

This chapter provides a tutorial on how to use EPA SWMM. If you are not familiar with the elements that comprise a drainage system, and how these are represented in a SWMM model, you might want to review the material in Chapter 3 first.

## 2.1 Example Study Area

In this tutorial we will model the drainage system serving a 12-acre residential area. The system layout is shown in Figure 2-1 and consists of subcatchment areas¹ S1 through S3, storm sewer conduits C1 through C4, and conduit junctions J1 through J4. The system discharges to a creek at the point labeled Out1. We will first go through the steps of creating the objects shown in this diagram on SWMM's study area map and setting the various properties of these objects. Then we will simulate the water quantity and quality response to a 3-inch, 6-hour rainfall event, as well as a continuous, multi-year rainfall record.

![Example Study Area Diagram showing subcatchments, junctions, conduits, and an outfall.](../../Manual/images/Example%20Study%20Area%20Diagram.png)
*Figure 2-1 Example Study Area*

---
¹ A subcatchment is an area of land containing a mix of pervious and impervious surfaces whose runoff drains to a common outlet point, which could be either a node of the drainage network or another subcatchment.

---

## 2.2 Project Setup

Our first task is to create a new SWMM project and make sure that certain default options are selected. Using these defaults will simplify the data entry tasks later on.

1.  Launch EPA SWMM if it is not already running and select **File >> New** from the Main Menu bar to create a new project.
2.  Select **Project >> Defaults** to open the Project Defaults dialog.
3.  On the ID Labels page of the dialog, set the ID Prefixes as shown below. This will make SWMM automatically label new objects with consecutive numbers following the designated prefix.

    ![Project Defaults Dialog - ID Labels Tab](../../Manual/images/Project%20Defaults%20Dialog.png)

4.  On the Subcatchments page of the dialog set the following default values:
    *   **Area**: 4
    *   **Width**: 400
    *   **% Slope**: 0.5
    *   **% Imperv.**: 50
    *   **N-Imperv.**: 0.01
    *   **N-Perv.**: 0.10
    *   **Dstore-Imperv.**: 0.05
    *   **Dstore-Perv**: 0.05
    *   **%Zero-Imperv.**: 25
    *   **Infil. Model**: <click to edit>
        *   **Method**: Modified Green-Ampt
        *   **Suction Head**: 3.5
        *   **Conductivity**: 0.5
        *   **Initial Deficit**: 0.26

5.  On the Nodes/Links page set the following default values:
    *   **Node Invert**: 0
    *   **Node Max. Depth**: 4
    *   **Node Ponded Area**: 0
    *   **Conduit Length**: 400
    *   **Conduit Geometry**: <click to edit>
        *   **Barrels**: 1
        *   **Shape**: Circular
        *   **Max. Depth**: 1.0
    *   **Conduit Roughness**: 0.01
    *   **Flow Units**: CFS
    *   **Link Offsets**: DEPTH
    *   **Routing Model**: Kinematic Wave

6.  Click **OK** to accept these choices and close the dialog. If you wanted to save these choices for all future new projects you could check the Save box at the bottom of the form before accepting it.

Next we will set some map display options so that ID labels and symbols will be displayed as we add objects to the study area map, and links will have direction arrows.

1.  Select **Tools >> Map Display Options** to bring up the Map Options dialog.
2.  Select the Subcatchments page, set the Fill Style to Diagonal and the Symbol Size to 5.
3.  Then select the Nodes page and set the Node Size to 5.
4.  Select the Annotation page and check off the boxes that will display ID labels for Subcatchments, Nodes, and Links. Leave the others un-checked.
5.  Finally, select the Flow Arrows page, select the Filled arrow style, and set the arrow size to 7.
6.  Click the **OK** button to accept these choices and close the dialog.

    ![Map Options Dialog - Subcatchments Tab](../../Manual/images/Map%20Options%20Dialog.png)

Before placing objects on the map we should set its dimensions.

1.  Select **View >> Dimensions** to bring up the Map Dimensions dialog.
2.  You can leave the dimensions at their default values for this example.

Finally, look in the status bar at the bottom of the main window and check that the **Auto-Length** feature is off. If it is on, then click the down arrow button and select "Auto-Length: Off" from the popup menu that appears. Also make sure that the **Offsets** option is set to **Depth**. If set to **Elevation** then click the down arrow button and select "Depth Offsets" from the popup menu that appears.

## 2.3 Drawing Objects

We are now ready to begin adding components to the Study Area Map². We will start with the subcatchments.

1.  Begin by selecting the **Subcatchments** category (under Hydrology) in the Project Browser panel (on the left side of the main window).
2.  Then click the **+** button on the toolbar underneath the object category listing in the Project panel (or select **Project | Add a New Subcatchment** from the main menu). Notice how the mouse cursor changes shape to a pencil when you move it over the map.
3.  Move the mouse to the map location where one of the corners of subcatchment S1 lies and left-click the mouse.
4.  Do the same for the next three corners and then right-click the mouse (or hit the **Enter** key) to close up the rectangle that represents subcatchment S1. You can press the **Esc** key if instead you wanted to cancel your partially drawn subcatchment and start over again. Don't worry if the shape or position of the object isn't quite right. We will go back later and show how to fix this.
5.  Repeat this process for subcatchments S2 and S3³.

Observe how sequential ID labels are generated automatically as we add objects to the map.

Next we will add in the junction nodes and the outfall node that comprise part of the drainage network.

1.  To begin adding junctions, select the **Junctions** category from the Project Browser (under Hydraulics -> Nodes) and click the **+** button or select **Project | Add a New Junction** from the main menu.
2.  Move the mouse to the position of junction J1 and left-click it. Do the same for junctions J2 through J4.
3.  To add the outfall node, select **Outfalls** from the Project Browser, click the **+** button or select **Project | Add a New Outfall** from the main menu, move the mouse to the outfall's location on the map, and left-click. Note how the outfall was automatically given the name **Out1**.

At this point your map should look something like that shown in Figure 2-2.

![Map showing subcatchments and nodes for the example study area.](../../Manual/images/Map%20showing%20subcatchments.png)
*Figure 2-2 Subcatchments and nodes for example study area*

Now we will add the storm sewer conduits that connect our drainage system nodes to one another. (You must have created a link's end nodes as described previously before you can create the link.) We will begin with conduit C1, which connects junction J1 to J2.

1.  Select the **Conduits** category from the Project Browser (under Hydraulics -> Links) and press the **+** button or select **Project | Add a New Conduit** from the main menu. The mouse cursor will change shape to a cross hair when moved onto the map.
2.  Left-click the mouse on junction J1. Note how the mouse cursor changes shape to a pencil.
3.  Move the mouse over to junction J2 (note how an outline of the conduit is drawn as you move the mouse) and left-click to create the conduit. You could have cancelled the operation by either right clicking or by hitting the `<Esc>` key.
4.  Repeat this procedure for conduits C2 through C4.

Although all of our conduits were drawn as straight lines, it is possible to draw a curved link by left-clicking at intermediate points where the direction of the link changes before clicking on the end node.

To complete the construction of our study area schematic we need to add a rain gage.

1.  Select the **Rain Gages** category from the Project Browser panel (under Hydrology) and either click the **+** button or select **Project | Add a New Rain Gage** from the main menu.
2.  Move the mouse over the Study Area Map to where the gage should be located and left-click the mouse.

At this point we have completed drawing the example study area. Your system should look like the one in Figure 2-1. If a rain gage, subcatchment or node is out of position you can move it by doing the following:

1.  If the selection arrow button on the Map Toolbar is not already depressed, click it to place the map in **Object Selection mode**.
2.  Click on the object to be moved.
3.  Drag the object with the left mouse button held down to its new position.

To re-shape a subcatchment's outline:
1.  With the map in Object Selection mode, click on the subcatchment's centroid (indicated by a solid square within the subcatchment) to select it.
2.  Then click the vertex selection button on the Map Toolbar to put the map into **Vertex Selection mode**.
3.  Select a vertex point on the subcatchment outline by clicking on it (note how the selected vertex is indicated by a filled solid square).
4.  Drag the vertex to its new position with the left mouse button held down.
5.  If need be, vertices can be added or deleted from the outline by right-clicking the mouse and selecting the appropriate option from the popup menu that appears.
6.  When finished, click the selection arrow button to return to **Object Selection mode**.

This same procedure can also be used to re-shape a link.

---
² Drawing objects on the map is just one way of creating a project. For large projects it might be more convenient to first construct a SWMM project file external to the program. The project file is a text file that describes each object in a specified format as described in Appendix D of this manual. Data extracted from various sources, such as CAD drawings or GIS files, can be used to create the project file.

³ If you right-click (or press Enter) after adding the first point of a subcatchment's outline, the subcatchment will be shown as just a single point.

---

## 2.4 Setting Object Properties

As visual objects are added to our project, SWMM assigns them a default set of properties. To change the value of a specific property for an object we must select the object into the Property Editor. There are several different ways to do this. If the Editor is already visible, then you can simply click on the object or select it from the Project Browser. If the Editor is not visible then you can make it appear by one of the following actions:
*   double-click the object on the map,
*   or right-click on the object and select **Properties** from the pop-up menu that appears,
*   or select the object from the Project Browser and then click the Browser's **edit** button,
*   or after selecting the object choose **Edit >> Edit Object** from the Main Menu.

![Property Editor for Subcatchment S1](../../Manual/images/Property%20Editor%20for%20Subcatchment.png)

Whenever the Property Editor has the focus you can press the **F1** key to obtain a more detailed description of the properties listed.

Two key properties of our subcatchments that need to be set are the rain gage that supplies rainfall data to the subcatchment and the node of the drainage system that receives runoff from the subcatchment. Since all of our subcatchments utilize the same rain gage, **Gage1**, we can use a shortcut method to set this property for all subcatchments at once:

1.  From the Main Menu select **Edit >>Select All**.
2.  Then select **Edit >> Group Edit** to make a Group Editor dialog appear.
3.  Select **Subcatchment** as the type of object to edit, **Rain Gage** as the property to edit, and type in **Gage1** as the new value.
4.  Click **OK** to change the rain gage of all subcatchments to **Gage1**. A confirmation dialog will appear noting that 3 subcatchments have changed. Select “No” when asked to continue editing.

![Group Editor Dialog for changing the Rain Gage property of subcatchments.](../../Manual/images/Group%20Editor%20Dialog.png)

To set the outlet node of our subcatchments we have to proceed one by one, since these vary by subcatchment:

1.  Double click on subcatchment **S1** or select it from the Project Browser and click the Browser's **edit** button to bring up the Property Editor.
2.  Type **J1** in the Outlet field and press **Enter**. Note how a dotted line is drawn between the subcatchment and the node.
3.  Click on subcatchment **S2** and enter **J2** as its Outlet.
4.  Click on subcatchment **S3** and enter **J3** as its Outlet.

We also wish to represent area S3 as being less developed than the others. Select **S3** into the Property Editor and set its **% Imperviousness** to 25.

The junctions and outfall of our drainage system need to have invert elevations assigned to them. As we did with the subcatchments, select each junction individually into the Property Editor and set its **Invert Elevation** to the value shown below⁴.

| Node  | Invert |
| :---- | :----- |
| J1    | 96     |
| J2    | 90     |
| J3    | 93     |
| J4    | 88     |
| Out1  | 85     |

Only one of the conduits in our example system has a non-default property value. This is conduit **C4**, the outlet pipe, whose diameter should be 1.5 instead of 1 ft. To change its diameter, select conduit **C4** into the Property Editor and set the **Max. Depth** value to **1.5**.

In order to provide a source of rainfall input to our project we need to set the rain gage's properties. Select **Gage1** into the Property Editor and set the following properties:

*   **Rain Format**: INTENSITY
*   **Rain Interval**: 1:00
*   **Data Source**: TIMESERIES
*   **Series Name**: TS1

As mentioned earlier, we want to simulate the response of our study area to a 3-inch, 6-hour design storm. A time series named **TS1** will contain the hourly rainfall intensities that make up this storm. Thus we need to create a time series object and populate it with data. To do this:

1.  From the Project Browser select the **Time Series** category of objects.
2.  Click the **+** button on the Browser to bring up the Time Series Editor dialog⁵.
3.  Enter **TS1** in the Time Series Name field.
4.  Enter the values shown in the dialog on the next page into the **Time** and **Value** columns of the data entry grid (leave the **Date** column blank)⁶.
5.  You can click the **View** button on the dialog to see a graph of the time series values. Click the **OK** button to accept the new time series.

![Time Series Editor dialog populated with hourly rainfall data.](../../Manual/images/Time%20Series%20Editor%20dialog.png)

Having completed the initial design of our example project it is a good idea to give it a title and save our work to a file at this point. To do this:

1.  Select the **Title/Notes** category from the Project Browser and click the **edit** button.
2.  In the Project Title/Notes dialog that appears, enter "Tutorial Example" as the title of our project and click the **OK** button to close the dialog.
3.  From the **File** menu select the **Save As** option.
4.  In the Save As dialog that appears, select a folder and file name under which to save this project. We suggest naming the file `tutorial.inp`. (An extension of .inp will be added to the file name if one is not supplied.)
5.  Click **Save** to save the project to file.

![Title/Notes Editor with "Tutorial Example" entered.](../../Manual/images/TitleNotes%20Editor.png)

The project data are saved to the file in a readable text format. You can view what the file looks like by selecting **Project >> Details** from the main menu. To open our project at some later time, you would select the **Open** command from the **File** menu.

---
⁴ An alternative way to move from one object of a given type to the next in order (or to the previous one) in the Property Editor is to hit the Page Down (or Page Up) key.

⁵ The Time Series Editor can also be launched directly from the Rain Gage Property Editor by selecting the editor's Series Name field and double clicking on it.

⁶ Leaving off the dates for a time series means that SWMM will interpret the time values as hours from the start of the simulation. Otherwise, the time series follows the date/time values specified by the user.

---
## 2.5 Running a Simulation

### Setting Simulation Options

Before analyzing the performance of our example drainage system we need to set some options that determine how the analysis will be carried out. To do this:

1.  From the Project Browser, select the **Options** category and click the **edit** button.
2.  On the **General** page of the Simulation Options dialog that appears, select **Kinematic Wave** as the flow routing method. The infiltration method should already be set to **Modified Green-Ampt**. The **Allow Ponding** option should be unchecked.
3.  On the **Dates** page of the dialog, set the **End Analysis** time to `12:00:00`.
4.  On the **Time Steps** page, set the **Routing Time Step** to `60` seconds.
5.  Click **OK** to close the Simulation Options dialog.

![Simulation Options Dialog - General Tab](../../Manual/images/Simulation%20Options%20Dialog.png)

### Starting a Simulation

We are now ready to run the simulation. To do so, select **Project >> Run Simulation** (or click the lightning bolt button). If there was a problem with the simulation, a Status Report will appear describing what errors occurred. Upon successfully completing a run, there are numerous ways in which to view the results of the simulation. We will illustrate just a few here.

### Viewing the Status Report

The Status Report contains useful information about the quality of a simulation run, including a mass balance on rainfall, infiltration, evaporation, runoff, and inflow/outflow for the conveyance system. To view the report select **Report >> Status** (or click the report icon on the Standard Toolbar and then select **Status Report** from the drop down menu). A portion of the report for the system just analyzed is shown below:

**EPA STORM WATER MANAGEMENT MODEL - VERSION 5.2 (Build 5.2.0)**  
*Tutorial Example*

### Analysis Options

| Parameter | Value |
|-----------|-------|
| **Flow Units** | CFS |
| **Process Models:** | |
| Rainfall/Runoff | YES |
| RDII | NO |
| Snowmelt | NO |
| Groundwater | NO |
| Flow Routing | YES |
| Ponding Allowed | NO |
| Water Quality | NO |
| **Infiltration Method** | MODIFIED GREEN AMPT |
| **Flow Routing Method** | KINWAVE |
| **Starting Date** | JUN-27-2002 00:00:00 |
| **Ending Date** | JUN-27-2002 12:00:00 |
| **Antecedent Dry Days** | 0.0 |
| **Report Time Step** | 00:15:00 |
| **Wet Time Step** | 00:15:00 |
| **Dry Time Step** | 01:00:00 |
| **Routing Time Step** | 60.00 sec |

### Runoff Quantity Continuity

| Component | Volume (acre-feet) | Depth (inches) |
|-----------|-------------------|----------------|
| Total Precipitation | 3.000 | 3.000 |
| Evaporation Loss | 0.000 | 0.000 |
| Infiltration Loss | 1.750 | 1.750 |
| Surface Runoff | 1.246 | 1.246 |
| Final Storage | 0.016 | 0.016 |
| **Continuity Error (%)** | **-0.386** | |

### Flow Routing Continuity

| Component | Volume (acre-feet) | Volume (10^6 gal) |
|-----------|-------------------|-------------------|
| Dry Weather Inflow | 0.000 | 0.000 |
| Wet Weather Inflow | 1.246 | 0.406 |
| Groundwater Inflow | 0.000 | 0.000 |

For the system we just analyzed the report indicates the quality of the simulation is quite good, with negligible mass balance continuity errors for both runoff and routing (-0.39% and 0.03%, respectively, if all data were entered correctly). Also, of the 3 inches of rain that fell on the study area, 1.75 infiltrated into the ground and essentially the remainder became runoff.

### Viewing the Summary Report

The Summary Report contains tables listing summary results for each subcatchment, node and link in the drainage system. Total rainfall, total runoff, and peak runoff for each subcatchment, peak depth and hours flooded for each node, and peak flow, velocity, and depth for each conduit are just some of the outcomes included in the summary report.

To view the Summary Report select **Report | Summary** from the main menu (or click the report icon on the Standard Toolbar and then select **Summary Report** from the drop down menu). The report's window has a drop down list from which you select a particular report to view. For our example, the Node Flooding Summary table indicates there was internal flooding in the system at node J2. The Conduit Surcharge Summary table shows that Conduit C2, just downstream of node J2, was at full capacity and therefore appears to be slightly undersized.

![Summary Results for Node Flooding](../../Manual/images/Summary%20Results%20for%20Node%20Flooding.jpg)

![Summary Results for Conduit Surcharge](../../Manual/images/Summary%20Results%20for%20Conduit%20Surcharge.png)

In SWMM flooding will occur whenever the water surface at a node exceeds the maximum assigned depth. Normally such water will be lost from the system. The option also exists to have this water pond atop the node and be re-introduced into the drainage system when capacity exists to do so.

### Viewing Results on the Map

Simulation results (as well as some design parameters, such as subcatchment area, node invert elevation, and link maximum depth) can be viewed in color-coded fashion on the study area map.

![SWMM Main Window displaying simulation results on the map.](../../Manual/images/SWMM%20Main%20Window%20displaying%20simulation%20results.png)

To view a particular variable in this fashion:
1.  Select the **Map** page of the Browser panel.
2.  Select the variables to view for Subcatchments, Nodes, and Links from the dropdown combo boxes appearing in the Themes panel. As shown above, subcatchment runoff and link flow have been selected for viewing.
3.  The color-coding used for a particular variable is displayed with a legend on the study area map. To toggle the display of a legend, select **View >> Legends**.
4.  To move a legend to another location, drag it with the left mouse button held down.
5.  To change the color-coding and the breakpoint values for different colors, select **View >> Legends >> Modify** and then the pertinent class of object (or if the legend is already visible, simply right-click on it). To view numerical values for the variables being displayed on the map, select **Tools >> Map Display Options** and then select the Annotation page of the Map Options dialog. Use the check boxes for Subcatchment Values, Node Values, and Link Values to specify what kind of annotation to add.
6.  The Date / Time of Day / Elapsed Time controls on the Map Browser can be used to move through the simulation results in time. The map view shown above depicts results at 5 hours and 45 minutes into the simulation.
7.  You can use the controls in the Animator panel of the Map Browser to animate the map display through time. For example, pressing the **play** button will run the animation forward in time.

### Viewing a Time Series Plot

To generate a time series plot of a simulation result:
1.  Select **Report >> Graph >> Time Series** or simply click the graph icon on the Standard Toolbar.
2.  A Time Series Plot Selection dialog will appear. It is used to select the objects and variables to be plotted.

For our example, the Time Series Plot Selection dialog can be used to graph the flow in conduits C1 and C2 as follows:
1.  Click the **Add** button on the dialog to view the Data Series Selection dialog.
2.  Select conduit C1 (either on the map or in the Project Browser) and select **Flow** as the variable to be plotted. Click the **Accept** button to return to the Time Series Plot Selection dialog.
3.  Repeat steps 1 and 2 for conduit C2.
4.  Press **OK** to create the plot which should look like the graph shown below.

![Time Series Plot Selection dialog](../../Manual/images/Time%20Series%20Plot%20Selection.jpg)

![Data Series Selection dialog](../../Manual/images/Data%20Series%20Selection%20dialog.jpg)

![Graph showing flow for Link C1 and Link C2 over time.](../../Manual/images/Graph%20showing%20flow%20for%20Link%20C1%20and%20Link%20C2.png)

After a plot is created you can:
*   customize its appearance by selecting **Report >> Customize** or by clicking the customize button on the Standard Toolbar or by simply right clicking on the plot,
*   copy it to the clipboard and paste it into another application by selecting **Edit >> Copy To** or clicking the copy icon on the Standard Toolbar
*   print it by selecting **File >> Print** or **File >> Print Preview** (use **File >> Page Setup** first to set margins, orientation, etc.).

### Viewing a Profile Plot

SWMM can generate profile plots showing how water surface depth varies across a path of connected nodes and links. Let's create such a plot for the conduits connecting junction J1 to the outfall Out1 of our example drainage system. To do this:
1.  Select **Report >> Graph >> Profile** on the main menu or click the profile plot icon on the main Toolbar.
2.  Either enter **J1** in the **Start Node** field of the Profile Plot Selection dialog or select it on the map or from the Project Browser and click the **+** button next to the field.
3.  Do the same for node **Out1** in the **End Node** field of the dialog.
4.  Click the **Find Path** button. An ordered list of the links forming a connected path between the specified Start and End nodes will be displayed in the **Links in Profile** box. You can edit the entries in this box if need be.

![Profile Plot Selection Dialog](../../Manual/images/Profile%20Plot%20Selection%20Dialog.png)

5.  Click the **OK** button to create the plot, showing the water surface profile as it exists at the simulation time currently selected in the Map Browser (hour 02:45 for the plot shown below).

![Water Elevation Profile Plot from Node J1 to Out1](../../Manual/images/Water%20Elevation%20Profile%20Plot%20from%20Node%20J1%20to%20Out1.png)

As you move through time using the Map Browser or with the Animator control, the water depth profile on the plot will be updated. Observe how node J2 becomes flooded between hours 2 and 3 of the storm event. A Profile Plot's appearance can be customized and it can be copied or printed using the same procedures as for a Time Series Plot.

### Running a Full Dynamic Wave Analysis

In the analysis just run we chose to use the Kinematic Wave method of routing flows through our drainage system. This is an efficient but simplified approach that cannot deal with such phenomena as backwater effects, pressurized flow, flow reversal, and non-dendritic layouts. SWMM also includes a Dynamic Wave routing procedure that can represent these conditions. This procedure, however, requires more computation time, due to the need for smaller time steps to maintain numerical stability.

Most of the effects mentioned above would not apply to our example. However we had one conduit, C2, which flowed full and caused its upstream junction to flood. It could be that this pipe is actually being pressurized and could therefore convey more flow than was computed using Kinematic Wave routing. We would now like to see what would happen if we apply Dynamic Wave routing instead.

To run the analysis with Dynamic Wave routing:
1.  From the Project Browser, select the **Options** category and click the **edit** button.
2.  On the **General** page of the Simulation Options dialog that appears, select **Dynamic Wave** as the flow routing method.
3.  On the **Dynamic Wave** page of the dialog, use the settings shown below⁷.

![Simulation Options Dialog - Dynamic Wave Tab](../../Manual/images/Simulation%20Options%20Dialog%20-%20Dynamic%20Wave%20Tab.png)

4.  Click **OK** to close the form and select **Project >> Run Simulation** (or click the lightning bolt button) to re-run the analysis.

If you look at the Summary Report for this run, you will see that there is no longer any junction flooding and that the peak flow carried by conduit C2 has been increased from 3.52 cfs to 4.04 cfs.

---
⁷ Normally when running a Dynamic Wave analysis, one would also want to reduce the routing time step (on the Time Steps page of the dialog). We will keep it at 60 seconds.

---

## 2.6 Simulating Water Quality

In the next phase of this tutorial we will add water quality analysis to our example project. SWMM has the ability to analyze the buildup, washoff, transport and treatment of any number of water quality constituents. The steps needed to accomplish this are:
1.  Identify the pollutants to be analyzed.
2.  Define the categories of land uses that generate these pollutants.
3.  Set the parameters of buildup and washoff functions that determine the quality of runoff from each land use.
4.  Assign a mixture of land uses to each subcatchment area
5.  Define pollutant removal functions for nodes within the drainage system that contain treatment facilities.

We will now apply each of these steps, with the exception of number 5, to our example project⁸.

We will define two runoff pollutants; total suspended solids (TSS), measured as mg/L, and total Lead, measured in ug/L. In addition, we will specify that the concentration of Lead in runoff is a fixed fraction (0.25) of the TSS concentration. To add these pollutants to our project:

1.  Under the **Quality** category in the project Browser, select the **Pollutants** sub-category beneath it.
2.  Click the **+** button to add a new pollutant to the project.
3.  In the Pollutant Editor dialog that appears, enter **TSS** for the pollutant name and leave the other data fields at their default settings.
4.  Click the **OK** button to close the Editor.
5.  Click the **+** button on the Project Browser again to add our next pollutant.
6.  In the Pollutant Editor, enter **Lead** for the pollutant name, select **UG/L** for the concentration units, enter **TSS** as the name of the Co-Pollutant, and enter **0.25** as the Co-Fraction value.
7.  Click the **OK** button to close the Editor.

![Pollutant Editor Dialog](../../Manual/images/Pollutant%20Editor%20Dialog.png)

In SWMM, pollutants associated with runoff are generated by specific land uses assigned to subcatchments. In our example, we will define two categories of land uses: Residential and Undeveloped. To add these land uses to the project:

1.  Under the **Quality** category in the Project Browser, select the **Land Uses** sub-category and click the **+** button.
2.  In the Land Use Editor dialog that appears, enter **Residential** in the Name field and then click the **OK** button.
3.  Repeat steps 1 and 2 to create the **Undeveloped** land use category.

![Land Use Editor Dialog](../../Manual/images/Land%20Use%20Editor%20Dialog.png)

Next we need to define buildup and washoff functions for TSS in each of our land use categories. Functions for Lead are not needed since its runoff concentration was defined to be a fixed fraction of the TSS concentration. Normally, defining these functions requires site-specific calibration.

In this example we will assume that suspended solids in Residential areas builds up at a constant rate of 1 pound per acre per day until a limit of 50 lbs per acre is reached. For the Undeveloped area we will assume that buildup is only half as much. For the washoff function, we will assume a constant event mean concentration of 100 mg/L for Residential land and 50 mg/L for Undeveloped land. When runoff occurs, these concentrations will be maintained until the available buildup is exhausted. To define these functions for the Residential land use:

1.  Select the **Residential** land use category from the Project Browser and click the **edit** button.
2.  In the Land Use Editor dialog, move to the **Buildup** page.
3.  Select **TSS** as the pollutant and **POW** (for Power function) as the function type.
4.  Assign the function a maximum buildup of **50**, a rate constant of **1.0**, a power of **1** and select **AREA** as the normalizer.
    
    ![Land Use Editor - Buildup Tab](../../Manual/images/Land%20Use%20Editor%20-%20Buildup%20Tab.png)

5.  Move to the **Washoff** page of the dialog and select **TSS** as the pollutant, **EMC** as the function type, and enter **100** for the coefficient. Fill the other fields with 0.
6.  Click the **OK** button to accept your entries.

Now do the same for the **Undeveloped** land use category, except use a maximum buildup of **25**, a buildup rate constant of **0.5**, a buildup power of **1**, and a washoff EMC of **50**.

The final step in our water quality example is to assign a mixture of land uses to each subcatchment area:

1.  Select subcatchment **S1** into the Property Editor.
2.  Select the **Land Uses** property and click the ellipsis button (or press **Enter**).
3.  In the Land Use Assignment dialog that appears, enter **75** for the % Residential and **25** for the % Undeveloped. Then click the **OK** button to close the dialog.

    ![Land Use Assignment Dialog](../../Manual/images/Land%20Use%20Assignment%20Dialog.png)

4.  Repeat the same three steps for subcatchment **S2**.
5.  Repeat the same for subcatchment **S3**, except assign the land uses as **25%** Residential and **75%** Undeveloped.

Before we simulate the runoff quantities of TSS and Lead from our study area, an initial buildup of TSS should be defined so it can be washed off during our single rainfall event. We can either specify the number of antecedent dry days prior to the simulation or directly specify the initial buildup mass on each subcatchment. We will use the former method:

1.  From the **Options** category of the Project Browser, select the **Dates** sub-category and click the **edit** button.
2.  In the Simulation Options dialog that appears, enter **5** into the **Antecedent Dry Days** field.
3.  Leave the other simulation options the same as they were for the dynamic wave flow routing we just completed.
4.  Click the **OK** button to close the dialog.

Now run the simulation by selecting **Project >> Run Simulation** or by clicking the lightning bolt icon on the Standard Toolbar.

When the run is completed, view its Status Report. Note that two new sections have been added for **Runoff Quality Continuity** and **Quality Routing Continuity**. From the Runoff Quality Continuity table we see that there was an initial buildup of 47.5 lbs of TSS on the study area and an additional 2.2 lbs of buildup added during the dry periods of the simulation. About 47.9 lbs were washed off during the rainfall event. The quantity of Lead washed off is a fixed percentage (25% times 0.001 to convert from mg to ug) of the TSS as was specified.

If you plot the runoff concentration of TSS for subcatchment S1 and S3 together on the same time series graph as shown below, you will see the difference in concentrations resulting from the different mix of land uses in these two areas. You can also see that the duration over which pollutants are washed off is much shorter than the duration of the entire runoff hydrograph (i.e., 1 hour versus about 6 hours). This results from having exhausted the available buildup of TSS over this period of time.

![Graph of TSS concentration for Subcatchments S1 and S3](../../Manual/images/Graph%20of%20TSS%20concentration%20for%20Subcatchments%20S1%20and%20S3.png)

---
⁸ Aside from surface runoff, SWMM also allows pollutants to be introduced into the nodes of a drainage system through user-defined time series of direct inflows, dry weather inflows, groundwater interflow, and rainfall-dependent infiltration and inflow.

---

## 2.7 Running a Continuous Simulation

As a final exercise in this tutorial we will demonstrate how to run a long-term continuous simulation using a historical rainfall record and how to perform a statistical frequency analysis on the results. The rainfall record will come from a file named **sta310301.dat** that was included with the example data sets provided with EPA SWMM. It contains several years of hourly rainfall beginning in January 1998. The data are stored in the National Climatic Data Center's DSI 3240 format, which SWMM can automatically recognize.

To run a continuous simulation with this rainfall record:

1.  Select the rain gage **Gage1** into the Property Editor.
2.  Change the selection of **Data Source** to **FILE**.
3.  Select the **File Name** data field and click the ellipsis button (or press the **Enter** key) to bring up a standard Windows File Selection dialog.
4.  Navigate to the folder where the SWMM example files were stored, select the file named `sta310301.dat`, and click **Open** to select the file and close the dialog.
5.  In the **Station No.** field of the Property Editor enter **310301**.
6.  Select the **Options** category in the Project Browser and click the **edit** button to bring up the Simulation Options form.
7.  On the **General** page of the form, select **Kinematic Wave** as the Routing Method (this will help speed up the computations).
8.  On the **Dates** page of the form, set both the **Start Analysis** and **Start Reporting** dates to **01/01/1998**, and set the **End Analysis** date to **01/01/2000**.
9.  On the **Time Steps** page of the form, set the **Routing Time Step** to **300** seconds.
10. Close the Simulation Options form by clicking the **OK** button and start the simulation by selecting **Project >> Run Simulation** (or by clicking the lightning bolt icon on the Standard Toolbar).

After our continuous simulation is completed we can perform a statistical frequency analysis on any of the variables produced as output. For example, to determine the distribution of rainfall volumes within each storm event over the two-year period simulated:

1.  Select **Report >> Statistics** or click the **Σ** button on the Standard Toolbar.
2.  In the Statistics Report Selection dialog that appears, enter the values shown below.

![Statistics Report Selection Dialog](../../Manual/images/Statistics%20Report%20Selection%20Dialog.png)

3.  Click the **OK** button to close the form.

The results of this request will be a Statistics Report form containing four tabbed pages: a Summary page, an Events page containing a rank-ordered listing of each event, a Histogram page containing a plot of the occurrence frequency versus event magnitude, and a Frequency Plot page that plots event magnitude versus cumulative frequency.

![Statistics Report Summary Page](../../Manual/images/Statistics%20Report%20Summary%20Page.png)

The summary page shows that there were a total of 213 rainfall events. The Events page shows that the largest rainfall event had a volume of 3.35 inches and occurred over a 24-hour period. There were no events that matched the 3-inch, 6-hour design storm event used in our previous single-event analysis that had produced some internal flooding. In fact, the Summary Report for this continuous simulation indicates that there were no flooding or surcharge occurrences over the simulation period.

We have only touched the surface of SWMM's capabilities. Some additional features of the program that you will find useful include:
*   adding low impact development (LID) controls (i.e., green infrastructure) to reduce or delay runoff from subcatchments
*   utilizing additional types of drainage elements, such as storage units, flow dividers, pumps, and regulators, to model more complex types of systems
*   using control rules to simulate real-time operation of pumps and regulators
*   employing different types of externally-imposed inflows at drainage system nodes, such as direct time series inflows, dry weather inflows, and rainfall-dependent infiltration and inflow
*   modeling groundwater interflow between aquifers beneath subcatchment areas and drainage system nodes
*   modeling snow fall accumulation and melting within subcatchments
*   adding calibration data to a project so that simulated results can be compared with measured values
*   utilizing a background street, site plan, or topo map to assist in laying out a system's drainage elements and to help relate simulated results to real-world locations.

You can find more information on these and other features in the remaining chapters of this manual.