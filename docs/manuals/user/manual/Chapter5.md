@page user_manual_chapter_5 CHAPTER 5 - WORKING WITH PROJECTS

@tableofcontents

Project files contain all of the information used to model a study area. They are usually named with a .INP extension. This section describes how to create, open, and save EPA SWMM projects as well as setting their default properties.

    Creating a New Project

To create a new project:
    Select File >> New from the Main Menu or click  on the Main Toolbar.
    You will be prompted to save the existing project (if changes were made to it) before the new project is created.
    A new, unnamed project is created with all options set to their default values.

A new project is automatically created whenever EPA SWMM first begins.

     If you are going to use a backdrop image with automatic area and length calculation, then it is recommended that you set the map dimensions immediately after creating the new project (see Section 7.2 Setting the Map's Dimensions).

    Opening an Existing Project

To open an existing project stored on disk:
    Either select File >> Open from the Main Menu or click   on the Main Toolbar.
    You will be prompted to save the current project (if changes were made to it).
    Select the file to open from the Open File dialog form that will appear. 
    Click Open to open the selected file.
 
To open a project that was worked on recently: 
    Select File >> Reopen from the Main Menu.  
    Select a file from the list of recently used files to open.  


    Saving a Project

To save a project under its current name either select File >> Save from the Main Menu or click   on the Main Toolbar.

 To save a project using a different name:
    Select File >> Save As from the Main Menu.
    A standard File Save dialog form will appear from which you can select the folder and name that the project should be saved under.

    Setting Project Defaults

Each project has a set of default values that are used unless overridden by the SWMM user. These values fall into three categories:
    Default ID labels (labels used to identify nodes and links when they are first created)
    Default subcatchment properties (e.g., area, width, slope, etc.)
    Default node/link properties (e.g., node invert, conduit length, routing method).

To set default values for a project:
    Select Project >> Defaults from the Main Menu.
    A Project Defaults dialog will appear with three pages, one for each category listed above.     
    Check the box in the lower left of the dialog form if you want to save your choices for use in all new future projects as well.
    Click OK to accept your choice of defaults.

The specific items for each category of defaults will be discussed next.

Default ID Labels

The ID Labels page of the Project Defaults dialog form is used to determine how SWMM will assign default ID labels for the visual project components when they are first created. For each type of object you can enter a label prefix in the corresponding entry field or leave the field blank if an object's default name will simply be a number. In the last field you can enter an increment to be used when adding a numerical suffix to the default label. As an example, if C were used as a prefix for Conduits along with an increment of 5, then as conduits are created they receive default names of C5, C10, C15, and so on. An object’s default name can be changed by using the Property Editor for visual objects or the object-specific editor for non-visual objects. 

Default Subcatchment Properties

The Subcatchment page of the Project Defaults dialog sets default property values for newly created subcatchments. These properties include: 
    Subcatchment Area  
    Characteristic Width  
    Slope  
    % Impervious  
    Impervious Area Roughness  
    Pervious Area Roughness  
    Impervious Area Depression Storage  
    Pervious Area Depression Storage  
    % of Impervious Area with No Depression Storage  
    Infiltration Method  
 
The default properties of a subcatchment can be modified later by using the Property Editor. 

Default Node/Link Properties

The Nodes/Links page of the Project Defaults dialog sets default property values for newly created nodes and links. These properties include: 
    Node Invert Elevation  
    Node Maximum Depth
    Node Ponded Area  
    Conduit Length  
    Conduit Shape and Size
    Conduit Roughness  
    Flow Units
    Link Offsets Convention  
    Routing Method
    Force Main Equation  
 
The defaults automatically assigned to individual objects can be changed by using the object’s Property Editor. The choice of Flow Units and Link Offsets Convention can be changed directly on the main window’s Status Bar.

    Measurement Units

SWMM can use either US customary units or SI metric units. The choice of flow units determines what unit system is used for all other quantities: 
    selecting CFS (cubic feet per second), GPM (gallons per minutes), or MGD (million gallons per day) for flow units implies that US customary units will be used throughout
    selecting CMS (cubic meters per second), LPS (liters per second), or MLD (million liters per day) as flow units implies that SI metric units will be used throughout
     pollutant concentration  and Manning’s roughness coefficient (n) are always expressed in metric units.

Flow units can be selected directly on the main window's Status Bar or by setting a project's default values. In the latter case the selection can be saved so that all new future projects will automatically use those units.

     The units of previously entered data are not automatically adjusted if the unit system is changed.  






    Link Offset Conventions

Conduits and flow regulators (orifices, weirs, and outlets) can be offset some distance above the invert of their connecting end nodes as depicted below:

 

There are two different conventions available for specifying the location of these offsets. The Depth convention uses the offset distance from the node's invert (distance between  and ‚ in the figure above). The Elevation convention uses the absolute elevation of the offset location (the elevation of point  in the figure). The choice of convention can be made on the Status Bar of SWMM's main window or on the Node/Link Properties page of the Project Defaults dialog. When this convention is changed, a dialog will appear giving one the option to automatically re-calculate all existing link offsets in the current project using the newly selected convention.

    Calibration Data

SWMM can compare the results of a simulation with measured field data in its Time Series Plots, which are discussed in Section 9.4. Before SWMM can use such calibration data they must be entered into a specially formatted text file and registered with the project.

Calibration Files

Calibration Files contain measurements of a single parameter at one or more locations that can be compared with simulated values in Time Series Plots. Separate files can be used for each of the following parameters: 
    Subcatchment Runoff  
    Subcatchment Pollutant Washoff  
    Groundwater Flow
    Groundwater Elevation
    Snow Pack Depth
    Node Depth 
    Node Lateral Inflow 
    Node Flooding
    Node Water Quality  
    Link Flow Rate
    Link Flow Depth
    Link Flow Velocity 
The format of the file is described in Section 11.5. 

Registering Calibration Data

To register calibration data residing in a Calibration File:
    Select Project >> Calibration Data from the Main Menu.
    In the Calibration Data dialog form shown below, click in the box next to the parameter (e.g., node depth, link flow, etc.) whose calibration data will be registered.
    Then click the Add button to select a Calibration File from a standard Windows file selection dialog box.
    Click the Edit button if you want to open the Calibration File in Windows NotePad for editing.
    Click the Delete button if you wish to remove the Calibration File from the form.
    Repeat steps 2 - 4 for any other parameters that have calibration data.
    Click OK to accept your selections.

 


    Viewing All Project Data

A listing of all project data (with the exception of map coordinates) can be viewed in a non-editable window, formatted for input to SWMM's computational engine (see below). This can be useful for checking data consistency and to make sure that no key components are missing. To view such a listing select Project >> Details from the Main Menu. The format of the data in this listing is the same as that used when the file is saved to disk. It is described in detail in Appendix D.2.


