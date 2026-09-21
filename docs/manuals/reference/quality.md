@page quality_reference_manual OpenSWMM Water Quality Reference Manual

<center>
OpenSWMM Water Quality Reference Manual
=======================================
</center>

<center>

See @ref authors for the full list of authors and contributors.

</center>

## DISCLAIMER

This software is provided on an "as is" basis and the user assumes responsibility for its use. Although a reasonable effort has been made to assure that the results obtained are correct, the authors are not responsible and assume no liability whatsoever for any results or any use made of the results obtained from these programs, nor for any damages or litigation that result from the use of these programs for any purpose.

## ACKNOWLEDGEMENTS

This reference manual was originally prepared by **Lewis A. Rossman**, Environmental Scientist Emeritus, U.S. Environmental Protection Agency, Office of Research and Development, National Risk Management Research Laboratory, and **Wayne C. Huber**, Professor Emeritus, Oregon State University. Their foundational work on the SWMM water quality model and its documentation is gratefully acknowledged.

See @ref authors for the complete list of authors and contributors.

@tableofcontents

## Abstract

SWMM is a dynamic rainfall-runoff simulation model used for single event
or long-term (continuous) simulation of runoff quantity and quality from
primarily urban areas. This document describes the water quality modeling
capabilities of SWMM. It covers the simulation of pollutant buildup on
land surfaces, their washoff during storm events, transport through the
drainage system, and treatment by various control practices. The manual
also describes how to model Low Impact Development (LID) controls for
managing stormwater runoff quality.

## Acronyms and Abbreviations

**BMP** - Best Management Practice

**CSO** - Combined Sewer Overflow

**EPA** - Environmental Protection Agency

**LID** - Low Impact Development

**NPDES** - National Pollutant Discharge Elimination System

**SWMM** - Storm Water Management Model

**TSS** - Total Suspended Solids

**TKN** - Total Kjeldahl Nitrogen

**TP** - Total Phosphorus

**BOD** - Biochemical Oxygen Demand

**COD** - Chemical Oxygen Demand

**TDS** - Total Dissolved Solids

**TOC** - Total Organic Carbon

**VSS** - Volatile Suspended Solids

**FSS** - Fixed Suspended Solids

## List of Figures

<!-- BEGIN GENERATED: list-of-figures style=period -->

Figure 1-1. Elements of a typical urban drainage system

Figure 1-2. Objects of an OpenSWMM model

Figure 1-3. Processes modelled by OpenSWMM

Figure 1-4. Block diagram of SWMM's state transition process

Figure 1-5. Flow chart of SWMM's simulation procedure

Figure 1-6. Interpolation of reported values from computed values

Figure 1-7. The water quality of this manual: buildup, washoff, transport and reactions across the compartments

Figure 2-1. Hourly domestic sewage time patterns

Figure 2-2. Pollutant Buildup on Land Surfaces

Figure 2-3. Pollutant Washoff During Storm Events

Figure 3-1. Accumulation of solids on urban streets versus time (Sartor and Boyd, 1972)

Figure 3-2. Buildup of street solids in San Jose (from Pitt, 1979)

Figure 3-3. Comparison of buildup equations for a hypothetical pollutant

Figure 3-4. Evolution of buildup after a storm event

Figure 4-1. Washoff of street solids by flushing with a sprinkler system (from Sartor and Boyd, 1972)

Figure 4-2. Comparison of washoff functions

Figure 4-3. Two-stream approach to modeling pollutant washoff

Figure 4-4. Simulated load variations within a storm as a function of runoff rate

Figure 5-1. Representation of the conveyance network in SWMM

Figure 5-2. Comparison of completely mixed reactor equations for time varying inflow

Figure 5-3. Comparison of completely mixed reactor equations for a step inflow

Figure 5-4. Gravity settling treatment of TSS within a detention pond

Figure 6-1. A typical bio-retention cell

Figure 6-2. Flow path across the surface of a green roof

Figure 6-3. Representation of a permeable pavement system

Figure 6-4. Representation of rooftop disconnection

Figure 6-5. Representation of a vegetative swale

Figure 6-6. Different options for placing LID controls

Figure 6-7. Storm event used for the LID example

Figure 6-8. Flux rates through the bio-retention cell with no underdrain

Figure 6-9. Moisture levels in the bio-retention cell with no underdrain

Figure 6-10. Moisture levels in the bio-retention cell with underdrain

Figure 6-11. Flux rates through the bio-retention cell with underdrain

Figure 7-1. Flux-limited advection in the Eulerian engine: the face stencil and a step front under UPWIND and the three MUSCL limiters

Figure 7-2. The five phases of a Lagrangian transport step: drain, mix, release, decay and publish on one conduit's parcel slab

Figure 9-1. The heat budget of a water body: the six surface terms of the net flux by module, and the bed zone's conduction and hyporheic exchange

Figure 10-1. The transport policy matrix: which species class moves in which domain, and the gate ladder the 2D surface passes

Figure 10-2. Buildup and washoff on one mesh cell: land-use slices, accrual, sweeping and the mass handed to the network

<!-- END GENERATED -->

## List of Tables

<!-- BEGIN GENERATED: list-of-tables style=period -->

Table 1-1. Development history of SWMM

Table 1-2. SWMM's modeling objects

Table 1-3. State variables used by SWMM

Table 1-4. Units of expression used by SWMM

Table 2-1. Sources of contaminants in urban storm water runoff (US EPA, 1999)

Table 2-2. Typical pollutant loadings from runoff by urban land use (lbs/acre-yr)

Table 2-3. Median event mean concentrations for urban land uses

Table 2-4. Potency factors for the Detroit metropolitan area (mg/gram) Source: Roesner (1982).

Table 2-5. Potency factors for the Patuxent River Basin (mg/gram) Source: Aqua Terra (1994).

Table 2-6. Representative concentrations of constituents in rainfall

Table 2-7. Average daily dry weather flow in 29 cities Source: ASCE-WPCF (1969)

Table 2-8. Quality properties of untreated domestic wastewater Source: Metcalf and Eddy, Inc. (2003)

Table 2-9. Unit quality loads for domestic sewage, including effects of garbage grinders Source: Haseltine (1950); Metcalf and Eddy et al. (1971a).

Table 2-10. Autumn water use for six homes near Wheaton, MD Source: Tucker (1967).

Table 2-11. Typical hourly DWF correction factors Source: Metcalf and Eddy, Inc. (2003).

Table 2-12. Required temporal detail for receiving water analysis Source: Driscoll (1979) and Hydroscience (1979).

Table 3-1. Measured dust and dirt (DD) accumulation in Chicago Source: APWA (1969).

Table 3-2. Milligrams of pollutant per gram of dust and dirt (parts per thousand by mass) for four Chicago land uses Source: APWA (1969).

Table 3-3. Summary of buildup function coefficients

Table 3-4. Removal efficiencies from street cleaner path for various street cleaning programs (Pitt, 1979)

Table 3-5. Nationwide data on linear dust and dirt buildup rates and on pollutant fractions (after Manning et al., 1977)

Table 4-1. Units of the washoff coefficient *KW* for different washoff models

Table 4-2. Percent removals for vegetated swales and filter strips Source: ASCE (2001).

Table 4-3. Buildup/washoff calibration against annual loading rate for high-density residential land use Source: Tetra Tech (2010).

Table 4-4. National EMC's for stormwater Source: CWP (2003).

Table 4-5. EMC's for different regions Source: CWP (2003)

Table 5-1. Treatment processes used by various types of BMPs

Table 5-2. Median inlet and outlet EMCs for selected stormwater treatment practices

Table 5-3. Median pollutant removal percentages for select stormwater BMPs

Table 5-4. Variables available in treatment expressions

Table 5-5. Math functions available in treatment expressions

Table 6-1. Design manuals used as sources for LID parameter values

Table 6-2. Typical ranges for bio-retention cell parameters

Table 6-3. Soil characteristics for a typical bio-retention cell soil

Table 6-4. Typical ranges for green roof parameters

Table 6-5. Typical ranges for infiltration trench parameters

Table 6-6. Typical ranges for permeable pavement parameters

Table 6-7. Typical ranges for vegetative swale parameters

<!-- END GENERATED -->

## Manual Contents

- @subpage quality_ref_ch1_overview — Chapter 1: Overview
- @subpage quality_ref_ch2_urban_runoff_quality — Chapter 2 - Urban Runoff Quality
- @subpage quality_ref_ch3_pollutant_buildup — Chapter 3 - Surface Buildup
- @subpage quality_ref_ch4_surface_washoff — Chapter 4: Surface Washoff
- @subpage quality_ref_ch5_transport_treatment — Chapter 5: Transport and Treatment
- @subpage quality_ref_ch6_lid_controls — Chapter 6: Low Impact Development Controls
- @subpage quality_ref_ch7_ard_transport — Chapter 7: Advection–Reaction–Dispersion Transport
- @subpage quality_ref_ch8_msx_reactions — Chapter 8: Multi-Species Reactions
- @subpage quality_ref_ch9_age_heat — Chapter 9: Water Age and Heat Transport
- @subpage quality_ref_ch10_mesh_quality — Chapter 10: Surface Quality and Transport on the 2D Mesh
- @subpage quality_ref_ch11_planned — Chapter 11: Planned Formulations
- @subpage quality_ref_glossary — Glossary
- @subpage quality_ref_references — References
