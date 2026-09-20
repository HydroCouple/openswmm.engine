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

Figure 1-1. SWMM's Object Model

Figure 1-2. SWMM's Process Models

Figure 1-3. Simulation Process Overview

Figure 1-7. The water quality of this manual: buildup, washoff, transport and reactions across the compartments

Figure 2-1. Pollutant Sources in Urban Areas

Figure 2-2. Pollutant Buildup on Land Surfaces

Figure 2-3. Pollutant Washoff During Storm Events

Figure 3-1. Street Cleaning Effectiveness

Figure 4-1. Pollutant Transport in Drainage System

Figure 5-1. Treatment Processes

Figure 6-1. LID Control Types

Figure 6-2. LID Control Performance

Figure 7-1. Flux-limited advection in the Eulerian engine: the face stencil and a step front under UPWIND and the three MUSCL limiters

Figure 7-2. The five phases of a Lagrangian transport step: drain, mix, release, decay and publish on one conduit's parcel slab

Figure 9-1. The heat budget of a water body: the six surface terms of the net flux by module, and the bed zone's conduction and hyporheic exchange

## List of Tables

Table 1-1. SWMM Object Types

Table 2-1. Pollutant Types

Table 2-2. Land Use Categories

Table 3-1. Buildup Parameters

Table 4-1. Washoff Parameters

Table 5-1. Treatment Parameters

Table 5-4. Variables Available in Treatment Expressions

Table 5-5. Math Functions Available in Treatment Expressions

Table 6-1. LID Control Parameters

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
