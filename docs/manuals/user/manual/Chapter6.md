# Chapter 6 WORKING WITH OBJECTS

SWMM uses various types of objects to model a drainage area and its conveyance system. This section describes how these objects can be created, selected, edited, deleted, and repositioned.

## 6.1 Types of Objects

SWMM contains both physical objects that can appear on its Study Area Map, and non-physical objects that encompass design, loading, and operational information. These objects, which are listed in the Project Browser and were described in Chapter 3, consist of the following:

*   Project Title/Notes
*   Simulation Options
*   Climatology
*   Rain Gages
*   Subcatchments
*   Aquifers
*   Snow Packs
*   Unit Hydrographs
*   LID Controls
*   Pollutants
*   Land Uses
*   Nodes
*   Links
*   Transects
*   Streets
*   Inlets
*   Control Rules
*   Curves
*   Time Series
*   Time Patterns
*   Map Labels

## 6.2 Adding Objects

To add a new object to a project, select the type of object from the upper pane of the Project Browser and either select **Project >> Add a New ...** from the Main Menu or click the Browser's **+** button. If the object has a button on the Map Toolbar you can simply click the button instead.

If the object is a visual object that appears on the Study Area Map (a Rain Gage, Subcatchment, Node, Link, or Map Label) it will automatically receive a default ID name and a prompt will appear in the Status Bar telling you how to proceed. The steps used to draw each of these objects on the map are detailed below:

**Rain Gages**

Move the mouse to the desired location on the Map and left-click.

**Subcatchments**

Use the mouse to draw a polygon outline of the subcatchment on the Map:
*   left-click at each vertex
*   right-click or press `<Enter>` to close the polygon
*   press the `<Esc>` key if you wish to cancel the action.

**Nodes (Junctions, Outfalls, Flow Dividers, and Storage Units)**

Move the mouse to the desired location on the Study Area Map and left-click.

**Links (Conduits, Pumps, Orifices, Weirs, and Outlets)**
*   Left-click the mouse on the link's inlet (upstream) node.
*   Move the mouse (without pressing any button) in the direction of the link's outlet (downstream) node, clicking at all intermediate points necessary to define the link's alignment.
*   Left-click the mouse a final time over the link's outlet (downstream) node. (Pressing the right mouse button or the `<Esc>` key while drawing a link will cancel the operation.)

**Map Labels**
*   Left-click the mouse on the map location where the top left corner of the label should appear.
*   Enter the text for the label.
*   Press `<Enter>` to accept the label or `<Esc>` to cancel.

For all other non-visual types of objects, an object-specific dialog form will appear that allows you to name the object and edit its properties.

## 6.3 Selecting and Moving Objects

To select an object on the map:

1.  Make sure that the map is in Selection mode (the mouse cursor has the shape of an arrow pointing up to the left). To switch to this mode, either click the **Select Object** button on the Map Toolbar or choose **Edit >> Select Object** from the Main Menu.
2.  Click the mouse over the desired object on the map.

To select an object using the Project Browser:

1.  Select the object's category from the upper list in the Browser.
2.  Select the object from the lower list in the Browser.

Rain gages, subcatchments, nodes, and map labels can be moved to another location on the Study Area Map. To move an object to another location:

1.  Select the object on the map.
2.  With the left mouse button held down over the object, drag it to its new location.
3.  Release the mouse button.

The following alternative method can also be used:

1.  Select the object to be moved from the Project Browser (it must either be a rain gage, subcatchment, node, or map label).
2.  With the left mouse button held down, drag the item from the Items list box of the Data Browser to its new location on the map.
3.  Release the mouse button.

Note that the second method can be used to place objects on the map that were imported from a project file that had no coordinate information included in it.

## 6.4 Editing Objects

To edit an object appearing on the Study Area Map:

1.  Select the object on the map.
2.  If the Property Editor is not visible either:
    *   double click on the object
    *   or right-click on the object and select **Properties** from the pop-up menu that appears
    *   or click on the edit icon in the Project Browser
    *   or select **Edit >> Edit Object** from the Main Menu.
3.  Edit the object's properties in the Property Editor.

Appendix B lists the properties associated with each of SWMM's visual objects.

To edit an object listed in the Project Browser:

1.  Select the object in the Project Browser.
2.  Either:
    *   click on the edit icon in the Project Browser,
    *   or select **Edit >> Edit Object** from the Main Menu,
    *   or double-click the item in the Objects list,
    *   or press the `<Enter>` key.

Depending on the class of object selected, a special property editor will appear in which the object's properties can be modified. Appendix C describes all of the special property editors used with SWMM's non-visual objects.

> **Note:** The unit system in which object properties are expressed depends on the choice of units for flow rate. Using a flow rate expressed in cubic feet, gallons or acre-feet implies that US customary units will be used for all quantities. Using a flow rate expressed in liters or cubic meters means that SI metric units will be used. Flow units are selected either from the project's default Node/Link properties (see Section 5.4) or directly from the main window's Status Bar (see Section 4.5). The units used for all properties are listed in Appendix A.1.

## 6.5 Converting an Object

It is possible to convert a node or link from one type to another without having to first delete the object and add a new one in its place. An example would be converting a Junction node into an Outfall node, or converting an Orifice link into a Weir link. To convert a node or link to another type:

1.  Right-click the object on the map.
2.  Select **Convert To** from the popup menu that appears.
3.  Select the new type of node or link to convert to from the sub-menu that appears.
4.  Edit the object to provide any data that was not included with the previous type of object.

Only data that is common to both types of objects will be preserved after an object is converted to a different type. For nodes this includes its name, position, description, tag, external inflows, treatment functions, and invert elevation. For links it includes just its name, end nodes, description, and tag.

## 6.6 Copying and Pasting Objects

The properties of an object displayed on the Study Area Map can be copied and pasted into another object from the same category.

To copy the properties of an object to SWMM's internal clipboard:

1.  Right-click the object on the map.
2.  Select **Copy** from the pop-up menu that appears.

To paste copied properties into an object:

1.  Right-click the object on the map.
2.  Select **Paste** from the pop-up menu that appears.

Only data that can be shared between objects of the same type can be copied and pasted. Properties not copied include the object's name, coordinates, end nodes (for links), tag property and any descriptive comment associated with the object. For Map Labels, only font properties are copied and pasted.

## 6.7 Shaping and Reversing Links

Links can be drawn as polylines containing any number of straight-line segments that define the alignment or curvature of the link. Once a link has been drawn on the map, interior points that define these line segments can be added, deleted, and moved. To edit the interior points of a link:

1.  Select the link to edit on the map and put the map in Vertex Selection mode either by clicking the vertex selection icon on the Map Toolbar, selecting **Edit >> Select Vertex** from the Main Menu, or right clicking on the link and selecting **Vertices** from the popup menu.
2.  The mouse pointer will change shape to an arrow tip, and any existing vertex points on the link will be displayed as small open squares. The currently selected vertex will be displayed as a filled square. To select a particular vertex, click the mouse over it.
3.  To add a new vertex to the link, right-click the mouse and select **Add Vertex** from the popup menu (or simply press the `<Insert>` key on the keyboard).
4.  To delete the currently selected vertex, right-click the mouse and select **Delete Vertex** from the popup menu (or simply press the `<Delete>` key on the keyboard).
5.  To move a vertex to another location, drag it to its new position with the left mouse button held down.

While in Vertex Selection mode you can begin editing the vertices for another link by simply clicking on the link. To leave Vertex Selection mode, right-click on the map and select **Quit Editing** from the popup menu, or simply select one of the other buttons on the Map Toolbar.

A link can also have its direction reversed (i.e., its end nodes switched) by right clicking on it and selecting **Reverse** from the pop-up menu that appears. Normally, links should be oriented so that the upstream end is at a higher elevation than the downstream end.

## 6.8 Shaping a Subcatchment

Subcatchments are drawn on the Study Area Map as closed polygons. To edit or add vertices to the polygon, follow the same procedures used for links. If the subcatchment is originally drawn or is edited to have two or less vertices, then only its centroid symbol will be displayed on the Study Area Map.

## 6.9 Deleting an Object

To delete an object:

1.  Select the object on the Study Area Map or from the Project Browser.
2.  Either click the **-** button on the Project Browser or press the `<Delete>` key on the keyboard, or select **Edit >> Delete Object** from the Main Menu, or right-click the object on the map and select **Delete** from the pop-up menu that appears.

> **Note:** You can require that all deletions be confirmed before they take effect. See the General Preferences page of the Program Preferences dialog box described in Section 4.9.

## 6.10 Editing or Deleting a Group of Objects

A group of objects located within an irregular region of the Study Area Map can have a common property edited or be deleted all together. To select such a group of objects:

1.  Choose **Edit >> Select Region** from the Main Menu or click the select region icon on the Map Toolbar.
2.  Draw a polygon around the region of interest on the map by clicking the left mouse button at each successive vertex of the polygon.
3.  Close the polygon by clicking the right button or by pressing the `<Enter>` key; cancel the selection by pressing the `<Esc>` key.

To select all objects in the project, whether in view or not, select **Edit >> Select All** from the Main Menu.

Once a group of objects has been selected, you can edit a common property shared among them:

1.  Select **Edit >> Group Edit** from the Main Menu.
2.  Use the Group Editor dialog that appears to select a property and specify its new value.

The Group Editor dialog, shown below, is used to modify a property for a selected group of objects. To use the dialog:

![Group Editor dialog box for editing properties of multiple objects at once.](../../Manual/images/group-editor.png)

1.  Select a type of object (Subcatchments, Infiltration, Junctions, Storage Units, or Conduits) to edit.
2.  Check the "with Tag equal to" box if you want to add a filter that will limit the objects selected for editing to those with a specific Tag value. (For Infiltration, the Tag will be that of the subcatchment to which the infiltration parameters belong.)
3.  Enter a Tag value to filter on if you have selected that option.
4.  Select the property to edit.
5.  Select whether to replace, multiply, or add to the existing value of the property. Note that for some non-numerical properties the only available choice is to replace the value.
6.  In the lower-right edit box, enter the value that should replace, multiply, or be added to the existing value for all selected objects. Some properties will have an ellipsis button displayed in the edit box which should be clicked to bring up a specialized editor for the property.
7.  Click OK to execute the group edit.  

After the group edit is executed a confirmation dialog box will appear informing you of how many items were modified. It will ask if you wish to continue editing or not. Select Yes to return to the Group Edit dialog box to edit another parameter or No to dismiss the Group Edit dialog.

To delete the objects located within a selected area of the map, select Edit >> Group Delete from the Main Menu. Then select the categories of objects you wish to delete from the dialog box that appears. As an option, you can specify that only objects within the selected area that have a specific Tag property should be deleted. Keep in mind that deleting a node will also delete any links connected to the node. 
