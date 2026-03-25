# Storm Water Management Model
## User's Manual Version 5.2

**United States Environmental Protection Agency**  
**Office of Research and Development**  
**Washington, D.C. 20460**

**ΕΡΑ-600/R-22/xxx**  
**February 2022**  
**www.epa.gov**

---

## Disclaimer

> The information in this document has been funded wholly or in part by the U.S. Environmental Protection Agency (EPA). It has been subjected to the Agency's peer and administrative review, and has been approved for publication as an EPA document. Note that approval does not signify that the contents necessarily reflect the views of the Agency. Mention of trade names or commercial products does not constitute endorsement or recommendation for use.

> **NOTICE:** This report was prepared as an account of work sponsored by an agency of the United States Government. Neither the United States Government, nor any agency thereof, nor any of their employees, nor any of their contractors, subcontractors, or their employees, make any warranty, express or implied, or assume any legal liability or responsibility for the accuracy, completeness, or usefulness of any information, apparatus, product, or process disclosed, or represent that its use would not infringe privately owned rights. Reference herein to any specific commercial product, process, or service by trade name, trademark, manufacturer, or otherwise, does not necessarily constitute or imply its endorsement, recommendation, or favoring by the United States Government, any agency thereof, or any of their contractors or subcontractors. The views and opinions expressed herein do not necessarily state or reflect those of the United States Government, any agency thereof, or any of their contractors.

---

## Abstract

The EPA Storm Water Management Model (SWMM) is a dynamic rainfall-runoff simulation model used for single event or long-term (continuous) simulation of runoff quantity and quality from primarily urban areas. The runoff component of SWMM operates on a collection of subcatchment areas that receive precipitation and generate runoff and pollutant loads. The routing portion of SWMM transports this runoff through a system of pipes, channels, storage/treatment devices, pumps, and regulators. SWMM tracks the quantity and quality of runoff generated within each subcatchment, and the flow rate, flow depth, and quality of water in each pipe and channel during a simulation period comprised of multiple time steps. Running under Windows, SWMM 5 provides an integrated environment for editing study area input data, running hydrologic, hydraulic and water quality simulations, and viewing the results in a variety of formats. These include color-coded drainage area and conveyance system maps, time series graphs and tables, profile plots, and statistical frequency analyses. This user's manual describes in detail how to run SWMM 5.2. It includes instructions on how to build a drainage system model, how to set various simulation options, and how to view results in a variety of formats. It also describes the different types of files used by SWMM and provides useful tables of parameter values. Detailed descriptions of the theory behind SWMM 5 and the numerical methods it employs can be found in a separate set of reference manuals.

---

## Forward

The U.S. Environmental Protection Agency (EPA) is charged by Congress with protecting the Nation's land, air, and water resources. Under a mandate of national environmental laws, the Agency strives to formulate and implement actions leading to a compatible balance between human activities and the ability of natural systems to support and nurture life. To meet this mandate, EPA's research program is providing data and technical support for solving environmental problems today and building a science knowledge base necessary to manage our ecological resources wisely, understand how pollutants affect our health, and prevent or reduce environmental risks in the future.

The Center for Environmental Solutions and Emergency Response (CESER) within the Office of Research and Development (ORD) is the Agency's center for investigation of technological and management approaches for preventing and reducing risks from pollution that threaten human health and the environment. The focus of the Center's research program is on methods and their cost-effectiveness for prevention and control of pollution to air, land, water, and subsurface resources; protection of water quality in public water systems; remediation of contaminated sites, sediments and ground water; prevention and control of indoor air pollution; and restoration of ecosystems. CESER collaborates with both public and private sector partners to foster technologies that reduce the cost of compliance and to anticipate emerging problems. CESER's research provides solutions to environmental problems by: developing and promoting technologies that protect and improve the environment; advancing scientific and engineering information to support regulatory and policy decisions; and providing the technical support and information transfer to ensure implementation of environmental regulations and strategies at the national, state, and community levels.

EPA's Storm Water Management Model (SWMM) is used throughout the world for planning, analysis, and design related to stormwater runoff, combined and sanitary sewers, and other drainage systems. It can be used to evaluate gray infrastructure stormwater control strategies, such as pipes and storm drains, and is a useful tool for creating cost-effective green/gray hybrid stormwater control solutions. SWMM was developed to help support local, state, and national stormwater management objectives to reduce runoff through infiltration and retention, and help to reduce discharges that cause impairment of waterbodies.

**Gregory Sayles, PhD., Director**  
**Center for Environmental Solutions and Emergency Response**

---

## Acknowledgments

This User's Manual was prepared by Lewis A. Rossman, Environmental Scientist Retired, U.S. Environmental Protection Agency (USEPA), Cincinnati, OH by adding instructions on how to use new program features that were added since the release of manual's previous edition.

This document was reviewed by Michelle Simon, Katherine Ratliff, and Anne Mikelonis, all of the USEPA, by Robert Dickinson (Innovyze), Mitch Heineman (CDM Smith), Mike Gregory (CHI), and Nandana Perera (CHI).

---

## CONTENTS

*   **DISCLAIMER**
*   **ABSTRACT**
*   **FORWARD**
*   **ACKNOWLEDGMENTS**

### CHAPTER 1: INTRODUCTION
*   1.1 What is SWMM
*   1.2 Modeling Capabilities
*   1.3 Typical Applications of SWMM
*   1.4 Installing EPA SWMM
*   1.5 Steps in Using SWMM
*   1.6 About This Manual

### CHAPTER 2: QUICK START TUTORIAL
*   2.1 Example Study Area
*   2.2 Project Setup
*   2.3 Drawing Objects
*   2.4 Setting Object Properties
*   2.5 Running a Simulation
*   2.6 Simulating Water Quality
*   2.7 Running a Continuous Simulation

### CHAPTER 3: SWMM'S CONCEPTUAL MODEL
*   3.1 Introduction
*   3.2 Visual Objects
*   3.3 Non-Visual Objects
*   3.4 Computational Methods

### CHAPTER 4: SWMM'S MAIN WINDOW
*   4.1 Overview
*   4.2 Main Menu
*   4.3 Keyboard Shortcuts
*   4.4 Toolbars
*   4.5 Status Bar
*   4.6 Study Area Map
*   4.7 Project Browser
*   4.8 Map Browser
*   4.9 Property Editor
*   4.10 Setting Program Preferences

### CHAPTER 5: WORKING WITH PROJECTS
*   5.1 Creating a New Project
*   5.2 Opening an Existing Project
*   5.3 Saving a Project
*   5.4 Setting Project Defaults
*   5.5 Measurement Units
*   5.6 Link Offset Conventions
*   5.7 Calibration Data
*   5.8 Viewing All Project Data

### CHAPTER 6: WORKING WITH OBJECTS
*   6.1 Types of Objects
*   6.2 Adding Objects
*   6.3 Selecting and Moving Objects
*   6.4 Editing Objects
*   6.5 Converting an Object
*   6.6 Copying and Pasting Objects
*   6.7 Shaping and Reversing Links
*   6.8 Shaping a Subcatchment
*   6.9 Deleting an Object
*   6.10 Editing or Deleting a Group of Objects

### CHAPTER 7: WORKING WITH THE MAP
*   7.1 Viewing Map Layers
*   7.2 Selecting a Map Theme
*   7.3 Setting the Map's Dimensions
*   7.4 Utilizing a Backdrop Image
*   7.5 Measuring Distances
*   7.6 Zooming the Map
*   7.7 Panning the Map
*   7.8 Viewing at Full Extent
*   7.9 Finding an Object
*   7.10 Submitting a Map Query
*   7.11 Using the Map Legends
*   7.12 Using the Overview Map
*   7.13 Setting Map Display Options
*   7.14 Exporting the Map

### CHAPTER 8: RUNNING A SIMULATION
*   8.1 Setting Simulation Options
*   8.2 Setting Reporting Options
*   8.3 Selecting Event Periods
*   8.4 Starting a Simulation
*   8.5 Troubleshooting Results

### CHAPTER 9: VIEWING RESULTS
*   9.1 Viewing a Status Report
*   9.2 Viewing Summary Results
*   9.3 Time Series Results
*   9.4 Viewing Results on the Map
*   9.5 Viewing Results with a Graph
*   9.6 Customizing a Graph's Appearance
*   9.7 Viewing Results with a Table
*   9.8 Viewing a Statistics Report

### CHAPTER 10: PRINTING AND COPYING
*   10.1 Selecting a Printer
*   10.2 Setting the Page Format
*   10.3 Print Preview
*   10.4 Printing the Current View
*   10.5 Copying to the Clipboard or to a File

### CHAPTER 11: FILES USED BY SWMM
*   11.1 Project Files
*   11.2 Report and Output Files
*   11.3 Rainfall Files
*   11.4 Climate Files
*   11.5 Calibration Files
*   11.6 Time Series Files
*   11.7 Interface Files

### CHAPTER 12: USING ADD-IN TOOLS
*   12.1 What Are Add-In Tools
*   12.2 Configuring Add-In Tools

### APPENDIX A: USEFUL TABLES
*   A.1 Units of Measurement
*   A.2 Soil Characteristics
*   A.3 NRCS Hydrologic Soil Group Definitions
*   A.4 SCS Curve Numbers
*   A.5 Depression Storage
*   A.6 Manning's Coefficient (n) – Overland Flow
*   A.7 Manning's Coefficient (n) – Closed Conduits
*   A.8 Manning's Coefficient (n) – Open Channels
*   A.9 Water Quality Characteristics of Urban Runoff
*   A.10 Culvert Code Numbers
*   A.11 Culvert Entrance Loss Coefficients
*   A.12 Standard Elliptical Pipe Sizes
*   A.13 Standard Arch Pipe Sizes

### APPENDIX B: VISUAL OBJECT PROPERTIES
*   B.1 Rain Gage Properties
*   B.2 Subcatchment Properties
*   B.3 Junction Properties
*   B.4 Outfall Properties
*   B.5 Flow Divider Properties
*   B.6 Storage Unit Properties
*   B.7 Conduit Properties
*   B.8 Pump Properties
*   B.9 Orifice Properties
*   B.10 Weir Properties
*   B.11 Outlet Properties
*   B.12 Map Label Properties

### APPENDIX C: SPECIALIZED PROPERTY EDITORS
*   C.1 Aquifer Editor
*   C.2 Climatology Editor
*   C.3 Control Rules Editor
*   C.4 Cross-Section Editor
*   C.5 Curve Editor
*   C.6 Groundwater Flow Editor
*   C.7 Groundwater Equation Editor
*   C.8 Infiltration Editor
*   C.9 Inflows Editor
*   C.10 Initial Buildup Editor
*   C.11 Inlet Structure Editor
*   C.12 Inlet Usage Editor
*   C.13 Land Use Assignment Editor
*   C.14 Land Use Editor
*   C.15 LID Control Editor
*   C.16 LID Group Editor
*   C.17 LID Usage Editor
*   C.18 Pollutant Editor
*   C.19 Snow Pack Editor
*   C.20 Storage Shape Editor
*   C.21 Street Section Editor
*   C.22 Time Pattern Editor
*   C.23 Time Series Editor
*   C.24 Title/Notes Editor
*   C.25 Transect Editor
*   C.26 Treatment Editor
*   C.27 Unit Hydrograph Editor

### APPENDIX D: COMMAND LINE SWMM
*   D.1 General Instructions
*   D.2 Input File Format
*   D.3 Map Data Section

### APPENDIX E: ERROR AND WARNING MESSAGES

---

## LIST OF FIGURES
*   **FIGURE 2-1** EXAMPLE STUDY AREA
*   **FIGURE 2-2** SUBCATCHMENTS AND NODES FOR EXAMPLE STUDY AREA
*   **FIGURE 3-1** PHYSICAL OBJECTS USED TO MODEL A DRAINAGE SYSTEM
*   **FIGURE 3-2** CONCRETE BOX CULVERT
*   **FIGURE 3-3** STORM DRAIN INLET
*   **FIGURE 3-4** AREAL DEPLETION CURVE FOR A NATURAL AREA
*   **FIGURE 3-5** AN RDII UNIT HYDROGRAPH
*   **FIGURE 3-6** EXAMPLE OF A NATURAL CHANNEL TRANSECT
*   **FIGURE 3-7** DEFINITIONAL SKETCH OF A STREET CROSS-SECTION
*   **FIGURE 3-8** REPRESENTATION OF A DUAL DRAINAGE SYSTEM
*   **FIGURE 3-9** HEC-22 INLETS SUPPORTED BY SWMM
*   **FIGURE 3-10** ADJUSTMENT OF SUBCATCHMENT PARAMETERS AFTER LID PLACEMENT
*   **FIGURE 3-11** CONCEPTUAL VIEW OF SURFACE RUNOFF
*   **FIGURE 3-12** TWO-ZONE GROUNDWATER MODEL
*   **FIGURE 3-13** CONCEPTUAL DIAGRAM OF A BIO-RETENTION CELL LID
*   **FIGURE 8-1** FLOW INSTABILITY INDEX FOR A FLOW HYDROGRAPH
*   **FIGURE 11-1** COMBINING ROUTING INTERFACE FILES
*   **FIGURE D-1** EXAMPLE SWMM PROJECT FILE
*   **FIGURE D-2** EXAMPLE STUDY AREA MAP
*   **FIGURE D-3** DATA FOR EXAMPLE STUDY AREA MAP

---

## LIST OF TABLES
*   **TABLE 3-1** AVAILABLE CROSS SECTION SHAPES FOR CONDUITS
*   **TABLE 3-2** AVAILABLE TYPES OF WEIRS
*   **TABLE 3-3** LAYERS USED TO MODEL DIFFERENT TYPES OF LID UNITS
*   **TABLE 9-1** TIME SERIES VARIABLES AVAILABLE FOR VIEWING
*   **TABLE C-1** TYPES OF GRATE INLETS
*   **TABLE D-1** GEOMETRIC PARAMETERS OF CONDUIT CROSS SECTIONS
*   **TABLE D-2** POLLUTANT BUILDUP FUNCTIONS
*   **TABLE D-3** POLLUTANT WASH OFF FUNCTIONS