# Chapter 8 RUNNING A SIMULATION

After a study area has been suitably described, its runoff response, flow routing and water quality behavior can be simulated. This section describes how to specify options to be used in the analysis, how to run the simulation and how to troubleshoot common problems that might occur.

## 8.1 Setting Simulation Options

SWMM has a number of options that control how the simulation of a stormwater drainage system is carried out. To set these options:

1.  Select the **Options** category from the Project Browser.
2.  Select one of the following categories of options to edit:
    a.  General Options
    b.  Date Options
    c.  Time Step Options
    d.  Dynamic Wave Routing Options
    e.  Interface File Options
    f.  Reporting Options
    g.  Event Options
3.  Click the edit button on the Browser panel or select **Edit >> Edit Object** to invoke the appropriate editor for the chosen option category (the Simulation Options dialog is used for the first five categories while the Reporting Options dialog and the Events Editor dialog are used, respectively, for the last two).

The Simulations Options dialog contains a separate tabbed page for each of the first five option categories listed above. Each page is described in more detail below.

### 8.1.1 General Options

![Simulation Options dialog box, General tab.](../../Manual/images/sim-options-general.png)

The **General** page of the Simulation Options dialog sets values for the following options:

**Process Models**

This section selects which of SWMM's process models will be applied to the current project. For example, a model that contained Aquifer and Groundwater elements could be run first with the groundwater computations turned on and then again with them turned off to see what effect this process had on the site's hydrology. Note that if there are no elements in the project needed to model a given process then that process option is disabled (e.g., if there were no Aquifers defined for the project then the Groundwater check box will appear disabled in an unchecked state).

**Infiltration Model**

This option selects the default method used to model infiltration of rainfall into the upper soil zone of subcatchments. The choices are:
*   Horton
*   Modified Horton
*   Green-Ampt
*   Modified Green-Ampt
*   Curve Number

Each of these methods is briefly described in Section 3.4.2. All new subcatchments added to a project will default to using the selected method. For existing subcatchments, their infiltration method will only change if they had been using the previous default option. That would require re-entering values for the infiltration parameters in each such subcatchment, unless the change was between the two Horton options or the two Green-Ampt options. A prompt is issued asking if SWMM should automatically assign a default set of parameter values to all subcatchments that switch between two incompatible types of infiltration methods. Different infiltration models can be used with different subcatchments by editing their **Infiltration** property.

**Routing Model**

This option determines which method is used to route flows through the conveyance system. The choices are:
*   Steady Flow
*   Kinematic Wave
*   Dynamic Wave

Review Section 3.4.5 for a brief description of each of these alternatives.

**Allow Ponding**

Checking this option will allow excess water to collect atop nodes and be re-introduced into the system as conditions permit. In order for ponding to actually occur at a particular node, a non-zero value for its **Ponded Area** attribute must be used.

**Minimum Conduit Slope**

This is the minimum value allowed for a conduit's slope (%). If blank or zero (the default) then no minimum is imposed (although SWMM uses a lower limit on elevation drop of 0.001 ft (0.00035 m) when computing a conduit slope).

### 8.1.2 Date Options

The **Dates** page of the Simulation Options dialog determines the starting and ending dates/times of a simulation.

*   **Start Analysis On**
    Enter the date (month/day/year) and time of day when the simulation begins.
*   **Start Reporting On**
    Enter the date and time of day when reporting of simulation results is to begin. Using a date prior to the start date is the same as using the start date.
*   **End Analysis On**
    Enter the date and time when the simulation is to end.
*   **Start Sweeping On**
    Enter the day of the year (month/day) when street sweeping operations begin. The default is January 1.
*   **End Sweeping On**
    Enter the day of the year (month/day) when street sweeping operations end. The default is December 31.
*   **Antecedent Dry Days**
    Enter the number of days with no rainfall prior to the start of the simulation. This value is used to compute an initial buildup of pollutant load on the surface of subcatchments.

> **Note:** If rainfall or climate data are read from external files, then the simulation dates should be set to coincide with the dates recorded in these files.

### 8.1.3 Time Step Options

The **Time Steps** page of the Simulation Options dialog establishes the length of the time steps used for runoff computation, routing computation and results reporting. Time steps are specified in days and hours:minutes:seconds except for flow routing which is entered as decimal seconds.

*   **Reporting Time Step**
    Enter the time interval for reporting of computed results.
*   **Runoff - Wet Weather Time Step**
    Enter the time step length used to compute runoff from subcatchments during periods of rainfall, or when ponded water still remains on the surface, or when LID controls are still infiltrating or evaporating runoff.
*   **Runoff - Dry Weather Time Step**
    Enter the time step length used for runoff computations (consisting essentially of pollutant buildup) during periods when there is no rainfall, no ponded water, and LID controls are dry. This must be greater or equal to the Wet Weather time step.
*   **Control Rule Time Step**
    Enter the time step length used for evaluating Control Rules. The default is 0 which means that controls are evaluated at every routing time step.
*   **Routing Time Step**
    Enter the time step length in decimal seconds used for routing flows and water quality constituents through the conveyance system. Note that Dynamic Wave routing requires a much smaller time step than the other methods of flow routing.

**Steady Flow Periods**

This set of options tells SWMM how to identify and treat periods of time when system hydraulics is not changing. The system is considered to be in a steady flow period if:

1.  The percent difference between total system inflow and total system outflow is below the **System Flow Tolerance**,
2.  The percent differences between the current lateral inflow and that from the previous time step for all points in the conveyance system are below the **Lateral Flow Tolerance**.

Checking the **Skip Steady Flow Periods** box will make SWMM keep using the most recently computed conveyance system flows (instead of computing a new flow solution) whenever the above criteria are met. Using this feature can help speed up simulation run times at the expense of reduced accuracy.

### 8.1.4 Dynamic Wave Options

The **Dynamic Wave** page of the Simulation Options dialog sets several parameters that control how the dynamic wave flow routing computations are made. These parameters have no effect for the other flow routing methods.

*   **Inertial Terms**
    Indicates how the inertial terms in the St. Venant momentum equation will be handled.
    *   **KEEP** maintains these terms at their full value under all conditions.
    *   **DAMPEN** reduces the terms as flow comes closer to being critical and ignores them when flow is supercritical.
    *   **IGNORE** drops the terms altogether from the momentum equation, producing what is essentially a Diffusion Wave solution.
*   **Define Supercritical Flow By**
    Selects the basis used to determine when supercritical flow occurs in a conduit. The choices are:
    *   water surface slope only (i.e., water surface slope > conduit slope)
    *   Froude number only (i.e., Froude number > 1.0)
    *   both water surface slope and Froude number.
    The first two choices were used in earlier versions of SWMM while the third choice, which checks for either condition, is now the recommended one.
*   **Force Main Equation**
    Selects which equation will be used to compute friction losses during pressurized flow for conduits that have been assigned a Circular Force Main cross-section. The choices are either the **Hazen-Williams** equation or the **Darcy-Weisbach** equation.
*   **Surcharge Method**
    Selects which method will be used to handle surcharge conditions. The **Extran** option uses a variation of the Surcharge Algorithm from previous versions of SWMM to update nodal heads when all connecting links become full. The **Slot** option uses a Preissmann Slot to add a small amount of virtual top surface width to full flowing pipes so that SWMM's normal procedure for updating nodal heads can continue to be used.
*   **Use Variable Time Steps**
    Check the box if an internally computed variable time step should be used at each routing time period and select an adjustment (or safety) factor to apply to this time step. The variable time step is computed so as to satisfy the Courant condition within each conduit. A typical adjustment factor would be 75% to provide some margin of conservatism. The computed variable time step will not be less than the minimum variable step discussed below nor be greater than the fixed time step specified on the **Time Steps** page of the dialog.
*   **Minimum Variable Time Step**
    This is the smallest time step allowed when variable time steps are used. The default value is 0.5 seconds. Smaller steps may be warranted, but they can lead to longer simulations runs without much improvement in solution quality.
*   **Time Step for Conduit Lengthening**
    This is a time step, in seconds, used to artificially lengthen conduits so that they meet the Courant stability criterion under full-flow conditions (i.e., the travel time of a wave will not be smaller than the specified conduit lengthening time step). As this value is decreased, fewer conduits will require lengthening. A value of zero means that no conduits will be lengthened. The ratio of the artificial length to the original length for each conduit is listed in the Flow Classification table that appears in the simulation's Summary Report (see Section 9.2).
*   **Minimum Nodal Surface Area**
    This is a minimum surface area used at nodes when computing changes in water depth. If 0 is entered, then the default value of 12.566 ft² (1.167 m²) is used. This is the area of a 4-ft diameter manhole. The value entered should be in square feet for US units or square meters for SI units.
*   **Head Convergence Tolerance**
    This is the maximum difference in computed heads between successive trials of SWMM's iterative method for computing a dynamic wave hydraulic solution that determines when convergence is reached within a given time step. The default tolerance is 0.005 ft (0.0015 m).
*   **Maximum Trials per Time Step**
    This is the maximum number of trials that SWMM will use in its iterative method for computing a dynamic wave hydraulic solution within each time step. The default value is 8.
*   **Number of Parallel Threads to Use**
    This selects the number of parallel computing threads to use on machines equipped with multi-core processors. The default is 1. Clicking the info button will display the number of physical cores and logical processors available.

Clicking the **Apply Defaults** label will set all the Dynamic Wave options to their default values.

### 8.1.5 File Options

The **Files** page of the Simulation Options dialog is used to specify which interface files will be used or saved during the simulation. (Interface files are described in Chapter 11.) The page contains a list box with three buttons underneath it. The list box lists the currently selected files, while the buttons are used as follows:

*   **Add**: adds a new interface file specification to the list.
*   **Edit**: edits the properties of the currently selected interface file.
*   **Delete**: deletes the currently selected interface from the project (but not from your hard drive).

![Simulation Options, Files tab.](../../Manual/images/sim-options-files.png)

When the **Add** or **Edit** buttons are clicked, an Interface File Selector dialog appears where one can specify the type of interface file, whether it should be used or saved, and its name. The entries on this dialog are as follows:

![Interface File Selector dialog.](../../Manual/images/interface-file-selector.png)

*   **File Type**: Select the type of interface file to be specified.
*   **Use / Save Buttons**: Select whether the named interface file will be used to supply input to a simulation run or whether simulation results will be saved to it.
*   **File Name**: Click the Select File button to specify the file name from a standard Windows file selection dialog box.

## 8.2 Setting Reporting Options

The Reporting Options dialog is used to select individual subcatchments, nodes, and links that will have detailed time series results saved for viewing after a simulation has been run. The default for new projects is that all objects will have detailed results saved for them. The dialog is invoked by selecting the **Reporting** category of Options from the Project Browser and clicking the edit button (or by selecting **Edit >> Edit Object** from the main menu).

The dialog contains three tabbed pages - one each for subcatchments, nodes, and links. It is a stay-on-top form which means that you can select items directly from the Study Area Map or Project Browser while the dialog remains visible.

![Reporting Options dialog.](../../Manual/images/reporting-options.png)

To include an object in the set that is reported on:

1.  Select the tab to which the object belongs (**Subcatchments**, **Nodes** or **Links**).
2.  Unselect the "**All**" check box if it is currently checked.
3.  Select the specific object either from the **Study Area Map** or from the listing in the Project Browser.
4.  Click the **Add** button on the dialog.
5.  Repeat the above steps for any additional objects.

To remove an item from the set selected for reporting:

1.  Select the desired item in the dialog's list box.
2.  Click the **Remove** button to remove the item.

To remove all items from the reporting set of a given object category, select the object category's page and click the **Clear** button.

To include all objects of a given category in the reporting set, check the "**All**" box on the page for that category (i.e., subcatchments, nodes, or links). This will override any individual items that may be currently listed on the page. To dismiss the dialog click the **Close** button. In addition the following reporting options can be selected from this dialog:

*   **Report Input Summary**
    Check this option to have the simulation's Status Report list a summary of the project's input data.
*   **Report Control Actions**
    Check this option to have the simulation's Status Report list all discrete control actions taken by the Control Rules associated with a project (continuous modulated control actions are not listed). This option should only be used for short-term simulation.
*   **Report Average Results**
    Check this option to have the average of the results for all routing time steps that fall within a reporting time step be reported instead of the instantaneous point results that occur at the end of the reporting time step.

## 8.3 Selecting Event Periods

Simulation events allow one to limit the periods of time in which a full unsteady hydraulic analysis of the drainage network is performed. For times outside of these periods, the hydraulic state of the network stays the same as it was at the end of the previous hydraulic event. Although hydraulic calculations are restricted to these pre-defined event periods, a full accounting of the system's hydrology is still computed over the entire simulation duration. During inter-event periods any inflows to the network, from runoff, groundwater flow, dry weather flow, etc., are ignored. The purpose of only computing hydraulics for particular time periods is to speed up long-term continuous simulations where one knows in advance which periods of time (such as representative or critical storm events) are of most interest.

To define a set of simulation events select the **Events** sub-category of Options from the Project Browser and click the edit button on the Browser panel or select **Edit >> Edit Object** from the Main Menu. This will bring up the Events Editor in which multiple event time periods can be defined.

![Events Editor dialog.](../../Manual/images/events-editor.png)

The editor consists of a table listing the start and end date of each event, plus a blank line at the end of the list used for adding a new event. The events do not have to be entered in chronological order. There are date and time selection controls below the table used to edit the dates of a selected event. Clicking the **Replace Event** button will replace the row with the entries in these controls. The **Delete Event** button will delete the selected event and the **Delete All** button will delete all events from the table. The first column of the table contains a check box which determines if the event should be used in the analysis or not.

> **Note:** To identify event periods of interest, one can first run a simulation with Flow Routing turned off and then perform a statistical frequency analysis on the system's rainfall record (see Section 9.8 Viewing a Statistics Report).

> **Note:** When a new event occurs, the water in a storage unit node will remain at the same level it had at the end of the previous event. Therefore one may want to choose event intervals long enough to minimize the effect that storage carryover might have.

## 8.4 Starting a Simulation

To start a simulation either select **Project >> Run Simulation** from the Main Menu or click the run icon on the Main Toolbar. A Run Status window will appear which displays the progress of the simulation.

![Run Status window showing simulation progress.](../../Manual/images/run-status.png)

To stop a run before its normal termination, click the **Stop** button on the Run Status window or press the `<Esc>` key. Simulation results up until the time when the run was stopped will be available for viewing. To minimize the SWMM program while a simulation is running, click the **Minimize** button on the Run Status window.

If the analysis runs successfully the green check icon will appear in the Run Status section of the Status Bar at the bottom of SWMM's main window. Any error or warning messages will appear in a Status Report window. If you modify the project after a successful run has been made, the status flag changes to the yellow warning icon indicating that the current computed results no longer apply to the modified project.

## 8.5 Troubleshooting Results

When a run ends prematurely, the Run Status dialog will indicate the run was unsuccessful and direct the user to the Status Report for details. The Status Report will include an error statement, code, and description of the problem (e.g., ERROR 138: Node TG040 has initial depth greater than maximum depth). Consult Appendix E for a description of SWMM's error messages. Even if a run completes successfully, one should check to insure that the results are reasonable. The following are the most common reasons for a run to end prematurely or to contain questionable results.

*   **Unknown ID Error Message**
    This message typically appears when an object references another object that was never defined. An example would be a subcatchment whose outlet was designated as N29, but no such subcatchment or node with that name exists. Similar situations can exist for incorrect references made to Curves, Time Series, Time Patterns, Aquifers, Snow Packs, Streets, Inlets, Transects, Pollutants, and Land Uses.

*   **File Errors**
    File errors can occur when:
    *   a file cannot be located on the user's computer
    *   a file being used has the wrong format
    *   a file to be written to cannot be opened because the user does not have write privileges for the directory (folder) where the file is to be stored.

*   **Drainage System Layout Errors**
    A valid drainage system layout must obey the following conditions:
    *   An outfall node can have only one conduit link connected to it.
    *   A flow divider node must have exactly two outflow links.
    *   A node cannot have more than one dummy link connected to it.
    *   Under Kinematic Wave routing, a junction node can only have one outflow link and a regulator link cannot be the outflow link of a non-storage node.
    *   Under Dynamic Wave routing there must be at least one outfall node in the network.
    An error message will be generated if any of these conditions are violated.

*   **Excessive Continuity Errors**
    When a run completes successfully, the mass continuity errors for runoff, flow routing, and pollutant routing will be displayed in the Run Status window. These errors represent the percent difference between initial storage + total inflow and final storage + total outflow for the entire drainage system. If they exceed some reasonable level, such as 10 percent, then the validity of the analysis results must be questioned. The most common reasons for an excessive continuity error are computational time steps that are too long or conduits that are too short.

    ![Run Status dialog after a successful run, showing continuity error values.](../../Manual/images/run-status-success.png)

    In addition to the system continuity error, the Status Report produced by a run (see Section 9.1) will list those nodes of the drainage network that have the largest flow continuity errors. If the error for a node is excessive, then one should first consider if the node in question is of importance to the purpose of the simulation. If it is, then further study is warranted to determine how the error might be reduced.

*   **Unstable Flow Routing Results**
    Due to the explicit nature of the numerical methods used for Dynamic Wave routing (and to a lesser extent, Kinematic Wave routing), the flows in some links or water depths at some nodes may fluctuate or oscillate significantly at certain periods of time as a result of numerical instabilities in the solution method. SWMM does not automatically identify when such conditions exist, so it is up to the user to verify the numerical stability of the model and to determine if the simulation results are valid for the modeling objectives. Time series plots at key locations in the network can help identify such situations as can a scatter plot between a link's flow and the corresponding water depth at its upstream node (see Section 9.5, Viewing Results with a Graph).

    Numerical instabilities can occur over short durations and may not be apparent when time series are plotted with a long time interval. When detecting such instabilities, it is recommended that a reporting time step of 1 minute or less be used, at least for an initial screening of results.

    The run's Status Report lists the links having the five highest values of a Flow Instability Index (FII). This index counts the number of times that the flow value in a link is higher (or lower) than the flow in both the previous and subsequent time periods. The index is normalized with respect to the expected number of such 'turns' that would occur for a purely random series of values and can range from 0 to 150.

    As an example of how the Flow Instability Index can be used, consider Figure 8-1. The solid line plots the flow hydrograph for the link identified as having the highest FII value (100) in a dynamic wave flow routing run that used a fixed time step of 30 seconds. The dashed line shows the hydrograph that results when a variable time step was used instead, which is now completely stable.

    ![Graph showing the Flow Instability Index for a flow hydrograph under fixed and variable time steps.](../../Manual/images/flow-instability-graph.png)
    *Figure 8-1 Flow Instability Index for a flow hydrograph*

    Flow time series plots for the links having the highest FII's should be inspected to insure that flow routing results are acceptably stable. Numerical instabilities under Dynamic Wave flow routing can be reduced by:
    *   reducing the routing time step
    *   utilizing the variable time step option with a smaller time step factor
    *   selecting to ignore the inertial terms of the momentum equation
    *   selecting the option to lengthen short conduits.