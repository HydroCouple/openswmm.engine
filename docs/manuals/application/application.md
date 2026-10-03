@page application_manual OpenSWMM Application Manual

<center>
OpenSWMM Application Manual
============================
</center>

<center>

See @ref authors for the full list of authors and contributors.

</center>

## DISCLAIMER

This software is provided on an "as is" basis and the user assumes responsibility for its use. Although a reasonable effort has been made to assure that the results obtained are correct, the authors are not responsible and assume no liability whatsoever for any results or any use made of the results obtained from these programs, nor for any damages or litigation that result from the use of these programs for any purpose.

## ACKNOWLEDGEMENTS

The original SWMM 5 Applications Manual was prepared by **Lewis A. Rossman**, Environmental Scientist Emeritus, U.S. Environmental Protection Agency, Office of Research and Development, National Risk Management Research Laboratory. His foundational work in providing practical application examples for SWMM is gratefully acknowledged.

See @ref authors for the complete list of authors and contributors.

@tableofcontents

## Introduction

This manual answers one question: **which formulation should this model
use, and what changes if you pick another one?**

OpenSWMM offers more than one representation of nearly every process it
models — four flow routers, four pressurisation closures, three transport
engines, three two-dimensional momentum closures, two groundwater models.
The reference manuals derive them and the Engine Manual gives their
grammar. Neither tells a modeller facing a surcharging trunk sewer which
one to reach for, or what the answer would have been under the other.

So this manual is built from decision workflows and worked examples rather
than from a tour of the software:

- @ref application_manual_ch1_choosing "Chapter 1" is six decision
  workflows — routing, pressurisation, the 2D surface, transport, age and
  heat, groundwater — each ending at the literal `[OPTIONS]` lines that
  select a formulation, and a table naming every formulation's keys,
  status, theory and worked example.
- Chapters 2 to 11 are worked examples. Each names an in-tree deck under
  `docs/figures/decks/`, shows the options that select the formulation,
  runs the same model under the alternatives, and reports what changed.
  Every figure in them is generated from those runs.
- @ref application_manual_ch12_python_plugins "Chapter 12" runs the same
  models from Python and from plugins.
- @ref application_manual_ch13_planned "Chapter 13" is what is coming, and
  is careful to present nothing planned as if it worked today.

Each worked chapter keeps the same five headings — **Problem**, **Deck**,
**Options that select the formulation**, **Results**, **Where to go next** —
so a reader looking for one of them can find it without reading the rest.

**How to read a result here.** Every comparison in this manual is between
runs of *the same deck* with one group of keys changed. Where two
formulations disagree, the chapter says which one is believed and why —
against a measurement, an analytic solution, or a conservation argument —
rather than presenting the difference as a matter of preference.

## Where the SWMM 5 Applications Manual topics went

The EPA SWMM 5 Applications Manual was a tour of the interface built around
one site. That ground is now covered by the GUI tutorials, and this manual
covers the formulations instead.

| Former chapter | Now |
|---|---|
| Ex. 1 – Site Drainage | @ref tutorial_site_drainage |
| Ex. 2 – Surface Water Quality | @ref application_manual_ch8_transport_engines "Chapter 8: the three transport engines", and @ref quality_ref_ch3_pollutant_buildup for the theory |
| Ex. 3 – Runoff Water Quality | @ref quality_ref_ch4_surface_washoff |
| Ex. 4 – Low Impact Development | @ref engine_manual_sect_LID_USAGE for the grammar and @ref quality_ref_ch6_lid_controls for the theory |
| Ex. 5 – Continuous Simulation | @ref application_manual_ch11_hydrology "Chapter 11: long-term hydrology" |
| Ex. 6 – Detention Pond Design | not yet rewritten; the EPA SWMM 5 Applications Manual remains the reference |
| Ex. 7 – Combined Sewer Overflow | not yet rewritten; the EPA SWMM 5 Applications Manual remains the reference |
| Ex. 8 – Real-Time Control | not yet rewritten; the EPA SWMM 5 Applications Manual remains the reference |

## Manual Contents

- @subpage application_manual_ch1_choosing — Chapter 1: Choosing a Formulation
- @subpage application_manual_ch2_filling_pipe — Chapter 2: A Filling Pipe under the Dynamic Wave and Finite-Volume Routers
- @subpage application_manual_ch3_subatmospheric — Chapter 3: A Sub-Atmospheric Transient
- @subpage application_manual_ch4_virtual_junctions — Chapter 4: Virtual Junctions on a Surveyed Trunk
- @subpage application_manual_ch5_street_inlets — Chapter 5: Street Inlets and the Inlet Junction
- @subpage application_manual_ch6_coupled_1d2d — Chapter 6: Building a Coupled One- and Two-Dimensional Model
- @subpage application_manual_ch7_mesh_hydrology — Chapter 7: Rain, Infiltration and Groundwater on the Mesh
- @subpage application_manual_ch8_transport_engines — Chapter 8: The Three Transport Engines
- @subpage application_manual_ch9_msx — Chapter 9: Multi-Species Reactions
- @subpage application_manual_ch10_age_heat — Chapter 10: Water Age and Heat
- @subpage application_manual_ch11_hydrology — Chapter 11: Long-Term Hydrology
- @subpage application_manual_ch12_python_plugins — Chapter 12: Running Models from Python and Plugins
- @subpage application_manual_ch13_planned — Chapter 13: What Is Coming
