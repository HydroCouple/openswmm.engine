# Chapter 4 SWMM'S MAIN WINDOW

This chapter discusses the essential features of SWMM's workspace. It describes the main menu bar, the tool and status bars, and the three windows used most often – the Study Area Map, the Browser, and the Property Editor. It also shows how to set program preferences.

## 4.1 Overview

The EPA SWMM main window is pictured below. It consists of the following user interface elements: a Main Menu, a Main Toolbar, a Status Bar, the Study Area Map window containing a Map Toolbar, a Browser panel, and a Property Editor window. A description of each of these elements is provided in the sections that follow.

![The main window of SWMM 5.2, showing the Main Menu, Main Toolbar, Project/Map Browser, Study Area Map with Map Toolbar, Property Editor, and Status Bar.](../../Manual/images/swmm-main-window.jpg)

## 4.2 Main Menu

The Main Menu located across the top of the EPA SWMM main window contains a collection of menus used to control the program. These include:
*   File Menu
*   Edit Menu
*   View Menu
*   Project Menu
*   Report Menu
*   Tools Menu
*   Window Menu
*   Help Menu

### 4.2.1 File Menu

The File Menu contains commands for opening and saving data files and for printing:

| Command | Description |
| :--- | :--- |
| **New** | Creates a new SWMM project |
| **Open** | Opens an existing project |
| **Reopen** | Reopens a recently used project |
| **Save** | Saves the current project |
| **Save As** | Saves the current project under a different name |
| **Export** | Exports study area map to a file in a variety of formats; Exports current results to a Hot Start file; Exports the current result's Status/Summary reports |
| **Combine** | Combines two Routing Interface files together |
| **Page Setup** | Sets page margins and orientation for printing |
| **Print Preview** | Previews a printout of the currently active view (map, report, graph, or table) |
| **Print** | Prints the current view |
| **Exit** | Exits SWMM |

### 4.2.2 Edit Menu

The Edit Menu contains commands for editing and copying:

| Command | Description |
| :--- | :--- |
| **Copy To** | Copies the currently active view (map, report, graph or table) to the clipboard or to a file |
| **Select Object** | Enables the user to select an object on the map |
| **Select Vertex** | Enables the user to select the vertex of a subcatchment or link |
| **Select Region**| Enables the user to delineate a region on the map for selecting multiple objects |
| **Select All** | Selects all objects when the map is the active window or all cells of a table when a tabular report is the active window |
| **Find Object** | Locates a specific object by name on the map |
| **Edit Object** | Edits the properties of the currently selected object |
| **Delete Object** | Deletes the currently selected object |
| **Group Edit** | Edits a property for the group of objects that fall within the outlined region of the map |
| **Group Delete** | Deletes a group of objects that fall within the outlined region of the map |

### 4.2.3 View Menu

The View Menu contains commands for viewing the Study Area Map:

| Command | Description |
| :--- | :--- |
| **Dimensions** | Sets reference coordinates and distance units for the study area map |
| **Backdrop** | Allows a backdrop image to be added, positioned, and viewed behind the map |
| **Pan** | Pans across the map |
| **Zoom In** | Zooms in on the map |
| **Zoom Out** | Zooms out on the map |
| **Full Extent** | Redraws the map at full extent |
| **Query** | Highlights objects on the map that meet specific criteria |
| **Overview** | Toggles the display of the Overview Map |
| **Layers** | Toggles display of object layers on the map |
| **Legends** | Controls display of the map legends |
| **Toolbar** | Toggles display of the toolbar |

### 4.2.4 Project Menu

The Project menu contains commands related to the current project being analyzed:

| Command | Description |
| :--- | :--- |
| **Summary** | Lists the number of each type of object in the project |
| **Details** | Shows a detailed listing of all project data |
| **Defaults** | Edits a project's default properties |
| **Calibration Data** | Registers files containing calibration data with the project |
| **Add a New Object** | Adds a new context sensitive object to the project |
| **Run a Simulation**| Runs a simulation |

### 4.2.5 Report Menu

The Report menu contains commands used to report analysis results in different formats:

| Command | Description |
| :--- | :--- |
| **Status** | Displays a status report for the most recent simulation run |
| **Summary** | Displays summary results in tabular form |
| **Graph** | Displays simulation results in graphical form |
| **Table** | Displays simulation results in tabular form |
| **Statistics** | Displays a statistical analysis of simulation results |
| **Customize** | Customizes the display style of the currently active graph |

### 4.2.6 Tools Menu

The Tools menu contains commands used to configure program preferences, study area map display options, and external add-in tools:

| Command | Description |
| :--- | :--- |
| **Program Preferences** | Sets program preferences, such as font size, confirm deletions, number of decimal places displayed, etc. |
| **Map Display Options** | Sets appearance options for the Map, such as object size, annotation, flow direction arrows, and back-ground color |
| **Configure Tools** | Adds, deletes, or modifies external add-in tools |

### 4.2.7 Window Menu

The Window Menu contains commands for arranging and selecting windows within the SWMM workspace:

| Command | Description |
| :--- | :--- |
| **Cascade** | Arranges windows in cascaded style, with the study area map filling the entire display area |
| **Tile** | Minimizes the study area map and tiles the remaining windows vertically in the display area |
| **Close All** | Closes all open windows except for the study area map |
| **Window List** | Lists all open windows; the currently selected window has the focus and is denoted with a check mark |

### 4.2.8 Help Menu

The Help Menu contains commands for getting help in using EPA SWMM:

| Command | Description |
| :--- | :--- |
| **User Guide** | Displays the User Guide's Table of Contents |
| **How Do I** | Displays a list of topics covering the most common operations |
| **What's New** | Lists new program features that have been added |
| **Keyboard Shortcuts**| Displays a list of keyboard shortcuts for main menu commands |
| **Measurement Units** | Shows measurement units for all of SWMM's parameters |
| **Error Messages** | Lists the meaning of all error messages |
| **Tutorials** | Lists tutorials that show how to use EPA SWMM |
| **Welcome Screen** | Displays SWMM's Welcome screen |
| **About** | Displays information about the version of EPA SWMM being used |

## 4.3 Keyboard Shortcuts

Several main menu commands have keyboard shortcuts that can be used to select them. They are listed below.

| Menu Command | Shortcut Key |
| :--- | :--- |
| File \| New | Ctrl-N |
| File \| Open | Ctrl-O |
| File \| Save | Ctrl-S |
| File \| Save As | Ctrl-Alt-S |
| File \| Exit | Alt-F4 |
| Edit \| Copy To | Ctrl-C |
| Edit \| Select All | Ctrl-A |
| Edit \| Find Object | Ctrl-F |
| Edit \| Edit Object | F2 |
| Edit \| Delete Object| Ctrl-Delete |
| Edit \| Group Edit | Shift-F2 |
| View \| Query | Ctrl-Q |
| Project \| Add a New <object> | Ctrl-Insert |
| Project \| Run Simulation | F9 |
| Report \| Graph \| Time Series | Ctrl-G |
| Window \| Cascade | Shift-F5 |
| Window \| Tile | Shift-F4 |
| Window \| Close All | Shift-Ctrl-F4 |
| Help \| User Guide | Ctrl-F1 |

In addition the F1 key can be used to bring up context-sensitive Help in most of SWMM's data editing windows

## 4.4 Toolbars

The Main Toolbar appears at the top of SWMM's Main Window and provides shortcuts to the following Main Menu commands:

*   ![New project icon](../../Manual/images/icon-new.png) Creates a new project (**File >> New**)
*   ![Open project icon](../../Manual/images/icon-open.png) Opens an existing project (**File >> Open**)
*   ![Save project icon](../../Manual/images/icon-save.png) Saves the current project (**File >> Save**)
*   ![Print icon](../../Manual/images/icon-print.png) Prints the currently active window (**File >> Print**)
*   ![Copy to icon](../../Manual/images/icon-copy.png) Copies selection to the clipboard or to a file (**Edit >> Copy To**)
*   ![Find object icon](../../Manual/images/icon-find.png) Finds a specific object on the Study Area Map (**Edit >> Find Object**)
*   ![Visual query icon](../../Manual/images/icon-query.png) Makes a visual query of the Study Area Map (**View >> Query**)
*   ![Overview map icon](../../Manual/images/icon-overview.png) Toggles the display of the Overview Map (**View >> Overview**)
*   ![Run simulation icon](../../Manual/images/icon-run.png) Runs a simulation (**Project >> Run Simulation**)
*   ![Report icon](../../Manual/images/icon-report.png) Displays a run's Status or Summary reports (**Report >> Status** and **Report >> Summary** appear in a dropdown menu)
*   ![Profile plot icon](../../Manual/images/icon-profile.png) Creates a profile plot of simulation results (**Report >> Graph >> Profile**)
*   ![Time series plot icon](../../Manual/images/icon-time-series.png) Creates a time series plot of simulation results (**Report >> Graph >> Time Series**)
*   ![Time series table icon](../../Manual/images/icon-table.png) Creates a time series table of simulation results (**Report >> Table**)
*   ![Scatter plot icon](../../Manual/images/icon-scatter.png) Creates a scatter plot of simulation results (**Report >> Graph >> Scatter**)
*   ![Statistics icon](../../Manual/images/icon-statistics.png) Performs a statistical analysis of simulation results (**Report >> Statistics**)
*   ![Customize icon](../../Manual/images/icon-customize.png) Modifies display options for the currently active view (**Tools >> Map Display Options** or **Report >> Customize**)
*   ![Cascade windows icon](../../Manual/images/icon-cascade.png) Arranges windows in cascaded style, with the Study Area Map filling the entire display area (**Window >> Cascade**)

The Main Toolbar can be made visible or invisible by selecting **View >> Toolbar** from the Main Menu.

The Map Toolbar appears on the right side of the Study Area Map and contains buttons for selecting items and viewing the Study Area Map:
*   ![Select object icon](../../Manual/images/icon-select-object.png) Selects an object on the map (**Edit >> Select Object**)
*   ![Select vertex icon](../../Manual/images/icon-select-vertex.png) Selects link or subcatchment vertex points (**Edit >> Select Vertex**)
*   ![Select region icon](../../Manual/images/icon-select-region.png) Selects a region on the map (**Edit >> Select Region**)
*   ![Pan icon](../../Manual/images/icon-pan.png) Pans across the map (**View >> Pan**)
*   ![Zoom in icon](../../Manual/images/icon-zoom-in.png) Zooms in on the map (**View >> Zoom In**)
*   ![Zoom out icon](../../Manual/images/icon-zoom-out.png) Zooms out on the map (**View >> Zoom Out**)
*   ![Full extent icon](../../Manual/images/icon-full-extent.png) Draws map at full extent (**View >> Full Extent**)
*   ![Measure icon](../../Manual/images/icon-measure.png) Measures a length or area on the map

The mouse wheel can also be used to pan, zoom in or zoom out of the map at any time without having to select the Pan, Zoom In or Zoom Out buttons.

The Map Toolbar also contains buttons used to add objects to a project via the Study Area Map:
*   ![Add rain gage icon](../../Manual/images/icon-add-raingage.png) Adds a rain gage to the map.
*   ![Add subcatchment icon](../../Manual/images/icon-add-subcatchment.png) Adds a subcatchment to the map
*   ![Add junction icon](../../Manual/images/icon-add-junction.png) Adds a junction node to the map
*   ![Add outfall icon](../../Manual/images/icon-add-outfall.png) Adds an outfall node to the map
*   ![Add flow divider icon](../../Manual/images/icon-add-divider.png) Adds a flow divider node to the map
*   ![Add storage unit icon](../../Manual/images/icon-add-storage.png) Adds a storage unit node to the map
*   ![Add conduit icon](../../Manual/images/icon-add-conduit.png) Adds a conduit link to the map
*   ![Add pump icon](../../Manual/images/icon-add-pump.png) Adds a pump link to the map
*   ![Add orifice icon](../../Manual/images/icon-add-orifice.png) Adds an orifice link to the map
*   ![Add weir icon](../../Manual/images/icon-add-weir.png) Adds a weir link to the map
*   ![Add outlet icon](../../Manual/images/icon-add-outlet.png) Adds an outlet link to the map
*   ![Add text icon](../../Manual/images/icon-add-text.png) Adds a text label to the map

## 4.5 Status Bar

The Status Bar appears at the bottom of SWMM's Main Window and is divided into six sections:

`Auto-Length: Off | Offsets: Depth | Flow Units: CFS | [Run Status] | Zoom Level: 100% | X,Y: -1103.723, 53.191`
![Status Bar](../../Manual/images/status-bar.jpg)

*   **Auto-Length**
    Indicates whether the automatic computation of conduit lengths and subcatchment areas is turned on or off. The setting can be changed by clicking the drop down arrow.

*   **Offsets**
    Indicates whether the positions of links above the invert of their connecting nodes are expressed as a Depth above the node invert or as the Elevation of the offset. Click the drop down arrow to change this option. If changed, a dialog box will appear asking if all existing offsets in the current project should be changed or not (i.e., convert Depth offsets to Elevation offsets or Elevation offsets to Depth offsets, depending on the option selected).

*   **Flow Units**
    Displays the current flow units that are in effect. Click the drop down arrow to change the choice of flow units. Selecting a US flow unit means that all other quantities will be expressed in US units, while choosing a metric flow unit will force all quantities to be expressed in metric units. The units of previously entered data are not automatically adjusted if the unit system is changed.

*   **Run Status**
    *   ![Run status not available icon](../../Manual/images/icon-run-not-available.png) results are not available because no simulation has been run yet.
    *   ![Run status up to date icon](../../Manual/images/icon-run-ok.png) results are up to date.
    *   ![Run status out of date icon](../../Manual/images/icon-run-outdated.png) results are out of date because project data have changed.
    *   ![Run status error icon](../../Manual/images/icon-run-error.png) results are not available because the last simulation had errors.

*   **Zoom Level**
    Displays the current zoom level for the map (100% is full-scale).

*   **XY Location**
    Displays the map coordinates of the current position of the mouse pointer.

## 4.6 Study Area Map

The Study Area Map provides a planar schematic diagram of the objects comprising a drainage system. Its pertinent features are as follows:
*   The location of objects and the distances between them do not necessarily have to conform to their actual physical scale.
*   Selected properties of these objects, such as water quality at nodes or flow velocity in links, can be displayed by using different colors. The color-coding is described in a Legend, which can be edited.
*   New objects can be directly added to the map and existing objects can be selected for editing, deleting, and repositioning.
*   A backdrop drawing (such as a street or topographic map) can be placed behind the network map for reference.
*   The map can be zoomed to any scale and panned from one position to another.
*   Nodes and links can be drawn at different sizes, flow direction arrows added, and object symbols, ID labels and numerical property values displayed.
*   The map can be printed, copied onto the Windows clipboard, or exported as a DXF file or Windows metafile.

![Example Study Area Map showing a network of conduits and nodes over a colored subcatchment layout.](../../Manual/images/study-area-map.png)

## 4.7 Project Browser

The Project Browser panel appears when the Project tab on the left panel of SWMM's main window is selected. It provides access to all of the data objects in a project. The vertical sizes of the list boxes in the browser can be adjusted by using the splitter bar located just below the upper list box. The width of the Browser panel can be adjusted by using the splitter bar located along its right edge.

![Project Browser panel showing a tree view of project data categories (Climatology, Hydrology, Hydraulics) and a list of conduits.](../../Manual/images/project-browser.jpg)

The upper list box displays the various categories of data objects available to a SWMM project. The lower list box lists the name of each individual object of the currently selected data category.

The buttons between the two list boxes are used as follows:
*   **+** adds a new object
*   **-** deletes the selected object
*   *edit icon* edits the selected object
*   *up arrow* moves the selected object up one position
*   *down arrow* moves the selected object down one position
*   *sort icon* sorts the objects in ascending order

Selections made in the Project Browser are coordinated with objects highlighted on the Study Area Map, and vice versa. For example, selecting a conduit in the Browser will cause that conduit to be highlighted on the map, while selecting it on the map will cause it to become the selected object in the Browser.

## 4.8 Map Browser

The Map Browser panel appears when the Map tab on the left panel of the SWMM's main window is selected. It controls the mapping themes and time periods viewed on the Study Area Map. The width of the Map Browser panel can be adjusted by using the splitter bar located along its right edge. The Map Browser consists of the following three panels that control what results are displayed on the map:

![Map Browser panel showing controls for Themes, Time Period, and Animator.](../../Manual/images/map-browser.jpg)

*   The **Themes** panel selects a set of variables to view in color-coded fashion on the Map:
    *   **Subcatchments** - selects the theme to display for the subcatchment areas shown on the Map.
    *   **Nodes** - selects the theme to display for the drainage system nodes shown on the Map.
    *   **Links** - selects the theme to display for the drainage system links shown on the Map.
*   The **Time Period** panel selects which time period of the simulation results are viewed on the Map.
    *   **Date** - selects the day for which simulation results will be viewed.
    *   **Time of Day** - selects the time of the current date for which simulation results will be viewed.
    *   **Elapsed Time** - selects the elapsed time from the start of the simulation (in days.hours:minutes:seconds) for which results will be viewed.
*   The **Animator** panel controls the animated display of the Study Area Map and all Profile Plots over time.
    *   *Rewind* - Returns to the starting period.
    *   *Play backward* - Starts animating backwards in time
    *   *Stop* - Stops the animation
    *   *Play forward* - Starts animating forwards in time
    *   The slider bar is used to adjust the animation speed.

## 4.9 Property Editor

The Property Editor is used to edit the properties of data objects that can appear on the Study Area Map. It is invoked when one of these objects is selected (either on the map or in the Project Browser) and double-clicked or when the Project Browser's Edit button is clicked.

![Property Editor for a conduit, showing properties like Name, Nodes, Shape, Depth, Length, etc.](../../Manual/images/property-editor-conduit.png)

Key features of the Property Editor include:
*   The Editor is a grid with two columns - one for the property's name and the other for its value.
*   The columns can be re-sized by re-sizing the header at the top of the Editor with the mouse.
*   A hint area is displayed at the bottom of the Editor with an expanded description of the property being edited. The size of this area can be adjusted by dragging the splitter bar located just above it.
*   The Editor window can be moved and re-sized via the normal Windows operations.
*   Depending on the property, the value field can be one of the following:
    *   a text box in which you enter a value
    *   a dropdown combo box from which you select a value from a list of choices
    *   a dropdown combo box in which you can enter a value or select from a list of choices
    *   an ellipsis button which you click to bring up a specialized editor.
*   The field in the Editor that currently has the focus will have a focus rectangle drawn around it.
*   Both the mouse and the Up and Down arrow keys on the keyboard can be used to move between property fields.
*   The Page Up key can be used to select the previous object of the same type (as listed in the Project Browser) into the Editor, while the Page Down key will select the next object of the same type into the Editor.
*   To begin editing the property with the focus, either begin typing a value or hit the Enter key.
*   To have the program accept edits made in a property field, either press the Enter key or move to another property. To cancel the edits, press the Esc key.
*   The Property Editor can be hidden by clicking the button in the upper right corner of its title bar.

## 4.10 Setting Program Preferences

Program preferences allow one to customize certain program features. To set program preferences, select Program Preferences from the Tools menu. A Preferences dialog form will appear containing two tabbed pages – one for General Preferences and one for Numerical Precision.

![Preferences dialog showing the General Options and Numerical Precision tabs.](../../Manual/images/preferences.png)
![Preferences dialog showing the General Options and Numerical Precision tabs.](../../Manual/images/preferences2.png)

The following preferences can be set on the **General Preferences** page of the Preferences dialog:

| Preference | Description |
| :--- | :--- |
| **Blinking Map Highlighter** | Check to make the selected object on the study area map blink on and off. |
| **Flyover Map Labeling** | Check to display the ID label and current theme value in a hint-style box whenever the mouse is placed over an object on the study area map. |
| **Confirm Deletions** | Check to display a confirmation dialog box before deleting any object. |
| **Automatic Backup File** | Check to save a backup copy of a newly opened project to disk named with a .bak extension. |
| **Tab Delimited Project File** | Check to use tabs to delimit data values when saving a project to file. |
| **Report Elapsed Time by Default** | Check to use elapsed time (rather than date/time) as the default for time series graphs and tables. |
| **Prompt to Save Results**| If left unchecked then simulation results are automatically saved to disk when the current project is closed. Otherwise the user will be asked if results should be saved. |
| **Show Welcome Screen at Startup** | Check to have SWMM display a welcome screen when started. |
| **Clear Recent Project List** | Check to clear the list of most recently used files appearing when File >> Reopen is selected from the Main Menu. |
| **Style Theme** | Selects a color theme to use for SWMM's user interface. |

![Different style themes available in SWMM Preferences: Windows, Iceberg Classico, Smokey Quartz Kamri.](../../Manual/images/themes2.jpg)
![Different style themes available in SWMM Preferences: Windows, Iceberg Classico, Smokey Quartz Kamri.](../../Manual/images/themes.jpg)
![Different style themes available in SWMM Preferences: Windows, Iceberg Classico, Smokey Quartz Kamri.](../../Manual/images/themes3.jpg)

The **Numerical Precision** page of the Preferences dialog controls the number of decimal places displayed when simulation results are reported. Use the dropdown list boxes to select a specific Subcatchment, Node or Link parameter, and then use the edit boxes next to them to select the number of decimal places to use when displaying computed results for the parameter. Note that there is no such limit to the number of decimal places displayed for any particular input design parameter, such as slope, diameter, length, etc. The number of decimal places displayed is whatever the user enters.