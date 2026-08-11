
@page user_manual OpenSWMM User Manual

<center>
OpenSWMM User Manual
=====================================
</center>

<center>

See @ref authors for the full list of authors and contributors.

</center>

## DISCLAIMER {#user_manual_disclaimer}

This software is provided on an "as is" basis and the user assumes responsibility for its use. Although a reasonable effort has been made to assure that the results obtained are correct, the authors are not responsible and assume no liability whatsoever for any results or any use made of the results obtained from these programs, nor for any damages or litigation that result from the use of these programs for any purpose.

## ABSTRACT {#user_manual_abstract}

The Storm Water Management Model (SWMM) is a dynamic rainfall-runoff simulation model used for single event or long-term (continuous) simulation of runoff quantity and quality from primarily urban areas. The runoff component of SWMM operates on a collection of subcatchment areas that receive precipitation and generate runoff and pollutant loads. The routing portion of SWMM transports this runoff through a system of pipes, channels, storage/treatment devices, pumps, and regulators. SWMM tracks the quantity and quality of runoff generated within each subcatchment, and the flow rate, flow depth, and quality of water in each pipe and channel during a simulation period comprised of multiple time steps. This user's manual describes in detail how to use the OpenSWMM computational engine. It includes instructions on how to build a drainage system model, how to set various simulation options, and how to view results in a variety of formats. It also describes the different types of files used and provides useful tables of parameter values. Detailed descriptions of the theory and numerical methods can be found in the separate reference manuals.

## FOREWORD {#user_manual_foreward}

OpenSWMM is the next generation of the EPA Storm Water Management Model, maintained and advanced as a community-driven open source project. It builds on the foundational work of EPA's SWMM, which was first released in 1971 and has undergone several major upgrades since then. OpenSWMM preserves the rich legacy of SWMM while advancing the codebase with modern architecture, improved modularity, enhanced performance, and support for model coupling through the HydroCouple framework.

SWMM is used throughout the world for planning, analysis, and design related to stormwater runoff, combined and sanitary sewers, and other drainage systems. It can be used to evaluate gray infrastructure stormwater control strategies, such as pipes and storm drains, and is a useful tool for creating cost-effective green/gray hybrid stormwater control solutions.

## ACKNOWLEDGEMENTS {#user_manual_acknowledgements}

OpenSWMM builds on the original EPA Storm Water Management Model (SWMM), developed by the U.S. Environmental Protection Agency, Office of Research and Development. The original user's manual and SWMM 5 software were created by **Lewis A. Rossman**, Environmental Scientist Emeritus at the U.S. EPA. His extraordinary contribution to the field of stormwater modeling is gratefully acknowledged.

The original SWMM documentation was reviewed by Michelle Simon, Katherine Ratliff, and Anne Mikelonis, all of the U.S. EPA, by Robert Dickinson (Innovyze), Mitch Heineman (CDM Smith), Mike Gregory (CHI), and Nandana Perera (CHI).

See @ref authors for the complete list of authors and contributors.

@tableofcontents

## Manual Contents

- @subpage user_manual_introductions — CHAPTER 1 – INTRODUCTION
- @subpage user_manual_chapter_2 — CHAPTER 2 – QUICK START TUTORIAL
- @subpage user_manual_chapter_3 — CHAPTER 3 - SWMM’S CONCEPTUAL MODEL
- @subpage user_manual_chapter_4 — CHAPTER 4 - SWMM’S MAIN WINDOW
- @subpage user_manual_chapter_5 — CHAPTER 5 - WORKING WITH PROJECTS
- @subpage user_manual_chapter_6 — CHAPTER 6 - WORKING WITH OBJECTS
- @subpage user_manual_chapter_7 — CHAPTER 7 - WORKING WITH THE MAP
- @subpage user_manual_chapter_8 — CHAPTER 8 - RUNNING A SIMULATION
- @subpage user_manual_chapter_9 — CHAPTER 9 - VIEWING RESULTS
- @subpage user_manual_chapter_10 — CHAPTER 10 - PRINTING AND COPYING
- @subpage user_manual_chapter_11 — CHAPTER 11 - FILES USED BY SWMM
- @subpage user_manual_chapter_12 — CHAPTER 12 - USING ADD-IN TOOLS
- @subpage user_manual_chapter_13 — CHAPTER 13 – PROGRAMMATIC C API
- @subpage user_manual_appendix_a — APPENDIX A - USEFUL TABLES
- @subpage user_manual_appendix_b — APPENDIX B - VISUAL OBJECT PROPERTIES
- @subpage user_manual_appendix_c — APPENDIX C - SPECIALIZED PROPERTY EDITORS
- @subpage user_manual_appendix_d — APPENDIX D - COMMAND LINE SWMM
- @subpage user_manual_appendix_e — APPENDIX E - ERROR AND WARNING MESSAGES
