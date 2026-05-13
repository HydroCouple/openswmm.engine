# Chapter 7 WORKING WITH THE MAP

EPA SWMM can display a map of the study area being modeled. This section describes how you can manipulate this map to enhance your visualization of the system.

## 7.1 Viewing Map Layers

The layers that can be viewed on the Study Area consist of rain gages, subcatchments, nodes, links, labels, and the backdrop image. The display of each of these can be toggled on or off by selecting **View >> Layers** from the Main Menu or by right-clicking on the map and selecting **Layers** from the pop-up menu that appears.

## 7.2 Selecting a Map Theme

![SWMM interface showing the Map Browser with map themes for Subcatchments, Nodes, and Links.](../../Manual/images/map-theme.png)

A map theme corresponds to a specific layer property whose value is drawn in color-coded fashion on the Study Area Map. The dropdown list boxes on the Map Browser are used for selecting a theme to display for the subcatchment, node and link layers. Methods for changing the color-coding associated with a theme are discussed in Section 7.10 below.

## 7.3 Setting the Map's Dimensions

The physical dimensions of the map can be defined so that map coordinates can be properly scaled to the computer's video display. To set the map's dimensions:

1.  Select **View >> Dimensions** from the Main Menu.
2.  Enter coordinates for the lower-left and upper-right corners of the map into the Map Dimensions dialog (see below) that appears or click the **Auto-Size** button to automatically set the dimensions based on the coordinates of the objects currently included in the map.

    ![Map Dimensions dialog box for setting map coordinates and units.](../../Manual/images/map-dimensions.png)

3.  Select the distance units to use for these coordinates.
4.  If the **Auto-Length** option is in effect, check the “Re-compute all lengths and areas" box if you would like SWMM to re-calculate all conduit lengths and subcatchment areas under the new set of map dimensions.
5.  Click the **OK** button to resize the map.

> **Note:** If you are going to use a backdrop image with the automatic distance and area calculation feature, then it is recommended that you set the map dimensions immediately after creating a new project. Map distance units can be different from conduit length units. The latter (feet or meters) depend on whether flow rates are expressed in US or metric units. SWMM will automatically convert from map units if necessary.

> **Note:** If you just want to re-compute conduit lengths and subcatchment areas without changing the map's dimensions, then just check the Re-compute Lengths and Areas box and leave the coordinate boxes as they are.

## 7.4 Utilizing a Backdrop Image

SWMM can display a backdrop image behind the Study Area Map. The backdrop image might be a street map, utility map, topographic map, site development plan, or any other relevant picture or drawing. For example, using a street map would simplify the process of adding sewer lines to the project since one could essentially digitize the drainage system's nodes and links directly on top of it.

![Study Area Map with a topographic map as a backdrop image.](../../Manual/images/backdrop-image.png)

The backdrop image must be a Windows metafile, bitmap, JPEG, or PNG image created outside of SWMM. Once imported, its features cannot be edited, although its scale and viewing area will change as the map window is zoomed and panned. For this reason metafiles work better than the other formats since they will not lose resolution when re-scaled. Most CAD and GIS programs have the ability to save their drawings and maps as metafiles.

Selecting **View >> Backdrop** from the Main Menu will display a sub-menu with the following commands:
*   **Load** (loads a backdrop image file into the project)
*   **Unload** (unloads the backdrop image from the project)
*   **Align** (aligns the drainage system schematic with the backdrop)
*   **Resize** (resizes the map dimensions of the backdrop)
*   **Watermark** (toggles the backdrop image appearance between normal and lightened)

To load a backdrop image select **View >> Backdrop >> Load** from the Main Menu. A Backdrop Image Selector dialog form will be displayed. The entries on this form are as follows:

![Backdrop Image Selector dialog box.](../../Manual/images/backdrop-selector.png)

**Backdrop Image File**

Enter the name of the file that contains the image. You can click the folder icon button to bring up a standard Windows file selection dialog from which you can search for the image file.

**World Coordinates File**

If a "world" file exists for the image, enter its name here, or click the folder icon button to search for it. A world file contains geo-referencing information for the image and can be created from the software that produced the image file or by using a text editor. It contains six lines with the following information:

1.  Line 1: real world width of a pixel in the horizontal direction.
2.  Line 2: X rotation parameter (not used).
3.  Line 3: Y rotation parameter (not used).
4.  Line 4: negative of the real world height of a pixel in the vertical direction.
5.  Line 5: real world X coordinate of the upper left corner of the image.
6.  Line 6: real world Y coordinate of the upper left corner of the image.

If no world file is specified, then the backdrop will be scaled to fit into the center of the map display window.

**Scale Map to Backdrop Image**

This option is only available when a world file has been specified. Selecting it forces the dimensions of the Study Area Map to coincide with those of the backdrop image. In addition, all existing objects on the map will have their coordinates adjusted so that they appear within the new map dimensions yet maintain their relative positions to one another. Selecting this option may then require that the backdrop be re-aligned so that its position relative to the drainage area objects is correct. How to do this is described below.

The backdrop image can be re-positioned relative to the drainage system by selecting **View >> Backdrop >> Align**. This allows the backdrop image to be moved across the drainage system (by moving the mouse with the left button held down) until one decides that it lines up properly.

The backdrop image can also be resized by selecting **View >> Backdrop >> Resize**. In this case a Backdrop Dimensions dialog will appear (see next page). The dialog lets you manually enter the X,Y coordinates of the backdrop's lower left and upper right corners. The Study Area Map's dimensions are also displayed for reference. While the dialog is visible you can view map coordinates by moving the mouse over the map window and noting the X,Y values displayed in SWMM's Status Panel (at the bottom of the main window).

![Backdrop Dimensions dialog box.](../../Manual/images/backdrop-dimensions.png)

Selecting the **Resize Backdrop Image Only** button will resize only the backdrop, and not the Study Area Map, according to the coordinates specified. Selecting the **Scale Backdrop Image to Map** button will position the backdrop image in the center of the Study Area Map and have it resized to fill the display window without changing its aspect ratio. The map's lower left and upper right coordinates will be placed in the data entry fields for the backdrop coordinates, and these fields will become disabled. Selecting **Scale Map to Backdrop Image** makes the dimensions of the map coincide with the dimensions being set for the backdrop image. Note that this option will change the coordinates of all objects currently on the map so that their positions relative to one another remain unchanged. Selecting this option may then require that the backdrop be re-aligned so that its position relative to the drainage area objects is correct.

> **Warning:** Exercise caution when selecting the **Scale Map to Backdrop Image** option in either the Backdrop Image Selector dialog or the Backdrop Dimensions dialog as it will modify the coordinates of all existing objects currently on the Study Area Map. You might want to save your project before carrying out this step in case the results are not what you expected.

The name of the backdrop image file and its map dimensions are saved along with the rest of a project's data whenever the project is saved to file.

For best results in using a backdrop image:
*   Use a metafile, not a bitmap.
*   If the image is loaded before any objects are added to the project then scale the map to it.

## 7.5 Measuring Distances

To measure a distance or area on the Study Area Map:

1.  Click the measure tool icon on the Map Toolbar.
2.  Left-click on the map where you wish to begin measuring from.
3.  Move the mouse over the distance being measured, left-clicking at each intermediate location where the measured path changes direction.
4.  Right-click the mouse or press `<Enter>` to complete the measurement.
5.  The distance measured in project units (feet or meters) will be displayed in a dialog box. If the last point on the measured path coincides with the first point then the area of the enclosed polygon will also be displayed.

## 7.6 Zooming the Map

To Zoom In on the Study Area Map:

1.  Select **View >> Zoom In** from the Main Menu or click the zoom in icon on the Map Toolbar.
2.  To zoom in 100% (i.e., 2X), move the mouse to the center of the zoom area and click the left button.
3.  To perform a custom zoom, move the mouse to the upper left corner of the zoom area and with the left button pressed down, draw a rectangular outline around the zoom area. Then release the left button.

To Zoom Out on the Study Area Map:

1.  Select **View >> Zoom Out** from the Main Menu or click the zoom out icon on the Toolbar.
2.  The map will be returned to the view in effect at the previous zoom level.

The mouse wheel can also be used to zoom in and out on the map at any time.

## 7.7 Panning the Map

To pan across the Study Area Map window:

1.  Select **View >> Pan** from the Main Menu or click the pan icon on the Map Toolbar.
2.  With the left button held down over any point on the map, drag the mouse in the direction you wish to pan.
3.  Release the mouse button to complete the pan.

To pan using the Overview Map (which is described in Section 7.11 below):

1.  If not already visible, bring up the Overview Map by selecting **View >> Overview Map** from the Main Menu or click the overview map button on the Main Toolbar.
2.  If the Study Area Map has been zoomed in, an outline of the current viewing area will appear on the Overview Map. Position the mouse within this outline on the Overview Map.
3.  With the left button held down, drag the outline to a new position.
4.  Release the mouse button and the Study Area Map will be panned to an area corresponding to the outline on the Overview Map.

The mouse wheel can also be use to pan the Study Area Map at any time by holding it down and dragging the mouse in the direction you wish to pan.

## 7.8 Viewing at Full Extent

To view the Study Area Map at full extent, either:
*   select **View >> Full Extent** from the Main Menu, or
*   press the full extent icon on the Map Toolbar.

## 7.9 Finding an Object

To find an object on the Study Area Map whose name is known:

1.  Select **View >> Find Object** from the Main Menu or click the find object icon on the Main Toolbar.
2.  In the Map Finder dialog that appears, select the type of object to find and enter its name.
3.  Click the **Go** button.

![Map Finder dialog box.](../../Manual/images/map-finder.png)

If the object exists, it will be highlighted on the map and in the Data Browser. If the map is currently zoomed in and the object falls outside the current map boundaries, the map will be panned so that the object comes into view.

> **Note:** User-assigned object names in SWMM are not case sensitive. E.g., NODE123 is equivalent to Node123.

After an object is found, the Map Finder dialog will also list:
*   the outlet connections for a subcatchment
*   the connecting links for a node
*   the connecting nodes for a link.

## 7.10 Submitting a Map Query

A Map Query identifies objects on the study area map that meet a specific criterion (e.g., nodes which flood, links with velocity below 2 ft/sec, etc.). It can also identify which subcatchments have LID controls and which nodes have external inflows. To submit a map query:

1.  Select a time period in which to query the map from the Map Browser.
2.  Select **View >> Query** or click the query icon on the Main Toolbar.
3.  Fill in the following information in the Query dialog that appears:
    *   Select whether to search for Subcatchments, Nodes, Links, LID Subcatchments or Inflow Nodes.
    *   Select a parameter to query or the type of LID or inflow to locate.
    *   Select the appropriate operator: Above, Below, or Equals.
    *   Enter a value to compare against.
4.  Click the **Go** button. The number of objects that meet the criterion will be displayed in the Query dialog and each such object will be highlighted on the Study Area Map.
5.  As a new time period is selected in the Browser, the query results are automatically updated.
6.  You can submit another query using the dialog box or close it by clicking the button in the upper right corner.

![Study Area Map with the Map Query dialog box open, showing a query for nodes with flooding.](../../Manual/images/map-query.png)

After the Query box is closed the map will revert back to its original display.

## 7.11 Using the Map Legends

![Map legend for flow.](../../Manual/images/map-legend.jpg)

Map Legends associate a color with a range of values for the current theme being viewed. Separate legends exist for Subcatchments, Nodes, and Links. A Date/Time Legend is also available for displaying the date and clock time of the simulation period being viewed on the map.

To display or hide a map legend:

1.  Select **View >> Legends** from the Main Menu or right-click on the map and select Legends from the pop-up menu that appears
2.  Click on the type of legend whose display should be toggled on or off.

A visible legend can also be hidden by double clicking on it.

To move a legend to another location press the left mouse button over the legend, drag the legend to its new location with the button held down, and then release the button.

To edit a legend, either select **View >> Legends >> Modify** from the Main Menu or right-click on the legend if it is visible. Then use the Legend Editor dialog that appears to modify the legend's colors and intervals.

![Legend Editor dialog box.](../../Manual/images/legend-editor.png)

The Legend Editor is used to set numerical ranges to which different colors are assigned for viewing a particular parameter on the network map. It works as follows:
*   Numerical values, in increasing order, are entered in the edit boxes to define the ranges. Not all four boxes need to have values.
*   To change a color, click on its color band in the Editor and then select a new color from the Color Dialog that will appear.
*   Click the **Auto-Scale** button to automatically assign ranges based on the minimum and maximum values attained by the parameter in question at the current time period.
*   The **Color Ramp** button is used to select from a list of built-in color schemes.
*   The **Reverse Colors** button reverses the ordering of the current set of colors (the color in the lowest range becomes that of the highest range and so on).
*   Check **Framed** if you want a frame drawn around the legend.

Changes made to a legend are saved with the project's settings and remain in effect when the project is re-opened in a subsequent session.

## 7.12 Using the Overview Map

The Overview Map, as pictured below, allows one to see where in terms of the overall system the main Study Area Map is currently focused. This zoom area is depicted by the rectangular outline displayed on the Overview Map. As you drag this rectangle to another position the view within the main map will be redrawn accordingly. The Overview Map can be toggled on and off by selecting **View >> Overview Map** from the Main Menu or by clicking the overview icon on the Main Toolbar. The Overview Map window can also be dragged to any position as well as be re-sized.

![The Study Area Map with the Overview Map window visible, showing a zoomed-in portion of the main map.](../../Manual/images/overview-map.png)

## 7.13 Setting Map Display Options

The Map Options dialog (shown below) is used to change the appearance of the Study Area Map. There are several ways to invoke it:
*   select **Tools >> Map Display Options** from the Main Menu or,
*   click the **Options** button on the Main Toolbar when the Study Area Map window has the focus or,
*   right-click on any empty portion of the map and select **Options** from the popup menu that appears.

![Map Options dialog box.](../../Manual/images/map-options.png)

The dialog contains a separate page, selected from the panel on the left side of the form, for each of the following display option categories:
*   **Subcatchments** (controls fill style, symbol size, and outline thickness of subcatchment areas)
*   **Nodes** (controls size of nodes and making size be proportional to value)
*   **Links** (controls thickness of links and making thickness be proportional to value)
*   **Labels** (turns display of map labels on/off)
*   **Annotation** (displays or hides node/link ID labels and parameter values)
*   **Symbols** (turns display of storage unit, pump, and regulator symbols on/off)
*   **Flow Arrows** (selects visibility and style of flow direction arrows)
*   **Background** (changes color of map's background).

### Subcatchment Options

The Subcatchments page of the Map Options dialog controls how subcatchment areas are displayed on the study area map.

| Option | Description |
| :--- | :--- |
| **Fill Style** | Selects style used to fill interior of subcatchment area |
| **Symbol Size**| Sets the size of the symbol (in pixels) placed at the centroid of a subcatchment area |
| **Border Size**| Sets the thickness of the line used to draw a subcatchment's border; if set to zero then only the subcatchment centroid will be displayed |
| **Display Link to Outlet** | If checked then a dashed line is drawn between the subcatchment centroid and the subcatchment's outlet node (or outlet subcatchment) |

### Node Options

The Nodes page of the Map Options dialog controls how nodes are displayed on the study area map.

| Option | Description |
| :--- | :--- |
| **Node Size** | Selects node diameter in pixels |
| **Proportional to Value**| Select if node size should increase as the viewed parameter increases in value |
| **Display Border** | Select if a border should be drawn around each node (recommended for light-colored backgrounds) |

### Link Options

The Links page of the Map Options dialog controls how links are displayed on the map.

| Option | Description |
| :--- | :--- |
| **Link Size** | Sets thickness of links displayed on map (in pixels) |
| **Proportional to Value**| Select if link thickness should increase as the viewed parameter increases in value |
| **Display Border** | Check if a black border should be drawn around each link |

### Label Options

The Labels page of the Map Options dialog controls how user-created map labels are displayed on the study area map.

| Option | Description |
| :--- | :--- |
| **Use Transparent Text** | Check to display label with a transparent background (otherwise an opaque background is used) |
| **At Zoom Of**| Selects minimum zoom at which labels should be displayed; labels will be hidden at zooms smaller than this |

### Annotation Options

The Annotation page of the Map Options dialog form determines what kind of annotation is provided alongside of the objects on the study area map.

| Option | Description |
| :--- | :--- |
| **Rain Gage IDs** | Check to display rain gage ID names |
| **Subcatch IDs** | Check to display subcatchment ID names |
| **Node IDs** | Check to display node ID names |
| **Link IDs** | Check to display link ID names |
| **Subcatch Values**| Check to display value of current subcatchment variable |
| **Node Values** | Check to display value of current node variable |
| **Link Values** | Check to display value of current link variable |
| **Use Transparent Text** | Check to display text with a transparent background (otherwise an opaque background is used) |
| **Font Size** | Adjusts the size of the font used to display annotation |
| **At Zoom Of** | Selects minimum zoom at which annotation should be displayed; all annotation will be hidden at zooms smaller than this |

### Symbol Options

The Symbols page of the Map Options dialog determines which types of objects are represented with special symbols on the map.

| Option | Description |
| :--- | :--- |
| **Display Node Symbols** | If checked then special node symbols will be used |
| **Display Link Symbols** | If checked then special link symbols will be used |
| **At Zoom Of** | Selects minimum zoom at which symbols should be displayed; symbols will be hidden at zooms smaller than this |

### Flow Arrow Options

The Flow Arrows page of the Map Options dialog controls how flow-direction arrows are displayed on the map.

| Option | Description |
| :--- | :--- |
| **Arrow Style**| Selects style (shape) of arrow to display (select None to hide arrows) |
| **Arrow Size** | Sets arrow size |
| **At Zoom Of** | Selects minimum zoom at which arrows should be displayed; arrows will be hidden at zooms smaller than this |

> **Note:** Flow direction arrows will only be displayed after a successful simulation has been made and a computed parameter has been selected for viewing. Otherwise the direction arrow will point from the user-designated start node to end node.

### Background Options

The Background page of the Map Options dialog offers a selection of colors used to paint the map's background.

## 7.14 Exporting the Map

The full extent view of the study area map can be saved to file using either:
*   Autodesk's DXF (Drawing Exchange Format) format,
*   the Windows enhanced metafile (EMF) format,
*   EPA SWMM's own ASCII text (.map) format.

The DXF format is readable by many Computer Aided Design (CAD) programs. Metafiles can be inserted into word processing documents and loaded into drawing programs for re-scaling and editing. Both formats are vector-based and will not lose resolution when they are displayed at different scales.

To export the map to a DXF, metafile, or text file:

1.  Select **File >> Export >> Map**.
2.  In the Map Export dialog that appears select the format that you want the map saved in.

![Map Export dialog box.](../../Manual/images/map-export.png)

If you select DXF format, you have a choice of how nodes will be represented in the DXF file. They can be drawn as filled circles, as open circles, or as filled squares. Not all DXF readers can recognize the format used in the DXF file to draw a filled circle. Also note that map annotation, such as node and link ID labels will not be exported, but map label objects will be.

After choosing a format, click **OK** and enter a name for the file in the Save As dialog that appears.