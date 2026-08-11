@page hydraulics_reference_manual OpenSWMM Hydraulics Reference Manual

<center>
OpenSWMM Hydraulics Reference Manual
====================================
</center>

<center>

See @ref authors for the full list of authors and contributors.

</center>

## DISCLAIMER

This software is provided on an "as is" basis and the user assumes responsibility for its use. Although a reasonable effort has been made to assure that the results obtained are correct, the authors are not responsible and assume no liability whatsoever for any results or any use made of the results obtained from these programs, nor for any damages or litigation that result from the use of these programs for any purpose.

## ACKNOWLEDGEMENTS

This reference manual was originally prepared by **Lewis A. Rossman**, Environmental Scientist Emeritus, U.S. Environmental Protection Agency, Office of Research and Development, National Risk Management Research Laboratory. His foundational work on the SWMM hydraulics model and its documentation is gratefully acknowledged.

See @ref authors for the complete list of authors and contributors.

@tableofcontents

## Abstract

SWMM is a dynamic rainfall-runoff simulation model used for single event
or long-term (continuous) simulation of runoff quantity and quality from
primarily urban areas. The runoff component of SWMM operates on a
collection of subcatchment areas that receive precipitation and generate
runoff and pollutant loads. The routing portion of SWMM transports this
runoff through a system of pipes, channels, storage/treatment devices,
pumps, and regulators. SWMM tracks the quantity and quality of runoff
generated within each subcatchment, and the flow rate, flow depth, and
quality of water in each pipe and channel during a simulation period
comprised of multiple time steps. The reference manual for this edition
of SWMM is comprised of three volumes. Volume I describes SWMM's
hydrologic models, Volume II its hydraulic models, and Volume III its
water quality and low impact development models.

## List of Figures

Figure 1-1 Elements of a typical urban drainage system

Figure 1-2 SWMM's conceptual model of a stormwater drainage system

Figure 1-3 Processes modeled by SWMM

Figure 1-4 Block diagram of SWMM's state transition process

Figure 1-5 Flow chart of SWMM's simulation procedure

Figure 1-6 Interpolation of reported values from computed values

Figure 2-1 Node-link representation of a sewer system

Figure 2-2 Comparison of dynamic wave and kinematic wave solutions

Figure 3-1 Node-link representation of a conveyance network in SWMM

Figure 3-2 Special flow conditions for dynamic wave analysis

Figure 3-3 Illustration of a surcharged node

Figure 3-4 Ponding of excess water above a junction

Figure 3-5 Profile view of example rectangular conduit (not to scale)

Figure 3-6 Outflow hydrographs for example conduit -I

Figure 3-7 Outflow hydrographs for example conduit -- II

Figure 3-8 Conceptual representation of the dynamic Preissmann slot

Figure 3-9 State transitions of the dynamic Preissmann slot at a conduit end

Figure 3-10 Virtual junction representation of a conduit grade break

Figure 3-11 Workflow of one Anderson-accelerated iteration of the successive-approximation loop

Figure 4-1 Section factor versus area for a circular shape

Figure 4-2 Space-time grid for kinematic wave analysis

Figure 4-3 Outflow hydrograph for example conduit

Figure 5-1 Power law cross section shape

Figure 5-2 Geometric properties of a partly filled circular shape based on depth

Figure 5-3 Geometric properties of a partly filled circular shape based on area

Figure 5-4 Ellipsoid and arch pipe cross sectional shapes

Figure 5-5 Masonry sewer shapes

Figure 5-6 Composite cross section shapes

Figure 5-7 A Shape Curve with a depth segment shown

Figure 5-8 A natural channel transect

Figure 5-9 A transect depth increment with three compound segments

Figure 5-10 Example of a storage curve and its section view

Figure 5-11 Finding the volume at a given depth for a storage curve

Figure 6-1 Orifice orientations

Figure 6-2 Determination of effective head for an orifice

Figure 6-3 Orifice with unsubmerged inlet

Figure 6-4 Transverse weir shapes

Figure 6-5 Coefficient for triangular weirs (from Brater and King, 1976)

Figure 6-6 Definitions of submerged and surcharged weir flow

Figure 6-7 Rating curve for a vortex device compared to an orifice

Figure 7-1 Depths used for computing seepage in storage units

Figure 7-2 Concrete box culvert (from FHWA, 2012)

Figure 7-3 Example of a culvert rating curve (from FHWA, 2012)

Figure 7-4 Roadway overtopping (from FHWA, 2012)

Figure 7-5 SWMM node-link representation of a culvert with a roadway weir

Figure 7-6 Discharge coefficients for roadway weirs (from FHWA, 2012)

Figure 8-1 Substep workflow of the explicit finite-volume solver

Figure 8-2 Wet/dry and exception handling in one face flux evaluation

Figure 8-3 Hydrostatic reconstruction at a wet/dry front

Figure 8-4 Node ghost-state construction at a coupling face

Figure 9-1 One 1D–2D co-advance batch and the explicit marcher's substep loop within it

Figure 9-2 Wetting cases of a planar-bed triangular cell and the wetted-edge face gate

## List of Tables

Table 1-1 Development history of SWMM

Table 1-2 SWMM's modeling objects

Table 1-3 State variables used by SWMM

Table 1-4 Units of expression used by SWMM

Table 2-1 Features and limitations of dynamic wave and kinematic wave solutions

Table 3-1 Surface area adjustments for various dynamic wave flow conditions

Table 3-2 Conditions under which Anderson acceleration reverts to standard iteration

Table 5-1 Geometric properties for open channel shapes as functions of water depth

Table 5-2 Geometric properties for open channel shapes as functions of flow area

Table 5-3 Geometric properties for the power law shape

Table 5-4 Geometric properties of a full circular cross section

Table 5-5 Full area and hydraulic radius of custom ellipsoid and arch pipe sections

Table 5-6 Number of entries in geometric property tables for masonry sewer shapes

Table 5-7 Geometric parameters of masonry sewer sections

Table 5-8 Geometric properties for a sediment filled circular cross section

Table 5-9 Properties of the rectangular section of a rectangular-triangular shape

Table 5-10 Geometric parameters for rectangular-round shapes

Table 5-11 Geometric properties for rectangular--round shapes

Table 5-12 Properties in the rounded top section of a modified basket handle shape

Table 5-13 Area at maximum flow to full area for standard closed conduits shapes

Table 5-14 Critical depth formulas for simple section shapes

Table 6-1 Pump curves recognized by SWMM

Table 6-2 Kindsvater-Carter constants for rectangular weir coefficient

Table 6-3 Rectangular broad-crested weir coefficients (ft<sup>1/2</sup>/sec)

Table 6-4 Formulas for flow derivatives of various types of weirs

Table 7-1 Relative depth at maximum width for select cross section shapes

Table 7-2 Types of minor losses in drainage systems (from Frost, 2006)

Table 7-3 Hazen-Williams C-factors for different pipe materials

Table 7-4 Darcy-Weisbach roughness heights for different pipe materials

Table 8-1 Dry-state constants of the explicit finite-volume solver

Table 9-1 Wetting and drying thresholds and guards of the 2D solver

Table C-1 Circular section properties as function of area

Table C-2 Circular section properties as function of depth

Table D-1 Standard elliptical pipe sizes

Table D-2 Elliptical section properties as function of depth

Table E-1 Standard arch pipe sizes

Table E-2 Arch pipe section properties as function of depth

Table F-1 Area of masonry sewers as function of depth

Table F-2 Width of masonry sewers as function of depth - I

Table F-3 Width of masonry sewers as function of depth - II

Table F-4 Hydraulic radius of masonry sewers as function of depth

Table F-5 Depth of masonry sewers as function of area - I

Table F-6 Depth of masonry sewers as function of area - II

Table F-7 Section factor for masonry sewers as function of area - I

Table F-8 Section factor for masonry sewers as function of area - II

Table G-1 Manning's roughness coefficient n for open channels

Table G-2 Manning's roughness coefficient n for closed conduits

Table G-3 Manning's roughness coefficient n for corrugated steel pipe

Table H-1 Culvert codes

Table H-2 Culvert coefficients

## List of Symbols

| Symbol | Description |
|--------|-------------|
| *A* | cross section flow area within a conduit (ft²) |
| *Ā* | average cross section flow area along a conduit (ft²) |
| *Ā̄* | average cross section flow area along a conduit over a time period (ft²) |
| *A*<sub>full</sub> | full cross section area of a conduit (ft²) |
| *A*<sub>max</sub> | cross section area at depth where a conduit's section factor is a maximum (ft²) |
| *A*<sub>O</sub> | area of an orifice opening (ft²) |
| *A*<sub>SP</sub> | surface area of water ponded above a node (ft²) |
| *A*<sub>S</sub> | surface area of a node and its connected links (ft²) |
| *A*<sub>SL</sub> | surface area of flow within a link (ft²) |
| *A*<sub>S</sub><sup>last</sup> | surface area of a node the last time it was not surcharged (ft²) |
| *A*<sub>Smin</sub> | minimum surface area associated with a node (ft²) |
| *A*<sub>SN</sub> | surface area associated with a storage node (ft²) |
| *A*<sub>W</sub> | area of a weir opening (ft²) |
| *b* | bottom or top width (depending on shape) of a conduit's cross section (ft) |
| *c* | wave celerity (ft/sec) |
| *c*<sub>I</sub> | inlet control constant for submerged culverts |
| *c*<sub>W</sub> | coefficient for a weir-type flow divider (ft<sup>1/2</sup>/sec) |
| *C*<sub>d</sub> | orifice discharge coefficient (dimensionless) |
| *C*<sub>HW</sub> | Hazen-Williams C-factor coefficient (dimensionless) |
| *C*<sub>O</sub> | equivalent orifice constant for a surcharged weir (ft<sup>5/2</sup>/sec) |
| *Cr* | Courant number (dimensionless) |
| *C*<sub>w</sub> | weir coefficient (ft<sup>1/2</sup>/sec) |
| *D* | circular pipe diameter (ft) |
| *e*<sub>t</sub> | potential evaporation rate at time *t* (ft/sec) |
| *E* | elevation of a node's invert (ft) |
| *E*<sub>C</sub> | specific head at critical depth (ft) |
| *f* | Darcy-Weisbach friction factor (dimensionless) |
| *f*<sub>C</sub> | monthly climate adjustment factor (dimensionless) |
| *f*<sub>E</sub> | storage node evaporation factor (dimensionless) |
| *f*<sub>S</sub> | weir submergence adjustment factor (dimensionless) |
| *F* | cumulative depth of infiltrated water (ft) |
| *Fr* | Froude number (dimensionless) |
| *g* | acceleration of gravity (ft/sec²) |
| *h*<sub>L</sub> | minor head loss per unit length of a conduit (ft/ft) |
| *h*<sub>W</sub> | height of the opening for a weir-type flow divider node (ft) |
| *H* | hydraulic head (ft) |
| *H*<sub>crown</sub> | elevation of the crown of the highest conduit at a node (ft) |
| *H*<sub>e</sub> | effective head seen by an orifice or weir (ft) |
| *H*<sub>IS</sub> | minimum head at a culvert's inlet for it to be submerged (ft) |
| *H*<sub>IU</sub> | maximum head at a culvert's inlet for it to be unsubmerged (ft) |
| *H*<sub>max</sub> | maximum head at a node before flooding occurs (ft) |
| *H*<sub>Outfall</sub> | head assigned to an outfall node (ft) |
| *K* | cross section flow conductance (cfs) (equal to \f$nAR^{2/3}\f$) |
| *K*<sub>I</sub> | inlet control constant for unsubmerged culverts |
| *K*<sub>m</sub> | minor loss coefficient (dimensionless) |
| *K*<sub>S</sub> | soil saturated hydraulic conductivity (ft/sec) |
| *L* | conduit length or weir crest length (ft) |
| *L*<sub>e</sub> | effective weir crest length (ft) |
| *M*<sub>I</sub> | inlet control exponent for unsubmerged culverts |
| *n* | Manning roughness coefficient (sec/m<sup>1/3</sup>) |
| *P* | wetted perimeter of a conduit's cross section (ft) |
| *q*<sub>E</sub> | uniformly distributed evaporation rate along a channel (cfs/ft) |
| *q*<sub>L</sub> | total uniformly distributed outflow rate along a conduit (cfs/ft) |
| *q*<sub>MIN</sub> | minimum flow needed to activate a flow divider node (cfs) |
| *q*<sub>S</sub> | uniformly distributed seepage rate along a conduit (cfs/ft) |
| *q*<sub>SN</sub> | seepage rate per unit area for a storage node (cfs/ft²) |
| *Q* | flow rate within a conduit, pump, or regulator link (cfs) |
| *Q*<sub>div</sub> | flow rate diverted to a second outflow conduit from a flow divider node (cfs) |
| *Q*<sub>EN</sub> | evaporation loss rate from a storage unit node (cfs) |
| *Q*<sub>full</sub> | normal uniform flow rate for a full conduit (cfs) |
| *Q*<sub>IC</sub> | culvert flow rate under inlet control (cfs) |
| *Q*<sub>in</sub> | total inflow rate to a node (cfs) |
| *Q*<sub>LN</sub> | total loss rate from a storage unit node (cfs) |
| *Q*<sub>norm</sub> | normal uniform flow rate (cfs) |
| *Q*<sub>out</sub> | total outflow rate leaving a node (cfs) |
| *Q*<sub>ovfl</sub> | excess flow that overflows a node (cfs) |
| *Q̄*<sub>net</sub> | average net inflow minus outflow over a time step (cfs) |
| *Q*<sub>SN</sub> | seepage loss rate from a storage node (cfs) |
| *R* | hydraulic radius of flow cross section in a conduit (ft) |
| *R̄* | average hydraulic radius of flow cross sections along a conduit (ft) |
| *Re* | Reynolds number (dimensionless) |
| *R*<sub>full</sub> | hydraulic radius of a conduit cross section when full (ft) |
| *s* | seepage rate per unit area for a conduit (ft/sec) |
| *Scf* | culvert slope correction factor |
| *S*<sub>f</sub> | friction slope (ft/ft) |
| *S*<sub>0</sub> | conduit slope (ft/ft) |
| *t* | time (sec) |
| *U* | flow velocity at a point along a conduit (ft/sec) |
| *Ū* | average flow velocity along a conduit (ft/sec) |
| *V* | node assembly volume (ft³) |
| *V*<sub>P</sub> | ponded volume (ft³) |
| *V*<sub>N</sub> | storage node volume (ft³) |
| *V*<sub>Nfull</sub> | volume of a storage node when full (ft³) |
| *W* | top width of the water surface at a point along a conduit (ft) |
| *W̄* | average top width of the water surface along a conduit (ft) |
| *W*<sub>max</sub> | maximum width of a conduit cross section (ft) |
| *x* | horizontal distance (ft) |
| *y* | vertical distance (ft) |
| *y*<sub>I</sub> | inlet control constant for submerged culverts |
| *Y* | depth of flow within a conduit or of water in a storage unit (ft) |
| *Ȳ* | average depth of flow along a conduit (ft) |
| *Y*<sub>c</sub> | critical depth within a conduit at a given flow rate (ft) |
| *Y*<sub>full</sub> | full depth of a conduit, orifice opening or weir height (ft) |
| *Y*<sub>N</sub> | normal flow depth (ft) |
| *Y*<sup>*</sup> | smaller of the critical and normal flow depth in a conduit (ft) |
| *Z* | elevation of a conduit's invert (ft) |
| *Z*<sub>O</sub> | elevation of the bottom of an orifice's opening (ft) |
| *Z*<sub>W</sub> | elevation of a weir's crest in its lowest position (ft) |
| *α* | generic coefficient |
| *β* | the square root of a conduit's slope divided by its roughness |
| *∆t* | time step (sec) |
| *ε* | convergence tolerance |
| *ε* | Darcy-Weisbach roughness length (ft) |
| *γ* | exponent in power law cross section shape |
| *η* | Manning's roughness coefficient (sec/ft<sup>1/3</sup>) \f$\left( equal\ to\ \frac{n}{1.486} \right)\f$ |
| *σ* | inertial damping factor |
| *θ* | time weighting factor, relaxation factor, or subtended angle |
| *φ* | distance weighting factor |
| *θ*<sub>d</sub> | soil moisture deficit (dimensionless) |
| *μ* | kinematic viscosity (ft²/sec) |
| *ω* | pump speed setting or degree to which a regulator is opened |
| *ψ*<sub>S</sub> | soil capillary suction head (ft) |
| *Ψ* | conduit section factor (equal to \f$AR^{2/3}\f$) (ft<sup>8/3</sup>) |
| *Ψ*<sub>full</sub> | section factor of a conduit at full depth (ft<sup>8/3</sup>) |
| *Ψ*<sub>max</sub> | maximum section factor for a conduit (ft<sup>8/3</sup>) |

## Manual Contents

- @subpage hydraulics_ref_ch1_overview — Chapter 1: SWMM Overview
- @subpage hydraulics_ref_ch2_hydraulic_model — Chapter 2: SWMM's Hydraulic Model
- @subpage hydraulics_ref_ch3_dynamic_wave — Chapter 3: Dynamic Wave Analysis
- @subpage hydraulics_ref_ch4_kinematic_wave — Chapter 4: Kinematic Wave Analysis
- @subpage hydraulics_ref_ch5_cross_section — Chapter 5: Cross-Section Geometry
- @subpage hydraulics_ref_ch6_pumps_regulators — Chapter 6: Pumps and Regulators
- @subpage hydraulics_ref_ch7_advanced_features — Chapter 7: Advanced Features
- @subpage hydraulics_ref_ch8_finite_volume — Chapter 8: Explicit Finite-Volume Analysis
- @subpage hydraulics_ref_ch9_two_dimensional — Chapter 9: Two-Dimensional Overland Flow Analysis
- @subpage hydraulics_ref_references — References
- @subpage hydraulics_ref_appendix — Appendix
