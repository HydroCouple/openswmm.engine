# Chapter 5 WORKING WITH PROJECTS

Project files contain all of the information used to model a study area. They are usually named with a `.INP` extension. This section describes how to create, open, and save EPA SWMM projects as well as setting their default properties.

## 5.1 Creating a New Project

To create a new project:

1.  Select **File >> New** from the Main Menu or click the new file icon on the Main Toolbar.
2.  You will be prompted to save the existing project (if changes were made to it) before the new project is created.
3.  A new, unnamed project is created with all options set to their default values.

A new project is automatically created whenever EPA SWMM first begins.

> **Note:** If you are going to use a backdrop image with automatic area and length calculation, then it is recommended that you set the map dimensions immediately after creating the new project (see Section 7.2 Setting the Map's Dimensions).

## 5.2 Opening an Existing Project

To open an existing project stored on disk:

1.  Either select **File >> Open** from the Main Menu or click the open file icon on the Main Toolbar.
2.  You will be prompted to save the current project (if changes were made to it).
3.  Select the file to open from the Open File dialog form that will appear.
4.  Click **Open** to open the selected file.

To open a project that was worked on recently:

1.  Select **File >> Reopen** from the Main Menu.
2.  Select a file from the list of recently used files to open.

## 5.3 Saving a Project

To save a project under its current name either select **File >> Save** from the Main Menu or click the save icon on the Main Toolbar.

To save a project using a different name:

1.  Select **File >> Save As** from the Main Menu.
2.  A standard File Save dialog form will appear from which you can select the folder and name that the project should be saved under.

## 5.4 Setting Project Defaults

Each project has a set of default values that are used unless overridden by the SWMM user. These values fall into three categories:

1.  Default ID labels (labels used to identify nodes and links when they are first created)
2.  Default subcatchment properties (e.g., area, width, slope, etc.)
3.  Default node/link properties (e.g., node invert, conduit length, routing method).

To set default values for a project:

1.  Select **Project >> Defaults** from the Main Menu.
2.  A Project Defaults dialog will appear with three pages, one for each category listed above.
    ![Project Defaults dialog box with tabs for ID Labels, Subcatchments, and Nodes/Links.](../../Manual/images/project-defaults.png)
3.  Check the box in the lower left of the dialog form if you want to save your choices for use in all new future projects as well.
4.  Click OK to accept your choice of defaults.

The specific items for each category of defaults will be discussed next.

### 5.4.1 Default ID Labels

The ID Labels page of the Project Defaults dialog form is used to determine how SWMM will assign default ID labels for the visual project components when they are first created. For each type of object you can enter a label prefix in the corresponding entry field or leave the field blank if an object's default name will simply be a number. In the last field you can enter an increment to be used when adding a numerical suffix to the default label. As an example, if C were used as a prefix for Conduits along with an increment of 5, then as conduits are created they receive default names of C5, C10, C15, and so on. An object's default name can be changed by using the Property Editor for visual objects or the object-specific editor for non-visual objects.

### 5.4.2 Default Subcatchment Properties

The Subcatchment page of the Project Defaults dialog sets default property values for newly created subcatchments. These properties include:
*   Subcatchment Area
*   Characteristic Width
*   Slope
*   % Impervious
*   Impervious Area Roughness
*   Pervious Area Roughness
*   Impervious Area Depression Storage
*   Pervious Area Depression Storage
*   % of Impervious Area with No Depression Storage
*   Infiltration Method

The default properties of a subcatchment can be modified later by using the Property Editor.

### 5.4.3 Default Node/Link Properties

The Nodes/Links page of the Project Defaults dialog sets default property values for newly created nodes and links. These properties include:
*   Node Invert Elevation
*   Node Maximum Depth
*   Node Ponded Area
*   Conduit Length
*   Conduit Shape and Size
*   Conduit Roughness
*   Flow Units
*   Link Offsets Convention
*   Routing Method
*   Force Main Equation

The defaults automatically assigned to individual objects can be changed by using the object's Property Editor. The choice of Flow Units and Link Offsets Convention can be changed directly on the main window's Status Bar.

## 5.5 Measurement Units

SWMM can use either US customary units or SI metric units. The choice of flow units determines what unit system is used for all other quantities:
*   selecting CFS (cubic feet per second), GPM (gallons per minutes), or MGD (million gallons per day) for flow units implies that US customary units will be used throughout
*   selecting CMS (cubic meters per second), LPS (liters per second), or MLD (million liters per day) as flow units implies that SI metric units will be used throughout
*   pollutant concentration and Manning's roughness coefficient (n) are always expressed in metric units.

Flow units can be selected directly on the main window's Status Bar or by setting a project's default values. In the latter case the selection can be saved so that all new future projects will automatically use those units.

> **Note:** The units of previously entered data are not automatically adjusted if the unit system is changed.

## 5.6 Link Offset Conventions

Conduits and flow regulators (orifices, weirs, and outlets) can be offset some distance above the invert of their connecting end nodes as depicted below:

![Diagram showing a conduit connecting two nodes, with the offset distance labeled between the node invert and the conduit invert.](../../Manual/images/link-offset.png)

There are two different conventions available for specifying the location of these offsets. The **Depth** convention uses the offset distance from the node's invert (distance between ① and ②, in the figure above). The **Elevation** convention uses the absolute elevation of the offset location (the elevation of point ① in the figure). The choice of convention can be made on the Status Bar of SWMM's main window or on the Node/Link Properties page of the Project Defaults dialog. When this convention is changed, a dialog will appear giving one the option to automatically re-calculate all existing link offsets in the current project using the newly selected convention.

## 5.7 Calibration Data

SWMM can compare the results of a simulation with measured field data in its Time Series Plots, which are discussed in Section 9.4. Before SWMM can use such calibration data they must be entered into a specially formatted text file and registered with the project.

### 5.7.1 Calibration Files

Calibration Files contain measurements of a single parameter at one or more locations that can be compared with simulated values in Time Series Plots. Separate files can be used for each of the following parameters:
*   Subcatchment Runoff
*   Subcatchment Pollutant Washoff
*   Groundwater Flow
*   Groundwater Elevation
*   Snow Pack Depth
*   Node Depth
*   Node Lateral Inflow
*   Node Flooding
*   Node Water Quality
*   Link Flow Rate
*   Link Flow Depth
*   Link Flow Velocity

The format of the file is described in Section 11.5.

### 5.7.2 Registering Calibration Data

To register calibration data residing in a Calibration File:

1.  Select **Project >> Calibration Data** from the Main Menu.
2.  In the Calibration Data dialog form shown below, click in the box next to the parameter (e.g., node depth, link flow, etc.) whose calibration data will be registered.
    ![Calibration Data dialog box, allowing users to add, edit, or delete calibration files for various simulation variables.](../../Manual/images/calibration-data.jpg)
3.  Then click the **Add** button to select a Calibration File from a standard Windows file selection dialog box.
4.  Click the **Edit** button if you want to open the Calibration File in Windows NotePad for editing.
5.  Click the **Delete** button if you wish to remove the Calibration File from the form.
6.  Repeat steps 2 - 4 for any other parameters that have calibration data.
7.  Click **OK** to accept your selections.

## 5.8 Viewing All Project Data

A listing of all project data (with the exception of map coordinates) can be viewed in a non-editable window, formatted for input to SWMM's computational engine (see below). This can be useful for checking data consistency and to make sure that no key components are missing. To view such a listing select **Project >> Details** from the Main Menu. The format of the data in this listing is the same as that used when the file is saved to disk. It is described in detail in Appendix D.2.

![Project Data window showing project data in a tabular format, categorized by sections like [TITLE], [OPTIONS], [SUBCATCHMENTS], etc.](../../Manual/images/project-data.png)