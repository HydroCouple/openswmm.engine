EPA/600/R-17/111

May 2017

(Revised July 2021)

Storm Water Management Model

Reference Manual

Volume II -- Hydraulics

By:

Lewis A. Rossman

Office of Research and Development

National Risk Management Laboratory

Cincinnati, OH 45268

Center for Environmental Solutions and Emergency Response

Office of Research and Development

U.S. Environmental Protection Agency

26 Martin Luther King Drive

Cincinnati, OH 45268

July 2021

# Disclaimer

The information in this document has been funded wholly or in part by
the U.S. Environmental Protection Agency (EPA). It has been subjected to
the Agency's peer and administrative review, and has been approved for
publication as an EPA document. Mention of trade names or commercial
products does not constitute endorsement or recommendation for use.

Although a reasonable effort has been made to assure that the results
obtained are correct, the computer programs described in this manual are
experimental. Therefore the author and the U.S. Environmental Protection
Agency are not responsible and assume no liability whatsoever for any
results or any use made of the results obtained from these programs, nor
for any damages or litigation that result from the use of these programs
for any purpose.

# Abstract

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

# Acknowledgements

This report was written by Lewis A. Rossman, Environmental Scientist
Emeritus, U.S. Environmental Protection Agency, Cincinnati, OH.

The author would like to acknowledge the contributions made by the
following individuals to previous versions of SWMM that were drawn
heavily upon in writing this report: John Aldrich, Douglas Ammon, Carl
W. Chen, Brett Cunningham, Robert Dickinson, James Heaney, Wayne Huber,
Miguel Medina, Russell Mein, Charles Moore, Stephan Nix, Alan Peltz, Don
Polmann, Larry Roesner, Charles Rowney, and Robert Shubinsky. The
efforts of Wayne Huber (Oregon State University emeritus), Thomas
Barnwell (US EPA retired), Richard Field (US EPA retired), Harry Torno
(US EPA retired) and William James (University of Guelph emeritus) to
support and maintain the program over the past several decades are also
gratefully acknowledged.

**Table of Contents**

[Disclaimer [ii](#disclaimer)](#disclaimer)

[Abstract [iii](#abstract)](#abstract)

[Acknowledgements [iv](#acknowledgements)](#acknowledgements)

[List of Figures [vii](#list-of-figures)](#list-of-figures)

[List of Tables [ix](#list-of-tables)](#list-of-tables)

[List of Symbols [xi](#list-of-symbols)](#list-of-symbols)

[Chapter 1 - SWMM Overview [15](#swmm-overview)](#swmm-overview)

[1.1 Introduction [15](#introduction)](#introduction)

[1.2 SWMM's Object Model [16](#swmms-object-model)](#swmms-object-model)

[1.3 SWMM's Process Models
[21](#swmms-process-models)](#swmms-process-models)

[1.4 Simulation Process Overview
[23](#simulation-process-overview)](#simulation-process-overview)

[1.5 Interpolation and Units
[27](#interpolation-and-units)](#interpolation-and-units)

[Chapter 2 - SWMM's Hydraulic Model
[30](#swmms-hydraulic-model)](#swmms-hydraulic-model)

[2.1 Network Components [31](#network-components)](#network-components)

[2.2 Analysis Methods [35](#analysis-methods)](#analysis-methods)

[2.3 Boundary and Initial Conditions
[38](#boundary-and-initial-conditions)](#boundary-and-initial-conditions)

[Chapter 3 - Dynamic Wave Analysis
[40](#dynamic-wave-analysis)](#dynamic-wave-analysis)

[3.1 Governing Equations
[40](#governing-equations)](#governing-equations)

[3.2 Solution Method [44](#solution-method)](#solution-method)

[3.3 Computational Details
[46](#computational-details)](#computational-details)

[3.4 Numerical Stability
[59](#numerical-stability)](#numerical-stability)

[Chapter 4 - Kinematic Wave Analysis
[65](#kinematic-wave-analysis)](#kinematic-wave-analysis)

[4.1 Governing Equations
[65](#governing-equations-1)](#governing-equations-1)

[4.2 Solution Method [67](#solution-method-1)](#solution-method-1)

[4.3 Computational Details
[69](#computational-details-1)](#computational-details-1)

[4.4 Numerical Stability
[74](#numerical-stability-1)](#numerical-stability-1)

[Chapter 5 - Cross-Section Geometry
[76](#cross-section-geometry)](#cross-section-geometry)

[5.1 Standard Conduit Shapes
[76](#standard-conduit-shapes)](#standard-conduit-shapes)

[5.2 Custom Conduit Shapes
[94](#custom-conduit-shapes)](#custom-conduit-shapes)

[5.3 Irregular Natural Channels
[96](#irregular-natural-channels)](#irregular-natural-channels)

[5.4 Street Cross-Sections
[99](#street-cross-sections)](#street-cross-sections)

[5.5 Storage Unit Geometry
[100](#storage-unit-geometry)](#storage-unit-geometry)

[5.6 Critical and Normal Depths
[105](#critical-and-normal-depths)](#critical-and-normal-depths)

[Chapter 6 - Pumps and Regulators
[109](#pumps-and-regulators)](#pumps-and-regulators)

[6.1 Pumps [109](#pumps)](#pumps)

[6.2 Orifices [112](#orifices)](#orifices)

[6.3 Weirs [120](#weirs)](#weirs)

[6.4 Outlets [130](#outlets)](#outlets)

[Chapter 7 - Advanced Features
[132](#advanced-features)](#advanced-features)

[7.1 Evaporation and Seepage
[132](#evaporation-and-seepage)](#evaporation-and-seepage)

[7.2 Minor Losses [141](#minor-losses)](#minor-losses)

[7.3 Force Mains [143](#force-mains)](#force-mains)

[7.4 Culverts [147](#culverts)](#culverts)

[7.5 Roadway Weirs [152](#roadway-weirs)](#roadway-weirs)

[7.6 Storm Drain Inlets [155](#storm-drain-inlets)](#storm-drain-inlets)

[Appendix [159](#_Toc73193124)](#_Toc73193124)

[References [193](#references)](#references)

**\**

# List of Figures

[Figure 1‑1 Elements of a typical urban drainage system
[17](#_Toc484694706)](#_Toc484694706)

[Figure 1‑2 SWMM\'s conceptual model of a stormwater drainage system
[18](#_Toc401645528)](#_Toc401645528)

[Figure 1‑3 Processes modeled by SWMM
[21](#_Toc401645529)](#_Toc401645529)

[Figure 1‑4 Block diagram of SWMM\'s state transition process
[23](#_Toc401645530)](#_Toc401645530)

[Figure 1‑5 Flow chart of SWMM\'s simulation procedure
[26](#_Toc401645531)](#_Toc401645531)

[Figure 1‑6 Interpolation of reported values from computed values
[28](#_Toc401645532)](#_Toc401645532)

[Figure 2‑1 Node-link representation of a sewer system
[30](#_Toc484694712)](#_Toc484694712)

[Figure 2‑2 Comparison of dynamic wave and kinematic wave solutions
[38](#_Toc484694713)](#_Toc484694713)

[Figure 3‑1 Node-link representation of a conveyance network in SWMM
[43](#_Toc484694714)](#_Toc484694714)

[Figure 3‑2 Special flow conditions for dynamic wave analysis
[50](#_Toc484694715)](#_Toc484694715)

[Figure 3‑3 Illustration of a surcharged node
[53](#_Toc484694716)](#_Toc484694716)

[Figure 3‑4 Ponding of excess water above a junction
[56](#_Toc484694717)](#_Toc484694717)

[Figure 3‑5 Profile view of example rectangular conduit (not to scale)
[60](#_Toc484694718)](#_Toc484694718)

[Figure 3‑6 Outflow hydrographs for example conduit -I
[61](#_Toc484694719)](#_Toc484694719)

[Figure 3‑7 Outflow hydrographs for example conduit -- II
[62](#_Toc484694720)](#_Toc484694720)

[Figure 4‑1 Section factor versus area for a circular shape
[64](#_Toc484694721)](#_Toc484694721)

[Figure 4‑2 Space-time grid for kinematic wave analysis
[65](#_Toc484694722)](#_Toc484694722)

[Figure 4‑3 Outflow hydrograph for example conduit
[73](#_Toc484694723)](#_Toc484694723)

[Figure 5‑1 Power law cross section shape
[77](#_Toc484694724)](#_Toc484694724)

[Figure 5‑2 Geometric properties of a partly filled circular shape based
on depth [80](#_Toc484694725)](#_Toc484694725)

[Figure 5‑3 Geometric properties of a partly filled circular shape based
on area [80](#_Toc484694726)](#_Toc484694726)

[Figure 5‑4 Ellipsoid and arch pipe cross sectional shapes
[81](#_Toc484694727)](#_Toc484694727)

[Figure 5‑5 Masonry sewer shapes [84](#_Toc484694728)](#_Toc484694728)

[Figure 5‑6 Composite cross section shapes
[86](#_Toc484694729)](#_Toc484694729)

[Figure 5‑7 A Shape Curve with a depth segment shown
[93](#_Toc484694730)](#_Toc484694730)

[Figure 5‑8 A natural channel transect
[94](#_Toc484694731)](#_Toc484694731)

[Figure 5‑9 A transect depth increment with three compound segments
[95](#_Toc484694732)](#_Toc484694732)

[Figure 5‑10 Example of a storage curve and its section view
[98](#_Toc484694733)](#_Toc484694733)

[Figure 5‑11 Finding the volume at a given depth for a storage curve
[99](#_Toc484694734)](#_Toc484694734)

[Figure 6‑1 Orifice orientations [107](#_Toc484694735)](#_Toc484694735)

[Figure 6‑2 Determination of effective head for an orifice
[110](#_Toc484694736)](#_Toc484694736)

[Figure 6‑3 Orifice with unsubmerged inlet
[110](#_Toc484694737)](#_Toc484694737)

[Figure 6‑4 Transverse weir shapes
[115](#_Toc484694738)](#_Toc484694738)

[Figure 6‑5 Coefficient for triangular weirs (from Brater and King,
1976) [118](#_Toc484694739)](#_Toc484694739)

[Figure 6‑6 Definitions of submerged and surcharged weir flow
[120](#_Toc484694740)](#_Toc484694740)

[Figure 6‑7 Rating curve for a vortex device compared to an orifice
[125](#_Toc484694741)](#_Toc484694741)

[Figure 7‑1 Depths used for computing seepage in storage units
[135](#_Toc484694742)](#_Toc484694742)

[Figure 7‑2 Concrete box culvert (from FHWA, 2012)
[142](#_Toc484694743)](#_Toc484694743)

[Figure 7‑3 Example of a culvert rating curve (from FHWA, 2012)
[143](#_Toc484694744)](#_Toc484694744)

[Figure 7‑4 Roadway overtopping (from FHWA, 2012)
[148](#_Toc484694745)](#_Toc484694745)

[Figure 7‑5 SWMM node-link representation of a culvert with a roadway
weir [148](#_Toc484694746)](#_Toc484694746)

[Figure 7‑6 Discharge coefficients for roadway weirs (from FHWA, 2012)
[149](#_Toc484694747)](#_Toc484694747)

# List of Tables

# 

[Table 1‑1 Development history of SWMM
[16](#_Toc484690413)](#_Toc484690413)

[Table 1‑2 SWMM's modeling objects [19](#_Toc484690414)](#_Toc484690414)

[Table 1‑3 State variables used by SWMM
[24](#_Toc484690415)](#_Toc484690415)

[Table 1‑4 Units of expression used by SWMM
[29](#_Toc484690416)](#_Toc484690416)

[Table 2‑1 Features and limitations of dynamic wave and kinematic wave
solutions [37](#_Toc484690417)](#_Toc484690417)

[Table 3‑1 Surface area adjustments for various dynamic wave flow
conditions [51](#_Toc484690418)](#_Toc484690418)

[Table 5‑1 Geometric properties for open channel shapes as functions of
water depth [75](#_Toc484690419)](#_Toc484690419)

[Table 5‑2 Geometric properties for open channel shapes as functions of
flow area [76](#_Toc484690420)](#_Toc484690420)

[Table 5‑3 Geometric properties for the power law shape
[78](#_Toc484690421)](#_Toc484690421)

[Table 5‑4 Geometric properties of a full circular cross section
[79](#_Toc484690422)](#_Toc484690422)

[Table 5‑5 Full area and hydraulic radius of custom ellipsoid and arch
pipe sections [83](#_Toc484690423)](#_Toc484690423)

[Table 5‑6 Number of entries in geometric property tables for masonry
sewer shapes [85](#_Toc484690424)](#_Toc484690424)

[Table 5‑7 Geometric parameters of masonry sewer sections
[85](#_Toc484690425)](#_Toc484690425)

[Table 5‑8 Geometric properties for a sediment filled circular cross
section [87](#_Toc484690426)](#_Toc484690426)

[Table 5‑9 Properties of the rectangular section of a
rectangular-triangular shape [88](#_Toc484690427)](#_Toc484690427)

[Table 5‑10 Geometric parameters for rectangular-round shapes
[89](#_Toc484690428)](#_Toc484690428)

[Table 5‑11 Geometric properties for rectangular--round shapes
[90](#_Toc484690429)](#_Toc484690429)

[Table 5‑12 Properties in the rounded top section of a modified basket
handle shape [91](#_Toc484690430)](#_Toc484690430)

[Table 5‑13 Area at maximum flow to full area for standard closed
conduits shapes [91](#_Toc484690431)](#_Toc484690431)

[Table 5‑14 Critical depth formulas for simple section shapes
[101](#_Toc484690432)](#_Toc484690432)

[Table 6‑1 Pump curves recognized by SWMM
[105](#_Toc484690433)](#_Toc484690433)

[Table 6‑2 Kindsvater-Carter constants for rectangular weir coefficient
[118](#_Toc484690434)](#_Toc484690434)

[Table 6‑3 Rectangular broad-crested weir coefficients (ft^1/2^/sec)
[119](#_Toc484690435)](#_Toc484690435)

[Table 6‑4 Formulas for flow derivatives of various types of weirs
[123](#_Toc484690436)](#_Toc484690436)

[Table 7‑1 Relative depth at maximum width for select cross section
shapes [129](#_Toc484690437)](#_Toc484690437)

[Table 7‑2 Types of minor losses in drainage systems (from Frost, 2006)
[137](#_Toc484690438)](#_Toc484690438)

[Table 7‑3 Hazen-Williams C-factors for different pipe materials
[139](#_Toc484690439)](#_Toc484690439)

[Table 7‑4 Darcy-Weisbach roughness heights for different pipe materials
[140](#_Toc484690440)](#_Toc484690440)

[Table C‑1 Circular section properties as function of area
[153](#_Toc484690441)](#_Toc484690441)

[Table C‑2 Circular section properties as function of depth
[154](#_Toc484695150)](#_Toc484695150)

[Table D‑1 Standard elliptical pipe sizes
[155](#_Toc484695151)](#_Toc484695151)

[Table D‑2 Elliptical section properties as function of depth
[156](#_Toc484695152)](#_Toc484695152)

[Table E‑1 Standard arch pipe sizes
[157](#_Toc484695153)](#_Toc484695153)

[Table E‑2 Arch pipe section properties as function of depth
[160](#_Toc484695154)](#_Toc484695154)

[Table F‑1 Area of masonry sewers as function of depth
[161](#_Toc484695155)](#_Toc484695155)

[Table F‑2 Width of masonry sewers as function of depth - I
[162](#_Toc484695156)](#_Toc484695156)

[Table F‑3 Width of masonry sewers as function of depth - II
[163](#_Toc484695157)](#_Toc484695157)

[Table F‑4 Hydraulic radius of masonry sewers as function of depth
[164](#_Toc484695158)](#_Toc484695158)

[Table F‑5 Depth of masonry sewers as function of area - I
[165](#_Toc484695159)](#_Toc484695159)

[Table F‑6 Depth of masonry sewers as function of area - II
[167](#_Toc484695160)](#_Toc484695160)

[Table F‑7 Section factor for masonry sewers as function of area - I
[169](#_Toc484695161)](#_Toc484695161)

[Table F‑8 Section factor for masonry sewers as function of area - II
[171](#_Toc484695162)](#_Toc484695162)

[Table G‑1 Manning's roughness coefficient n for open channels
[173](#_Toc484695163)](#_Toc484695163)

[Table G‑2 Manning's roughness coefficient n for closed conduits
[177](#_Toc484695164)](#_Toc484695164)

[Table G‑3 Manning's roughness coefficient n for corrugated steel pipe
[179](#_Toc484695165)](#_Toc484695165)

[Table H‑1 Culvert codes [180](#_Toc484695166)](#_Toc484695166)

[Table H‑2 Culvert coefficients [183](#_Toc484695167)](#_Toc484695167)

# List of Symbols

  ----------------------------------------------------------------------------------------------
  *A*                           cross section flow area within a conduit (ft^2^)
  ----------------------------- ----------------------------------------------------------------
  $$\overline{A}$$              average cross section flow area along a conduit (ft^2^)

  $$\overline{\overline{A}}$$   average cross section flow area along a conduit over a time
                                period (ft^2^)

  *A~full~*                     full cross section area of a conduit (ft^2^)

  *A~max~*                      cross section area at depth where a conduit's section factor is
                                a maximum (ft^2^)

  *A~O~*                        area of an orifice opening (ft^2^)

  *A~SP~*                       surface area of water ponded above a node (ft^2^)

  *A~S~*                        surface area of a node and its connected links (ft^2^)

  *A~SL~*                       surface area of flow within a link (ft^2^)

  *A~S~^last^*                  surface area of a node the last time it was not surcharged
                                (ft^2^)

  *A~Smin~*                     minimum surface area associated with a node (ft^2^)

  *A~SN~*                       surface area associated with a storage node (ft^2^)

  *A~W~*                        area of a weir opening (ft^2^)

  *b*                           bottom or top width (depending on shape) of a conduit's cross
                                section (ft)

  *c*                           wave celerity (ft/sec)

  *c~I~*                        inlet control constant for submerged culverts

  *c~W~*                        coefficient for a weir-type flow divider (ft^1/2^/sec)

  *C~d~*                        orifice discharge coefficient (dimensionless)

  *C~HW~*                       Hazen-Williams C-factor coefficient (dimensionless)

  *C~O~*                        equivalent orifice constant for a surcharged weir (ft^5/2^/sec)

  *Cr*                          Courant number (dimensionless)

  *C~w~*                        weir coefficient (ft^1/2^/sec)

  *D*                           circular pipe diameter (ft)

  *e~t~*                        potential evaporation rate at time *t* (ft/sec)

  *E*                           elevation of a node's invert (ft)

  *E~C~*                        specific head at critical depth (ft)

  *f*                           Darcy-Weisbach friction factor (dimensionless)

  *f~C~*                        monthly climate adjustment factor (dimensionless)

  *f~E~*                        storage node evaporation factor (dimensionless)

  *f~S~*                        weir submergence adjustment factor (dimensionless)

  *F*                           cumulative depth of infiltrated water (ft)

  *Fr*                          Froude number (dimensionless)

  *g*                           acceleration of gravity (ft/sec^2^)

  *h~L~*                        minor head loss per unit length of a conduit (ft/ft)

  *h~W~*                        height of the opening for a weir-type flow divider node (ft)

  *H*                           hydraulic head (ft)

  *H~crown~*                    elevation of the crown of the highest conduit at a node (ft)

  *H~e~*                        effective head seen by an orifice or weir (ft)

  *H~IS~*                       minimum head at a culvert's inlet for it to be submerged (ft)

  *H~IU~*                       maximum head at a culvert's inlet for it to be unsubmerged (ft)

  *H~max~*                      maximum head at a node before flooding occurs (ft)

  *H~Outfall~*                  head assigned to an outfall node (ft)

  *K*                           cross section flow conductance (cfs) (equal to $nAR^{2/3}$)

  *K~I~*                        inlet control constant for unsubmerged culverts

  *K~m~*                        minor loss coefficient (dimensionless)

  *K~S~*                        soil saturated hydraulic conductivity (ft/sec)

  *L*                           conduit length or weir crest length (ft)

  *L~e~*                        effective weir crest length (ft)

  *M~I~*                        inlet control exponent for unsubmerged culverts

  *n*                           Manning roughness coefficient (sec/m^1/3^)

  *P*                           wetted perimeter of a conduit's cross section (ft)

  *q~E~*                        uniformly distributed evaporation rate along a channel (cfs/ft)

  *q~L~*                        total uniformly distributed outflow rate along a conduit
                                (cfs/ft)

  *q~MIN~*                      minimum flow needed to activate a flow divider node (cfs)

  *q~S~*                        uniformly distributed seepage rate along a conduit (cfs/ft)

  *q~SN~*                       seepage rate per unit area for a storage node (cfs/ft^2^)

  *Q*                           flow rate within a conduit, pump, or regulator link (cfs)

  *Q~div~*                      flow rate diverted to a second outflow conduit from a flow
                                divider node (cfs)

  *Q~EN~*                       evaporation loss rate from a storage unit node (cfs)

  *Q~full~*                     normal uniform flow rate for a full conduit (cfs)

  *Q~IC~*                       culvert flow rate under inlet control (cfs)

  *Q~in~*                       total inflow rate to a node (cfs)

  *Q~LN~*                       total loss rate from a storage unit node (cfs)

  *Q~norm~*                     normal uniform flow rate (cfs)

  *Q~out~*                      total outflow rate leaving a node (cfs)

  *Q~ovfl~*                     excess flow that overflows a node (cfs)

  $${\overline{Q}}_{net}$$      average net inflow minus outflow over a time step (cfs)

  *Q~SN~*                       seepage loss rate from a storage node (cfs)

  *R*                           hydraulic radius of flow cross section in a conduit (ft)

  $$\overline{R}$$              average hydraulic radius of flow cross sections along a conduit
                                (ft)

  *Re*                          Reynolds number (dimensionless)

  *R~full~*                     hydraulic radius of a conduit cross section when full (ft)

  *s*                           seepage rate per unit area for a conduit (ft/sec)

  *Scf*                         culvert slope correction factor

  *S~f~*                        friction slope (ft/ft)

  *S~0~*                        conduit slope (ft/ft)

  *t*                           time (sec)

  *U*                           flow velocity at a point along a conduit (ft/sec)

  $$\overline{U}$$              average flow velocity along a conduit (ft/sec)

  *V*                           node assembly volume (ft^3^)

  *V~P~*                        ponded volume (ft^3^)

  *V~N~*                        storage node volume (ft^3^)

  *V~Nfull~*                    volume of a storage node when full (ft^3^)

  *W*                           top width of the water surface at a point along a conduit (ft)

  $$\overline{W}$$              average top width of the water surface along a conduit (ft)

  *W~max~*                      maximum width of a conduit cross section (ft)

  *x*                           horizontal distance (ft)

  *y*                           vertical distance (ft)

  *y~I~*                        inlet control constant for submerged culverts

  *Y*                           depth of flow within a conduit or of water in a storage unit
                                (ft)

  $$\overline{Y}$$              average depth of flow along a conduit (ft)

  *Y~c~*                        critical depth within a conduit at a given flow rate (ft)

  *Y~full~*                     full depth of a conduit, orifice opening or weir height (ft)

  *Y~N~*                        normal flow depth (ft)

  *Y\**                         smaller of the critical and normal flow depth in a conduit (ft)

  *Z*                           elevation of a conduit's invert (ft)

  *Z~O~*                        elevation of the bottom of an orifice's opening (ft)

  *Z~W~*                        elevation of a weir's crest in its lowest position (ft)

  *α*                           generic coefficient

  *β*                           the square root of a conduit's slope divided by its roughness

  *∆t*                          time step (sec)

  *ε*                           convergence tolerance

  $$\epsilon$$                  Darcy-Weisbach roughness length (ft)

  *γ*                           exponent in power law cross section shape

  *η*                           Manning's roughness coefficient (sec/ft^1/3^)
                                $\left( equal\ to\ \frac{n}{1.486} \right)$

  *σ*                           inertial damping factor

  *θ*                           time weighting factor, relaxation factor, or subtended angle

  *φ*                           distance weighting factor

  *θ~d~*                        soil moisture deficit (dimensionless)

  *μ*                           kinematic viscosity (ft^2^/sec)

  *ω*                           pump speed setting or degree to which a regulator is opened

  $$\psi_{S}$$                  soil capillary suction head (ft)

  *Ψ*                           conduit section factor (equal to $AR^{2/3}$) (ft^8/3^)

  *Ψ~full~*                     section factor of a conduit at full depth (ft^8/3^)

  *Ψ~max~*                      maximum section factor for a conduit (ft^8/3^)
  ----------------------------------------------------------------------------------------------

# - SWMM Overview

\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_

## 1.1 Introduction

Urban runoff quantity and quality constitute problems of both a
historical and current nature. Cities have long assumed the
responsibility of control of stormwater flooding and treatment of point
sources (e.g., municipal sewage) of wastewater. Since the 1960s, the
severe pollution potential of urban nonpoint sources, principally
combined sewer overflows and stormwater discharges, has been recognized,
both through field observation and federal legislation. The advent of
modern computers has led to the development of complex, sophisticated
tools for analysis of both quantity and quality pollution problems in
urban areas and elsewhere (Singh, 1995). The EPA Storm Water Management
Model, SWMM, first developed in 1969-71, was one of the first such
models. It has been continually maintained and updated and is perhaps
the best known and most widely used of the available urban runoff
quantity/quality models (Huber and Roesner, 2013).

SWMM is a dynamic rainfall-runoff simulation model used for single event
or long-term (continuous) simulation of runoff quantity and quality from
primarily urban areas. The runoff component of SWMM operates on a
collection of subcatchment areas that receive precipitation and generate
runoff and pollutant loads. The routing portion of SWMM transports this
runoff through a system of pipes, channels, storage/treatment devices,
pumps, and regulators. SWMM tracks the quantity and quality of runoff
generated within each subcatchment, and the flow rate, flow depth, and
quality of water in each pipe and channel during a simulation period
comprised of multiple time steps.

Table 1-1 summarizes the development history of SWMM. The current
edition, Version 5, is a complete re-write of the previous releases. The
reference manual for this edition of SWMM is comprised of three volumes.
Volume I describes SWMM's hydrologic models, Volume II its hydraulic
models, and Volume III its water quality and low impact development
models. These manuals complement the SWMM 5 User's Manual (US EPA,
2010), which explains how to run the program, and the SWMM 5
Applications Manual (US EPA, 2009) which presents a number of worked-out
examples. The procedures described in this reference manual are based on
earlier descriptions included in the original SWMM documentation
(Metcalf and Eddy et al., 1971a, 1971b, 1971c, 1971d), intermediate
reports (Huber et al., 1975; Heaney et al., 1975; Huber et al., 1981),
plus new material. This information supersedes the Version 4.0
documentation (Huber and Dickinson, 1988; Roesner et al., 1992) and
includes descriptions of some newer procedures implemented since 1988.
More information on current documentation and the general status of the
EPA Storm Water Management Model as well as the full program and its
source code is available on the EPA SWMM web site:.
<http://www2.epa.gov/water-research/storm-water-management-model-swmm>.

+----------------+----------+--------------------+-----------------------+
| **Version**    | **Year** | **Contributors**   | **Comments**          |
+:===============+:=========+:===================+:======================+
| SWMM I         | 1971     | Metcalf & Eddy,    | First version of      |
|                |          | Inc.               | SWMM; focus was CSO   |
|                |          |                    | modeling; Few of its  |
|                |          | Water Resources    | methods are still     |
|                |          | Engineers          | used today.           |
|                |          |                    |                       |
|                |          | University of      |                       |
|                |          | Florida            |                       |
+----------------+----------+--------------------+-----------------------+
| SWMM II        | 1975     | University of      | First widely          |
|                |          | Florida            | distributed version   |
|                |          |                    | of SWMM.              |
+----------------+----------+--------------------+-----------------------+
| SWMM 3         | 1981     | University of      | Full dynamic wave     |
|                |          | Florida            | flow routine,         |
|                |          |                    | Green-Ampt            |
|                |          | Camp Dresser &     | infiltration, snow    |
|                |          | McKee              | melt, and continuous  |
|                |          |                    | simulation added.     |
+----------------+----------+--------------------+-----------------------+
| SWMM 3.3       | 1983     | US EPA             | First PC version of   |
|                |          |                    | SWMM.                 |
+----------------+----------+--------------------+-----------------------+
| SWMM 4         | 1988     | Oregon State       | Groundwater, RDII,    |
|                |          | University         | irregular channel     |
|                |          |                    | cross-sections and    |
|                |          | Camp Dresser &     | other refinements     |
|                |          | McKee              | added over a series   |
|                |          |                    | of updates throughout |
|                |          |                    | the 1990's.           |
+----------------+----------+--------------------+-----------------------+
| SWMM 5         | 2005     | US EPA             | Complete re-write of  |
|                |          |                    | the SWMM engine in C; |
|                |          | CDM-Smith          | graphical user        |
|                |          |                    | interface added;      |
|                |          |                    | improved algorithms   |
|                |          |                    | and new features      |
|                |          |                    | (e.g., LID modeling)  |
|                |          |                    | added.                |
+----------------+----------+--------------------+-----------------------+

: []{#_Toc484690413 .anchor}**Table 1‑1 Development history of SWMM**

## 1.2 SWMM's Object Model

Figure 1-1 depicts the elements included in a typical urban drainage
system. SWMM conceptualizes this system as a series of water and
material flows between several major environmental compartments. These
compartments include:

<figure>
<img src="VolumeII/media/media/image1.jpeg"
style="width:6.5in;height:4.20726in"
alt="http://www.epa.ohio.gov/portals/35/cso/wet_weather_flow_graphic.jpg" />
<figcaption><p><span id="_Toc484694706"
class="anchor"></span><strong>Figure 1‑1 Elements of a typical urban
drainage system</strong></p></figcaption>
</figure>

- The Atmosphere compartment, which generates precipitation and deposits
  pollutants onto the Land Surface compartment.

- The Land Surface compartment receives precipitation from the
  Atmosphere compartment in the form of rain or snow. It sends outflow
  in the forms of 1) evaporation back to the Atmosphere compartment, 2)
  infiltration into the Sub-Surface compartment and 3) surface runoff
  and pollutant loadings on to the Conveyance compartment.

- The Sub-Surface compartment receives infiltration from the Land
  Surface compartment and transfers a portion of this inflow to the
  Conveyance compartment as groundwater interflow.

- The Conveyance compartment contains a network of elements (channels,
  pipes, pumps, and regulators) and storage/treatment units that convey
  water to outfalls or to treatment facilities. Inflows to this
  compartment can come from surface runoff, groundwater interflow,
  sanitary dry weather flow, or from user-defined time series.

Not all compartments need appear in a particular SWMM model. For
example, one could model just the Conveyance compartment, using
pre-defined hydrographs and pollutographs as inputs. As illustrated in
Figure 1-1, SWMM can be used to model any combination of stormwater
collection systems, both separate and combined sanitary sewer systems,
as well as natural catchment and river channel systems.

Figure 1-2 shows how SWMM conceptualizes the physical elements of the
actual system depicted in Figure 1-1 with a standard set of modeling
objects. The principal objects used to model the rainfall/runoff process
are Rain Gages and Subcatchments. Snowmelt is modeled with Snow Pack
objects placed on top of subcatchments while Aquifer objects placed
below subcatchments are used to model groundwater flow. The conveyance
portion of the drainage system is modeled with a network of Nodes and
Links. Nodes are points that represent simple junctions, flow dividers,
storage units, or outfalls. Links connect nodes to one another with
conduits (pipes and channels), pumps, or flow regulators (orifices,
weirs, or outlets). Land Use and Pollutant objects are used to describe
water quality. Finally, a group of data objects that includes Curves,
Time Series, Time Patterns, and Control Rules, are used to characterize
the inflows and operating behavior of the various physical objects in a
SWMM model. Table 1-2 provides a summary of the various objects used in
SWMM. Their properties and functions will be described in more detail
throughout the course of this manual.

<figure>
<img src="VolumeII/media/media/image2.png"
style="width:4.22447in;height:3.2121in" alt="Objects2" />
<figcaption><p><span id="_Toc401645528"
class="anchor"></span><strong>Figure 1‑2 SWMM's conceptual model of a
stormwater drainage system</strong></p></figcaption>
</figure>

+--------------+---------------+--------------------------------------+
| **Category** | **Object      | **Description**                      |
|              | Type**        |                                      |
+:=============+:==============+:=====================================+
| Hydrology    | Rain Gage     | Source of precipitation data to one  |
|              |               | or more subcatchments.               |
|              +---------------+--------------------------------------+
|              | Subcatchment  | A land parcel that receives          |
|              |               | precipitation associated with a rain |
|              |               | gage and generates runoff that flows |
|              |               | into a drainage system node or to    |
|              |               | another subcatchment.                |
|              +---------------+--------------------------------------+
|              | Aquifer       | A subsurface area that receives      |
|              |               | infiltration from the subcatchment   |
|              |               | above it and exchanges groundwater   |
|              |               | flow with a conveyance system node.  |
|              +---------------+--------------------------------------+
|              | Snow Pack     | Accumulated snow that covers a       |
|              |               | subcatchment.                        |
|              +---------------+--------------------------------------+
|              | Unit          | A response function that describes   |
|              | Hydrograph    | the amount of sewer                  |
|              |               | inflow/infiltration (RDII) generated |
|              |               | over time per unit of instantaneous  |
|              |               | rainfall.                            |
+--------------+---------------+--------------------------------------+
| Hydraulics   | Junction      | A point in the conveyance system     |
|              |               | where conduits connect to one        |
|              |               | another with negligible storage      |
|              |               | volume (e.g., manholes, pipe         |
|              |               | fittings, or stream junctions).      |
|              +---------------+--------------------------------------+
|              | Outfall       | An end point of the conveyance       |
|              |               | system where water is discharged to  |
|              |               | a receptor (such as a receiving      |
|              |               | stream or treatment plant) with      |
|              |               | known water surface elevation.       |
|              +---------------+--------------------------------------+
|              | Divider       | A point in the conveyance system     |
|              |               | where the inflow splits into two     |
|              |               | outflow conduits according to a      |
|              |               | known relationship.                  |
|              +---------------+--------------------------------------+
|              | Storage Unit  | A pond, lake, impoundment, or        |
|              |               | chamber that provides water storage. |
|              +---------------+--------------------------------------+
|              | Conduit       | A channel or pipe that conveys water |
|              |               | from one conveyance system node to   |
|              |               | another.                             |
|              +---------------+--------------------------------------+
|              | Pump          | A device that raises the hydraulic   |
|              |               | head of water.                       |
|              +---------------+--------------------------------------+
|              | Regulator     | A weir, orifice or outlet used to    |
|              |               | direct and regulate flow between two |
|              |               | nodes of the conveyance system.      |
+--------------+---------------+--------------------------------------+

: []{#_Toc484690414 .anchor}**Table 1‑2 SWMM's modeling objects**

**Table 1-2 Continued**

+--------------+---------------+--------------------------------------+
| **Category** | **Object      | **Description**                      |
|              | Type**        |                                      |
+:=============+:==============+:=====================================+
| Water        | Pollutant     | A contaminant that can build up and  |
| Quality      |               | be washed off of the land surface or |
|              |               | be introduced directly into the      |
|              |               | conveyance system.                   |
|              +---------------+--------------------------------------+
|              | Land Use      | A classification used to             |
|              |               | characterize the functions that      |
|              |               | describe pollutant buildup and       |
|              |               | washoff.                             |
+--------------+---------------+--------------------------------------+
| Treatment    | LID Control   | A low impact development control,    |
|              |               | such as a bio-retention cell, porous |
|              |               | pavement, or vegetative swale, used  |
|              |               | to reduce surface runoff through     |
|              |               | enhanced infiltration.               |
|              +---------------+--------------------------------------+
|              | Treatment     | A user-defined function that         |
|              | Function      | describes how pollutant              |
|              |               | concentrations are reduced at a      |
|              |               | conveyance system node as a function |
|              |               | of certain variables, such as        |
|              |               | concentration, flow rate, water      |
|              |               | depth, etc.                          |
+--------------+---------------+--------------------------------------+
| Data Object  | Curve         | A tabular function that defines the  |
|              |               | relationship between two quantities  |
|              |               | (e.g., flow rate and hydraulic head  |
|              |               | for a pump, surface area and depth   |
|              |               | for a storage node, etc.).           |
|              +---------------+--------------------------------------+
|              | Time Series   | A tabular function that describes    |
|              |               | how a quantity varies with time      |
|              |               | (e.g., rainfall, outfall surface     |
|              |               | elevation, etc.).                    |
|              +---------------+--------------------------------------+
|              | Time Pattern  | A set of factors that repeats over a |
|              |               | period of time (e.g., diurnal hourly |
|              |               | pattern, weekly daily pattern,       |
|              |               | etc.).                               |
|              +---------------+--------------------------------------+
|              | Control Rules | IF-THEN-ELSE statements that         |
|              |               | determine when specific control      |
|              |               | actions are taken (e.g., turn a pump |
|              |               | on or off when the flow depth at a   |
|              |               | given node is above or below a       |
|              |               | certain value).                      |
+--------------+---------------+--------------------------------------+

## 1.3 SWMM's Process Models

Figure 1-3 depicts the processes that SWMM models using the objects
described previously and how they are tied to one another. The
hydrological processes depicted in this diagram include:

<figure>
<img src="VolumeII/media/media/image3.png"
style="width:6.15711in;height:5.47993in" alt="ProcessModels.png" />
<figcaption><p><span id="_Toc401645529"
class="anchor"></span><strong>Figure 1‑3 Processes modeled by
SWMM</strong></p></figcaption>
</figure>

- time-varying precipitation

- snow accumulation and melting

- rainfall interception from depression storage (initial abstraction)

- evaporation of standing surface water

- infiltration of rainfall into unsaturated soil layers

- percolation of infiltrated water into groundwater layers

- interflow between groundwater and the drainage system

- nonlinear reservoir routing of overland flow

- infiltration and evaporation of rainfall/runoff captured by Low Impact
  Development controls.

The hydraulic processes occurring within SWMM's conveyance compartment
include:

- external inflow of surface runoff, groundwater interflow,
  rainfall-dependent infiltration/inflow, dry weather sanitary flow, and
  user-defined inflows

- unsteady, non-uniform flow routing through any configuration of open
  channels, pipes and storage units

- various possible flow regimes such as backwater, surcharging, reverse
  flow, and surface ponding

- flow regulation via pumps, weirs, and orifices including time- and
  state-dependent control rules that govern their operation.

Regarding water quality, the following processes can be modeled for any
number of user-defined water quality constituents:

- dry-weather pollutant buildup over different land uses

- pollutant washoff from specific land uses during storm events

- direct contribution of rainfall deposition

- reduction in dry-weather buildup due to street cleaning

- reduction in washoff loads due to BMPs

- entry of dry weather sanitary flows and user-specified external
  inflows at any point in the drainage system

- routing of water quality constituents through the drainage system

- reduction in constituent concentration through treatment in storage
  units or by natural processes in pipes and channels.

The numerical procedures that SWMM uses to model the hydraulic processes
listed above are discussed in detail in subsequent chapters of this
volume. SWMM's hydrologic and water quality processes are described in
volumes I and III of this manual.

## 1.4 Simulation Process Overview

SWMM is a distributed discrete time simulation model. It computes new
values of its state variables over a sequence of time steps, where at
each time step the system is subjected to a new set of external inputs.
As its state variables are updated, other output variables of interest
are computed and reported. This process is represented mathematically
with the following general set of equations that are solved at each time
step as the simulation unfolds:

  ------------------------------------------------------------------------
        $$X_{t} = f(X_{t - 1},I_{t},P)$$                             (1-1)
  ----- ---------------------------------------------------------- -------
        $$Y_{t} = g(X_{t},P)$$                                       (1-2)

  ------------------------------------------------------------------------

where

  -----------------------------------------------------------------------------
  *X~t~*   =   a vector of state variables at time *t*,
  -------- --- ----------------------------------------------------------------
  *Y~t~*   =   a vector of output variables at time *t*,

  *I~t~*   =   a vector of inputs at time *t*,

  *P*      =   a vector of constant parameters,

  *f*      =   a vector-valued state transition function,

  *g*      =   a vector-valued output transform function,
  -----------------------------------------------------------------------------

Figure 1-4 depicts the simulation process in block diagram fashion.

<figure>
<img src="VolumeII/media/media/image4.png" style="width:6.5in;height:2.48611in"
alt="StateTransition.png" />
<figcaption><p><span id="_Toc401645530"
class="anchor"></span><strong>Figure 1‑4 Block diagram of SWMM's state
transition process</strong></p></figcaption>
</figure>

The variables that make up the state vector *X~t~* are listed in Table
1-3. This is a surprisingly small number given the comprehensive nature
of SWMM. All other quantities can be computed from these variables,
external inputs, and fixed input parameters. The meaning of some of the
less obvious state variables, such as those used for snow melt, is
discussed in other sections of this set of manuals.

+----------------+--------------+---------------------------------+------------+
| **Process**    | **Variable** | **Description**                 | **Initial  |
|                |              |                                 | Value**    |
+:===============+:=============+:================================+:===========+
| Runoff         | *d*          | Depth of runoff on a            | 0          |
|                |              | subcatchment surface            |            |
+----------------+--------------+---------------------------------+------------+
| Infiltration\* | *t~p~*       | Equivalent time on the Horton   | 0          |
|                |              | curve                           |            |
|                +--------------+---------------------------------+------------+
|                | *F~e~*       | Cumulative excess infiltration  | 0          |
|                |              | volume                          |            |
|                +--------------+---------------------------------+------------+
|                | *Fu*         | Upper zone moisture content     | 0          |
|                +--------------+---------------------------------+------------+
|                | *T*          | Time until the next rainfall    | 0          |
|                |              | event                           |            |
|                +--------------+---------------------------------+------------+
|                | *P*          | Cumulative rainfall for current | 0          |
|                |              | event                           |            |
|                +--------------+---------------------------------+------------+
|                | *S*          | Soil moisture storage capacity  | User       |
|                |              | remaining                       | supplied   |
+----------------+--------------+---------------------------------+------------+
| Groundwater    | *θ~u~*       | Unsaturated zone moisture       | User       |
|                |              | content                         | supplied   |
|                +--------------+---------------------------------+------------+
|                | *d~L~*       | Depth of saturated zone         | User       |
|                |              |                                 | supplied   |
+----------------+--------------+---------------------------------+------------+
| Snowmelt       | *wsnow*      | Snow pack depth                 | User       |
|                |              |                                 | supplied   |
|                +--------------+---------------------------------+------------+
|                | *fw*         | Snow pack free water depth      | User       |
|                |              |                                 | supplied   |
|                +--------------+---------------------------------+------------+
|                | *ati*        | Snow pack surface temperature   | User       |
|                |              |                                 | supplied   |
|                +--------------+---------------------------------+------------+
|                | *cc*         | Snow pack cold content          | 0          |
+----------------+--------------+---------------------------------+------------+
| Flow Routing   | *H*          | Hydraulic head of water at a    | User       |
|                |              | node                            | supplied   |
|                +--------------+---------------------------------+------------+
|                | *Q*          | Flow rate in a link             | User       |
|                |              |                                 | supplied   |
|                +--------------+---------------------------------+------------+
|                | *A*          | Flow area in a link             | Inferred   |
|                |              |                                 | from *Q*   |
+----------------+--------------+---------------------------------+------------+
| Water Quality  | *t~sweep~*   | Time since a subcatchment was   | User       |
|                |              | last swept                      | supplied   |
|                +--------------+---------------------------------+------------+
|                | *m~B~*       | Pollutant buildup on            | User       |
|                |              | subcatchment surface            | supplied   |
|                +--------------+---------------------------------+------------+
|                | *m~P~*       | Pollutant mass ponded on        | 0          |
|                |              | subcatchment                    |            |
|                +--------------+---------------------------------+------------+
|                | *c~N~*       | Concentration of pollutant at a | User       |
|                |              | node                            | supplied   |
|                +--------------+---------------------------------+------------+
|                | *c~L~*       | Concentration of pollutant in a | User       |
|                |              | link                            | supplied   |
+----------------+--------------+---------------------------------+------------+

: []{#_Toc484690415 .anchor}**Table 1‑3 State variables used by SWMM**

\*Only a sub-set of these variables is used, depending on the user's
choice of infiltration method.

Examples of user-supplied input variables *I~t~* that produce changes to
these state variables include:

- meteorological conditions, such as precipitation, air temperature,
  evaporation rate and wind speed

- externally imposed inflow hydrographs and pollutographs at specific
  nodes of the conveyance system

- dry weather sanitary inflows to specific nodes of the conveyance
  system

- water surface elevations at specific outfalls of the conveyance system

- control settings for pumps and regulators.

The output vector *Y~t~* that SWMM computes from its updated state
variables contains such reportable quantities as:

- runoff flow rate and pollutant concentrations from each subcatchment

- snow depth, infiltration rate and evaporation losses from each
  subcatchment

- groundwater table elevation and lateral groundwater outflow for each
  subcatchment

- total lateral inflow (from runoff, groundwater flow, dry weather flow,
  etc.), water depth, and pollutant concentration for each conveyance
  system node

- overflow rate and ponded volume at each flooded node

- flow rate, velocity, depth and pollutant concentration for each
  conveyance system link.

Regarding the constant parameter vector *P,* SWMM contains over 150
different user-supplied constants and coefficients within its collection
of process models. Most of these are either physical dimensions (e.g.,
land areas, pipe diameters, invert elevations) or quantities that can be
obtained from field observation (e.g., percent impervious cover),
laboratory testing (e.g., various soil properties), or previously
published data tables (e.g., pipe roughness based on pipe material). A
smaller remaining number might require some degree of model calibration
to determine their proper values. Of course not all parameters are
required for every project (e.g., the 14 groundwater parameters for each
subcatchment are not needed if groundwater is not being modeled). The
subsequent chapters of this manual carefully define each parameter and
make suggestions on how to estimate its value.

A flowchart of the overall simulation process is shown in Figure 1-5.
The process begins by reading a description of each object and its
parameters from an input file whose format is described in the SWMM 5
Users Manual (US EPA, 2010). Next the values of all state variables are
initialized, as is the current simulation time (T), runoff time
(T~roff~), and reporting time (T~rpt~).

<figure>
<img src="VolumeII/media/media/image5.png" style="width:6.5in;height:8.14097in"
alt="SimulationProcedure.png" />
<figcaption><p><span id="_Toc401645531"
class="anchor"></span><strong>Figure 1‑5 Flow chart of SWMM's simulation
procedure</strong></p></figcaption>
</figure>

The program then enters a loop that first determines the time T1 at the
end of the current routing time step (∆T~rout~). If the current runoff
time T~roff~ is less than T1, then new runoff calculations are
repeatedly made and the runoff time updated until it equals or exceeds
time T1. Each set of runoff calculations accounts for any precipitation,
evaporation, snowmelt, infiltration, ground water seepage, overland
flow, and pollutant buildup and washoff that can contribute flow and
pollutant loads into the conveyance system.

Once the runoff time is current, all inflows and pollutant loads
occurring at time T are routed through the conveyance system over the
time interval from T to T1. This process updates the flow, depth and
velocity in each conduit, the water elevation at each node, the pumping
rate for each pump, and the water level and volume in each storage unit.
In addition, new values for the concentrations of all pollutants at each
node and within each conduit are computed. Next a check is made to see
if the current reporting time T~rpt~ falls within the interval from T to
T1. If it does, then a new set of output results at time T~rpt~ are
interpolated from the results at times T and T1 and are saved to an
output file. The reporting time is also advanced by the reporting time
step ∆T~rpt.~ The simulation time T is then updated to T1 and the
process continues until T reaches the desired total duration. SWMM's
Windows-based user interface provides graphical tools for building the
aforementioned input file and for viewing the computed output.

## 1.5 Interpolation and Units

SWMM uses linear interpolation to obtain values for quantities at times
that fall in between times at which input time series are recorded or at
which output results are computed. The concept is illustrated in Figure
1-6 which shows how reported flow values are derived from the computed
flow values on either side of it for the typical case where the
reporting time step is larger than the routing time step. One exception
to this convention is for precipitation and infiltration rates. These
remain constant within a runoff time step and no interpolation is made
when these values are used within SWMM's runoff algorithms or for
reporting purposes. In other words, if a reporting time falls within a
runoff time step the reported rainfall intensity is the value associated
with the start of the runoff time step.

![[]{#_Toc401645532 .anchor}**Figure 1‑6 Interpolation of reported
values from computed
values**](VolumeII/media/media/image6.emf){width="6.1097222222222225in"
height="3.4194444444444443in"}

The units of expression used by SWMM's input variables, parameters, and
output variables depend on the user's choice of flow units. If flow rate
is expressed in US customary units then so are all other quantities; if
SI metric units are used for flow rate then all other quantities use SI
metric units. Table 1-4 lists the units associated with each of SWMM's
major variables and parameters, for both US and SI systems. Internally
within the computer code all calculations are carried out using feet as
the unit of length and seconds as the unit of time.

+------------------------+----------------------+----------------------+
| **Variable or          | **US Customary       | **SI Metric Units**  |
| Parameter**            | Units**              |                      |
+:=======================+:=====================+:=====================+
| Area (subcatchment)    | acres                | hectares             |
+------------------------+----------------------+----------------------+
| Area (storage surface  | square feet          | square meters        |
| area)                  |                      |                      |
+------------------------+----------------------+----------------------+
| Depression Storage     | inches               | millimeters          |
+------------------------+----------------------+----------------------+
| Depth                  | feet                 | meters               |
+------------------------+----------------------+----------------------+
| Elevation              | feet                 | meters               |
+------------------------+----------------------+----------------------+
| Evaporation            | inches/day           | millimeters/day      |
+------------------------+----------------------+----------------------+
| Flow Rate              | cubic feet/sec (cfs) | cubic meters/sec     |
|                        |                      | (cms)                |
|                        | gallons/min (gpm)    |                      |
|                        |                      | liters/sec (lps)     |
|                        | 10^6^ gallons/day    |                      |
|                        | (mgd)                | 10^6^ liters/day     |
|                        |                      | (mld)                |
+------------------------+----------------------+----------------------+
| Hydraulic Conductivity | inches/hour          | millimeters/hour     |
+------------------------+----------------------+----------------------+
| Hydraulic Head         | feet                 | meters               |
+------------------------+----------------------+----------------------+
| Infiltration Rate      | inches/hour          | millimeters/hour     |
+------------------------+----------------------+----------------------+
| Length                 | feet                 | meters               |
+------------------------+----------------------+----------------------+
| Manning's n            | seconds/meter^1/3^   | seconds/meter^1/3^   |
+------------------------+----------------------+----------------------+
| Pollutant Buildup      | mass/acre            | mass/hectare         |
+------------------------+----------------------+----------------------+
| Pollutant              | milligrams/liter     | milligrams/liter     |
| Concentration          | (mg/L)               | (mg/L)               |
|                        |                      |                      |
|                        | micrograms/liter     | micrograms/liter     |
|                        | (μg/L)               | (μg/L)               |
|                        |                      |                      |
|                        | organism             | organism             |
|                        | counts/liter         | counts/liter         |
+------------------------+----------------------+----------------------+
| Rainfall Intensity     | inches/hour          | millimeters/hour     |
+------------------------+----------------------+----------------------+
| Rainfall Volume        | inches               | millimeters          |
+------------------------+----------------------+----------------------+
| Storage Volume         | cubic feet           | cubic meters         |
+------------------------+----------------------+----------------------+
| Temperature            | degrees Fahrenheit   | degrees Celsius      |
+------------------------+----------------------+----------------------+
| Velocity               | feet/second          | meters/second        |
+------------------------+----------------------+----------------------+
| Width                  | feet                 | meters               |
+------------------------+----------------------+----------------------+
| Wind Speed             | miles/hour           | kilometers/hour      |
+------------------------+----------------------+----------------------+

: []{#_Toc484690416 .anchor}**Table 1‑4 Units of expression used by
SWMM**

# - SWMM's Hydraulic Model

\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_

As mentioned in Chapter 1, SWMM models the conveyance portion of a
drainage system as a network of links connected together at nodes.
External flows from various sources enter the network at specific nodes,
are transported along links, are combined together and split apart at
internal nodes while filling and emptying the volume of storage nodes,
and exit the system at terminal nodes. Figure 2-1 shows how a physical
system of sewer lines and their appurtenances are abstracted into a
network of nodes and links of different types (pipe and pump links;
junction, storage and outfall nodes for this particular example).

![SewerSystem2.png](VolumeII/media/media/image7.png){width="6.5in"
height="5.308333333333334in"}

[]{#_Toc484694712 .anchor}**Figure 2‑1 Node-link representation of a
sewer system**

**(Background from http://www.sewerhistory.org/photosgraphics/japan/)**

Table 1-2 has already summarized the different types of node and link
objects that can appear in a SWMM conveyance network model. The
remainder of this chapter provides more details on the properties of
network objects, briefly describes and compares the capabilities of the
two principal methods used for analyzing the unsteady hydraulic behavior
of a network, and discusses the boundary and initial conditions needed
to compute network hydraulics.

## 2.1 Network Components

The two principal components of a SWMM conveyance system network are
nodes and links. Nodes represent the end points of conveyance links that
form the connection between two or more links. They are also the points
where external inflows (runoff, dry weather flows, etc.) can enter the
network or where internal flows leave the network. Links are conveyance
elements that transport flow between nodes. The following paragraphs
describe the different types of nodes and links that SWMM can model.

### 2.1.1 Junction Nodes

Junction nodes are points in the drainage system where conveyance links
join together. Physically they can represent the confluence of natural
surface channels, manholes in a sewer system, or pipe connection
fittings. Excess water at a junction can become partially pressurized
when connecting conduits are surcharged and can either be lost from the
system or be allowed to pond atop the junction and subsequently drain
back into the junction.

The principal input parameters for a junction node are:

- invert (channel or manhole bottom) elevation

- height between its invert and the ground surface

- additional pressure head that can be accepted before flooding occurs

- ponded surface area when flooded.

### 2.1.2 Outfall Nodes

Outfall nodes are terminal nodes of the drainage system used to define
final downstream boundary locations. The boundary conditions at an
outfall can be described by any one of the following stage
relationships:

- the critical or normal flow depth in the connecting conduit

- a fixed stage elevation

- a tidal stage described in a table of tide height versus hour of the
  day

- a user-defined time series of stage versus time.

The principal input parameters for an outfall node are:

- invert elevation

- type of boundary condition and its associated stage data

- presence of a flap gate to prevent backflow through the outfall.

### 2.1.3 Flow Divider Nodes

Flow divider nodes divert inflows to a specific link in a prescribed
manner. A flow divider can have no more than two conduit links on its
discharge side. There are four types of flow dividers, defined by the
manner in which inflows are diverted:

- *Cutoff* diverts all inflow above a defined cutoff value.

- *Overflow* diverts all inflow above the flow capacity of the
  non-diverted conduit.

- *Tabular* uses a table that expresses diverted flow as a function of
  total inflow.

- *Weir* uses a weir equation to compute diverted flow.

The principal input parameters for a flow divider node are:

- junction parameters (see above)

- name of the link receiving the diverted flow

- method used for computing the amount of diverted flow.

### 2.1.4 Storage Unit Nodes

Storage unit nodes are the only type of node that can provide storage
volume and possess surface area. Physically they could represent storage
facilities as small as a catch basin or as large as a lake. The
volumetric properties of a storage unit are described by a function or
table of surface area versus height. In addition to receiving inflows
and discharging outflows to other nodes in the drainage network, storage
nodes can also lose water from surface evaporation and from seepage into
native soil. Unlike other nodes, storage nodes are not allowed to
pressurize (i.e., they always maintain a free surface).

The principal input parameters for a storage unit are:

- invert (bottom) elevation

- maximum depth

- depth-surface area data

- evaporation potential

- seepage parameters.

### 2.1.5 Conduit Links

Conduit links are pipes or channels that move water from one node to
another in the conveyance network. Their cross-sectional shapes can be
selected from a variety of standard open and closed geometries. Custom
closed shapes for pipes and irregular cross-section profiles for open
channels can also be specified. Conduit geometry is discussed in more
detail in Chapter 5.

The required input parameters for a conduit link are:

- identities of the inlet and outlet nodes

- offset height or elevation above the inlet and outlet node inverts

- conduit length

- Manning\'s roughness coefficient

- cross-section shape and dimensions.

  ----------------------------------------------------------------------------------------------------------------------
  SWMM allows conduits to be offset some       ![Link_offset.bmp](VolumeII/media/media/image8.png){width="1.853934820647419in"
  distance above the invert of their                                height="1.8061942257217847in"}
  connecting end nodes as shown in the figure 
  on the right. The offset can be specified   
  as either a distance above the invert       
  (i.e., the distance between points 1 and 2  
  in the figure) or as the elevation of the   
  conduit's invert (i.e., the elevation of    
  point 1). Internally the offset is          
  maintained as an elevation.                 
  ------------------------------------------- --------------------------------------------------------------------------
  SWMM also makes use of a conduit's slope in     ![slope.png](VolumeII/media/media/image9.png){width="2.38575021872266in"
  its hydraulic calculations. Slope is not                          height="1.5002099737532808in"}
  provided directly as an input variable but  
  is instead computed from the elevation of a 
  conduit's end node inverts and its offsets. 
  Let *L* be the length of the conduit, *∆y*  
  be the difference in elevation and *∆x* the 
  horizontal distance between the invert at   
  each end of the conduit. Then from the      
  diagram on the right:                       

  ----------------------------------------------------------------------------------------------------------------------

  --------------------------------------------------------------------------
        $$\mathrm{\Delta}x = \sqrt{L^{2} - \mathrm{\Delta}y^{2}}$$   (2-1)
  ----- ------------------------------------------------------------ -------

  --------------------------------------------------------------------------

and the conduit slope *S~0~* is:

  ------------------------------------------------------------------------
        $$S_{0} = \frac{\mathrm{\Delta}y}{\mathrm{\Delta}x}$$      (2-2)
  ----- ---------------------------------------------------------- -------

  ------------------------------------------------------------------------

SWMM does not allow a slope of 0. Therefore it imposes a minimum value
of 0.001 ft on *∆y*. It also allows the user to set a non-zero value for
minimum slope which will override any smaller computed slope.

SWMM uses the Manning equation to relate conduit flow rate to flow depth
and conduit bed or friction slope. It therefore requires the user to
supply a Manning's "*n*" coefficient that represents the roughness
characteristics of the conduit's surface. Values of the coefficient for
a wide range of channel types and pipe materials can be found in
Appendix G.

Conduits can also include the following optional parameters:

- presence of a flap gate to prevent reverse flow

- entrance/exit loss coefficients

- seepage rate

- inlet geometry code number if the conduit acts as a culvert.

The latter three properties are employed by the advanced modeling
features covered in Chapter 7 of this manual.

### 2.1.6 Pump Links

Pump links are used to lift water from an inlet node to an outlet node
at higher elevation. The principal input parameters for a pump include:

- identities of its inlet and outlet nodes

- pump curve data

- initial on/off status

- startup and shutoff depths.

A pump curve describes the relation between a pump\'s flow rate and the
head at its inlet and outlet nodes. The inlet node's startup and shutoff
water depths are monitored continuously during the course of a
simulation to allow for automated control of the pump's on/off status.

Pumps are directional devices that are not allowed to have reverse flow
through them. Their hydraulic performance is described in more detail in
Chapter 6.

### 2.1.7 Flow Regulator Links

Flow regulator links model structures or devices used to control and
divert flows within a conveyance system. They are typically used to
control releases from storage facilities, prevent unacceptable
surcharging, and divert flow to treatment facilities and interceptors.

SWMM can model the following types of flow regulators: orifices, weirs,
and outlets. The hydraulic behavior of orifices and weirs is modeled
using standard rating curves (the nonlinear relation between hydraulic
head applied to the regulator and the flow rate through it). Outlets
utilize a user-supplied rating curve.

The principal input parameters for a flow regulator link include:

- identities of its inlet and outlet nodes

- offset above the invert of its inlet node

- dimensions of its opening (for orifices and weirs)

- parameters that describe its rating curve

- presence of a flap gate to prevent reverse flow.

The hydraulic performance of regulator links is described in more detail
in Chapter 6.

### 2.1.8 Control Rules

Each pump and flow regulator has a setting property that can adjust:

- a pump's on/off status

<!-- -->

- a pump's speed

- the size of an orifice opening

- the crest height of a weir

- the flow through an outlet link

The setting can be changed during a simulation by using control rules.
These specify conditions, such as water elevation at certain nodes, flow
in certain links, and simulation time, that trigger a specified change
in a link's setting. SWMM's hydraulic analysis methods take into account
the current setting for each pump and flow regulator in the conveyance
network. More details on the formats used for control rules can be found
in the SWMM 5 Users Manual (US EPA, 2010).

## 2.2 Analysis Methods

SWMM's hydraulics solves the equations of one-dimensional, gradually
varied, unsteady flow throughout a node-link network to determine the
water level at each node and the flow rate and flow depth within each
link at each time step of an extended simulation period. Flow routing of
inflow hydrographs along channels and sewers entails wave dispersion,
wave attenuation or amplification, and wave retardation or acceleration.
These wave characteristics constitute the hydraulics of flow routing or
propagation and are greatly affected by the geometric characteristics of
the conduits, the characteristics of sources and/or sinks, and by
initial and boundary conditions.

The hydraulics of unsteady non-uniform flow is represented in SWMM by a
pair of partial differential equations of conservation of mass and
momentum known as the St. Venant equations. Simultaneous solution of
these equations for each conduit, coupled with a conservation of volume
at each node, provides information on the spatial and temporal variation
of water levels and discharge rates throughout the network. SWMM offers
the user two principal alternative methods for solving these equations -
dynamic wave or kinematic wave analysis

Dynamic wave analysis solves the complete form of the St. Venant flow
equations and therefore produces the most theoretically accurate
results. It can account for channel storage, backwater effects,
entrance/exit losses, culvert flow, flow reversal, and pressurized flow.
Because it couples together the solution for both water levels at nodes
and flow in conduits it can be applied to any general network layout,
even those containing multiple downstream diversions and loops. It is
the method of choice for systems subjected to significant backwater due
to downstream flow restrictions and with flow regulation via weirs and
orifices. This generality comes at a price of having to use small time
steps to maintain numerical stability.

Kinematic wave analysis solves the continuity equation along with a
simplified form of the momentum equation in each conduit. It cannot
account for backwater effects, entrance/exit losses, flow reversal, or
pressurized flow. It is most applicable to steeply sloped (e.g., \>
0.1%) conduits with shallow flow with high velocity. It can usually
maintain numerical stability with much larger large time steps than are
required for dynamic wave analysis. If the aforementioned effects are
not expected to be significant then this alternative can be an accurate
and efficient hydraulic analysis method, especially for long-term
simulations.

Because kinematic wave analysis ignores both inertial and pressure
forces there are limits on its applicability:

1.  It can only analyze directed acyclic networks (where conduits are
    oriented in the direction of positive slope and there are no paths
    that start and end at the same node).

2.  Junction nodes can only have at most one outlet link which must be a
    conduit.

3.  Divider nodes must have two outlet links which must be conduits.

4.  Storage nodes can have any number of outlet links of any type.

5.  Upstream offsets for conduits are ignored except at storage nodes.

SWMM also offers a steady flow analysis option which assumes that within
each computational time step flow is uniform and steady. It simply
translates inflow hydrographs at the upstream end of a conduit to its
downstream end, with no delay or change in shape. The Manning equation
is used to relate flow rate to flow area (or depth). It is subject to
the same limitations as the kinematic wave method. Because it ignores
the dynamics of free surface wave propagation it is only appropriate for
rough preliminary analysis of long-term continuous simulations.

Table 2-1 compares the features and limitations of the dynamic wave and
kinematic wave methods of hydraulic analysis. Dynamic wave solutions
tend to attenuate and disperse an inflow hydrograph as it routed
downstream through a series of conduits while kinematic wave solutions
show no attenuation, no dispersion, and some distortion of the
hydrograph shape. This behavior is depicted in Figure 2-2 from Miller
(1984) which shows the results of routing an inflow hydrograph down a
100-foot wide rectangular channel of 1% slope with a Manning's *n* of
0.06.

  -----------------------------------------------------------------------
  **Feature**                **Dynamic Wave**       **Kinematic Wave**
  ----------------------- ----------------------- -----------------------
  Network topology          branched and looped        branched only

  Flow splits                       yes           with flow divider nodes

  Adverse slopes                    yes                     no

  Invert offsets                    yes                   ignored

  Pumping                           yes           only from storage nodes

  Weirs and orifices                yes           only from storage nodes

  Ponded overflows                  yes                     yes

  Lateral seepage                   yes                     yes

  Evaporation                       yes                     yes

  Minor losses                      yes                     no

  Culvert analysis                  yes                     no

  Hydrograph attenuation            yes                     no

  Backwater effects                 yes                     no

  Surcharge /                       yes                     no
  Pressurization                                  

  Reverse flow                      yes                     no

  Tidal effects                     yes                     no
  -----------------------------------------------------------------------

  : []{#_Toc484690417 .anchor}**Table 2‑1 Features and limitations of
  dynamic wave and kinematic wave solutions**

![KWvsDW.png](VolumeII/media/media/image10.png){width="6.5in"
height="4.427083333333333in"}

[]{#_Toc484694713 .anchor}**Figure 2‑2 Comparison of dynamic wave and
kinematic wave solutions**

> **(from Miller, 1984)**

## 2.3 Boundary and Initial Conditions

### 2.3.1 Boundary Conditions

There are two types of boundary conditions that a user must supply to a
SWMM conveyance network model:

1.  the hydraulic head to be maintained at each outfall node of the
    network,

2.  the external inflow received by specific nodes of the network.

Both types of conditions can vary with time. Outfall node heads are only
required for dynamic wave analysis. The options available for specifying
their values were described in Section 2.1.2. External inflows can
originate from any of the following sources:

- subcatchment runoff

- groundwater discharges

- rainfall-dependent infiltration/inflow (RDII)

- user-defined values.

Time-dependent runoff, groundwater, and RDII inflows are normally
provided by SWMM's hydrology module (see Volume I). It automatically
links the computed flow from each of these sources at each time period
to their designated receiving node. (Each SWMM subcatchment object that
generates runoff is assigned a conveyance system node that receives this
runoff. See Figure 1-2.)

User-defined external inflows can be attached to any node of the
network. They are typically used to describe dry weather sewage flows in
sanitary sewer systems, base flows in natural stream channels, or
inflows in the absence of any hydrologic modeling. They are expressed in
the following general format:

Flow rate at time *t* = (baseline value) × (baseline pattern factor) +

> (scale factor) × (time series value at time *t*)

The baseline value is some constant. The baseline pattern is a
combination of repeating hourly, daily, and monthly multiplier factors
applied to the baseline value. The time series value is a time varying
value and the scale factor is a constant multiplier applied to each time
series value. Time series values can be specified at unequal intervals
of time with interpolation used to obtain values at intermediate times.

### 2.3.2 Initial Conditions

A set of initial conditions at time 0 for all node heads and link flows
in the conveyance network must be specified before a hydraulic analysis
can begin. The default is to set all these values to 0, with the user
having the option to specify initial heads at selected nodes and initial
flow rates in selected conduit links.

Any initial flow rate assigned to a conduit link is assumed to represent
a uniform steady flow. Therefore its flow depth can be set to the normal
depth determined by the Manning equation as described in Section 5.5.2.
From this depth an initial cross-section flow area for the conduit can
be found which is required for kinematic wave analysis.

For dynamic wave analysis, if a non-storage, non-outfall node has not
had an initial head assigned to it then it's initial head is set equal
to the average elevation of the initial flow depths in the conduits that
deliver flow into it.

# - Dynamic Wave Analysis

\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_

The movement of water through a conveyance network of channels and pipes
is governed by the conservation of mass and momentum equations for
gradually varied, unsteady free surface flow. Dynamic wave analysis
solves the complete form of these equations and therefore produces the
most theoretically accurate results. It can account for channel storage,
backwater effects, entrance/exit losses, flow reversal, and pressurized
flow. Because it couples together the solution for both water levels at
nodes and flow in conduits it can be applied to any general network
layout, even those containing multiple downstream diversions and loops.
It is the method of choice for systems subjected to significant
backwater due to downstream flow restrictions and with flow regulation
via weirs and orifices. This generality comes at a price of having to
use small time steps to maintain numerical stability.

Dynamic wave modeling was first introduced into version 3 of SWMM in
1981 as a separate program module known as EXTRAN (Extended Transport)
(Roesner et al., 1983). The node-link solution method it uses had its
origins in the Sacramento-San Joaquin Delta Model (Shubinski et al.,
1965) and the WRE Transport Model (Kibler et al., 1975). Although more
powerful solution techniques are available (such as implicit finite
difference schemes (Cunge et al., 1980) and shock-capturing finite
volume schemes (Toro, 2001)), SWMM 5 continues to use EXTRAN's node-link
approach, with modifications made to enhance its stability, because of
its simplicity and versatility.

## 3.1 Governing Equations

The conservation of mass and momentum for unsteady free surface flow
through a channel or pipe are known as the St. Venant equations and can
be expressed as:

  ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------
        $$\frac{\partial A}{\partial t} + \frac{\partial Q}{\partial x} = 0$$                                                                           Continuity      (3-1)
  ----- ----------------------------------------------------------------------------------------------------------------------------------------------- ------------- -------
                                                                                                                                                                      

        $$\frac{\partial Q}{\partial t} + \frac{\partial\left( \frac{Q^{2}}{A} \right)}{\partial x} + gA\frac{\partial H}{\partial x} + gAS_{f} = 0$$   Momentum        (3-2)
  ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------

where

  ----------------------------------------------------------------------------
  *x*      =   distance (ft)
  -------- --- ---------------------------------------------------------------
  *t*      =   time (sec)

  *A*      =   flow cross-sectional area (ft^2^)

  *Q*      =   flow rate (cfs)

  *H*      =   hydraulic head of water in the conduit (*Z + Y*) (ft)

  *Z*      =   conduit invert elevation (ft)

  *Y*      =   conduit water depth (ft)

  *S~f~*   =   friction slope (head loss per unit length)

  *g*      =   acceleration of gravity (ft/sec^2^)
  ----------------------------------------------------------------------------

The derivation of these equations can be found in standard texts such as
Henderson (1966), Cunge et al. (1980) and French (1985). The assumptions
on which they are based are:

1.  flow is one dimensional

2.  pressure is hydrostatic

3.  the cosine of the channel bed slope angle is close to unity

4.  boundary friction can be represented in the same manner as for
    steady flow.

The friction slope *S~f~* can be expressed in terms of the Manning
equation used to model steady uniform flow:

  -----------------------------------------------------------------------------------
        $$S_{f} = \left( \frac{n}{1.486} \right)^{2}\frac{Q|U|}{AR^{4/3}}$$   (3-3)
  ----- --------------------------------------------------------------------- -------

  -----------------------------------------------------------------------------------

where

  -------------------------------------------------------------------------
  *n*   =   the Manning roughness coefficient (sec/m^1/3^)
  ----- --- ---------------------------------------------------------------
  *R*   =   the hydraulic radius of the flow cross-section (ft)

  *U*   =   flow velocity, equal to $\frac{Q}{A}$ (ft/sec).
  -------------------------------------------------------------------------

and 1.486 converts from m^1/3^ to ft^1/3^. Use of the absolute value
sign on the velocity term makes *S~f~* a directional quantity (since *Q*
can be either positive or negative) and ensures that the frictional
force always opposes the flow. Manning roughness coefficients for wide
range of channel surfaces and pipe materials can be found in Appendix G.

For a specific cross-sectional geometry, the flow area *A* is a known
function of water depth *Y* which in turn can be obtained from the head
*H*. Thus the dependent variables in these equations are flow rate *Q*
and head *H*, which are functions of distance *x* and time *t*. To solve
these equations over a single conduit of length *L*, one needs a set of
initial conditions for *H* and *Q* at time 0 as well as boundary
conditions at *x* = 0 and *x* = *L* for all times *t*.

The continuity equation 3-1 can be combined with the momentum equation
3-2 to produce the following form of the momentum equation for a conduit
(see sidebar below for details):

  ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
        $$\frac{\partial Q}{\partial t} = 2U\frac{\partial A}{\partial t} + U^{2}\frac{\partial A}{\partial x} - gA\frac{\partial H}{\partial x} - gAS_{f}$$                 (3-4)
  ----- ------------------------------------------------------------------------------------------------------------------------------------------------------ ------------- -------

  ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

While this equation can be used to compute the time trajectory of flow
in a conduit, another relationship is needed to do likewise for heads.
SWMM's node -- link representation of the conveyance network,
conceptualized in Figure 3-1, does this by providing a continuity
relationship at junction nodes that connect conduits together within a
conveyance network. As shown in the figure, a continuous water surface
is assumed to exist between the water elevation at a node and in the
conduits that enter and leave it. Two types of nodes are possible.
Non-storage junction nodes are assumed to be points with zero volume and
surface area while storage nodes (such as ponds and tanks) contain both
volume and surface area.

![Node-Link.bmp](VolumeII/media/media/image11.png){width="5.854166666666667in"
height="2.7916666666666665in"}

[]{#_Toc484694714 .anchor}**Figure 3‑1 Node-link representation of a
conveyance network in SWMM**

**(from Roesner et al, 1992).**

Each "node assembly" consists of the node itself and half the length of
each link connected to it. Conservation of flow for the assembly
requires that the change in volume with respect to time equal the
difference between inflow and outflow. In equation terms:

  -----------------------------------------------------------------------------------------------------------------------------------------------------------------
        $$\frac{\partial V}{\partial t} = \frac{\partial V}{\partial H}\frac{\partial H}{\partial t} = A_{S}\frac{\partial H}{\partial t} = \sum_{}^{}Q$$   (3-5)
  ----- --------------------------------------------------------------------------------------------------------------------------------------------------- -------

  -----------------------------------------------------------------------------------------------------------------------------------------------------------------

where:

  ----------------------------------------------------------------------------
  *V*      =   node assembly volume (ft^3^)
  -------- --- ---------------------------------------------------------------
  *A~S~*   =   node assembly surface area (ft^2^)

  *ΣQ*     =   net flow into the node assembly (inflow -- outflow) (cfs)
  ----------------------------------------------------------------------------

The $\sum_{}^{}Q$ term includes the flow in the conduits connected to
the node as well as any externally imposed inflows such as wet weather
runoff or dry weather sanitary flow.

Each node assembly's surface area consists of the node's storage surface
area *A~SN~* (if it's a storage node) plus the surface area contributed
by the links connected to it, $\sum_{}^{}A_{SL}$, where *A~SL\ ~*is the
surface area contributed by a connecting link. Thus the node continuity
equation can be written as:

  -------------------------------------------------------------------------------------------------
        $$\frac{\partial H}{\partial t} = \frac{\sum_{}^{}Q}{A_{SN} + \sum_{}^{}A_{SL}}$$   (3-6)
  ----- ----------------------------------------------------------------------------------- -------

  -------------------------------------------------------------------------------------------------

The flow depth at the end of a conduit connected to a node can be
computed as the difference between the head at the node and the invert
elevation of the conduit. The node and link surface areas are computed
as functions of their respective flow depths.

Equations 3-4 and 3-6 provide a coupled set of partial differential
equations that solve for flow *Q* in the conduits and head *H* at the
nodes of the conveyance network. Because they cannot be solved
analytically a numerical solution procedure must be used instead.

## 3.2 Solution Method

The material that follows applies to networks containing only conduits.
Inclusion of flow control devices (pumps, orifices, and weirs) and other
processes (seepage, evaporation, and minor losses) will be covered in
subsequent chapters of this manual.

The spatial and temporal derivatives in equations 3-4 and 3-6 can be
replaced with the following finite difference approximations:

  --------------------------------------------------------------------------------------------------------
        $$\frac{\partial A}{\partial x} = \frac{\left( A_{2} - A_{1} \right)}{L}$$                 (3-7)
  ----- ---------------------------------------------------------------------------- ------------- -------

  --------------------------------------------------------------------------------------------------------

  --------------------------------------------------------------------------------------------------------
        $$\frac{\partial H}{\partial x} = \frac{\left( H_{2} - H_{1} \right)}{L}$$                 (3-8)
  ----- ---------------------------------------------------------------------------- ------------- -------

  --------------------------------------------------------------------------------------------------------

  ----------------------------------------------------------------------------------------------------------------------
        $$\frac{\partial A}{\partial t} = \frac{\mathrm{\Delta}\overline{A}}{\mathrm{\Delta}t}$$                 (3-9)
  ----- ------------------------------------------------------------------------------------------ ------------- -------

  ----------------------------------------------------------------------------------------------------------------------

  ------------------------------------------------------------------------------------------------------------
        $$\frac{\partial Q}{\partial t} = \frac{\mathrm{\Delta}Q}{\mathrm{\Delta}t}$$                 (3-10)
  ----- ------------------------------------------------------------------------------- ------------- --------

  ------------------------------------------------------------------------------------------------------------

  ------------------------------------------------------------------------------------------------------------
        $$\frac{\partial H}{\partial t} = \frac{\mathrm{\Delta}H}{\mathrm{\Delta}t}$$                 (3-11)
  ----- ------------------------------------------------------------------------------- ------------- --------

  ------------------------------------------------------------------------------------------------------------

where

  ----------------------------------------------------------------------------------------------------------
  *A~1~*                =   flow area at the upstream end of the conduit (ft^2^)
  --------------------- --- --------------------------------------------------------------------------------
  *A~2~*                =   flow area at the downstream end of the conduit (ft^2^)

  *H~1~*                =   hydraulic head at the upstream end of the conduit (ft)

  *H~2~*                =   hydraulic head at the downstream end of the conduit (ft)

  *L*                   =   conduit length (ft)

  *∆t*                  =   time step (sec)

  *∆*$\ \overline{A}$   =   change in average flow area,
                            $\left( {\overline{A}}^{t + \mathrm{\Delta}t} - {\overline{A}}^{\ t} \right)$,
                            over time step *∆t* (ft^2^)

  *∆Q*                  =   change in conduit flow, $\left( Q^{t + \mathrm{\Delta}t} - Q^{t} \right)$, over
                            time step *∆t* (cfs)

  *∆H*                  =   change in nodal head, $\left( H^{t + \mathrm{\Delta}t} - H^{t} \right)$, over
                            time step *∆t* (ft).
  ----------------------------------------------------------------------------------------------------------

with the superscripts referring to time periods.

Substituting these finite difference approximations into the link
momentum Equation 3-4, replacing *S~f~* with Equation 3-3, and replacing
*A, U*, and *R* with their average values over the conduit length (as
indicated by over scores) allows the finite difference form of the link
momentum equation to be written as:

  -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
        $$\frac{\mathrm{\Delta}Q}{\mathrm{\Delta}t} = 2\overline{U}\frac{\mathrm{\Delta}\overline{A}}{\mathrm{\Delta}t} + {\overline{U}}^{2}\frac{\left( A_{2} - A_{1} \right)}{L} - g\overline{A}\frac{\left( H_{2} - H_{1} \right)}{L} - g\eta^{2}\frac{Q\left| \overline{U} \right|}{{\overline{R}}^{4/3}}$$   (3-12)
  ----- --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- ---------

  -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

where $\eta = \frac{n}{1.486}$. Average values for *A, U*, and *R* can
be approximated using the heads *H~1~* and *H~2~* as described later on
in section 3.3.1.

The finite difference form of the nodal continuity equation 3-6 is:

  --------------------------------------------------------------------------------------------------------------
        $$\frac{\mathrm{\Delta}H}{\mathrm{\Delta}t} = \frac{\sum_{}^{}Q}{A_{SN} + \sum_{}^{}A_{SL}}$$   (3-13)
  ----- ----------------------------------------------------------------------------------------------- --------

  --------------------------------------------------------------------------------------------------------------

Previous versions of SWMM used an explicit forward Euler method (or more
precisely the two-step Modified Euler method) to solve Equation 3-12,
where known values of *Q, H, A*, $\overline{A}$, $\overline{U}$, and
$\overline{R}$ at time *t* were used to solve for *Q* at time *t + ∆t*.
Then Equation 3-13 was solved with the new conduit flows to find new
head values *H* at time *t + ∆t*.

SWMM 5 uses an implicit backwards Euler method instead to provide
improved stability (Ascher and Petzold, 1998). Under this scheme
Equation 3-12 is re-written as:

  -------------------------------------------------------------------------------------------------------------------------------------------------------------
       $$Q^{t + \mathrm{\Delta}t} = \frac{Q^{t} + {\mathrm{\Delta}Q}_{inertia} + {\mathrm{\Delta}Q}_{pressure}}{1 + {\mathrm{\Delta}Q}_{friction}}$$   (3-14)
  ---- ----------------------------------------------------------------------------------------------------------------------------------------------- --------

  -------------------------------------------------------------------------------------------------------------------------------------------------------------

where

  -----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
        $${\mathrm{\Delta}Q}_{inertia} = 2\overline{U}\left( {\overline{A}}^{t + \mathrm{\Delta}t} - {\overline{A}}^{\ t} \right) + {\overline{U}}^{2}\frac{\left( A_{2} - A_{1} \right)}{L}\Delta t$$   (Inertial Term)      (3-14a)
  ----- ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ ------------------ ---------
        $${\mathrm{\Delta}Q}_{pressure} = - g\overline{A}\frac{\left( H_{2} - H_{1} \right)}{L}\Delta t$$                                                                                                (Pressure Term)      (3-14b)

        $${\mathrm{\Delta}Q}_{friction} = g\eta^{2}\frac{\left| \overline{U} \right|\Delta t}{{\overline{R}}^{4/3}}$$                                                                                    (Friction Term)      (3-14c)
  -----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

and now *H* and the quantities *A*, $\overline{A}$, $\overline{U}$, and
$\overline{R}$ derived from it are all evaluated at the new time *t+∆t*.
The finite difference form of the nodal continuity equation 3-12 can be
expressed as:

  -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
       $$H^{t + \mathrm{\Delta}t} = H^{t} + \frac{\frac{\Delta t}{2}\left( \sum_{}^{}{Q^{t} + \sum_{}^{}Q^{t + \mathrm{\Delta}t}} \right)}{\left( A_{SN} + \sum_{}^{}A_{SL} \right)^{t + \mathrm{\Delta}t}}$$   for non-outfall       (3-15a)
                                                                                                                                                                                                                nodes              
  ---- -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- ------------------ ----------
                                                                                                                                                                                                                                   

       $$H^{t + \mathrm{\Delta}t} = H_{Outfall}$$                                                                                                                                                               for outfall nodes     (3-15b)
  -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

*H~Outfall~* is a user-supplied value that sets the head at a terminal
outfall node. It can be a constant value, a value extracted from a
user-supplied time series, or the elevation of the critical or normal
flow depth in the connecting conduit. For the latter option, critical or
normal depth is computed internally as a function of the conduit's flow
rate and geometry as described in Chapter 5.

Equations 3-14 and 3-15 can be solved implicitly over a given time step
*∆t* using functional iteration (also known as successive approximations
or Picard's method). The method is described in the sidebar titled
"*Dynamic Wave Solution Procedure*". Because flows and heads are updated
one conduit and node at a time and not simultaneously, the results at
each time step are invariant to the order in which the conduits and
links are evaluated. This allows Steps 2 and 4 of the solution procedure
to be implemented using separate threads running in parallel on
multi-processor computers which can offer a significant reduction in
computation time.

## 3.3 Computational Details

### 3.3.1 Average Cross-Section Properties

Evaluation of the flow updating formula 3-14 requires values for the
average area ($\overline{A}$), hydraulic radius ($\overline{R}$), and
velocity ($\overline{U}$) for the conduit in question. These values are
computed using heads *H~1~* and *H~2~* belonging to the most recently
computed head estimates *H^last^* at either end of the conduit. The flow
depth *Y~1~* at the upstream end of the conduit is computed as:

+----+----------------------------------+----------------------------------------------+--------+
|    | $$Y_{1} = \left\{ \begin{matrix} | $$for\ H_{1} \leq Z_{1}$$                    | (3-16) |
|    | 0 \\                             |                                              |        |
|    | H_{1} - Z_{1} \\                 | $$for\ Z_{1} < H_{1} \leq Z_{1} + Y_{full}$$ |        |
|    | Y_{full}                         |                                              |        |
|    | \end{matrix} \right.\ $$         | $$for\ H_{1} > {Z_{1} + Y}_{full}$$          |        |
+====+==================================+==============================================+========+

where Z*~1~* is the elevation of the invert of the upstream end of the
conduit and *Y~full~* is the full depth of the conduit. A similar
expression using *H~2~* and *Z~2~* applies to *Y~2~* at the downstream
end of the conduit.

Values of $\overline{A}$ and $\overline{R}$ are computed from the
conduit's cross section geometry at the average flow depth
$\frac{\overline{Y} = \left( Y_{1} + Y_{2} \right)}{2}$. Formulas for
doing so are described in Chapter 5 of this manual. The average velocity
$\overline{U}$ is found by dividing the most current flow value
*Q^last^* by the average area $\overline{A}$.

In addition, the average area and hydraulic radius used in the pressure
and friction terms of equation 3-14 are upstream weighted to reflect how
close a conduit's flow is to being supercritical. Supercritical flow is
influenced only by upstream conditions (i.e., wave disturbances
propagate only in the downstream direction). The weight is derived from
the Froude number *Fr* for *Q^last^*:

  -------------------------------------------------------------------------------------------------------
       $$Fr = \frac{\left| \overline{U} \right|}{\sqrt{g\frac{\overline{A}}{\overline{W}}}}$$   (3-17)
  ---- ---------------------------------------------------------------------------------------- ---------

  -------------------------------------------------------------------------------------------------------

where $\overline{W}$ is the top water surface width at the average depth
$\overline{Y}$. (*Fr* is set to 0 for closed conduits flowing full). A
factor *σ* is then computed as:

+----+-----------------------------------+--------------------------------------+--------+
|    | $$\sigma = \left\{ \begin{matrix} | $$for\ Fr \leq 0.5$$                 | (3-18) |
|    | 1 \\                              |                                      |        |
|    | 2(1 - Fr) \\                      | $$for\ 0.5 < Fr < 1$$                |        |
|    | 0                                 |                                      |        |
|    | \end{matrix} \right.\ $$          | $$for\ Fr \geq 1$$                   |        |
+====+===================================+======================================+========+

It is used to modify the average area in Equation 3-14b and the average
hydraulic radius in Equation 3-14c as follows:

  ------------------------------------------------------------------------------------------
       $${\overline{A}}' = A_{1} + \ \sigma\left( \overline{A} - A_{1} \right)$$      (3-19)
  ---- --------------------------------------------------------------------------- ---------
                                                                                   

       $${\overline{R}}' = R_{1} + \ \sigma\left( \overline{R} - R_{1} \right)$$      (3-20)
  ------------------------------------------------------------------------------------------

where *A~1~* and *R~1~* are the flow area and hydraulic radius,
respectively, based on the upstream flow depth *Y~1~*.

### 3.3.2 Surface Area Calculations

Under normal conditions the surface area that a conduit contributes to
its upstream node (*A~SL1~*) is the average top width of the water
surface over the upstream half of the conduit times half of the
conduit's length. In equation form:

  ------------------------------------------------------------------------------------------------------------
        $$A_{SL1} = \left( \frac{W\left( Y_{1} \right) + \ W(\overline{Y})}{2} \right)\frac{L}{2}$$   (3-21)
  ----- --------------------------------------------------------------------------------------------- --------

  ------------------------------------------------------------------------------------------------------------

where *W(Y)* is the flow cross-section top width at a given flow depth
*Y* and $\overline{Y} = \frac{\left( Y_{1} + Y_{2} \right)}{2}$. A
similar expression applies to the downstream surface area *A~SL2~*.
*W(Y)* is computed from the conduit's cross-section geometry as
described in Chapter 5.

Because sewer systems are frequently built with pipe invert
discontinuities at manholes they can encounter free-fall conditions
where the water elevation in the node receiving flow is below the pipe's
invert elevation or the flow's critical depth. Also during periods of
filling or draining, conduits can have one end or the other dry. These
conditions require that adjustments be made to the way that flow depth
is assigned and to how surface area is computed.

Figure 3-2 illustrates the various types of special flow conditions that
affect surface area calculations:

1.  Case one is the normal situation of subcritical flow where flow
    depths and surface areas are computed as previously described.

2.  Case two represents a critical downstream condition. The conduit has
    a downstream offset and the water level at the node is below the
    flow's critical depth. The downstream depth is set equal to the
    smaller of the critical depth and normal depth for the current flow
    and all of the conduit's surface area is assigned to the upstream
    node.

3.  Case three is a critical upstream condition. There is reverse flow
    with a free-fall discharge into the upstream node. Adjustments
    equivalent to those for case two are made but with the definitions
    of upstream and downstream reversed.

4.  Case four depicts an upstream dry condition. The upstream end of the
    conduit is dry and the water level at the downstream end is below
    the upstream conduit invert. If there is an upstream invert offset
    then no surface area is assigned to the upstream node. A
    complementary set of rules applies to the opposite case of a
    downstream dry condition.

Table 3-1 summarizes the various flow conditions and the adjustments
that are made for each. Procedures for computing the critical depth and
normal depth for a given flow rate and cross-section geometry are
discussed in Chapter 5 of this manual.

Finally, to guard against the nodal head change formula 3-15 from
becoming unbounded as surface area becomes vanishingly small, a global
minimum surface area *A~Smin~* is imposed as follows:

  -----------------------------------------------------------------------------------
        $$A_{S} = max\left( A_{Smin},\ A_{SN} + \sum_{}^{}A_{SL} \right)$$   (3-22)
  ----- -------------------------------------------------------------------- --------

  -----------------------------------------------------------------------------------

Its default value is 12.56 sq ft (i.e., the area of a 4-ft diameter
manhole) which can be overridden by the user. This is strictly a
computational device and does not add volume to a junction node (where
*A~SN~ = 0*) nor change it into a storage node.

![Pipe.bmp](VolumeII/media/media/image12.png){width="5.666666666666667in"
height="2.622649825021872in"}

![SurfaceArea2.bmp](VolumeII/media/media/image13.png){width="4.864583333333333in"
height="3.2291666666666665in"}

[]{#_Toc484694715 .anchor}**Figure 3‑2 Special flow conditions for
dynamic wave analysis**

+-------------------------+-------------------------+--------------------------------------------------------------+
| **Condition**           | **Criteria**            | **Adjustments**                                              |
+=========================+=========================+:=============================================================+
| Upstream Dry            | *Y~1~ = 0*              | *A~SL1~ = 0* if $H_{2} \leq Z_{1}$                           |
|                         |                         |                                                              |
|                         | *Z~1~ \> E~1~*          | otherwise use Upstream Critical adjustment                   |
+-------------------------+-------------------------+--------------------------------------------------------------+
| Downstream Dry          | *Y~2~ = 0*              | *A~SL2~ = 0* if $H_{1} \leq Z_{2}$                           |
|                         |                         |                                                              |
|                         | *Z~2~ \> E~2~*          | otherwise use Downstream Critical adjustment                 |
+-------------------------+-------------------------+--------------------------------------------------------------+
| Upstream Critical       | *Q \< 0*                | *Y~1~ = Y\**                                                 |
|                         |                         |                                                              |
|                         | *Z~1~ \> E~1~*          | *H~1~ = Y\* + Z~1~*                                          |
|                         |                         |                                                              |
|                         | *H~1~ -- Z~1~ \< Y\**   | *A~SL1~ = 0*                                                 |
|                         |                         |                                                              |
|                         |                         | $$A_{SL2} = L\frac{\left( \overline{W} + W_{2} \right)}{2}$$ |
+-------------------------+-------------------------+--------------------------------------------------------------+
| Downstream Critical     | *Q \> 0*                | *Y~2~ = Y\**                                                 |
|                         |                         |                                                              |
|                         | *Z~2~ \> E~2~*          | *H~2~ = Y\* + Z~2~*                                          |
|                         |                         |                                                              |
|                         | *H~2~ -- Z~2~ \< Y\**   | *A~SL2~ = 0*                                                 |
|                         |                         |                                                              |
|                         |                         | $$A_{SL1} = L\frac{\left( \overline{W} + W_{1} \right)}{2}$$ |
+-------------------------+-------------------------+--------------------------------------------------------------+
| Notes:                                                                                                           |
|                                                                                                                  |
| 1.  *E~1~* = upstream node invert elevation, *E~2~* = downstream node invert elevation.                          |
|                                                                                                                  |
| 2.  *Z~1\ ~*= upstream conduit invert elevation, *Z~2~* = downstream conduit invert elevation.                   |
|                                                                                                                  |
| 3\. *Y\** = smaller of critical depth and normal depth at current conduit flow rate.                             |
|                                                                                                                  |
| 4\. Adjusted *H* values are only used in the flow updating Equation 3-14 and do not replace nodal head values.   |
+------------------------------------------------------------------------------------------------------------------+

: []{#_Toc484690418 .anchor}**Table 3‑1 Surface area adjustments for
various dynamic wave flow conditions**

### 3.3.3 Inertial Damping

It has been found that reducing the contribution of the inertial terms
in the Saint Venant equation as the flow shifts between sub-critical and
supercritical states improves the solution's stability (see Fread et al.
(1996) where it is referred to as the Local Partial Inertia technique).
SWMM 5 offers the option to use the aforementioned σ factor to dampen
the inertial term ${\mathrm{\Delta}Q}_{inertia}$ in the flow updating
formula 3-14. As seen by equation 3-18, the factor is 1 for Froude
numbers up to 0.5, 0 for Froude numbers at 1 or higher, and varies
linearly in between. The damping factor σ is computed and applied on a
conduit by conduit basis.

Another option offered by SWMM 5 is to ignore the inertial term
completely. This corresponds to the so-called local inertial formulation
of the St. Venant equation (de Almeida and Bates, 2013). It drops the
convective acceleration term
$\left( \frac{\partial\left( \frac{Q^{2}}{A} \right)}{\partial x} \right)$
of the momentum equation 3-2 altogether resulting in
${\mathrm{\Delta}Q}_{inertia}$ being 0 in all conduits. (This is not the
same as the diffusion wave formulation which also drops the local
acceleration term $\left( \frac{\partial Q}{\partial t} \right)$ of the
momentum equation as well.) This option can also result in improved
stability particularly during periods of rapid flow change.

### 3.3.4 Flow Limitations

Each time a new flow is computed using Equation 3-14 it is checked to
see if it should be limited by the normal flow value for the upstream
flow depth and conduit slope. The following criteria are used to perform
this check:

1.  The computed flow is positive.

2.  The conduit is not flowing full.

3.  The conduit does not fall into any of the categories listed in Table
    3-1 (upstream / downstream dry or upstream / downstream critical).

4.  The water surface slope is less than the conduit's slope or the
    flow's Froude number based on upstream velocity and depth is greater
    than 1.

The last criterion can be limited to just slope, just Froude number or
either slope or Froude number as a program option. When all of these
criteria are satisfied the flow is limited to be no greater than that
found by the Manning equation (*Q~norm~*) using upstream conditions:

  --------------------------------------------------------------------------
        $$Q_{norm} = \frac{1.49}{n}A_{1}R_{1}^{2/3}\sqrt{S_{0}}$$   (3-23)
  ----- ----------------------------------------------------------- --------

  --------------------------------------------------------------------------

where *S~0~* is the conduit slope. Two other flow limiting conditions
are also checked. If the conduit was assigned an upper flow limit then
the flow is not allowed to exceed that value. If the conduit contains a
flap gate and the computed flow is negative then the flow is set to 0.

### 3.3.5 Surcharge Conditions

SWMM defines a node to be in a surcharged condition when all conduits
connected to it are full or when the node's water level exceeds the
crown of the highest conduit connected to it (see Figure 3-3). It should
be noted that surcharged (or pressurized) flow can occur in a closed
conduit without either of its end nodes being surcharged. For example,
if the node water level in Figure 3-3 was above the invert of pipe N+1
but below its crown, then pipes N and N-1 would remain pressurized
(assuming they were also full at their upstream ends) while the node
itself would no longer be surcharged.

![Surcharge4.bmp](VolumeII/media/media/image14.png){width="4.191363735783027in"
height="4.183096019247594in"}

[]{#_Toc484694716 .anchor}**Figure 3‑3 Illustration of a surcharged
node**

When a node becomes surcharged there is no more volume available in the
conduits forming the node's assembly to absorb the difference between
inflow and outflow at the node. Thus $\frac{\partial V}{\partial t}$ in
the flow continuity Equation 3-5 is 0 and the surcharged nodal
continuity condition becomes:

  -------------------------------------------------------------------------
        $$\sum_{}^{}Q = 0$$                                        (3-24)
  ----- ---------------------------------------------------------- --------

  -------------------------------------------------------------------------

By itself, this equation is insufficient to update nodal heads at the
new time step since it only contains flows. In addition, because the
flow and head updating equations for the system are not solved
simultaneously, there is no guarantee that the condition will hold at
the surcharged nodes after a flow solution has been reached.

To enforce the surcharge flow continuity condition, it can be expressed
in the form of a perturbation equation:

  ---------------------------------------------------------------------------------------------------------------
        $$\sum_{}^{}\left\lbrack Q + \frac{\partial Q}{\partial H}\mathrm{\Delta}H \right\rbrack = 0$$   (3-25)
  ----- ------------------------------------------------------------------------------------------------ --------

  ---------------------------------------------------------------------------------------------------------------

where *∆H* is the adjustment to the node's head that must be made to
achieve a flow balance. Solving for *∆H* yields:

  -----------------------------------------------------------------------------------------------------
        $$\mathrm{\Delta}H = \frac{- \sum_{}^{}Q}{\sum_{}^{}\frac{\partial Q}{\partial H}}$$   (3-26)
  ----- -------------------------------------------------------------------------------------- --------

  -----------------------------------------------------------------------------------------------------

where the summations are made over all conduits that are connected to
the node in question.

The gradient of flow in a conduit with respect to the head at either end
node can be evaluated by differentiating the flow updating equation 3-14
resulting in:

  --------------------------------------------------------------------------------------------------------------------------------------
        $$\frac{\partial Q}{\partial H} = \frac{\frac{- g\overline{A}\mathrm{\Delta}t}{L}}{1 + \mathrm{\Delta}Q_{friction}}$$   (3-27)
  ----- ----------------------------------------------------------------------------------------------------------------------- --------

  --------------------------------------------------------------------------------------------------------------------------------------

The numerator of $\frac{\partial Q}{\partial H}$ has a negative sign in
front of it because when evaluating ΣQ flow directed out of a node is
considered negative while flow into the node is positive. It is computed
for each link at the same time that the link's flow is updated at Step 2
of the iterative process described in Section 3.3. The surcharge
equation 3-26 is analogous to the head updating formula used in the
Hardy Cross method for pressurized water distribution networks (Bhave,
1991).

To accommodate node surcharging, Step 4 of the iterative process that
updates a node's head is modified as follows. First the node is checked
to see if it is in a surcharged state, i.e., that it is not a storage or
outfall node and has *H^last^* greater than the top of the highest
connecting conduit *H~crown~*. If it is not surcharged then Equation
3-15 is used as before to update its head. Otherwise the following
modified form of Equation 3-26 is used to estimate the new head *H^new^*
for time *t + ∆t*:

  -----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
       $$H^{new} = H^{last} + \frac{\alpha\sum_{}^{}Q^{new}}{(1 - \beta)\sum_{}^{}\left( \frac{\partial Q}{\partial H} \right)^{last} + \frac{\beta A_{S}^{last}}{\mathrm{\Delta}t}}$$   (3-28)
  ---- --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- --------

  -----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

where

  ---------------------------------------------------------------------------------------------------
  *α*                =   0.6 for upstream terminal nodes with only outflow links and 1.0 otherwise
  ------------------ --- ----------------------------------------------------------------------------
  *β*                =   $exp( - 15.0f_{H})$

  *f~H~*             =   $$\frac{\left( H^{last} - E \right)}{\left( H_{crown} - E \right) - \ 1}$$

  *H~crown~*         =   elevation of the crown of the node's highest connecting flowing conduit (ft)

  *E*                =   elevation of the node's invert (ft)

  $$A_{S}^{last}$$   =   surface area of the node the last time it was not surcharged (ft^2^)
  ---------------------------------------------------------------------------------------------------

The *α* factor is used to reduce oscillations in head at upstream
terminal nodes that have only outflow links (Roesner et al., 1992). The
*β* factor helps to reduce fluctuations in head when the node first
begins to surcharge (Roesner et al., 1980). At low surcharge depths it
makes the denominator in the head update formula be a weighted
combination of the pure surcharge formula 3-26 and the surface area
formula 3-15. By the time that the water level rises 25% above the
highest conduit, the equation is 98% pure surcharge.

The flow values used for $\sum_{}^{}Q$ are the new flow estimates found
from Step 3 of the solution procedure. The
$\frac{\partial Q}{\partial H}$ values are those that were last
evaluated at Step 2. And finally, empirical testing has shown that more
robust performance is obtained when under-relaxation is not applied to
*H^new^* at Step 5 of the solution procedure when surcharging occurs.

### 3.3.6 Preissmann Slot

As an alternative to the surcharge algorithm described in the previous
section, SWMM can utilize the Preissmann Slot Method (Cunge and Wegner,
1964) for handling pressurized flow in closed conduits. In this case the
conduit's cross-section is assumed to have a thin open slot at its top
which runs down its length. This permits the water level in the conduit
to exceed its full depth while only slightly increasing its flow area.
It thus becomes possible to compute a surface area contribution to the
conduit's end nodes once it reaches full depth. As a result, SWMM is
able to use its regular procedure for solving the open channel flow
equations 3-14 and 3-15 for all flow conditions without having to resort
to the surcharge algorithm.

In theory the width of the slot should be determined based on having the
celerity of an open channel gravity wave equal the speed of a pressure
wave affected by the compressibility of the elastic pipe wall. This
would result in a slot width *w~slot~* equal to:

> $w_{slot} = gA/c^{2}$ (3-29)

where *g* is the acceleration of gravity, *A* is the conduit's
cross-sectional area when full and *c* is the speed of the pressure
wave. The latter quantity depends on the conduit's diameter, wall
thickness, and modulus of elasticity and typically ranges from a few
hundred to several thousand ft/sec (Yen, 2001).

Some care is needed in choosing a slot width since too large a value
will result in reduced accuracy while too small a value can cause
numerical instabilities. There is also the issue of maintaining a smooth
transition between almost full flow and slot flow. The choice used by
SWMM is a modified version of a formula proposed by Sjőberg (1982) and
is given by:

> $\frac{w_{slot}}{W_{\max}} = 0.5423\exp\left( - \left( \frac{Y}{Y_{full}} \right)^{2.4} \right)$
> (3-30)

where *W~max~* is the conduit's maximum width, *Y~full~* is its full
depth, and Y is depth of flow. This equation applies to
$\frac{Y}{Y_{full}}$ values between 0.985257 and 1.7. Below this range
the slot is not used while above it the slot width relative to *W~max~*
is clamped at 0.01. The range's lower limit was chosen so that the width
computed from equation 3-30 is the same as the width across a circular
pipe at that flow depth. This helps produce a smooth transition between
open channel and pressurized flow regimes.

When the slot method is employed, equation 3-16 is modified so that *Y*
is no longer limited by *Y~full~*. When *Y* reaches the limit at which
the slot formula applies, its resulting width is used to compute the
surface area that a conduit contributes to its end nodes as described in
Section 3.3.2. It also contributes to the conduit's flow area when it
rises above the full depth. It is not used when computing the conduit's
hydraulic radius.

### 3.3.7 Flooding and Ponding

Each non-outfall node is assigned a maximum allowable head *H~max~* by
the user. It consists of both a maximum free water surface elevation
that can exist at the node plus an optional "surcharge" depth that
allows for pressurization. For example, if the node were a manhole
junction *H~max~* would typically be the ground surface elevation. If it
were a storage unit it would be the water surface elevation when the
unit is full. For a junction between natural channels it would be the
top of the highest channel. For a fitting that connects pipe segments
together it would be the top of the highest pipe. In the latter case a
large surcharge depth (such as several hundred feet) should be assigned
to the fitting junction so that the connected pipes can pressurize if
need be. A manhole junction might also be assigned a surcharge depth if
it has a bolted cover.

Normally when the new head estimate *H^new^* at a node computed at Step
5 of the iterative solution process exceeds *H~max~* it is set equal to
*H~max~* and the node becomes flooded. The overflow rate *Q~ovfl~*
associated with this condition is the average net flow rate (inflow --
outflow) seen by the node over the current time step:

  --------------------------------------------------------------------------------------------------------
        $$Q_{ovfl} = 0.5\left( \sum_{}^{}{Q^{t} + \sum_{}^{}Q^{t + \mathrm{\Delta}t}} \right)$$   (3-31)
  ----- ----------------------------------------------------------------------------------------- --------

  --------------------------------------------------------------------------------------------------------

This flow is then lost from the system, the same as the flow entering a
terminal outfall node.

The option exists for a junction node with no surcharge depth (and thus
always maintaining a free surface) to have excess flooded water pond
atop the node (see Figure 3-4). In this case the user assigns the node a
"ponded area" parameter, *A~P~*, that creates a virtual storage area on
top of the node and *H^new^* is no longer limited to *H~max~* . When
*H^new^* exceeds *H~max~* the ponded node is treated as a normal storage
node whose head is updated using the normal, non-surcharge formula
Equation 3-15 with *A~SN~ = A~P~*. The only exception to this is when
the node transitions between having a head below *H~max~* to a flooded
head above *H~max~* (or vice versa) within a time step. In this case the
updated head is restricted to be just a small value above *H~max~* (or
below it in the opposite case) to avoid wide swings in head during the
transition.

<figure>
<img src="VolumeII/media/media/image15.png"
style="width:4.18826in;height:4.18in" alt="Surcharge5.bmp" />
<figcaption><p><span id="_Toc484694717"
class="anchor"></span><strong>Figure 3‑4 Ponding of excess water above a
junction</strong></p></figcaption>
</figure>

When a node is allowed to pond, flooded water is not lost from the
system. The ponded depth above the node will rise during periods of flow
excess (i.e., inflow greater than outflow) and fall during periods of
flow deficit. A node with a large ponded area will see smaller changes
in ponded depth for a given flow excess (or deficit) than will one with
a small ponded area. Selection of which nodes can pond and their
respective ponded areas would depend on local topography, typically
occurring along flat sections or at sag points of the drainage system.

### 3.3.8 Summary of Special Conditions

Here is a summary of the special conditions that are applied to the
basic iterative solution process for dynamic wave analysis described
earlier in Section 3.2:

1.  Upstream weighting, based on the current flow's Froude number, is
    applied to the average area in the pressure term and to the average
    hydraulic radius in the friction term of the flow updating formula
    3-14 (see Section 3.3.1).

2.  Optional inertial damping, again based on the Froude number, is
    applied to the inertial term of the flow updating formula 3-14 (see
    Section 3.3.2).

3.  The surface area contributed by a conduit to its end nodes in the
    head updating formula 3-15 is modified when either critical flow
    depth or dry conditions occur (see Section 3.3.3).

4.  A conduit's updated flow is limited to the Manning normal flow if
    warranted by water surface slope and/or Froude number criteria (see
    Section 3.3.4).

5.  If SWMM's Surcharge Algorithm is used then the head updating formula
    3-15 is replaced with equation 3-28 when a node is in a surcharged
    state (see Section 3.3.5).

6.  If SWMM's Slot Method is used then no adjustment to equation 3-15 is
    necessary as the computed slot width will be used to compute surface
    and flow areas in a full flowing conduit (see Section 3.3.6).

7.  If a node is assigned a ponded area then a virtual storage unit of
    constant surface area is used along with equation 3-15 to update its
    head when it exceeds the node's maximum value. Otherwise a node's
    head cannot exceed its maximum value and any excess inflow it
    receives is lost from the system (see Section 3.3.7).

## 3.4 Numerical Stability

The numerical stability of SWMM's dynamic wave results can be affected
by the choice of the simulation time step. Numerical instability is
characterized by oscillations in flow and water surface elevation that
do not dampen out over time. Another indicator of numerical instability
is a node which continues to "dry up" on each time-step despite a
constant or increasing inflow from upstream sources.

Aside from examining the results for each conduit and node, SWMM 5
provides two metrics in its Status Report that can help determine if a
solution shows signs of instability. One is the overall flow continuity
error for the system. This is the difference between inflow and outflow
for the entire system over the duration of the simulation. If this
number is greater than 5 to 10 percent then the cause may be numerical
instability (although other factors can affect the continuity error as
well).

A second metric is a link's Flow Instability Index (FII). This index
counts the number of times that the flow value in a link is higher (or
lower) than the flow in both the previous and subsequent time periods.
The index is normalized with respect to the expected number of such
'turns' that would occur for a purely random series of values and can
range from 0 to 150. The Status Report identifies the links having the
five highest FII's. Unfortunately since the FII does not take into
account the magnitude of the flow fluctuations it cannot determine
whether the instability is of engineering significance or not.

Stable explicit solutions of the St. Venant equations require that the
time step be no longer than the time it takes for a dynamic wave to
travel the length of the conduit (Cunge et al., 1980). This is known as
the Courant-Friedrichs-Lewy (CFL) condition and can be expressed as:

  ------------------------------------------------------------------------------------
        $$\mathrm{\Delta}t \leq \frac{L}{\left| \overline{U} + c \right|}$$   (3-30)
  ----- --------------------------------------------------------------------- --------

  ------------------------------------------------------------------------------------

where *c* is the wave celerity given by:

  -------------------------------------------------------------------------
        $$c = \sqrt{g\frac{\overline{A}}{\overline{W}}}$$          (3-31)
  ----- ---------------------------------------------------------- --------

  -------------------------------------------------------------------------

An equivalent form of this condition can be written as:

  ------------------------------------------------------------------------------------------------------------------
        $$\mathrm{\Delta}t \leq \frac{L}{\left| \overline{U} \right|}\left( \frac{Fr}{1 + Fr} \right)Cr$$   (3-32)
  ----- --------------------------------------------------------------------------------------------------- --------

  ------------------------------------------------------------------------------------------------------------------

where *Fr* is the flow's Froude number (see Equation 3-17) and *Cr* is
the Courant number. The latter serves as an adjustment parameter that
determines how conservative (*Cr* \< 1) or liberal (*Cr* \> 1) one
wishes to be in strictly meeting the CFL condition (*Cr* = 1).

Although the SWMM 5 solution method uses an iterative implicit procedure
in time to update flows and heads, it does so one conduit and node at a
time, not simultaneously. There is no spatial coupling between elements
as would occur in an unconditionally stable implicit solution scheme.
Thus the CFL condition would still apply but perhaps not as strictly (by
allowing one to use a *Cr* value greater than 1).

One can estimate a *∆t* for each conduit by using the conduit's full
depth *Y~full~* in place of $\frac{\overline{A}}{\overline{W}}$ in
Equation 3-31 and ignoring the velocity in Equation 3-30. The solution
time step would then be determined by the conduit with the smallest
value of $\frac{L}{\sqrt{gY_{full}}}$ . Short conduits lead to small
time steps and longer computational times. Time steps of 10 to 30
seconds should suffice for conduit lengths of 200 to 400 feet (the
typical spacing between sewer manholes) and full depths from 1 to 4
feet.

An option is available to artificially lengthen short conduits so that
the CFL condition for a given user-supplied time step *∆t* is met. The
modified length $L'$ is given by

  -------------------------------------------------------------------------------------------------------------------------------
        $$L' = max\left\{ L,\ \mathrm{\Delta}t\left( \sqrt{gY_{full}\ } + \frac{Q_{full}}{A_{full}} \right) \right\}$$   (3-33)
  ----- ---------------------------------------------------------------------------------------------------------------- --------

  -------------------------------------------------------------------------------------------------------------------------------

where *Q~full~* is the Manning's normal flow value (Equation 3-23)
evaluated at full depth *Y~full\ ~*and *A~full~* is the flow area at
full depth. This modified length is used in place of the original length
in the equations presented in section 3.4. To make the artificially
lengthened conduit have a flow resistance equivalent to the original
length, its slope *S~0~* and roughness coefficient *n* are adjusted so
that the Manning equation produces an equal head loss across both the
original and lengthened conduit for any given flow. The modified slope
$S_{0}'$ for the lengthened conduit is:

  -------------------------------------------------------------------------
        $$S_{0}' = S_{0}\sqrt{\frac{L}{L'}}$$                      (3-34)
  ----- ---------------------------------------------------------- --------

  -------------------------------------------------------------------------

while its modified roughness $n'$ is:

  -------------------------------------------------------------------------
        $$n' = n\sqrt{\frac{L}{L'}}$$                              (3-35)
  ----- ---------------------------------------------------------- --------

  -------------------------------------------------------------------------

The conduit lengthening option is applied to all conduits whenever the
user supplies a non-zero value for the "lengthening" time step to be
used in equation 3-33. This time step does not have to be the same as
the computational time step used to solve the dynamic wave equations.

Another option available in SWMM 5 is to have the program use a variable
computational time step that is adjusted throughout the simulation. The
user supplies values of the smallest allowable time step (*∆t~min~*),
the largest allowable time step (*∆t~max~*) and a desired Courant number
(*Cr*) to be met. At any time *t*, the next time step is computed from
the smaller of:

1.  The smallest value of

  ------------------------------------------------------------------------------------------
        $$\frac{L}{\left| \overline{U} \right|}\left( \frac{Fr}{1 + Fr} \right)Cr$$   
  ----- ----------------------------------------------------------------------------- ------

  ------------------------------------------------------------------------------------------

> for all conduits with non-negligible Fr.

2.  The smallest value of

  ----------------------------------------------------------------------------------
        $$\frac{0.25\left( H_{crown} - E \right)}{{\mathrm{\Delta}H}^{t}}$$   
  ----- --------------------------------------------------------------------- ------

  ----------------------------------------------------------------------------------

> for all non-outfall nodes that are not surcharged.

The second condition guards against an excessive change in node head
over a single time step. Both conditions are evaluated using the flow
and head solutions found at time *t* (${\mathrm{\Delta}H}^{t}$ is the
change in head found from the prior time step). The resulting time step
is not allowed to be less than *∆t~min~* nor greater than *∆t~max~*. The
initial time step used at time 0 is *∆t~min~*.

To illustrate these concepts consider a 2 ft x 2 ft rectangular conduit
that is 2,000 ft long with a 0.05% slope and has a Manning's roughness
of 0.015 (see Figure 3-5). When divided into 10 equal length sections of
200 ft each the estimated stable time step is
$\frac{200}{\sqrt{32.2 \times 2} = 25}$ seconds. When analyzed as just a
single 2,000 ft long section it increases to 250 seconds.

![Example1a.png](VolumeII/media/media/image16.png){width="6.5in"
height="3.240972222222222in"}

[]{#_Toc484694718 .anchor}**Figure 3‑5 Profile view of example
rectangular conduit (not to scale)**

Figure 3-6 shows the outflow hydrographs for these two analysis options
for a 1-hour sinusoidal inflow hydrograph with peak flow of 10 cfs (the
dotted curve in the figure). Both results are completely stable. The
option with the higher spatial resolution produces a more skewed
hydrograph with a slightly lower peak.

<figure>
<img src="VolumeII/media/media/image17.png"
style="width:5.30407in;height:3.95493in" alt="Example1b.png" />
<figcaption><p><span id="_Toc484694719"
class="anchor"></span><strong>Figure 3‑6 Outflow hydrographs for example
conduit -I</strong></p></figcaption>
</figure>

Now consider what happens when the 10-section conduit is analyzed with a
fixed time step of 120 seconds which is much larger than the stable
fixed step of 25 seconds. As shown in Figure 3-7 the solution becomes
completely unstable. When 120 seconds is used as the upper limit of a
variable time step a stable result is produced. In this case SWMM's
Status Report shows that the variable time step ranged from 24 to 120
seconds with the average being 42.

<figure>
<img src="VolumeII/media/media/image18.png"
style="width:5.30407in;height:3.95493in" alt="Example1c.png" />
<figcaption><p><span id="_Toc484694720"
class="anchor"></span><strong>Figure 3‑7 Outflow hydrographs for example
conduit – II</strong></p></figcaption>
</figure>

# - Kinematic Wave Analysis

\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_

The kinematic wave model is derived from a simplified form of the St.
Venant equations that combines the continuity equation with the uniform
flow equation. It cannot model pressurized flow, reverse flow, or
backwater effects. It is most applicable to steeply sloped conduits
subjected to long duration inflow hydrographs that produce shallow flow
with high velocity (Ponce et al., 1978). For these situations its
results will not be far off from those of dynamic wave analysis and can
be computed much more efficiently using much larger time steps.

Kinematic wave modeling was included in the original release of SWMM in
1971 before a full dynamic wave option was available. The original
method included an enhancement to approximate a backwater effect for
sub-critical flow. SWMM 5 has dropped this enhancement in favor of the
classical kinematic wave formulation since the code now includes a full
dynamic wave option (described in the previous chapter) that rigorously
models backwater effects.

## 4.1 Governing Equations

The kinematic wave model for unsteady flow in a channel or pipe is
derived from the same St. Venant equations for conservation of mass and
momentum that were used for dynamic wave analysis:

  ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------
        $$\frac{\partial A}{\partial t} + \frac{\partial Q}{\partial x} = 0$$                                                                           Continuity      (4-1)
  ----- ----------------------------------------------------------------------------------------------------------------------------------------------- ------------- -------
                                                                                                                                                                      

        $$\frac{\partial Q}{\partial t} + \frac{\partial\left( \frac{Q^{2}}{A} \right)}{\partial x} + gA\frac{\partial H}{\partial x} + gAS_{f} = 0$$   Momentum        (4-2)
  ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------

where all variables were defined previously in Chapter 3. Expressing
head *H* as *Z + Y* (invert elevation plus flow depth) and recognizing
that $\frac{\partial Z}{\partial x = {- S}_{0}}$ (the conduit's slope)
allows one to write the momentum equation as:

  ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
        $$\frac{\partial Q}{\partial t} + \frac{\partial\left( \frac{Q^{2}}{A} \right)}{\partial x} + gA\frac{\partial Y}{\partial x} = gA(S_{0} - S_{f})$$                 (4-3)
  ----- ----------------------------------------------------------------------------------------------------------------------------------------------------- ------------- -------

  ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

If one assumes that the terms on the left hand side of Equation 4-3 are
negligible one is left with the relation:

  -------------------------------------------------------------------------
        $$S_{0} = S_{f}$$                                           (4-4)
  ----- --------------------------------------------- ------------- -------

  -------------------------------------------------------------------------

Having the conduit's bottom slope equal the friction slope implies that
the fluid motion caused by gravity is balanced by the frictional
resistance to flow. Using the Manning equation to represent the friction
slope allows one to represent the relationship between flow rate *Q* and
flow area *A* with Manning's equation for steady uniform flow:

  -------------------------------------------------------------------------
        $$Q = \frac{AR^{2/3}\sqrt{S_{0}}}{\eta}$$                   (4-5)
  ----- --------------------------------------------- ------------- -------

  -------------------------------------------------------------------------

where the hydraulic radius *R* is an implicit function of flow area *A*
for a specific conduit cross-sectional shape. (*R* is defined as area
divided by wetted perimeter where the latter can be determined from flow
depth which can be inferred from the flow area *A*.)

By defining $\beta = \frac{\sqrt{S_{0}}}{\eta}$ and $\Psi = AR^{2/3}$
the Manning equation can be expressed as:

  -------------------------------------------------------------------------
        $$Q = \beta\Psi(A)$$                                        (4-6)
  ----- ----------------------------------------------------------- -------

  -------------------------------------------------------------------------

*Ψ* is known as the section factor (Chow, 1959) and is a function of the
flow area and conduit geometry. For some closed conduit shapes, such as
circular pipes, the section factor achieves a maximum value at a less
than full flow area (as shown in Figure 4-1) resulting in a maximum flow
that is larger than when the conduit flows full.

![[]{#_Toc484694721 .anchor}**Figure 4‑1 Section factor versus area for
a circular
shape**](VolumeII/media/media/image19.emf){width="4.847871828521435in"
height="3.6209416010498687in"}

Thus the governing equations for kinematic wave modeling along a single
conduit are the continuity equation 4-1 along with the "rating curve"
equation 4-6 that relates flow rate to area. The dependent variables in
these equations are flow rate *Q* and flow area *A*, which are functions
of distance *x* and time *t*. To solve these equations over a single
conduit of length *L*, one needs a set of initial conditions for either
*Q* or *A* at time 0 as well as boundary condition for either variable
at *x* = 0 for all times *t*.

## 4.2 Solution Method

The material that follows applies to networks containing only conduits
connected by junction nodes, where there is at most one outflow conduit
from each junction. Inclusion of flow control devices (pumps, orifices,
and weirs) and other processes (seepage and evaporation) will be covered
in subsequent chapters of this manual. Allowances for both flow divider
nodes and storage nodes are discussed later in this chapter in Section
4.3.

The continuity equation 4-1 is solved along a space-time grid depicted
in Figure 4-2. A weighted Wendroff implicit finite difference scheme
(Smith, 1978) is used to re-express the equation as:

  ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
        $$\frac{(1 - \theta)\left( A_{1}^{t + \Delta t} - A_{1}^{t} \right) + \theta\left( A_{2}^{t + \Delta t} - A_{2}^{t} \right)}{\Delta t} + \frac{(1 - \phi)\left( Q_{2}^{t} - Q_{1}^{t} \right) + \phi\left( Q_{2}^{t + \Delta t} - Q_{1}^{t + \Delta t} \right)}{L} = 0$$   (4-7)
  ----- -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- -------

  ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

<figure>
<img src="VolumeII/media/media/image20.png"
style="width:4.4023in;height:3.10907in" alt="Grid.png" />
<figcaption><p><span id="_Toc484694722"
class="anchor"></span><strong>Figure 4‑2 Space-time grid for kinematic
wave analysis</strong></p></figcaption>
</figure>

The subscripts 1 and 2 on *A* and *Q* refer to the upstream and
downstream ends of the conduit, respectively. The superscript refers to
the time period. *θ* and *φ* are weights chosen to be between 0.5 and 1.
At each time step this equation is applied conduit by conduit starting
at the most upstream node and working downstream. The only unknowns will
be $A_{2}^{t + \Delta t}$ and $Q_{2}^{t + \Delta t}$. As there is only
one outflow conduit connected to a junction node, $Q_{1}^{t + \Delta t}$
is known from the sum of the already computed $Q_{2}^{t + \Delta t}$
values of the upstream conduits that flow into the conduit being
analyzed, and also includes any externally imposed inflows such as wet
weather runoff or dry weather sanitary flow. The area
$A_{1}^{t + \Delta t}$ associated with this flow can be found by
evaluating the inverse of the section factor at the value of
$\frac{Q_{1}^{t + \Delta t}}{\beta}$.

The Manning equation 4-6 can be substituted into Equation 4-7 which
after some rearrangement results in the following nonlinear equation
with single unknown $A_{2}^{t + \Delta t}$:

  -----------------------------------------------------------------------------------------------------------------------------------------
        $$f\left( A_{2}^{t + \Delta t} \right) = \beta\Psi\left( A_{2}^{t + \Delta t} \right) + C1A_{2}^{t + \Delta t} + C2 = 0$$   (4-8)
  ----- --------------------------------------------------------------------------------------------------------------------------- -------

  -----------------------------------------------------------------------------------------------------------------------------------------

where the constants C1 and C2 are given by:

  --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
        $$C1 = \frac{L\theta}{\Delta t\phi}$$                                                                                                                                                                                            (4-9)
  ----- ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- --------
        $$C2 = \frac{L}{\Delta t\phi}\left\lbrack (1 - \theta)\left( A_{1}^{t + \Delta t} - A_{1}^{t} \right) - \theta A_{2}^{t} \right\rbrack + \frac{1 - \phi}{\phi}\left( Q_{2}^{t} - Q_{1}^{t} \right) - Q_{1}^{t + \Delta t}$$     (4-10)

  --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

After solving 4-8 for $A_{2}^{t + \Delta t}$ equation 4-6 can then be
used to find the corresponding flow $Q_{2}^{t + \Delta t}$.

Equation 4-8 is solved using a combination of bisection and
Newton-Raphson methods (Press et al., 1992) with both *θ* and *φ* set to
0.6. As a first step, a bracket
$\left\lbrack A_{LOW},\ A_{HIGH} \right\rbrack$ is sought where
$f(A_{LOW})$ and $f(A_{HIGH})$ are of opposite sign. For conduit shapes
whose section factor has a maximum value at an area *A~max~* below
*A~full~* (such as the circular conduit of Figure 4-1), these two areas
are tried first. If these areas do not form a valid bracket then *0* to
*A~max~* is used. For shapes whose section factor always increases with
increasing area, *0* to *A~full~* is used.

If a valid bracket is found then the procedure described in Appendix A,
"*Newton-Raphson-Bisection Root Finding Method*", is used to find
$A_{2}^{t + \Delta t}$. Its initial estimate is $A_{2}^{t}$ and a
convergence tolerance *ε* of 0.1 percent of *A~full~* is used. The
derivative of *f(A)* required by the method is
$f'(A) = \beta\Psi'(A) + C1$ where $\Psi'(A)$ is the derivative of the
section factor with respect to area *A*. If
$\left\lbrack A_{LOW},\ A_{HIGH} \right\rbrack$ does not form a valid
bracket then $A_{2}^{t + \Delta t}$ is set to 0 if both $f(A_{LOW})$ and
$f(A_{HIGH})$ are positive and set to $A_{full}$ if both are negative.

## 4.3 Computational Details

### 4.3.1 Order of Network Traversal

The kinematic wave procedure finds new flows, flow areas, and flow
depths at the downstream end of each conduit at the end of each time
period of the analysis. At time *t* each conduit is examined in its
topologically sorted order when updating it to time *t + ∆t*. This
allows the inflows to a conduit at *t + ∆t*
$\left( Q_{1}^{t + \mathrm{\Delta}t} \right)$ to be determined from the
previously computed downstream outflows
$\left( Q_{2}^{t + \mathrm{\Delta}t} \right)$ of the conduits that flow
into it.

The topological sort is performed just once, prior to time 0, using
Kahn's algorithm (Cormen et al, 2009) after all links have been oriented
in the direction of positive slope (meaning that the inflow end is at
higher elevation than the outflow end). Nodes with more than a single
outflow link (such as divider and storage nodes) will have those links
appearing consecutively in the sorted list. If the sorting algorithm
detects that a loop exists, then the program is terminated with an error
condition reported.

### 4.3.2 Cross Section Properties

The kinematic wave solution requires calculating a cross section's
section factor $\left( AR^{2/3} \right)$, the section factor's
derivative, and the depth of flow, all as a function of a conduit's flow
area. Flow depth is used only for reporting purposes. Chapter 5 provides
details on how these quantities are calculated for different conduit
shapes.

Also required is the inverse section factor - the area associated with a
particular section factor. It is used to find the upstream area for a
given upstream flow rate (that is, *A* for $\Psi = \frac{Q}{\beta}$).
Its calculation is also described in Chapter 5, in Section 5.1.8.

### 4.3.3 Flow Divider Nodes

A flow divider node splits its inflow between two outlet conduits in a
prescribed manner. It is only active for kinematic wave analysis and is
treated as a regular junction node under dynamic wave analysis.

There are four types of flow dividers available, defined by the manner
in which inflows are diverted:

  -----------------------------------------------------------------------
  *Cutoff          diverts all inflow above a user-supplied cutoff value
  Divider:*        *q~MIN~*.
  ---------------- ------------------------------------------------------
  *Overflow        diverts all inflow above the flow capacity
  Divider:*        $Q_{full} = \beta\Psi(A_{full})$ of the non-diversion
                   conduit

  *Tabular         uses a pre-defined table that expresses diverted flow
  Divider:*        as a function of total inflow.

  *Weir Divider:*  diverts inflow above a minimum *q~MIN~* as flow over a
                   weir of full height *h~W~* with discharge coefficient
                   *c~W~*.
  -----------------------------------------------------------------------

The diverted flow for a weir divider node with total inflow of *Q~in~*
is computed as:

+---+------------------------------------+-------------------------------------------+--------+
|   | $$Q_{div} = \left\{ \begin{matrix} | $$for\ Q_{in} \leq q_{MIN}$$              | (4-11) |
|   | 0 \\                               |                                           |        |
|   | q_{MAX}f^{1.5} \\                  | $$for\ q_{MIN} < Q_{in} \leq q_{MAX}$$    |        |
|   | Q_{MAX}\sqrt{f}                    |                                           |        |
|   | \end{matrix} \right.\ \ \ $$       | $$for\ Q_{in} > q_{MAX}$$                 |        |
+===+====================================+===========================================+========+

where $q_{MAX} = c_{W}h_{W}^{1.5}$ and
$f = \frac{\left( Q_{in} - q_{MIN} \right)}{\left( q_{MAX} - q_{MIN} \right)}$.

When the next conduit in sorted order is selected for routing analysis,
its upstream node is checked to see if it is a divider node. If it is,
then depending on its type, the diverted flow *Q~div~* is calculated
from the node's total inflow *Q~in~*. If the conduit is the node's
diversion link, then its inflow $Q_{1}^{t + \mathrm{\Delta}t}$ is set
equal to *Q~div~*. Otherwise its inflow is set to $Q_{in} - Q_{div}$.

### 4.3.4 Storage Nodes

Kinematic wave analysis allows a storage node to have more than one
outlet link of any type connected to it. The flow rate released from the
storage node into the upstream end of an outflow link will be a function
of the water level in the node. Thus whenever a storage node is
encountered as the topologically sorted list of conduits is traversed
its new water level must be determined before the routing process can
continue. Storage units that are terminal nodes (nodes with no outflow
links) are updated after all conduits have been analyzed.

Storage node updating is carried out using the following mass balance
equation:

  --------------------------------------------------------------------------
        $$\frac{dV_{N}}{dt} = Q_{in} - Q_{out}$$                    (4-12)
  ----- ----------------------------------------------------------- --------

  --------------------------------------------------------------------------

*V~N~* is the volume of stored water in the node, *Q~in~* is the total
rate of inflow to the node and *Q~out~* is the total rate of outflow
from the node. Replacing $\frac{dV_{N}}{dt}$ with its finite difference
equivalent and using the average flow rates over the time step being
updated produces the following expression for
$V_{N}^{t + \mathrm{\Delta}t}$:

  -----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
        $$V_{N}^{t + \mathrm{\Delta}t} = V_{N}^{t} + 0.5\left( Q_{in}^{t} + Q_{in}^{t + \mathrm{\Delta}t} \right)\mathrm{\Delta}t - 0.5\left( Q_{out}^{t} + Q_{out}^{t + \mathrm{\Delta}t} \right)\mathrm{\Delta}t$$   (4-13)
  ----- -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- --------

  -----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

Once $V_{N}^{t + \mathrm{\Delta}t}$ is known the corresponding water
surface elevation *H* can be found from the storage node's invert
elevation and its user-supplied relation between surface area and water
depth. A more detailed discussion of how this is done is provided in
Chapter 5.

Equation 4-13 can be re-written with all of the known values grouped
together in a constant *C~N~* as follows:

  ---------------------------------------------------------------------------------------------
        $$V_{N}^{t + \mathrm{\Delta}t} = C_{N} - 0.5Q_{out}^{t + \mathrm{\Delta}t}$$   (4-14)
  ----- ------------------------------------------------------------------------------ --------

  ---------------------------------------------------------------------------------------------

where *C~N~* is

  -----------------------------------------------------------------------------------------------------------------------------------
        $$C_{N} = V_{N}^{t} + 0.5\left( Q_{in}^{t} - Q_{out}^{t} + Q_{in}^{t + \mathrm{\Delta}t} \right)\mathrm{\Delta}t$$   (4-15)
  ----- -------------------------------------------------------------------------------------------------------------------- --------

  -----------------------------------------------------------------------------------------------------------------------------------

and contains the known volumes and flows from time *t* as well as the
known inflow to the storage node at time *t + ∆t*.

$Q_{out}^{t + \mathrm{\Delta}t}$ will be a function of the storage
unit's water surface elevation *H*. For a conduit outflow link, the flow
at its upstream end that contributes to *Q~out~* will be determined by
its upstream flow area via Equation 4-6: $Q_{1} = \beta\Psi(A_{1})$. The
upstream flow area *A~1~* is determined by the conduit's upstream water
depth where it meets the storage node. This depth, *Y~1~*, is given by:

+----+----------------------------------+----------------------------------------------+--------+
|    | $$Y_{1} = \left\{ \begin{matrix} | $$for\ H \leq Z_{1}$$                        | (4-16) |
|    | 0 \\                             |                                              |        |
|    | H - Z_{1} \\                     | $$for\ Z_{1} < H \leq Z_{1} + Y_{full}$$     |        |
|    | Y_{full}                         |                                              |        |
|    | \end{matrix} \right.\ $$         | $$for\ H > {Z_{1} + Y}_{full}$$              |        |
+====+==================================+==============================================+========+

where *Z~1~* is the elevation of the conduit's upstream invert and
*Y~full~* is the conduit's full depth. A similar situation exists for
other types of outflow links, such as pumps, orifices, and weirs as will
be discussed later in Chapter 6. As an example, the flow through an
orifice varies as the square root of the head across it:
$Q_{1} = c\sqrt{H - Z_{1}}$ where *c* is a constant.

As both $Q_{out}^{t + \mathrm{\Delta}t}$ and
$V_{N}^{t + \mathrm{\Delta}t}$ depend on *H,* equation 4-14 must be
solved in implicit fashion using successive approximations. The details
are given in the side bar entitled "*Updating a Storage Node*".

The hydraulic head at storage nodes is updated one more time after all
link flows at the end of a time step of size *∆t* have been found. First
a new volume for the node is found from:

  ----------------------------------------------------------------------------------------------------
        $$V_{N}^{t + \mathrm{\Delta}t} = V_{N}^{t} + {\overline{Q}}_{net}\mathrm{\Delta}t$$   (4-17)
  ----- ------------------------------------------------------------------------------------- --------

  ----------------------------------------------------------------------------------------------------

${\overline{Q}}_{net}$ is the average net inflow to the node between
times *t* and *t + ∆t* :

  ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------
        $${\overline{Q}}_{net} = \ 0.5\left( Q_{in}^{t} + Q_{in}^{t + \mathrm{\Delta}t} \right) - 0.5\left( Q_{out}^{t} + Q_{out}^{t + \mathrm{\Delta}t} \right)$$   (4-18)
  ----- ------------------------------------------------------------------------------------------------------------------------------------------------------------ --------

  ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------

with *Q~in~* being the total inflow entering the node from all upstream
links plus any external sources (such as runoff flow) and *Q~out~* being
the total flow rate in the links leaving the node. Then the new head at
the node, $H^{t + \mathrm{\Delta}t}$, can be found from the node's curve
of surface area versus depth as described in Chapter 5.

### 4.3.5 Nodal Heads

Kinematic wave analysis does not depend on or even define the hydraulic
head that exists at nodes that are not storage units. To make its
reported output compatible with that provided by dynamic wave analysis
the head at a non-storage node is arbitrarily set equal to the highest
water elevation in the links that are connected to it. For inflowing
conduits the water surface elevation at the downstream end of the
conduit, as derived from the downstream area, is considered. For out
flowing conduits it would be the water surface elevation at the upstream
end. It should be noted that kinematic wave analysis ignores the
presence of any offset that an outflow conduit at a non-storage node has
at its upstream end. Under dynamic wave analysis there would be no flow
into such a conduit until the water level at the node reached the offset
elevation. The kinematic wave model also ignores any surcharge depth
that may have been assigned to a node since neither conduits nor nodes
are allowed to pressurize.

### 4.3.6 Flooding and Ponding

Normally any excess inflow to a node under kinematic wave analysis over
what the outflow links can handle will be lost from the system. For
non-storage, non-terminal nodes the flooded overflow rate at time *t +
∆t* would be:

  --------------------------------------------------------------------------------------
        $$Q_{ovfl}^{t + \mathrm{\Delta}t} = \ max(0,\ {\overline{Q}}_{net})$$   (4-19)
  ----- ----------------------------------------------------------------------- --------

  --------------------------------------------------------------------------------------

where ${\overline{Q}}_{net}$ is given by Equation 4-18. For storage
nodes it would be:

  -----------------------------------------------------------------------------------------------------------------------------------------------------------------------
        $$Q_{ovfl}^{t + \mathrm{\Delta}t} = \ max\left( 0,\ \ \ {\overline{Q}}_{net} - \frac{\left( V_{Nfull} - V_{N}^{t} \right)}{\mathrm{\Delta}t} \right)$$   (4-20)
  ----- -------------------------------------------------------------------------------------------------------------------------------------------------------- --------

  -----------------------------------------------------------------------------------------------------------------------------------------------------------------------

where $V_{Nfull}$ is the volume of the storage node when full. When
*Q~ovfl~* is non-zero, the head at the node is set equal to its
elevation at full depth for reporting purposes.

As with dynamic wave analysis, the option exists for a junction or
divider node to have the excess inflow volume over a time step be stored
at the node and then released as external inflow during the next time
step. The node's "ponded area" parameter is used to indicate that
ponding is allowed if it is assigned a non-zero value (and does not
enter into any computations). In this case, the node's ponded volume
*V~P~* is kept track of as the simulation unfolds. Its initial value is
0. At time *t + ∆t* it is updated as follows:

  --------------------------------------------------------------------------------------------------------------------------------
        $$V_{P}^{t + \mathrm{\Delta}t} = \ max\left( 0,\ \ \ V_{P}^{t} + {\overline{Q}}_{net}\mathrm{\Delta}t \right)$$   (4-21)
  ----- ----------------------------------------------------------------------------------------------------------------- --------

  --------------------------------------------------------------------------------------------------------------------------------

The overflow reported for the time period is given by:

  -------------------------------------------------------------------------------------------------------------------------------------------------------------------
        $$Q_{ovfl}^{t + \mathrm{\Delta}t} = \ max\left( 0,\ \ \ \frac{\left( V_{P}^{t + \mathrm{\Delta}t} - V_{P}^{t} \right)}{\mathrm{\Delta}t} \right)$$   (4-22)
  ----- ---------------------------------------------------------------------------------------------------------------------------------------------------- --------

  -------------------------------------------------------------------------------------------------------------------------------------------------------------------

The flow added to the node's total inflow at the start of the next time
period is $\frac{V_{P}^{t + \mathrm{\Delta}t}}{\mathrm{\Delta}t}$. And
for reporting purposes, anytime *V~P~* is greater than 0 the node's head
is set equal to the elevation at full depth.

## 4.4 Numerical Stability

The authors of the original version of SWMM's kinematic wave routine
applied the techniques of O'Brien et al. (1951) to show that the method
was unconditionally stable for any choice of *θ* and *φ* both greater
than 0.5 ( Metcalf and Eddy et al., 1971a). Smith (1978, p. 188) showed
that the Wendroff implicit scheme using centered differences (*θ* = *φ =
0.5*) was also unconditionally stable. Because the scheme is stable it
does not employ the variable time step option as does dynamic wave
analysis. And although it is stable, it is still subject to numerical
dispersion when the Courant number differs from 1 and to numerical
diffusion (hydrograph attenuation) due to the discrete grid size (Ponce,
1991).

To illustrate the stability properties of SWMM's kinematic wave method
the example of Chapter 3 solved earlier under dynamic wave analysis will
now be solved again using the kinematic wave model. The example consists
of a 2,000 ft long, 2 ft x 2 ft rectangular conduit whose slope is 0.05
percent and Manning's roughness is 0.015. When divided into 10 equal
sub-conduits of 200 ft each the dynamic wave solution required a 25
second time step to produce a stable outflow hydrograph (refer to Figure
3-6). At a 120 second time step the solution was highly unstable (see
Figure 3-7).

As shown in Figure 4-3, kinematic wave (KW) is able to produce a stable
outflow hydrograph for the 120 second time step. A stable dynamic wave
(DW) solution required a much smaller time step of 25 seconds. The KW
solution exhibits the properties associated with this approximate method
of flow routing, with a very modest attenuation of the inflow hydrograph
peak and some distortion of the outflow hydrograph. The DW solution
should be considered the more accurate one, with its greater reduction
in peak flow due to the storage effect provided by the additional
inertia and pressure terms included in the dynamic wave formulation.

<figure>
<img src="VolumeII/media/media/image21.png"
style="width:6.13679in;height:4.54282in" alt="Example1d.png" />
<figcaption><p><span id="_Toc484694723"
class="anchor"></span><strong>Figure 4‑3 Outflow hydrograph for example
conduit</strong></p></figcaption>
</figure>

# - Cross-Section Geometry

\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_

The hydraulic modeling procedures described in chapters 3 and 4 require
calculation of several cross-section geometric properties for partially
full conduits. These include the following functions:

  --------------------------------------------------------------------------
  *A(Y)*    flow area *A* as a function of flow depth *Y*
  --------- ----------------------------------------------------------------
  *W(Y)*    top width *W* as a function of flow depth *Y*

  *R(Y)*    hydraulic radius *R* as a function of flow depth *Y*

  *Y(A)*    flow depth *Y* as a function of flow area *A*

  *Ψ(A)*    section factor *Ψ* as a function of flow area *A*

  *Ψ'(A)*   derivative of section factor *Ψ* with respect to area *A*

  *A(Ψ)*    flow area *A* as a function of section factor *Ψ*
  --------------------------------------------------------------------------

as well as the following constants used in evaluating these functions:

  ---------------------------------------------------------------------------
  *A~full~*   area at full depth
  ----------- ---------------------------------------------------------------
  *W~max~*    maximum width

  *R~full~*   hydraulic radius at full depth

  *Ψ~full~*   section factor at full depth

  *Ψ~max~*    maximum section factor

  *A~max~*    area corresponding to *Ψ~max~*.
  ---------------------------------------------------------------------------

This chapter describes how these properties are computed for the wide
range of conduit shapes, both standard and irregular, included in SWMM.
In addition, the procedures used to compute both the normal and critical
flow depths used in dynamic wave analysis are discussed.

## 5.1 Standard Conduit Shapes

SWMM recognizes a number of standard pre-defined conduit shapes. These
include five open channel shapes (rectangular, trapezoidal, triangular,
parabolic and power law), four commonly used closed pipe shapes
(circular, rectangular, ellipsoid and arch), seven closed shapes found
mainly in older masonry sewers, and four closed composite shapes that
are combinations of rectangular, triangular and circular sections.

### 5.1.1 Open Channel Shapes

SWMM can analyze the following standard open channel shapes:

- Rectangular with bottom width *b*

- Trapezoidal with bottom width *b* and side slope (run over rise) *s*

- Triangular with side slope *s*

- Parabolic with top width *b* at full depth *Y~full~*.

Table 5-1 lists the formulas used to compute the geometric properties of
these shapes that are functions of water depth *Y*: A(Y), *W(Y),* and
*R(Y).* Table 5-2 lists the formulas used to compute the properties that
are functions of flow area *A*: *Y(A), R(A),* and the derivative of the
wetted perimeter *P'(A)*). The latter quantity is used for computing the
derivative of the section factor as described below.

+------------------------------------------------+---------------------------+----------------+--------------------------------------------------+
| **Shape**                                      | ***A(Y)***                | ***W(Y)***     | ***R(Y)***                                       |
+:===============================================+:=========================:+:==============:+:================================================:+
| Rectangular                                    | $$bY$$                    | $$b$$          | $$\frac{bY}{b + 2Y}$$                            |
+------------------------------------------------+---------------------------+----------------+--------------------------------------------------+
| Trapezoidal                                    | $$(b + sY)Y$$             | $$b + 2sY$$    | $$\frac{(b + zY)Y}{b + 2Y\sqrt{1 + s^{2}}}$$     |
+------------------------------------------------+---------------------------+----------------+--------------------------------------------------+
| Triangular                                     | $$sY^{2}$$                | $$2sY$$        | $$\frac{sY}{2\sqrt{1 + s^{2}}}$$                 |
+------------------------------------------------+---------------------------+----------------+--------------------------------------------------+
| Parabolic                                      | $$\frac{4}{3}Y\sqrt{cY}$$ | $$2\sqrt{cY}$$ | $$\frac{2A(Y)}{c\left( xt + ln(x + t) \right)}$$ |
|                                                |                           |                |                                                  |
| $$c = \frac{b^{2}}{\left( 4Y_{full} \right)}$$ |                           |                | $$x = 2\sqrt{\frac{Y}{c}}$$                      |
|                                                |                           |                |                                                  |
|                                                |                           |                | $$t = \sqrt{1 + x^{2}}$$                         |
+------------------------------------------------+---------------------------+----------------+--------------------------------------------------+

: []{#_Toc484690419 .anchor}**Table 5‑1 Geometric properties for open
channel shapes as functions of water depth**

+------------------------------------------------+-----------------------------------------------+----------------------------------------+-------------------------------------------+
| **Shape**                                      | ***Y(A)***                                    | ***R(A)***                             | ***P'(A)***                               |
+:===============================================+:=============================================:+:======================================:+:=========================================:+
| Rectangular                                    | $$\frac{A}{b}$$                               | $$\frac{A}{b + 2\frac{A}{b}}$$         | $$\frac{2}{b}$$                           |
+------------------------------------------------+-----------------------------------------------+----------------------------------------+-------------------------------------------+
| Trapezoidal                                    | $$\frac{\sqrt{b^{2} + 4sA}}{2s}$$             | $$\frac{A\sqrt{1 + s^{2}}}{b + Y(A)}$$ | $$\frac{2\sqrt{1 + s^{2}}}{b^{2} + 4sA}$$ |
+------------------------------------------------+-----------------------------------------------+----------------------------------------+-------------------------------------------+
| Triangular                                     | $$\sqrt{\frac{A}{s}}$$                        | $$\frac{A}{2Y(A)\sqrt{1 + s^{2}}}$$    | $$\frac{\sqrt{1 + s^{2}}}{sA}$$           |
+------------------------------------------------+-----------------------------------------------+----------------------------------------+-------------------------------------------+
| Parabolic                                      | $$\left( \frac{3A}{4\sqrt{c}} \right)^{2/3}$$ | $$2c\left( xt + ln(x + t) \right)$$    | not used                                  |
|                                                |                                               |                                        |                                           |
| $$c = \frac{b^{2}}{\left( 4Y_{full} \right)}$$ |                                               | $$x = 2\sqrt{\frac{Y(A)}{c}}$$         |                                           |
|                                                |                                               |                                        |                                           |
|                                                |                                               | $$t = \sqrt{1 + x^{2}}$$               |                                           |
+------------------------------------------------+-----------------------------------------------+----------------------------------------+-------------------------------------------+

: []{#_Toc484690420 .anchor}**Table 5‑2 Geometric properties for open
channel shapes as functions of flow area**

The section factor *Ψ* for each of these shapes is given by:

  -------------------------------------------------------------------------
        $$\Psi(A) = A{R(A)}^{2/3}$$                                 (5-1)
  ----- ----------------------------------------------------------- -------

  -------------------------------------------------------------------------

With the exception of the parabolic shape, its derivative with respect
to area *A* is:

  -------------------------------------------------------------------------
        $$\Psi'(A) = (\frac{5}{3} - \frac{2}{3}P'R)R^{2/3}$$        (5-2)
  ----- ----------------------------------------------------------- -------

  -------------------------------------------------------------------------

where *P'* and *R* are evaluated at the desired value of *A*. For
parabolic channels the section factor derivative is computed using the
difference formula:

  ----------------------------------------------------------------------------------------
        $$\Psi'(A) = \frac{\Psi(A + \Delta A) - \Psi(A - \Delta A)}{2\Delta A}$$   (5-3)
  ----- -------------------------------------------------------------------------- -------

  ----------------------------------------------------------------------------------------

where *∆A* is 0.1% of the full cross section area.

In addition to the four open sections just described SWMM can also
analyze a cross section whose side wall shape is described by the power
law function:

  -------------------------------------------------------------------------
        $$y = \alpha x^{\frac{1}{\gamma}}$$                         (5-4)
  ----- ----------------------------------------------------------- -------

  -------------------------------------------------------------------------

where *x* is horizontal distance from the centerline, *y* is vertical
distance, 1/γ is an exponent and α is a constant. To use this shape the
user supplies values for 1/γ, the full depth *Y~full~* and the top width
when full *b* (see Figure 5-1). Note that the parabolic shape is a
special case of this power function shape where 1/γ equals 2.

<figure>
<img src="VolumeII/media/media/image22.png"
style="width:3.46565in;height:2.49014in" alt="PowerFunc.png" />
<figcaption><p><span id="_Toc484694724"
class="anchor"></span><strong>Figure 5‑1 Power law cross section
shape</strong></p></figcaption>
</figure>

With this shape it is more convenient to work with water surface width
*W* as a function of water depth *Y*, which can be done by re-expressing
Equation 5-4 as:

  -------------------------------------------------------------------------
        $$W = cY^{\gamma}$$                                         (5-5)
  ----- ----------------------------------------------------------- -------

  -------------------------------------------------------------------------

where *c* is another constant. Since *W* = *b* at *Y* = *Y~full~*, the
constant *c* equals $\frac{b}{Y_{full}^{\gamma}}$ . The full area
*A~full~* is $\frac{bY_{full}}{(\gamma + 1)}$. Table 5-3 lists the
expressions used to compute the geometric properties for partially full
power law shapes. The wetted perimeter *P* table entry is evaluated by
approximating each of the curved sides of the shape by a series of 50
line segments whose lengths up to height *Y* are added together.

### 5.1.2 Closed Rectangular Shape

A closed (or covered) rectangular conduit has the same *A(Y)*, *W(Y)*,
and *Y(A)* functions as its open counterpart. Its *R(Y)* and *Ψ(A)*
functions are also the same up to the point where the conduit becomes
full and the wetted perimeter then includes the top width. This
introduces a discontinuity in the relationship between *R* and *Y* as
well as between *Ψ* and *A*. To avoid this, a maximum section factor is
deemed to occur at 97% full after which it decreases linearly to the
section factor when completely full. These two section factors are given
by:

+--------------+---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------+
| **Property** | **Expression**                                                                                                                                                                                                |
+:=============+===============================================================================================================================================================================================================+
| *c*          | $$\frac{b}{Y_{full}^{\gamma}}$$                                                                                                                                                                               |
+--------------+---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------+
| *A(Y)*       | $$c\frac{Y^{\gamma + 1}}{(\gamma + 1)}$$                                                                                                                                                                      |
+--------------+---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------+
| *W(Y)*       | $$cY^{\gamma}$$                                                                                                                                                                                               |
+--------------+---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------+
| *P(Y)*       | $$2\sum_{i = 1}^{N}\sqrt{{\Delta x}_{i}^{2} + \Delta y^{2}}\ \ where\ \ \Delta y = 0.02Y_{full},\ \ N = \frac{Y}{\Delta y},$$                                                                                 |
|              |                                                                                                                                                                                                               |
|              | $$\ \ \ \ \ \ \ \ \ \ \ \ \ \ \ \ \ \ \ \ \ \ \ \ \ \ \ \ \ \ \ \ \ \ \ \ \ and\ {\Delta x}_{i} = \left( \frac{c}{2} \right)\left\{ (i\Delta y)^{\gamma} - \left( (i - 1)\Delta y \right)^{\gamma} \right\}$$ |
+--------------+---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------+
| *R(Y)*       | $$\frac{A(Y)}{P(Y)}$$                                                                                                                                                                                         |
+--------------+---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------+
| *Y(A)*       | $$\left\lbrack \frac{(\gamma + 1)A}{c} \right\rbrack^{\frac{1}{(\gamma + 1)}}$$                                                                                                                               |
+--------------+---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------+
| *R(A)*       | $$\frac{A}{P(Y(A))}$$                                                                                                                                                                                         |
+--------------+---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------+
| *Ψ(A)*       | $$A{R(A)}^{\frac{2}{3}}$$                                                                                                                                                                                     |
+--------------+---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------+
| *Ψ'(A)*      | $$\frac{\Psi(A + \Delta A) - \Psi(A - \Delta A)}{2\Delta A}\ \ where\ \Delta A = 0.001A_{full}$$                                                                                                              |
+--------------+---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------+

: []{#_Toc484690421 .anchor}**Table 5‑3 Geometric properties for the
power law shape**

  ----------------------------------------------------------------------------------------------------
        $$\Psi_{full} = A_{full}\left( \frac{A_{full}}{P_{full}} \right)^{2/3}$$                 (5-6)
  ----- -------------------------------------------------------------------------------------- -------
        $$\Psi_{\max} = {0.97A}_{full}\left( \frac{{0.97A}_{full}}{P_{\max}} \right)^{2/3}$$     (5-7)

  ----------------------------------------------------------------------------------------------------

where $A_{full} = bY_{full}$, $P_{full} = 2\left( b + Y_{full} \right)$,
and $P_{\max} = b + 2\left( 0.97Y_{full} \right)$.

When either *Y* or *A* do not exceed 97% of their full values, the
closed rectangular hydraulic radius and section factor are computed in
the same fashion as for the open rectangular shape described in section
5.2.1. Above this point the hydraulic radius at a given depth *Y* is:

  -------------------------------------------------------------------------
        $$R(Y) = \frac{A(Y)}{P(Y)}$$                                (5-8)
  ----- ----------------------------------------------------------- -------

  -------------------------------------------------------------------------

where

  ------------------------------------------------------------------------------------------
        $$P(Y) = 2Y + b + b\frac{\left( \frac{Y}{Y_{full} - 0.97} \right)}{0.03}$$   (5-9)
  ----- ---------------------------------------------------------------------------- -------

  ------------------------------------------------------------------------------------------

and the section factor and its derivative at a given flow area *A* are:

  ------------------------------------------------------------------------------------------------------------------------------------------
        $$\Psi(A) = \Psi_{\max} - \frac{\left( \Psi_{\max} - \Psi_{full} \right)\left( \frac{A}{A_{full} - 0.97} \right)}{0.03}$$     (5-10)
  ----- --------------------------------------------------------------------------------------------------------------------------- --------
        $$\Psi'(A) = \frac{\left( \Psi_{full} - \Psi_{\max} \right)}{\left( 0.03A_{full} \right)}$$                                   (5-11)

  ------------------------------------------------------------------------------------------------------------------------------------------

### 5.1.3 Circular Shape

Although analytical formulas are available for the properties of partly
full circular cross sections (see French, 1985), they contain
trigonometric functions that are time consuming to compute. Thus for
reasons of efficiency SWMM uses a set of lookup tables that are based on
those published by Chow (1959). The tables consist of the following:

  -----------------------------------------------------------------------------
  *A~tbl\ :~*   *A/A~full\ ~* as a function of *Y/Y~full~*
  ------------- ---------------------------------------------------------------
  *W~tbl\ :~*   *W/W~max~* as a function of *Y/Y~full~*

  *R~tbl\ :~*   *R/R~full~* as a function of *Y/Y~full~*

  *Y~tbl\ :~*   *Y/Y~full~* as a function of *A/A~full~*

  *Ψ~tbl\ :~*   *Ψ/Ψ~full~* as a function of *A/A~full~*
  -----------------------------------------------------------------------------

Each table consists of 51 equally spaced values of *Y/Y~full~* or
*A/A~full~* between 0 and 1. They are graphed in Figures 5-2 and 5-3 and
are listed in Appendix C. The normalizing factors used in the tables are
for full flow conditions $\left( Y = Y_{full} \right)$ whose formulas
are listed in Table 5-4.

  -----------------------------------------------------------------------
  **Property**             **Value**
  ------------------------ ----------------------------------------------
  Depth                    $$Y_{full}$$

  Area                     $A_{full} = 0.7854Y_{full}^{2}$,

  Maximum Width            $$W_{\max} = Y_{full}$$

  Hydraulic Radius         $$R_{full} = 0.25Y_{full}$$

  Section Factor           $$\Psi_{full} = A_{full}R_{full}^{2/3}$$
  -----------------------------------------------------------------------

  : []{#_Toc484690422 .anchor}**Table 5‑4 Geometric properties of a full
  circular cross section**

<figure>
<img src="VolumeII/media/media/image23.png"
style="width:4.52388in;height:3.94197in" alt="Circular1.png" />
<figcaption><p><span id="_Toc484694725"
class="anchor"></span><strong>Figure 5‑2 Geometric properties of a
partly filled circular shape based on depth</strong></p></figcaption>
</figure>

<figure>
<img src="VolumeII/media/media/image24.png"
style="width:4.60361in;height:4.01145in" alt="Circular2.png" />
<figcaption><p><span id="_Toc484694726"
class="anchor"></span><strong>Figure 5‑3 Geometric properties of a
partly filled circular shape based on area</strong></p></figcaption>
</figure>

To find *A*, *W*, or *R* for a given *Y* one first evaluates
$i = \left( \frac{Y}{Y_{full}} \right)(N - 1)$ rounded down to the
nearest integer value where *N* = 51, linearly interpolates the
appropriate table between the entries at index *i* and *i+1*, and then
multiplies by the appropriate normalizing factor (either *A~full~*,
*Y~full~*, or *R~full~*). A similar procedure is used to evaluate *Y* or
*Ψ* as a function of *A* normalized by *A~full~*. The section factor
derivative is determined directly from the *Ψ~tbl\ ~* as follows:

  ------------------------------------------------------------------------------------------------------------------------------------------------------------
        $$\Psi'(A) = \left( \Psi_{tbl}\lbrack i + 1\rbrack - \Psi_{tbl}\lbrack i\rbrack \right)(N - 1)\left( \frac{\Psi_{full}}{A_{full}} \right)$$   (5-12)
  ----- --------------------------------------------------------------------------------------------------------------------------------------------- --------

  ------------------------------------------------------------------------------------------------------------------------------------------------------------

where *i* is the integer value of
$\left( \frac{A}{A_{full}} \right)(N - 1)$ for *N* = 51. For added
accuracy, analytical functions are used to compute *Y,* *Ψ,* and *Ψ'*
for areas below 4% of *A~full~*. They are described in the side bar
entitled "*Analytical Functions for Circular Cross Sections*".

### 5.1.4 Ellipsoid and Arch Shapes

Figure 5-4 depicts standard ellipsoid and arch sewer pipe cross
sectional shapes. Next to circular pipes these are the most commonly
used shapes for newly installed sewers and culverts. Each shape is
defined by its "rise" which is its full depth *Y~full~*, and its "span"
which is its maximum width *W~max~*. The vertical and horizontal
ellipsoids have the same shape but rotated by 90 degrees (the span of
one is the rise of the other and vice versa).

<figure>
<img src="VolumeII/media/media/image25.png"
style="width:6.5in;height:2.27014in" alt="EllipseArch.png" />
<figcaption><p><span id="_Toc484694727"
class="anchor"></span><strong>Figure 5‑4 Ellipsoid and arch pipe cross
sectional shapes</strong></p></figcaption>
</figure>

SWMM contains a list of 23 standard ellipsoid pipe sizes and 102
standard arch pipe sizes taken from the American Concrete Pipe
Association's and the American Iron and Steel Institute's design manuals
(American Concrete Pipe Association, 2011; American Iron and Steel
Institute, 1999). The standard ellipsoid and arch pipe sizes are
tabulated in Appendixes D and E, respectively. Each size is
characterized by its rise, span, full area, and full hydraulic radius.
Users can either select from one of these standard sizes or supply their
own values for rise *Y~full~* and span *W~max~*, both in feet. In the
latter case the corresponding full area *A~full~* and hydraulic radius
*R~full~* are estimated using the formulas in Table 5-5.

  -----------------------------------------------------------------------------
  **Property**              **Ellipsoid Shape**           **Arch Shape**
  ----------------------- ------------------------ ----------------------------
  Full Area $A_{full}$     $$1.2692Y_{full}^{2}$$   $$0.7879Y_{full}W_{\max}$$

  Full Hydraulic Radius      $$0.3061Y_{full}$$         $$0.2991Y_{full}$$
  $R_{full}$                                       
  -----------------------------------------------------------------------------

  : []{#_Toc484690423 .anchor}**Table 5‑5 Full area and hydraulic radius
  of custom ellipsoid and arch pipe sections**

Information in the aforementioned design manuals was used to construct
the following tables for both the ellipsoid and arch shapes (only a
single set of tables is needed for the two ellipsoid shapes since they
are just rotated versions of one another):

  ------------------------------------------------------------------------------
  *A~tbl\ :~*   *A/A~full\ ~* as a function of *Y/Y~full~*
  ------------- ----------------------------------------------------------------
  *W~tbl\ :~*   *W/W~max~* as a function of *Y/Y~full~*

  *R~tbl\ :~*   *R/R~full~* as a function of *Y/Y~full~*
  ------------------------------------------------------------------------------

Each table contains entries for *N =* 26 equally spaced values of
*Y/Y~full~* between 0 and 1. The tables for ellipsoid pipes are in
Appendix D and those for arch pipes are in Appendix E. To find *A*, *W*,
or *R* for a given *Y* one first determines the integer portion of
$(N - 1)\left( \frac{Y}{Y_{full}} \right)$, linearly interpolates the
appropriate table between the entries at this and the next higher index,
and then multiplies by the appropriate normalizing factor (either
*A~full~*, *W~max~*, or *R~full~*).

To find the depth associated with a given area *Y(A)*, a bisection (or
interval halving) procedure is first used on the appropriate (either
ellipsoid or arch) area table *A~tbl~* to find the position *i* so that
$A_{tbl}\lbrack i\rbrack \leq \frac{A}{A_{full} \leq A_{tbl}\lbrack i + 1\rbrack}$.
Then the desired depth *Y* is interpolated from this position in the
table using the following expression with *N* = 26:

  ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
        $$Y(A) = \frac{Y_{full}}{(N - 1)}\left( i + \frac{\left( \frac{A}{A_{full} - A_{tbl}\lbrack i\rbrack} \right)}{\left( A_{tbl}\lbrack i + 1\rbrack - A_{tbl}\lbrack i\rbrack \right)} \right)$$   (5-14)
  ----- ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ --------

  ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

The following steps are used to find the section factor associated with
a given area *Ψ(A):*

1.  Use the aforementioned procedure to find the depth *Y* corresponding
    to area *A* from the appropriate *A~tbl~*.

2.  Use the shape's hydraulic radius table *R~tbl~* to find the
    hydraulic radius *R* for this depth.

3.  Compute the section factor as: $\Psi(A) = AR^{2/3}$.

The section factor derivative for a given area *Ψ'(A)* is found using
the following central difference equation:

  ------------------------------------------------------------------------------------------------------------------------------------
        $$\Psi'(A) = \frac{\Psi(A + \Delta A) - \Psi(A - \Delta A)}{2\Delta A}\ \ where\ \mathrm{\Delta}A = 0.001A_{full}$$   (5-15)
  ----- --------------------------------------------------------------------------------------------------------------------- --------

  ------------------------------------------------------------------------------------------------------------------------------------

### 5.1.5 Older Masonry Sewer Shapes

SWMM contains seven pre-defined closed conduit shapes shown in Figure
5-5 that were used primarily in older masonry sewers built over a
century ago. Their geometric properties have been derived from
information and drawings found in Metcalf and Eddy (1914) and Davis
(1952). These properties are represented using the same type of lookup
tables discussed previously for circular cross sections (see section
5.2.3). The number of entries *N* in each table for each shape is listed
in Table 5-6. The full tables are provided in Appendix F. The values of
*A~full~, R~full~*, and *W~max~* used to normalize the entries in the
tables for each shape are listed in Table 5-7. The full section factor
*Ψ~full~* used to normalize the section factor table is computed as
$A_{full}R_{full}^{2/3}$ .

![OldShapes.png](VolumeII/media/media/image27.png){width="4.834008092738408in"
height="2.510767716535433in"}

[]{#_Toc484694728 .anchor}**Figure 5‑5 Masonry sewer shapes**

  --------------------------------------------------------------------------------------------
  **Shape**          ***A~tbl~***   ***R~tbl~***   ***W~tbl~***   ***Y~tbl~***   ***Ψ~tbl~***
  ----------------- -------------- -------------- -------------- -------------- --------------
  Basket Handle           26             26             26             51             51

  Egg                     26             26             26             51             51

  Horseshoe               26             26             26             51             51

  Catenary                \-             \-             21             51             51

  Gothic                  \-             \-             21             51             51

  Semi-Circular           \-             \-             21             51             51

  Semi-Elliptical         \-             \-             21             51             51
  --------------------------------------------------------------------------------------------

  : []{#_Toc484690424 .anchor}**Table 5‑6 Number of entries in geometric
  property tables for masonry sewer shapes**

  -----------------------------------------------------------------------------------------------------------------------------
  **Shape**         ***A~full~***                ***R~full~***             ***W~max~***            ***Ψ~max~***
  ----------------- ---------------------------- ------------------------- ----------------------- ----------------------------
  Basket Handle     $${0.7862\ Y}_{full}^{2}$$   $$0.2464\ Y_{full}$$      $$0.944{\ Y}_{full}$$   $$1.06078{\ \Psi}_{full}$$

  Egg               $$0.5105\ Y_{full}^{2}$$     $$0.1931{\ Y}_{full}$$    $$0.667\ Y_{full}$$     $$1.065\ \Psi_{full}$$

  Horseshoe         $$0.8293\ Y_{full}^{2}$$     $$0.2538{\ Y}_{full}$$    $$Y_{full}$$            $$1.077\ \Psi_{full}$$

  Catenary          $$0.70277\ Y_{full}^{2}$$    $${0.23172\ Y}_{full}$$   $$0.9\ Y_{full}$$       $$1.05\ \Psi_{full}$$

  Gothic            $$0.6554\ Y_{full}^{2}$$     $$0.2269\ Y_{full}$$      $$0.84\ Y_{full}$$      $$1.065\ \Psi_{full}$$

  Semi-Circular     $$1.2697\ Y_{full}^{2}$$     $$0.2946{\ Y}_{full}$$    $$1.64{\ Y}_{full}$$    $$1.06637\ \Psi_{full}$$

  Semi-Elliptical   $${0.785\ Y}_{full}^{2}$$    $$0.242\ Y_{full}$$       $$Y_{full}$$            $$1.045\ \Psi_{full}$$
  -----------------------------------------------------------------------------------------------------------------------------

  : []{#_Toc484690425 .anchor}**Table 5‑7 Geometric parameters of
  masonry sewer sections**

The tables are used in the same manner as the ones for a circular shape
to directly evaluate *A(Y), W(Y), R(Y), Y(A), Ψ(A),* and *Ψ'(A).* For
the shapes that do not have an *A~tbl~*, *A(Y)* is determined using the
inverse lookup method on the *Y~tbl~* described in section 5.2.4 for
ellipsoids and arches. For shapes without an *R~tbl~*, *R(Y)* is found
by first finding *A(Y)* as just described, then finding *Ψ(A)* for the
resulting area *A*, and finally
evaluating$\left( \frac{\Psi}{A} \right)^{3/2}$. Equation 5-15 is used
to compute *Ψ'(A).*

### 5.1.6 Composite Shapes

Figure 5-6 shows four cross section shapes that are combinations of
circular, rectangular, and triangular sections. The formulas for
computing their geometrical properties are presented in the following
paragraphs.

<figure>
<img src="VolumeII/media/media/image28.png"
style="width:5.33549in;height:4.27256in" alt="CompositeShapes.png" />
<figcaption><p><span id="_Toc484694729"
class="anchor"></span><strong>Figure 5‑6 Composite cross section
shapes</strong></p></figcaption>
</figure>

[Sediment Filled Circular Shape]{.underline}

This is a circular cross section that is partially filled with immobile
sediment to a specified depth *Y~btm~*. (This filled depth remains
constant -- SWMM does not model how it might change over time due to
sediment transport processes.) The depth available for flow is
$Y_{full} - Y_{btm}$. To compute the geometric properties of this shape
one first uses the circular shape functions to compute the area
*A~btm~*, top width *W~btm~*, and hydraulic radius R*~btm~* at a depth
of *Y~btm~* for the full circular shape with diameter *Y~full~*. The
wetted perimeter at this depth, *P~btm~*, is $\frac{A_{btm}}{R_{btm}}$ .
Then the expressions listed in Table 5-8 can be used to find the section
properties for a specific flow depth *Y* above *Y~btm~* or area *A*
above *A~btm~* .

  -------------------------------------------------------------------------------------------------------------------------------------------
  **Property**   **Value Based on Full Circular Shape Functions**
  -------------- ----------------------------------------------------------------------------------------------------------------------------
  *A(Y)*         $$A\left( Y + Y_{btm} \right) - A_{btm}$$

  *W(Y)*         $$W(Y + Y_{btm})$$

  *R(Y)*         $$\frac{A\left( Y + Y_{btm} \right) - A_{btm}}{\left( \frac{A(Y + Y_{btm})}{R(Y + Y_{btm}}) \right) - P_{btm} + W_{btm}}$$

  *Y(A)*         $$Y\left( A + A_{btm} \right) - Y_{btm}$$

  *Ψ(A)*         $$A{R(\mathrm{\Delta}Y)}^{2/3}\ \ where\ \mathrm{\Delta}Y = Y\left( A + A_{btm} \right) - Y_{btm}$$

  *Ψ'(A)*        $$\frac{\Psi(A + \Delta A) - \Psi(A - \Delta A)}{2\Delta A}\ \ where\ \Delta A = 0.001(A_{full} - A_{btm})$$
  -------------------------------------------------------------------------------------------------------------------------------------------

  : []{#_Toc484690426 .anchor}**Table 5‑8 Geometric properties for a
  sediment filled circular cross section**

[Rectangular-Triangular Shape]{.underline}

This shape consists of a triangular bottom section of height *Y~btm~*
connected to a closed rectangular top section of width *b* and height
*Y~full~* -- *Y~btm~*. The slope of the triangular section's sidewalls
*s* is $\frac{b}{2Y_{btm}}$ . For depths below *Y~btm~* (or areas below
$Y_{btm}\frac{b}{2}$) the geometric properties are computed in the same
manner as for the open triangular shape of section 5.2.1. At higher
depths (or areas) the methods used for the closed rectangular shape of
section 5.2.2 are applied with some adjustments made to accommodate the
filled triangular section. The applicable formulas are listed in Table
5-9.

[Rectangular-Round Shape]{.underline}

This composite shape consists of a closed rectangular top with a rounded
bottom section. It has full height *Y~full~*, top width *b*, and bottom
radius of curvature *r* (see Figure 5-6). Table 5-10 lists the
parameters used to compute the section's properties whose formulas are
given in Table 5-11.

+-----------------+------------------------------------------------------------------------------------------------------------------------------------------+
| **Property**    | **Expression**                                                                                                                           |
+:================+==========================================================================================================================================+
| *s*             | $$\frac{b}{\left( 2Y_{btm} \right)}$$                                                                                                    |
+-----------------+------------------------------------------------------------------------------------------------------------------------------------------+
| $$A_{btm}$$     | $$bY_{btm}/2$$                                                                                                                           |
+-----------------+------------------------------------------------------------------------------------------------------------------------------------------+
| $$A_{full}$$    | $$b\left( Y_{full} - \frac{Y_{btm}}{2} \right)$$                                                                                         |
+-----------------+------------------------------------------------------------------------------------------------------------------------------------------+
| $$R_{full}$$    | $$\frac{A_{full}}{\left( 2Y_{btm}\sqrt{1 + s^{2}} + 2\left( Y_{full} - Y_{btm} \right) + b \right)}$$                                    |
+-----------------+------------------------------------------------------------------------------------------------------------------------------------------+
| $$\Psi_{full}$$ | $$A_{full}R_{full}^{2/3}$$                                                                                                               |
+-----------------+------------------------------------------------------------------------------------------------------------------------------------------+
| *A(Y)*          | $$A_{btm} + \left( Y - Y_{btm} \right)b$$                                                                                                |
+-----------------+------------------------------------------------------------------------------------------------------------------------------------------+
| *Y(A)*          | $$Y_{btm} + \frac{\left( A - A_{btm} \right)}{b}$$                                                                                       |
+-----------------+------------------------------------------------------------------------------------------------------------------------------------------+
| *W(Y)*          | $$b$$                                                                                                                                    |
+-----------------+------------------------------------------------------------------------------------------------------------------------------------------+
| *P(Y)*          | $$2Y_{btm}\sqrt{\left( 1 + s^{2} \right)} + 2\left( Y - Y_{btm} \right)$$                                                                |
|                 |                                                                                                                                          |
|                 | $$if\ \ A(Y) > 0.98A_{full}\ add\ on\ \left( \frac{A(Y)}{A_{full}} - 0.98 \right)\frac{b}{0.02}\ \ $$                                    |
+-----------------+------------------------------------------------------------------------------------------------------------------------------------------+
| *R(Y)*          | $$\frac{A(Y)}{P(Y)}$$                                                                                                                    |
+-----------------+------------------------------------------------------------------------------------------------------------------------------------------+
| *R(A)*          | $$\frac{A}{P(Y(A))}$$                                                                                                                    |
+-----------------+------------------------------------------------------------------------------------------------------------------------------------------+
| $$\Psi_{\max}$$ | $$0.98A_{full}{R(0.98A_{full})}^{2/3}$$                                                                                                  |
+-----------------+------------------------------------------------------------------------------------------------------------------------------------------+
| $$\Psi(A)$$     | $$A{R(A)}^{2/3}\ \ for\ A \leq 0.98A_{full}$$                                                                                            |
|                 |                                                                                                                                          |
|                 | $$\Psi_{\max} + \left( \Psi_{full} - \Psi_{\max} \right)\frac{\left( \frac{A}{A_{full} - 0.98} \right)}{0.02\ \ for\ A > 0.98A_{full}}$$ |
+-----------------+------------------------------------------------------------------------------------------------------------------------------------------+
| $$\Psi'(A)$$    | $$\left( \frac{5}{3} - \left( \frac{2}{3} \right)\left( \frac{2}{b} \right)R(A) \right){R(A)}^{2/3}\ \ for\ A \leq 0.98A_{full}$$        |
|                 |                                                                                                                                          |
|                 | $$\frac{\left( \Psi_{full} - \Psi_{\max} \right)}{0.02A_{full}\ for\ A > 0.98A_{full}\ }$$                                               |
+-----------------+------------------------------------------------------------------------------------------------------------------------------------------+

: []{#_Toc484690427 .anchor}**Table 5‑9 Properties of the rectangular
section of a rectangular-triangular shape**

[Modified Basket Handle Shape]{.underline}

The modified basket handle shape is the reverse of the rectangular-round
shape with a rectangular bottom section below a rounded top section. It
has full height *Y~full~*, bottom width *b*, and top section radius of
curvature *r* (see Figure 5-6). The central angle *θ* formed by the
rounded top section is:

  --------------------------------------------------------------------------
        $$\theta = 2\sin^{- 1}\left( \frac{b}{2r} \right)$$         (5-14)
  ----- ----------------------------------------------------------- --------

  --------------------------------------------------------------------------

The depth *Y~btm~* of the bottom rectangular section is:

  ------------------------------------------------------------------------------------------------
        $$Y_{btm} = Y_{full} - r\left( 1 - cos\left( \frac{\theta}{2} \right) \right)$$   (5-15)
  ----- --------------------------------------------------------------------------------- --------

  ------------------------------------------------------------------------------------------------

and its area *A~btm~* is *bY~btm~* . The shape's full area *A~full~* is:

  ----------------------------------------------------------------------------------------------
        $$A_{full} = A_{btm} + \frac{r^{2}}{\left\{ 2(\theta - sin\theta) \right\}}$$   (5-16)
  ----- ------------------------------------------------------------------------------- --------

  ----------------------------------------------------------------------------------------------

For depths up to *Y~btm~* and areas up to *A~btm~* the open rectangular
shape functions of Tables 5-1 and 5-2, respectively, are used to compute
the modified basket handle's section properties. For depths and areas
above this the functions listed in Table 5-12 are used.

  -----------------------------------------------------------------------------------------------------------------------------------
  **Parameter**                **Value**
  ---------------------------- ------------------------------------------------------------------------------------------------------
  Central Angle *θ*            $$2\sin^{- 1}\left( \frac{b}{2r} \right)$$

  Bottom Section Height        $$r\left( 1 - cos\left( \frac{\theta}{2} \right) \right)$$
  *Y~btm~*                     

  Bottom Section Area *A~btm~* $$\left( \frac{r^{2}}{2} \right)\left( \theta - sin(\theta) \right)$$

  Full Area *A~full~*          $$b\left( Y_{full} - Y_{btm} \right) + A_{btm}$$

  Full Hydraulic Radius        $$\frac{A_{full}}{\left\{ r\theta + 2\left( Y_{full} - Y_{btm} \right) + b \right\}}$$
  *R~full~*                    

  Full Section Factor          $$A_{full}R_{full}^{2/3}$$
  *Ψ~full~*                    

  Maximum Hydraulic Radius     $$\frac{0.98A_{full}}{\left\{ r\theta + \frac{2\left( 0.98A_{full} - A_{btm} \right)}{b} \right\}}$$
  *R~max~*                     

  Maximum Section Factor       $$0.98A_{full}R_{\max}^{2/3}$$
  *Ψ~max~*                     
  -----------------------------------------------------------------------------------------------------------------------------------

  : []{#_Toc484690428 .anchor}**Table 5‑10 Geometric parameters for
  rectangular-round shapes**

+--------------+-----------------------------------------------------------------------------------------------------------------+-------------------------------------+
| **Property** | **Formula**                                                                                                     | **Applicable Region**               |
+:=============+=================================================================================================================+:====================================+
| *A(Y)*       | $$0.5r^{2}\left( \phi - sin(\phi) \right)\ \ where\ \phi = 2\cos^{- 1}\left( 1 - \frac{Y}{r} \right)$$          | $$Y \leq Y_{btm}$$                  |
|              +-----------------------------------------------------------------------------------------------------------------+-------------------------------------+
|              | $$A_{btm} + \left( Y - Y_{btm} \right)b$$                                                                       | $$Y > Y_{btm}$$                     |
+--------------+-----------------------------------------------------------------------------------------------------------------+-------------------------------------+
| *W(Y)*       | $$2\sqrt{Y(2r - Y)}$$                                                                                           | $$Y \leq Y_{btm}$$                  |
|              +-----------------------------------------------------------------------------------------------------------------+-------------------------------------+
|              | *b*                                                                                                             | $$Y > Y_{btm}$$                     |
+--------------+-----------------------------------------------------------------------------------------------------------------+-------------------------------------+
| *R(Y)*       | $$0.5r\frac{\left( 1 - sin(\phi) \right)}{\phi\ \ where\ \ }\phi = 2\cos^{- 1}\left( 1 - \frac{Y}{r} \right)$$  | $$Y \leq Y_{btm}$$                  |
|              +-----------------------------------------------------------------------------------------------------------------+-------------------------------------+
|              | $$R\left( A(Y) \right)\ \ (see\ R(A)function\ below)$$                                                          | $$Y > Y_{btm}$$                     |
+--------------+-----------------------------------------------------------------------------------------------------------------+-------------------------------------+
| *Y(A)*       | $$Y(A)for\ circular\ shape\ with\ Y_{full} = 2r$$                                                               | $$A \leq A_{btm}$$                  |
|              +-----------------------------------------------------------------------------------------------------------------+-------------------------------------+
|              | $$Y_{btm} + \frac{\left( A - A_{btm} \right)}{b}$$                                                              | $$A > A_{btm}$$                     |
+--------------+-----------------------------------------------------------------------------------------------------------------+-------------------------------------+
| *P(A)*       | $$2r\cos^{- 1}\left( 1 - \frac{Y(A)}{r} \right)$$                                                               | $$A \leq A_{btm}$$                  |
|              +-----------------------------------------------------------------------------------------------------------------+-------------------------------------+
|              | $$2r\sin^{- 1}\left( \frac{b}{2r} \right) + 2\frac{\left( A - A_{btm} \right)}{b}$$                             | $$A_{btm} < A \leq {0.98A}_{full}$$ |
|              +-----------------------------------------------------------------------------------------------------------------+-------------------------------------+
|              | $$2r\sin^{- 1}\left( \frac{b}{2r} \right) + 2\frac{\left( A - A_{btm} \right)}{b}$$                             | $$A > 0.98A_{full}$$                |
|              |                                                                                                                 |                                     |
|              | $$+ \left( \frac{A}{A_{full} - 0.98} \right)\frac{b}{0.02}$$                                                    |                                     |
+--------------+-----------------------------------------------------------------------------------------------------------------+-------------------------------------+
| *R(A)*       | $$\frac{A}{P(A)}$$                                                                                              |                                     |
+--------------+-----------------------------------------------------------------------------------------------------------------+-------------------------------------+
| *Ψ(A)*       | $$\Psi(A)for\ circular\ shape\ with\ Y_{full} = 2r$$                                                            | $$A \leq A_{btm}$$                  |
|              +-----------------------------------------------------------------------------------------------------------------+-------------------------------------+
|              | $$A{R(A)}^{2/3}$$                                                                                               | $$A_{btm} < A \leq {0.98A}_{full}$$ |
|              +-----------------------------------------------------------------------------------------------------------------+-------------------------------------+
|              | $$\Psi_{\max} + \left( \Psi_{full} - \Psi_{\max} \right)\frac{\left( \frac{A}{A_{full} - 0.98} \right)}{0.02}$$ | $$A > 0.98A_{full}$$                |
+--------------+-----------------------------------------------------------------------------------------------------------------+-------------------------------------+
| *Ψ'(A)*      | $$\frac{\left\{ \Psi(A + \Delta A) - \Psi(A - \Delta A) \right\}}{2\Delta A}$$                                  | $$A \leq A_{btm}$$                  |
|              +-----------------------------------------------------------------------------------------------------------------+-------------------------------------+
|              | $$\left( \frac{5}{3} - \left( \frac{2}{3} \right)\left( \frac{2}{b} \right)R(A) \right){R(A)}^{2/3}$$           | $$A_{btm} < A \leq {0.98A}_{full}$$ |
|              +-----------------------------------------------------------------------------------------------------------------+-------------------------------------+
|              | $$\frac{\left( \Psi_{full} - \Psi_{\max} \right)}{\left( 0.02A_{full} \right)}$$                                | $$A > 0.98A_{full}$$                |
+--------------+-----------------------------------------------------------------------------------------------------------------+-------------------------------------+

: []{#_Toc484690429 .anchor}**Table 5‑11 Geometric properties for
rectangular--round shapes**

  -------------------------------------------------------------------------------------------------------------------------------------------------------------------
  **Property**   **Expression**
  -------------- ----------------------------------------------------------------------------------------------------------------------------------------------------
  *A(Y)*         $$A_{full} - \left( \frac{r^{2}}{2} \right)(\phi - sin\phi)\ \ where\ \phi = 2\cos^{- 1}\left( 1 - \frac{\left( Y_{full} - Y \right)}{r} \right)$$

  *W(Y)*         $$2\sqrt{\left( Y_{full} - Y \right)\left( 2r - \left( Y_{full} - Y \right) \right)}$$

  *R(Y)*         $$R\left( A(Y) \right)\ using\ R(A)function\ below$$

  *Y(A)*         $$Y_{full} - Y\left( A_{full} - A \right)\ \ using\ Y(A)for\ circular\ shape\ with\ Y_{full} = 2r$$

  *P(A)*         $$(\theta - \phi)r + 2\left( Y_{full} - Y(A) \right) + b\ \ where\ \phi = 2\cos^{- 1}\left( 1 - \frac{\left( Y_{full} - Y(A) \right)}{r} \right)$$

  *R(A)*         $$\frac{A}{P(A)}$$

  $$\Psi(A)$$    $$A{R(A)}^{2/3}$$

  $$\Psi'(A)$$   $$\frac{\left\{ \Psi(A + \Delta A) - \Psi(A - \Delta A) \right\}}{(2\Delta A)}\ \ where\ \Delta A = 0.001A_{full}$$
  -------------------------------------------------------------------------------------------------------------------------------------------------------------------

  : []{#_Toc484690430 .anchor}**Table 5‑12 Properties in the rounded top
  section of a modified basket handle shape**

### 5.1.7 Area at Maximum Flow

The solution method for kinematic wave analysis in a closed conduit
needs to know what cross-sectional area corresponds to the flow depth
where the section factor and hence the Manning equation flow rate is a
maximum (see Sections 4.2 and 4.3.2). Below this point the section
factor is an increasing function of area, after which it decreases until
the conduit becomes full. Table 5-13 lists the ratio of the area at
maximum flow (denoted as *A~max~*) to the full area (*A~full~*) for the
standard closed conduit shapes recognized by SWMM. For open shapes
*A~max~* is the same as *A~full~*.

  -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
  **Shape**             $$\frac{\mathbf{A}_{\mathbf{\max}}}{\mathbf{A}_{\mathbf{full}}}$$  **Shape**                 $$\frac{\mathbf{A}_{\mathbf{\max}}}{\mathbf{A}_{\mathbf{full}}}$$
  -------------------- ------------------------------------------------------------------- ------------------------ -------------------------------------------------------------------
  Rectangular                                         0.97                                 Circular                                               0.9756

  Elliptical                                          0.96                                 Arch                                                    0.92

  Basket Handle                                       0.96                                 Egg                                                     0.96

  Horseshoe                                           0.96                                 Catenary                                                0.98

  Gothic                                              0.96                                 Semi-Circular                                           0.96

  Semi-Elliptical                                     0.98                                 Rectangular-Triangular                                  0.98

  Rectangular-Round                                   0.98                                 Modified Basket Handle                                  0.96
  -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

  : []{#_Toc484690431 .anchor}**Table 5‑13 Area at maximum flow to full
  area for standard closed conduits shapes**

### 5.1.8 Area from Section Factor

Kinematic wave analysis also needs to know the area *A* corresponding to
a given normal flow rate *Q* from its associated section factor *Ψ*,
where $\Psi = \frac{Q\sqrt{S_{0}}}{\eta}$. For circular shapes and the
seven masonry sewer shapes discussed in section 5.2.5 the following
"reverse" lookup method is used with the shape's *Ψ* versus *A* table
(*Ψ~tbl~*) to determine *A* given *Ψ*.

Let *Ψ\** be the section factor value whose area is sought and let *N*
be the number of entries in *Ψ~tbl~*. First the interval in the table
that brackets $\frac{\Psi^{*}}{\Psi_{full}}$ is located. Since these are
all closed shapes, there will be a table entry index *i~max~* after
which the $\frac{\Psi}{\Psi_{full}}$ values begin to decrease. If
$\frac{\Psi^{*}}{\Psi_{full}}$ is between *Ψ~tbl~*\[*i~max~*\] and
*Ψ~tbl~*\[*N*\] then this portion of the table is examined to find the
index *i\** so that $\frac{\Psi^{*}}{\Psi_{full}}$ is between
*Ψ~tbl~*\[*i\**\] and *Ψ~tbl~*\[*i\*+1*\]. Otherwise a bisection search
is used between index 0 and *i~max~* to find the interval starting at
*i\** that brackets $\frac{\Psi^{*}}{\Psi_{full}}$. Then the area *A\**
corresponding to *Ψ\** is computed as:

  -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
        $$A^{*} = \frac{A_{full}}{(N - 1)}\left( i^{*} + \frac{\left( \Psi^{*} - \Psi_{tbl}\left\lbrack i^{*} \right\rbrack \right)}{\left( \Psi_{tbl}\left\lbrack i^{*} + 1 \right\rbrack - \Psi_{tbl}\left\lbrack i^{*} \right\rbrack \right)} \right)$$   (5-17)
  ----- ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- --------

  -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

For all other shapes the Newton-Raphson-Bisection method (see Appendix
A) is used to find the solution of

  --------------------------------------------------------------------------
        $f(A) = \Psi(A) - \Psi^{*} = 0$                             (5-18)
  ----- ----------------------------------------------------------- --------

  --------------------------------------------------------------------------

where *Ψ\** is the section factor value whose corresponding area is
sought. The derivative of *f(A)* required by the method is the shape's
*Ψ'(A)* function. If the shape is closed with *A~max~ \< A~full~* and
*Ψ\** is between *Ψ~full~* and *Ψ~max~* then the search interval is
\[*A~full~, A~max~*\]. Otherwise it is \[0, *A~max~*\]. The convergence
criterion is 0.01 percent of *A~full~* .

## 5.2 Custom Conduit Shapes

In addition to its catalog of standard pre-defined shapes, SWMM can also
utilize custom closed shapes that are defined by a Shape Curve supplied
by the user. This curve specifies how the width of the cross-section
varies with height, where both width and height are scaled relative to
the section\'s full height. This allows the same shape curve to be used
for conduits of differing sizes. An example shape curve along with its
table of width versus height is shown in Figure 5-7.

+--------------------------------------------------------------------------+---------------------------------------------------------------------------+
| ![ShapeCurve2.png](VolumeII/media/media/image29.png){width="2.50034886264217in" |   ----------------------------------------------------------------------- |
| height="2.375331364829396in"}                                            |    ***Y/Y~full~***   ***W/Y~full~***   ***Y/Y~full~***   ***W/Y~full~***  |
|                                                                          |   ----------------- ----------------- ----------------- ----------------- |
|                                                                          |         0.00              0.000             0.56              0.928       |
|                                                                          |                                                                           |
|                                                                          |         0.08              0.667             0.64              0.874       |
|                                                                          |                                                                           |
|                                                                          |         0.16              0.930             0.72              0.798       |
|                                                                          |                                                                           |
|                                                                          |         0.24              1.000             0.80              0.697       |
|                                                                          |                                                                           |
|                                                                          |         0.32              0.997             0.88              0.567       |
|                                                                          |                                                                           |
|                                                                          |         0.40              0.988             0.96              0.342       |
|                                                                          |                                                                           |
|                                                                          |         0.48              0.967             1.00              0.000       |
|                                                                          |   ----------------------------------------------------------------------- |
+==========================================================================+===========================================================================+

: []{#_Toc484694730 .anchor}**Figure 5‑7 A Shape Curve with a depth
segment shown**

The flow area *A*, top width *W* and hydraulic radius *R* of a custom
shape are pre-computed at 51 equally spaced vertical values between 0
and 1 along the shape curve and stored in tables *A~tbl~, W~tbl~*, and
*R~tbl~*, respectively. The tables are constructed by analyzing each
depth segment of size 1/50 = 0.02 starting at 0 and working upwards. As
shown in Figure 5-7, each depth segment forms a trapezoid within the
cross section. The area of this trapezoid is added to the shape's total
area *A~sum~* and the length of its side walls is added to the total
wetted perimeter *P~sum~*. If the depth segment straddles more than one
shape curve segment, then additional trapezoids are formed at the shape
curve's vertices, each of which contributes to *A~sum~* and *P~sum~*.
The *A~tbl~* entry for the segment is set to *A~sum~*, the *R~tbl~*
entry to $\frac{A_{sum}}{P_{sum}}$, and the *W~tbl~* entry to the
segment's top width.

When a conduit with full depth *Y~full~* is assigned a shape curve for
its cross section, the curve's geometry tables are used in the same
manner as the tables for ellipsoid and arch shapes described in section
5.1.4 for evaluating *A(Y), W(Y)*, *R(Y),* *Y(A), Ψ(A),* and *Ψ'(A)*.
The values of *A~full~, R~full~*, and *W~max~* used to convert the
normalized values in the tables to actual dimensions are as follows:

  ----------------------------------------------------------------------------------------------------------
        $$A_{full} = A_{tbl}\lbrack 50\rbrack Y_{full}^{2}$$                                          (5-19)
  ----- ------------------------------------------------------------------------------------------- --------
        $$R_{full} = R_{tbl}\lbrack 50\rbrack Y_{full}$$                                              (5-20)

        $$W_{\max} = \left\{ \max_{0 \leq i \leq 50}{W_{tbl}\lbrack i\rbrack} \right\} Y_{full}$$     (5-21)
  ----------------------------------------------------------------------------------------------------------

The value of *A~max~*, the area of the flow depth where the section
factor is a maximum, is given by:

  ----------------------------------------------------------------------------------------------------------------------------------------------------------
        $$A_{\max} = \left\{ \max_{0 \leq i \leq 50}\left( A_{tbl}\lbrack i\rbrack{R_{tbl}\lbrack i\rbrack}^{2/3} \right) \right\} Y_{full}^{2}$$   (5-22)
  ----- ------------------------------------------------------------------------------------------------------------------------------------------- --------

  ----------------------------------------------------------------------------------------------------------------------------------------------------------

The Newton-Raphson-Bisection method described in section 5.1.8 is used
to evaluate *A(Ψ).*

## 5.3 Irregular Natural Channels

SWMM also has the ability to model natural channels with irregular
shaped cross sections. The cross sectional shape is represented by a
transect that begins at the top of the left bank of the channel (looking
downstream) and extends transversely across the channel to the top of
its right bank. The channel's bed elevation (*y*) relative to a known
elevation is recorded at a series of measurement stations (*x*) across
the transect (see Figure 5-8). A single transect is used to represent a
channel's cross section along its entire length. This might require that
longer channels with varying cross section profiles be broken into
smaller more uniform segments.

As shown in Figure 5-8, transects can contain two overbank areas on
either side. Each is optional and is used to specify a different
Manning's roughness coefficient than that assigned to the main channel.
Each overbank boundary location must coincide with one of the transect's
measurement stations.

<figure>
<img src="VolumeII/media/media/image30.png"
style="width:6.5in;height:2.60208in" alt="Transect2.png" />
<figcaption><p><span id="_Toc484694731"
class="anchor"></span><strong>Figure 5‑8 A natural channel
transect</strong></p></figcaption>
</figure>

The flow area *A*, top width *W* and hydraulic radius *R* of a transect
are pre-computed at 51 equally spaced values of flow depth relative to
full depth $\left( \frac{Y}{Y_{full}} \right)$ and stored in tables
*A~tbl~, W~tbl~*, and *R~tbl~*, respectively. The table values are
normalized with respect to the full section area A*~full~*, the maximum
width *W~max~*, and the full section hydraulic radius *R~full~*,
respectively. These tables are used in the same manner as the tables for
ellipsoid and arch shapes described in section 5.1.4 for evaluating
*A(Y), W(Y)*, *R(Y)*, *Y(A), Ψ(A),* and *Ψ'(A)*. *A(Ψ)* is found using
the Newton-Raphson-Bisection method as described in section 5.1.8.

The first step in constructing the geometric property tables for a
transect is to find the measurement stations with the lowest and highest
elevations. The full channel depth *Y~full~* is set equal to the
difference between these values. If necessary, a new station is added at
either end of the transect so that both ends are at the highest
elevation. Then all station elevation values *y* are converted to the
height above the lowest elevation station.

Next table entries are generated for a series of depths that divide the
full depth into 50 equal increments starting at 0 (whose table entries
are set to 0). The procedure for finding each table's entry for the
*k-th* depth interval is described in the side bar entitled "*Computing
Geometry Table Entries for Irregular Cross Sections*". It traverses the
cross section's transect, computing the area, width, and wetted
perimeter for each measurement station segment that lays above the
current depth increment. It also finds the conductance (the section
factor times roughness) of compound segments that separate regions of
differing roughness or where valleys occur in the transect's profile.
(Figure 5-9 shows a flow depth increment with three compound segments.)
After the end of the transect is reached the sum of the compound
conductances is used along with the main channel roughness to find the
hydraulic radius for the current depth increment.

<figure>
<img src="VolumeII/media/media/image31.png"
style="width:6.5in;height:1.84028in"
alt="TransectCompoundSegments.png" />
<figcaption><p><span id="_Toc484694732"
class="anchor"></span><strong>Figure 5‑9 A transect depth increment with
three compound segments</strong></p></figcaption>
</figure>

Once table entries for all depth increments have been generated, the
following quantities are assigned and used to normalize the entries in
their respective tables:
$A_{full} = A_{tbl}\lbrack 50\rbrack,\ \ W_{\max} = W_{tbl}\lbrack 50\rbrack,\ \ R_{full} = R_{tbl}\lbrack 50\rbrack$.
Another adjustment is to set
$W_{tbl}\lbrack 0\rbrack = W_{tbl}\lbrack 1\rbrack$ since the above
procedure does not calculate a width at zero depth.

An irregular natural channel can also be assigned a meander modifier.
This is the ratio of the length of a meandering main channel to the
length of the overbank area that surrounds it. While the user-supplied
length for the overall channel is that of the longer main channel, SWMM
will use the shorter overbank length in its calculations. The Manning's
*n* of the main channel will be increased by the square root of the
meander modifier to provide an equivalent friction head loss over the
reduced main channel length.

## 5.4 Street Cross-Sections

SWMM defines the geometry of a street or roadway cross-section as a
special case of the irregular channel discussed in the previous section.
The shape of a one-sided street cross-section is shown in Figure 5-10.
In its most basic form it consists of a road surface with downward slope
*S~x~* extending a distance of *T~crown~* to a vertical curb of height
*H~curb~*. To this can be added:

- an optional depressed gutter section of width *W* that extends to a
  depth "*a*" below the normal curb height

- an optional backing section extending beyond the curb a distance
  *T~back~* that rises at a slope of *S~back~* .

A two sided street cross-section adds a mirror image of the one-sided
street to the right of the street crown with the same roadway, gutter,
curb, and backing dimensions.

![Street.png](VolumeII/media/media/image32.png){width="5.197267060367454in"
height="1.8747659667541556in"}

**Figure 5-10 A one-sided street cross-section (not to scale)**

Street cross-sections use the same procedures as irregular channel
transects, described in section 5.3, to compute tables of flow area, top
width, and hydraulic radius at 50 equally spaced increments of flow
depth (for both one-sided and two-sided streets). This requires that in
addition to the dimensions shown in Figure 5-10, a Manning roughness
coefficient must be specified for the road surface and for the backing
surface if present.

## 5.5 Storage Unit Geometry

SWMM's hydraulic modeling procedures require knowledge of how a storage
unit's surface area *A* and volume *V* vary with surface depth *Y* above
the bottom of the unit. It is sufficient to specify either an area or
volume relationship with respect to depth since one can be derived from
the other$\ \left( A = \frac{dV}{dY\ and\ V = \int_{}^{}{AdY}} \right)$.
SWMM uses surface area to describe a storage unit's shape. One can
select either from several standard shapes where *A* is a quadratic
function of *Y*, from a general power law relation between *A* and *Y*
or use a tabular listing of *Y* and *A* values.

### 5.5.1 Standard Storage Shapes

SWMM supports several common storage unit shapes, listed in Table 5-14,
whose top surface area A can be expressed as a quadratic function of
height Y:

  --------------------------------------------------------------------------
        $$A = a_{0} + a_{1}Y + a_{2}Y^{2}$$                         (5-23)
  ----- ----------------------------------------------------------- --------

  --------------------------------------------------------------------------

The constants *a~0~, a~1~*, and *a~2~* depend on the shape's dimensions
as shown in Table 5-14.

+-------------+----------------------------------------------------------------------------------+--------------------------------------------+------------------------+
| **Shape**   |                                                                                  | **Coefficients**                           | **Dimensions**         |
+:============+:=================================================================================+============================================+========================+
| Elliptical  | ![cylindrical.png](VolumeII/media/media/image33.png){width="0.8334492563429571in"       | $$a_{0} = \left( \frac{\pi}{4} \right)LW$$ | *L* = major axis       |
| Cylinder    | height="0.6667596237970254in"}                                                   |                                            | length                 |
|             |                                                                                  | $$a_{1} = a_{2} = 0$$                      |                        |
|             |                                                                                  |                                            | *W* = minor axis width |
+-------------+----------------------------------------------------------------------------------+--------------------------------------------+------------------------+
| Elliptical  | ![paraboloid.png](VolumeII/media/media/image34.png){width="0.8332294400699912in"        | $$a_{0} = a_{2} = 0$$                      | *L* = major axis       |
| Paraboloid  | height="0.708244750656168in"}                                                    |                                            | length                 |
|             |                                                                                  | $$a_{1} = (\frac{\pi}{4})\frac{LW}{H}$$    |                        |
|             |                                                                                  |                                            | *W* = minor axis width |
|             |                                                                                  |                                            |                        |
|             |                                                                                  |                                            | *H* = paraboloid       |
|             |                                                                                  |                                            | height                 |
+-------------+----------------------------------------------------------------------------------+--------------------------------------------+------------------------+
| Elliptical  | ![ConicStorageShape.bmp](VolumeII/media/media/image35.png){width="0.6979166666666666in" | $$a_{0} = \left( \frac{\pi}{4} \right)LW$$ | *L* = bottom major     |
|             | height="0.5625in"}                                                               |                                            | axis length            |
| Cone        |                                                                                  | $$a_{1} = \pi WZ$$                         |                        |
|             |                                                                                  |                                            | *W* = bottom minor     |
|             |                                                                                  | $$a_{2} = \pi(\frac{W}{L})Z^{2}$$          | axis width             |
|             |                                                                                  |                                            |                        |
|             |                                                                                  |                                            | *Z* = side slope       |
|             |                                                                                  |                                            | (run/rise) along major |
|             |                                                                                  |                                            | axis                   |
+-------------+----------------------------------------------------------------------------------+--------------------------------------------+------------------------+
| Rectangular | ![PrismaticStorageShape.bmp](VolumeII/media/media/image36.png){width="0.96875in"        | $$a_{0} = LW$$                             | L = bottom length      |
| Pyramid     | height="0.4270833333333333in"}                                                   |                                            |                        |
|             |                                                                                  | $$a_{1} = 2(L + W)Z$$                      | W = bottom width       |
|             |                                                                                  |                                            |                        |
|             |                                                                                  | $$a_{2} = 4Z^{2}$$                         | Z = wall slope         |
|             |                                                                                  |                                            | (run/rise) (same for   |
|             |                                                                                  |                                            | each face)             |
+-------------+----------------------------------------------------------------------------------+--------------------------------------------+------------------------+

: **Table 5-14. Standard storage unit shapes.**

Dynamic wave analysis needs to know how volume *V* varies with depth
*Y*. Integrating Equation 5-23 over depth yields:

  --------------------------------------------------------------------------
        $$V = a_{0}Y + {(a}_{1}/2)Y^{2} + (a_{2}/3)Y^{3}$$          (5-24)
  ----- ----------------------------------------------------------- --------

  --------------------------------------------------------------------------

Kinematic wave analysis needs to know the depth associated with a given
volume. For a cylindrical shape: $Y = V/a_{0}$, while for paraboloid
shape: $Y = \sqrt{2V/a_{1}}$ . For the other shapes the cubic equation
5-24 is solved numerically for *Y* given *V* using the
Newton-Raphson-Bisection method described in Appendix A over the
interval \[0, *Y~full~*\] with initial estimate $Y = \frac{V}{a_{0}},$
convergence tolerance of 0.001 ft and derivative given by Equation 5-23.

### 5.5.2 Functional Storage Shapes

SWMM's functional storage shape option uses a power law to relate
surface area to depth:

  --------------------------------------------------------------------------
        $$A = c_{0} + c_{1}Y^{c_{2}}$$                              (5-25)
  ----- ----------------------------------------------------------- --------

  --------------------------------------------------------------------------

where *c~0~, c~1~*, and *c~2~* are user-supplied constants.

The surface area at a given depth is found directly from this equation.
The relation between volume *V* and depth *Y* (required for dynamic wave
analysis) is:

  -------------------------------------------------------------------------------------
        $$V = c_{0}Y + \left( \frac{c_{1}}{c_{2} + 1} \right)Y^{c_{2} + 1}$$   (5-26)
  ----- ---------------------------------------------------------------------- --------

  -------------------------------------------------------------------------------------

To find the depth associated with a given volume (required for kinematic
wave analysis) one solves the following nonlinear equation for *Y* :

  ---------------------------------------------------------------------------------------------------------------
        $$f(Y) = V - \left( c_{0}Y + \left( \frac{c_{1}}{c_{2} + 1} \right)Y^{c_{2} + 1} \right) = 0$$   (5-27)
  ----- ------------------------------------------------------------------------------------------------ --------

  ---------------------------------------------------------------------------------------------------------------

It is solved using the Newton-Raphson-Bisection method described in
Appendix A over the interval \[0, *Y~full~*\] with initial estimate
$Y = \frac{V}{\left( c_{0} + c_{1} \right)},$ convergence tolerance of
0.001 ft and derivative $f'(Y)$ given by Equation 5-25.

Some shapes and their coefficients that can be represented with this
functional option include:

  -----------------------------------------------------------------------

  -----------------------------------------------------------------------

- Shapes with vertical sides and constant surface area no matter how
  irregular in outline, including cylinders and rectangular prisms:

*c~0~* = area of the base

*c~1~ = c~2~ = 0*.

- An open channel with a trapezoidal cross section and vertical ends:

$$c_{0} = WL$$

$$c_{1} = 2ZL$$

$$c_{2} = 1$$

> where *W* = bottom width of cross section, *L* = channel length, and
> *Z* = side slope.

- An open channel with a parabolic cross section and vertical ends:

$$c_{0} = 0$$

$$c_{1} = WLH^{0.5}$$

$$c_{2} = 1$$

> where *W* = top width, *L* = channel length and *H* = full height.

- An elliptical paraboloid:

$$c_{0} = 0$$

$$c_{1} = A/H$$

$$c_{2} = 1$$

where *A* is the surface area at height *H*.

### 5.5.3 Tabular Storage Shapes

The shape of a storage unit can also be defined by a Storage Curve which
is a series of user-supplied data pairs *Y~i~, A~i~* that represent the
points on a curve of surface area versus surface depth for the unit. An
example of this type of curve is shown in Figure 5-11. It can represent
natural depressions with irregular shaped contour intervals, spheroid
storage vessels or conventional shapes with different base sizes stacked
on top of one another. The first point supplied to the curve should be
the surface area of the unit\'s base at a depth of 0. Otherwise it will
be assumed that the unit has zero surface area at its base. The curve
will be extrapolated outwards to meet the unit\'s maximum depth if need
be.

![StorageCurve2.png](VolumeII/media/media/image37.png){width="4.168351924759405in"
height="3.12626312335958in"}

<figure>
<img src="VolumeII/media/media/image38.png"
style="width:4.3131in;height:3.03167in" alt="StorageCurve1.png" />
<figcaption><p><span id="_Toc484694733"
class="anchor"></span><strong>Figure 5‑11 Example of a storage curve and
its section view</strong></p></figcaption>
</figure>

To find the area associated with a given storage depth one interpolates
between the data points that bracket the depth value on the storage
curve. Determining the storage volume *V* at a given depth *Y* is
equivalent to finding the area under the storage curve from depth 0 to
*Y*. This can be done by using the Trapezoidal Rule (Atkinson, 1989)
which results in:

  -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
        $$V = \frac{1}{2}\left\{ \sum_{i = 1}^{n}{\left( Y_{i} - Y_{i - 1} \right)\left( A_{i} + A_{i - 1} \right)} \right\} + \ \frac{1}{2}\left( Y - Y_{n} \right)\left( A + A_{n} \right)$$   (5-28)
  ----- ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- --------

  -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

where *n* is the largest data point index with $Y_{n} \leq Y$ and *A* is
the surface area associated with depth *Y* as found from the storage
curve itself. The shaded rectangles in Figure 5-12 illustrate how the
trapezoidal rule is applied to a storage curve to find the stored volume
at a particular depth. This procedure is the same as the widely used
Average-End-Area method except that the area at the desired depth is
first interpolated from the storage curve rather than converting the
original area curve to a volume curve and interpolating directly from
it.

<figure>
<img src="VolumeII/media/media/image39.png"
style="width:4.21934in;height:3.17753in" alt="StorageCurve3.png" />
<figcaption><p><span id="_Toc484694734"
class="anchor"></span><strong>Figure 5‑12 Finding the volume at a given
depth for a storage curve</strong></p></figcaption>
</figure>

The depth that corresponds to a particular volume for a storage curve
can be found as follows. Using the trapezoidal rule, sum the volumes
contributed by each curve segment starting from 0 until the accumulated
volume *V~sum~* exceeds the target volume *V*. Let the data point index
at the start of this segment be denoted by *i*. Then the depth *Y* that
results in volume *V* is:

  ---------------------------------------------------------------------------------------------------------------------------------------
        $$Y = Y_{i} + \frac{\left\lbrack \sqrt{A_{i}^{2} + 2\alpha\left( V - V_{sum} \right)} - A_{i} \right\rbrack}{\alpha}$$   (5-29)
  ----- ------------------------------------------------------------------------------------------------------------------------ --------

  ---------------------------------------------------------------------------------------------------------------------------------------

where
$\alpha = \frac{\left( A_{i + 1} - A_{i} \right)}{\left( Y_{i + 1} - Y_{i} \right)}$.

## 5.6 Critical and Normal Depths

SWMM needs to calculate the critical and normal flow depths in a conduit
for dynamic wave analysis whenever:

1.  the conduit is connected to a free outfall node

2.  a discontinuity exists between the water level in the conduit and in
    its connecting node (i.e. a free fall condition exists).

These depths are functions of flow rate and cross section shape. For all
but the simplest shapes, iterative numerical methods are required to
compute them.

### 5.6.1 Critical Depth

Critical depth is defined as the depth *Y* where the specific energy at
a given flow rate *Q* is a minimum and the Froude number *Fr* equals 1
(Chow, 1959). From the latter condition

  --------------------------------------------------------------------------
        $$Fr = \frac{U}{\sqrt{g\frac{A}{W}} = 1}$$                  (5-30)
  ----- ----------------------------------------------------------- --------

  --------------------------------------------------------------------------

where *U* is flow velocity and *g* is the acceleration of gravity. Since
$U = \frac{Q}{A}$ and both area and width are functions of flow depth,
at the critical flow depth *Y~C~* the following relation holds:

  ------------------------------------------------------------------------------------------------
        $$\frac{{A\left( Y_{C} \right)}^{3}}{W\left( Y_{C} \right) = \frac{Q^{2}}{g}}$$   (5-31)
  ----- --------------------------------------------------------------------------------- --------

  ------------------------------------------------------------------------------------------------

*Y~C~* can be computed explicitly for several simple conduit shapes. The
formulas are listed in Table 5-14. Other shapes require that an
iterative root finding procedure be applied to the following re-arranged
form of Equation 5-29:

  --------------------------------------------------------------------------
        $$\frac{{f(Y) = A(Y)}^{3}}{W(Y) - \frac{Q^{2}}{g} = 0}$$    (5-32)
  ----- ----------------------------------------------------------- --------

  --------------------------------------------------------------------------

Because analytical derivatives of *f(Y)* are not available for most
shapes, derivative-free methods are used instead of the Newton-Raphson
method. Two such methods are interval enumeration and Ridder's method
(Press et al., 1992). Ridder's method is a variation on the method of
false position. The user supplies a set of depths *Y~1~* and *Y~2~* that
bracket *Y~C~* along with a stopping tolerance *ε*. The full algorithm
is described in Appendix B.

  ------------------------------------------------------------------------------------------------------------------------------------------------------------
  **Shape**        **Formula**                                                                                             **Remarks**
  ---------------- ------------------------------------------------------------------------------------------------------- -----------------------------------
  Rectangular^1^   $$Y_{C} = \left( \frac{Q^{2}}{gb^{2}} \right)^{1/3}$$                                                   *b* = width

  Triangular^1^    $$Y_{C} = \left( \frac{2Q^{2}}{gs^{2}} \right)^{1/5}$$                                                  *s* = side slope

  Parabolic^2^     $$Y_{C} = \left( \frac{27\alpha Q^{2}}{32g} \right)^{1/4}$$                                             Perimeter Equation:
                                                                                                                           $y = \alpha x^{2}$

  Power Law^2^     $$Y_{C} = \left( \frac{(1 + \gamma)^{3}\alpha^{2\gamma}Q^{2}}{4g} \right)^{\frac{1}{(3 + 2\gamma)}}$$   Perimeter Equation:
                                                                                                                           $y = \alpha x^{\frac{1}{\gamma}}$
  ------------------------------------------------------------------------------------------------------------------------------------------------------------

  : []{#_Toc484690432 .anchor}**Table 5‑14 Critical depth formulas for
  simple section shapes**

^1^French (1985).

^2^Swamee (1993).

With interval enumeration the full depth of the cross section is divided
into *N* equal intervals (SWMM 5 currently uses *N* = 25). Given a flow
*Q* and an initial estimate of its critical depth *Y~C~*, the following
steps are used to calculate its actual value:

1.  Let *i* be the integer part of $N\frac{Y_{C}}{Y_{full}}$ and set
    $Y = i\frac{Y_{full}}{N}$.

2.  Find $Q_{0} = \sqrt{g\frac{{A(Y)}^{3}}{W(Y)}}$.

3.  If $Q_{0} < Q$:

    a.  Set $i = i + 1$, $Y = i\frac{Y_{full}}{N}$, and
        $Q_{C} = \sqrt{g\frac{{A(Y)}^{3}}{W(Y)}}$.

    b.  If $Q_{C} \geq Q$ then stop with
        $Y_{C} = \left\lbrack \frac{\left( Q - Q_{0} \right)}{\left( Q_{C} - Q_{0} \right) + (i - 1)} \right\rbrack\left( \frac{Y_{full}}{N} \right)$.

    c.  Set $Q_{0} = Q_{C}$ and go to step a.

4.  Otherwise:

    a.  Set $i = i - 1$, $Y = i\frac{Y_{full}}{N}$, and
        $Q_{C} = \sqrt{g\frac{{A(Y)}^{3}}{W(Y)}}$.

    b.  If $Q_{C} < Q$ then stop with
        $Y_{C} = \left\lbrack \frac{\left( Q - Q_{C} \right)}{\left( Q_{0} - Q_{C} \right) + i} \right\rbrack\left( \frac{Y_{full}}{N} \right)$.

    c.  Set $Q_{0} = Q_{C}$ and go to step a.

Empirical testing has shown that the interval enumeration method tends
to use less iterations than Ridder's method when:

1.  The ratio of the section's full area to that of a circular section
    of same full depth is between 0.5 and 2.0

2.  The initial estimate of *Y~C~* is computed from the following
    approximation for circular sections (French 1985):

  -------------------------------------------------------------------------------------------------
        $$Y_{C} = 1.01\frac{\left( \frac{Q^{2}}{g} \right)^{0.25}}{Y_{full}^{0.26}}$$   (5-33)
  ----- ------------------------------------------------------------------------------- -----------

  -------------------------------------------------------------------------------------------------

Therefore interval enumeration is used when the first condition listed
above holds, with the second condition used to set the initial estimate
of *Y~C~*. Otherwise Ridder's method is used with Equation 5-30 as the
function whose root *Y~C~* is to be found with a convergence tolerance
of 0.001 feet. The initial bracket \[*Y~1~ , Y~2~*\] on *Y~C~* is
determined as follows:

1.  Let $Y_{1/2} = 0.5Y_{full}\ $and *Y~0~* be the value computed by
    Equation 5-31 above.

2.  Compute $Q_{0} = \sqrt{g\frac{{A(Y_{0})}^{3}}{W(Y_{0})}}$ and
    $Q_{1/2} = \sqrt{g\frac{{A(Y_{1/2})}^{3}}{W(Y_{1/2})}}$ .

3.  If $Q_{0} > Q$ then:

    a.  Set $Y_{2} = Y_{0}$.

    b.  If $Q_{1/2} < Q$ then set $Y_{1} = Y_{1/2}$, otherwise set
        $Y_{1} = 0$.

4.  Otherwise:

    a.  Set $Y_{1} = Y_{0}$.

    b.  If $Q_{1/2} > Q$ then set $Y_{2} = Y_{1/2}$, otherwise set
        $Y_{2} = 0.99Y_{full}$.

### 5.6.2 Normal Depth

Normal depth is defined as the flow depth that results in a given
uniform flow rate along a conduit. When the Manning equation is used to
describe uniform flow, the relation between flow rate *Q* and normal
depth *Y~N~* is:

  ----------------------------------------------------------------------------------------
        $$A(Y_{N}){R(Y_{N})}^{2/3} = \frac{Q\eta}{\sqrt{S_{0}}}$$                 (5-34)
  ----- ----------------------------------------------------------- ------------- --------

  ----------------------------------------------------------------------------------------

where *η is* the Manning roughness expressed in US units and *S~0~* is
the conduit's slope. From the definition of the section factor *Ψ*
introduced in Chapter 4, Equation 5-32 can be written as:

  --------------------------------------------------------------------------
        $$\Psi = \frac{Q\eta}{\sqrt{S_{0}}}$$                       (5-33)
  ----- --------------------------------------------- ------------- --------

  --------------------------------------------------------------------------

To find *Y~N~* for flow rate *Q* one first computes *Ψ* from Equation
5-33, then finds the flow area *A* that produces this value of *Ψ* using
the methods described in section 5.1.8 and finally evaluates the depth
that produces this area using the *Y(A)* function for the particular
shape being analyzed. In equation terms:

  ------------------------------------------------------------------------------------------------------------
        $$Y_{N} = Y\left( A\left( \Psi = \frac{Q\eta}{\sqrt{S_{0}}} \right) \right)$$                 (5-35)
  ----- ------------------------------------------------------------------------------- ------------- --------

  ------------------------------------------------------------------------------------------------------------

# - Pumps and Regulators

\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_

Pumps are used in stormwater and wastewater collection systems to lift
water to a higher elevation so that gravity flow at a reasonable
velocity can be maintained. They are also used to produce pressurized
flow within force mains. Regulators act like valves that restrict the
flow rate along a conduit or out of a storage unit. They can also serve
as diverters that split flow between different branches of a conveyance
system (e.g., between the interceptor and an overflow pipe in a combined
sewer system). Specific types of regulators include orifices, weirs, or
general outlets that differ in their geometry and relationship between
flow and head. This chapter describes how the flow rate through pumps
and regulators is computed for both the dynamic and kinematic wave
models.

## 6.1 Pumps

SWMM treats pumps as links that have a pre-defined relationship between
flow rate *Q* and head *H* or some suitable surrogate. This relationship
is defined by a user-supplied Pump Curve. Table 6-1 depicts the five
types of pump curves that SWMM recognizes. Although not a requirement, a
pump's inlet node would typically be a storage node that represents a
pump station's wet well. An exception would be an inline booster pump
placed inside a force main line under dynamic wave analysis. A sixth
type of pump, called an Ideal pump, does not use a pump curve but
instead has its flow rate equal the inflow rate into its inlet node. It
must be the only outflow link from its inlet node and is used mainly for
preliminary design.

A single point on a Type1 or Type2 curve would typically represent an
operating point for a constant flow positive displacement pump.
Additional points might represent flow rates at different pump speeds or
contributions from additional constant speed pumps running in parallel.
The Type3 curve represents the characteristic curve of a centrifugal
pump operating at some fixed speed, where there is a continuous range of
flows available depending on the head required. The Type4 curve could be
a positive displacement pump with continuous speed control or a
centrifugal pump that lifts water to a more or less fixed elevation so
that the required head depends only on the water level at its inlet
node. A Type5 pump is a variable speed version of the Type3 pump. As the
pump's impeller speed varies relative to some nominal value, flow
changes in direct proportion while head changes in proportion to speed
squared.

Whenever a pump link is encountered in either the dynamic wave or
kinematic wave methods its new flow is found directly from its pump
curve using whatever values were last computed for nodal heads and
volumes.

+--------------------------------------+------------------------------------------------------------------------+
| [Type1]{.underline}                  | > ![pump1.bmp](VolumeII/media/media/image40.png){width="1.5312489063867016in" |
|                                      | > height="1.3897747156605424in"}                                       |
| Consists of a series of constant     |                                                                        |
| flow rates that apply over a         |                                                                        |
| corresponding series of volume       |                                                                        |
| intervals at the pump's inlet node.  |                                                                        |
+:=====================================+:=======================================================================+
| [Type2]{.underline}                  | > ![pump2.bmp](VolumeII/media/media/image41.png){width="1.5833311461067368in" |
|                                      | > height="1.433332239720035in"}                                        |
| Similar to a Type1 pump except that  |                                                                        |
| the fixed flow rate levels vary over |                                                                        |
| a set of depth intervals at the      |                                                                        |
| pump's inlet node.                   |                                                                        |
+--------------------------------------+------------------------------------------------------------------------+
| [Type3]{.underline}                  | > ![pump3.bmp](VolumeII/media/media/image42.png){width="1.5784765966754155in" |
|                                      | > height="1.3541655730533684in"}                                       |
| A centrifugal pump characteristic    |                                                                        |
| curve at some nominal impeller speed |                                                                        |
| represented in a piecewise linear    |                                                                        |
| fashion. Flow is a function of the   |                                                                        |
| head difference between the inlet    |                                                                        |
| and outlet nodes.                    |                                                                        |
+--------------------------------------+------------------------------------------------------------------------+
| [Type4]{.underline}                  | > ![pump4.bmp](VolumeII/media/media/image43.png){width="1.5121522309711286in" |
|                                      | > height="1.395832239720035in"}                                        |
| A variable speed in-line pump where  |                                                                        |
| flow varies continuously with inlet  |                                                                        |
| node depth.                          |                                                                        |
+--------------------------------------+------------------------------------------------------------------------+
| [Type 5]{.underline}                 | > ![Pump5.bmp](VolumeII/media/media/image44.png){width="1.531058617672791in"  |
|                                      | > height="1.313487532808399in"}                                        |
| A variable speed version of the      |                                                                        |
| Type3 pump where the head v. flow    |                                                                        |
| curve shifts position depending on   |                                                                        |
| the pump's speed setting.            |                                                                        |
+--------------------------------------+------------------------------------------------------------------------+

: []{#_Toc484690433 .anchor}**Table 6‑1 Pump curves recognized by SWMM**

For Type1 and Type 2 curves, the curve is searched in step-wise fashion
for the first point whose volume or depth exceeds the volume or depth at
the pump's inlet node. The pump's flow is the flow associated with that
point. For the Type3 and Type5 curves, the flow is determined by first
finding the pair of adjacent data points that bracket the difference in
head between the pump's outlet and inlet nodes and then interpolating a
flow between these points for the given head difference. A similar
lookup procedure is used for the Type4 curve except that water level at
the pump's inlet node is used instead of head difference. A pump's flow
is not allowed to be outside the minimum and maximum values defined by
its pump curve and is not allowed to be negative.

The Type5 pump curve is shifted depending on what relative speed setting
*ω* the pump is currently operating under, where a setting of 1.0
applies to the original user-supplied curve. Following the pump affinity
laws (Sanks et al., 1998), a point with head *H* and flow *Q* on the
original curve becomes $\omega^{2}H$ and $\omega Q$, respectively on the
speed-adjusted curve. For the other pump types only the flow value found
from the original curve is multiplied by the speed setting.

Speed settings can be changed during a simulation by using control
rules. The setting can also be used to control pump operation based on
wet well level (e.g., set *ω* to 1 when the level is above a startup
depth and to 0 when below a shutoff depth). The adjusted pump flow is
checked to insure it does not cause the water level at the inlet node to
drop below 0 over the current time step. If the node is a storage node
then the pumping rate cannot exceed *Q~max~* where

  ------------------------------------------------------------------------------------
        $$Q_{\max} = Q_{in} + \frac{V_{N}}{\mathrm{\Delta}t}$$                 (6-1)
  ----- -------------------------------------------------------- ------------- -------

  ------------------------------------------------------------------------------------

and *Q~in~* is the most recently computed total inflow to the node,
*V~N~* is the node volume at the start of the time step, and *∆t* is the
current time step interval. If the inlet node is not a storage node and
dynamic wave analysis is being made, Equation 3-15a is used with the
current pumping rate to estimate what the inlet node head at the end of
the time step would be. If this head is below the node's invert
elevation then the pumping rate is set equal to the node's current
inflow.

Some additional computational details regarding pumps are as follows:

1.  If the inlet node of a Type1 (flow v. volume) pump is not a storage
    node then it is assigned a virtual wet well whose volume varies
    linearly with depth up to the highest volume on the pump curve at
    full node depth. While the normal non-storage node methods are used
    to update the node's water level, the virtual wet well volume
    corresponding to the node's water level is used to determine the
    pumping rate. Equation 6-1 is also used to limit the pump flow to
    the maximum flow that the node can release.

2.  For dynamic wave modeling:

    a.  Pumps do not contribute any surface area to the node-link
        assemblies at their inlet and outlet nodes.

    b.  For Type3, Type4 and Type5 pump curves the
        $\frac{\partial Q}{\partial H}$ term used for evaluating a
        surcharged node is the negative of the slope of the line segment
        on which the pumping rate lies. For the other pump types it is
        zero since their line segments have zero slope.

    c.  No under-relaxation is applied to consecutive pump flows at Step
        3 of the iterative solution method described in Section 3.2.

3.  SWMM computes the power consumed in kilowatt-hours by each pump over
    each time step *∆t* as:

  -------------------------------------------------------------------------------------------------------------------------------
        $$Kwh = 0.7457\left( H_{2} - H_{1} \right)\frac{Q\left( \frac{\mathrm{\Delta}t}{3600} \right)}{8.814}$$   (6-2)
  ----- --------------------------------------------------------------------------------------------------------- ---------------

  -------------------------------------------------------------------------------------------------------------------------------

> where heads *H~1~* and *H~2~* are in feet, flow *Q* is in cfs, and
> time step *∆t* is in seconds. The pump's wire to water efficiency is
> not included in this calculation. The power consumption in each time
> period is totaled up and reported for each pump in SWMM's Pumping
> Summary Report. Also reported are the percent of time each pump is
> online and operates at either the lower or upper end of its pump
> curve.

## 6.2 Orifices

Orifices are regularly shaped, submerged openings through which flow is
proportional to the square root of the head across the opening. Orifices
are typically used to:

- regulate flow out of detention ponds and other storage facilities

- regulate flow through channels in the form of sluice gates

- divert excess flow from interceptor sewers to overflow structures

- model storm drain inlets.

### 6.2.1 Representation

SWMM represents an orifice as a link between two nodes. The opening can
be oriented either in a vertical plane for a side orifice or in a
horizontal plane for a bottom orifice (see Figure 6-1) and be elevated
some distance above the inlet node's invert. A riser pipe or inlet box
used as an outlet structure in a detention pond can be modeled as a
bottom orifice with a vertical offset. For kinematic wave analysis the
inlet node must be a storage node since this is the only type of node
for which a true hydraulic head is computed. For dynamic wave analysis
it can be any type of node.

<figure>
<img src="VolumeII/media/media/image45.png"
style="width:4.59318in;height:1.81227in" alt="Orifice2.png" />
<figcaption><p><span id="_Toc484694735"
class="anchor"></span><strong>Figure 6‑1 Orifice
orientations</strong></p></figcaption>
</figure>

The properties of an orifice link include:

- the height of its opening above the invert of its upstream node

- the shape of its opening which can be either circular or rectangular

- the dimensions of its opening (the diameter for a circular orifice or
  the height and width for a rectangular orifice)

- its discharge coefficient (described in more detail below)

- whether or not it contains a flap gate that prevents reverse flow.

The size of the orifice opening can be changed during a simulation by
having its setting adjusted using control rules. An orifice's setting is
the fraction of its full height that remains open (such as would occur
due to the action of lowering or raising a sluice gate above a side
orifice). In this case an additional optional parameter is the time it
takes to fully close a completely open (or fully open a completely
closed) orifice.

### 6.2.2 Flow Rate for Submerged Inlet

Whenever an orifice link is encountered in the dynamic wave or kinematic
wave solution procedure with its inlet side fully submerged its flow
rate *Q* (cfs) can be found using Torricelli's equation (Brater et al.,
1996):

  -------------------------------------------------------------------------
        $$Q = C_{d}A_{O}\sqrt{2gH_{e}}$$                            (6-3)
  ----- --------------------------------------------- ------------- -------

  -------------------------------------------------------------------------

where *C~d~* is a dimensionless orifice discharge coefficient, *A~O~* is
the area of the opening (ft^2^), and *He* is the effective head seen by
the orifice (ft). The following paragraphs describe how each of these
parameters is evaluated.

[Discharge Coefficient (*C~d~*)]{.underline}

The most commonly cited value for *C~d~* is 0.6 while 0.4 is recommended
for ragged edge orifices (Federal Highway Administration, 2009). Brater
et al. (1996) review a number of experimental studies that show the
coefficient varying between 0.59 and 0.67 depending on orifice shape,
size, and effective head.

[Area of Opening (*A~O~*)]{.underline}

The area of the orifice's opening depends on what its setting is. Let
*ω* be the orifice setting (between 0 and 1) in place at the end of the
previous routing time step and *ω\** be the target setting that was
established the last time that a control rule involving the orifice was
activated. If the time to close/open the orifice, *∆t~O\ ~*, is 0 then
for the current time step ω = *ω\**. Otherwise let *∆ω* be defined as
*ω\* - ω* and *ω* gets updated as follows:

+---+--------------------------------------------------------------+-----------------------------------------------------------+-------+
|   | $$\omega = \left\{ \begin{matrix}                            | if                                                        | (6-4) |
|   | \omega + \frac{sgn(\Delta\omega)\Delta t}{{\Delta t}_{O}} \\ | $\frac{\Delta t}{{\Delta t}_{O}} < \mathrm{\Delta}\omega$ |       |
|   | \omega^{*}                                                   |                                                           |       |
|   | \end{matrix} \right.\ $$                                     | otherwise                                                 |       |
+===+==============================================================+===========================================================+=======+

where *∆t* is the length of the current time step. With the setting
established, the area of the orifice opening is determined using the
methods described in Chapter 5 to find the area of either a circular or
rectangular cross section, depending on orifice shape, at a fraction *ω*
of its full height.

[Effective Head (*H~e~*)]{.underline}

The effective head across the orifice depends on whether the water level
on its outflow side is below the orifice opening or not. Let *H~1~* be
the most recently computed head at the orifice's nominal upstream node
and *H~2~* be the same at the nominal downstream node. For kinematic
wave analysis, since the upstream node must be a storage node, *H~1~* is
the water surface elevation in the storage unit while *H~2~* is the
invert elevation of the downstream node. If $H_{1} < H_{2}$ and the
orifice does not have a flap gate then the head values are reversed (so
*H~1~* has the higher value) and the computed flow will be opposite to
the nominal downstream direction.

With *H~1~* and *H~2~* established the following rules are used to
determine the effective head across the orifice, where *Z~O~* is the
elevation of the bottom of the orifice opening and *Y~full~* is its full
height:

1.  For a side orifice:

+---+------------------------------------------------------------+--------------------------------------------+-------+
|   | $$H_{e} = \left\{ \begin{matrix}                           | for                                        | (6-5) |
|   | H_{1} - \left( Z_{O} + \omega\frac{Y_{full}}{2} \right) \\ | $H_{2} < Z_{O} + \omega\frac{Y_{full}}{2}$ |       |
|   | H_{1} - H_{2}                                              |                                            |       |
|   | \end{matrix} \right.\ $$                                   | otherwise                                  |       |
+===+============================================================+============================================+=======+

2.  For a bottom orifice:

+---+----------------------------------+--------------------------------------------+-------+
|   | $$H_{e} = \left\{ \begin{matrix} | for $H_{2} \leq Z_{O}$                     | (6-6) |
|   | H_{1} - Z_{O} \\                 |                                            |       |
|   | H_{1} - H_{2}                    | otherwise                                  |       |
|   | \end{matrix} \right.\ $$         |                                            |       |
+===+==================================+============================================+=======+

Figure 6-2 illustrates how *H~e~* is evaluated for a side orifice.

### 6.2.3 Flow Rate for Unsubmerged Inlet

When the water level at the inlet to a side orifice is below the top of
its opening the orifice behaves more like a weir and Equation 6-3 no
longer applies (see Figure 6-3). A similar situation occurs when the
head above a bottom orifice is below some threshold level. For these
cases SWMM determines what the threshold head for weir behavior is and
what the equivalent weir coefficient and crest length should be when
using the standard rectangular weir formula to compute the orifice's
flow rate. The details differ for side and bottom orifices as described
below.

  --------------------------------------------------------------------------------------------------------------------------------------------------
   ![Orifice4.png](VolumeII/media/media/image46.png){width="3.518180227471566in"   ![Orifice5.png](VolumeII/media/media/image47.png){width="3.5210892388451445in"
                       height="2.440871609798775in"}                                             height="2.442891513560805in"}
  ------------------------------------------------------------------------ -------------------------------------------------------------------------
                          Submerged Upstream Only                                              Submerged Both Up and Downstream

  --------------------------------------------------------------------------------------------------------------------------------------------------

[]{#_Toc484694736 .anchor}**Figure 6‑2 Determination of effective head
for an orifice**

<figure>
<img src="VolumeII/media/media/image48.png"
style="width:4.03288in;height:2.79833in" alt="Orifice6.png" />
<figcaption><p><span id="_Toc484694737"
class="anchor"></span><strong>Figure 6‑3 Orifice with unsubmerged
inlet</strong></p></figcaption>
</figure>

[Side Orifices]{.underline}

For a side orifice, weir behavior occurs when the inlet water level is
below the top of the orifice opening. Thus the threshold head *H\** is:

  -------------------------------------------------------------------------
        $$H^{*} = Z_{O} + \omega Y_{full}$$                         (6-7)
  ----- --------------------------------------------- ------------- -------

  -------------------------------------------------------------------------

When the inlet head *H~1~* is below this height the flow through the
orifice can be approximated by using the general weir formula:

  ------------------------------------------------------------------------------
        $$Q = C_{W}L\left( H_{1} - Z_{O} \right)^{1.5}$$                 (6-8)
  ----- -------------------------------------------------- ------------- -------

  ------------------------------------------------------------------------------

where *C~W~* is a weir coefficient (ft^1/2^/sec) and *L* is the crest
length of the equivalent weir (ft). Equating the flow from this equation
to that from the orifice equation 6-3 when $H_{1} = H^{*}$ and solving
for *C~W~L* results in:

  -------------------------------------------------------------------------------------
        $$C_{W}L = \frac{C_{d}A_{O}\sqrt{g}}{\omega Y_{full}}$$                 (6-9)
  ----- --------------------------------------------------------- ------------- -------

  -------------------------------------------------------------------------------------

Thus whenever the upstream head *H~1~* is below *H\**, flow through the
side orifice can be found using the weir formula 6-8 with *C~W~L* given
by Equation 6-9.

[Bottom Orifices]{.underline}

For a bottom orifice it is assumed that the threshold inlet head *H\**
for weir flow will be at a point where the flow through the orifice
using both the orifice and general weir equations will be the same. In
equation terms:

  ---------------------------------------------------------------------------------------------------------------------
        $$C_{d}A_{O}\sqrt{2g}\left( H^{*} - Z_{O} \right)^{0.5} = C_{W}L\left( H^{*} - Z_{O} \right)^{1.5}$$   (6-10)
  ----- ------------------------------------------------------------------------------------------------------ --------

  ---------------------------------------------------------------------------------------------------------------------

Solving for *H\** results in:

  --------------------------------------------------------------------------
        $$H^{*} = Z_{O} + \frac{C_{d}A_{O}\sqrt{2g}}{C_{W}L}$$      (6-11)
  ----- ----------------------------------------------------------- --------

  --------------------------------------------------------------------------

In order to evaluate *H\** values for *C~W~* and *L* must be assigned.
*C~W~* can be set to the commonly cited value of 3.33 ft^0.5^/sec used
for sharp crested weirs (Mays, 2001). *L* can be set to the
circumference of the opening as follows:

+---+---------------------------------------+------------------------------------------+--------+
|   | $$L = \left\{ \begin{array}{r}        | for a circular opening                   | (6-12) |
|   | \pi\omega Y_{full\ \ } \\             |                                          |        |
|   | 2\left( b + \omega Y_{full} \right)\  | for a rectangular opening                |        |
|   | \end{array} \right.\ $$               |                                          |        |
+===+=======================================+==========================================+========+

where *b* is the fixed width of the rectangular opening. Now *H\** can
be determined for a given orifice coefficient and opening dimensions.
Whenever the upstream head *H~1~* is below *H\**, flow through the
bottom orifice can be found using the general weir formula 6-8 with
*C~W~* = 3.33 and *L* given by Equation 6-12.

[Tailwater Submergence Correction]{.underline}

As described later on in Section 6.3, whenever the downstream water
level is above a weir's crest the Villemonte equation is applied to
account for the effects of submergence (Brater et al., 1996). So when
the general weir equation 6-8 is used to compute orifice flow and the
downstream head *H~2~* is above the bottom of the orifice opening
*Z~O~*, the following submergence adjustment factor *f~S~* is applied to
the computed flow value:

  ------------------------------------------------------------------------------------------------------------
        $$f_{S} = \left\lbrack 1 - \left( \frac{H_{2}}{H_{1}} \right)^{1.5} \right\rbrack^{0.385}$$   (6-13)
  ----- --------------------------------------------------------------------------------------------- --------

  ------------------------------------------------------------------------------------------------------------

### 6.2.4 Flap Gate Head Loss Adjustment

When an orifice has a flap gate it adds a small amount of head loss for
flow through the gate. An empirical formula for this head loss was
derived from experiments performed at Iowa State University in the
1930's and published by Armco (1978):

  -------------------------------------------------------------------------------------------------------
        $$\mathrm{\Delta}H = \frac{4U^{2}}{g}\exp\left( - 1.15\frac{U}{\sqrt{H_{e}}} \right)$$   (6-14)
  ----- ---------------------------------------------------------------------------------------- --------

  -------------------------------------------------------------------------------------------------------

where *∆H* is the head loss added by the flap gate (ft) and *U* is the
velocity through the orifice (ft/sec) which equals $\frac{Q}{A_{O}}$.
After the orifice's flow is first computed without this additional head
loss, *∆H* is computed with Equation 6-14 and subtracted from *H~e~*.
Then the flow is re-computed, this time using the adjusted value of
effective head.

### 6.2.5 Dynamic Wave Considerations

Dynamic wave modeling uses the surface area of the links attached to a
node to update the node's head when it is not in a surcharged state (see
Chapter 3). As an orifice has no length, its contribution to a node's
surface area should be zero. However in older versions of SWMM an
orifice was represented as an equivalent pipe that contributed surface
area to its end nodes just as a real conduit did. To maintain
compatibility with previous versions, SWMM 5 computes a surface area
*A~SL~* for an orifice as

+---+-------------------------------------+--------------------------------------------+--------+
|   | $$A_{SL} = \left\{ \begin{array}{r} | for a side orifice                         | (6-15) |
|   | W(Y_{O})L_{O} \\                    |                                            |        |
|   | A\left( \omega Y_{full} \right)\    | for a bottom orifice                       |        |
|   | \end{array} \right.\ $$             |                                            |        |
+===+=====================================+============================================+========+

where:

  -------------------------------------------------------------------------------
  *Y~O~*      =   depth of flow through the orifice (ft), equal to
                  $\min\left( H_{1} - Z_{O}\ ,\ \omega Y_{full} \right)$
  ----------- --- ---------------------------------------------------------------
  *L~O~*      =   equivalent conduit length of the orifice (ft), equal to
                  $\max\left( 2{\Delta t}_{\max}\sqrt{gY_{full}}\ ,200 \right)$

  *∆t~max~*   =   maximum time step assigned by the user to the simulation (sec)

  *W(Y)*      =   width of orifice opening at flow depth *Y* (ft)

  *A(Y)*      =   area of orifice opening at flow depth *Y* (ft)
  -------------------------------------------------------------------------------

*W(Y)* and *A(Y)* are evaluated using the formulas from Chapter 5 for
either a circular or closed rectangular cross section shape. Half of
*A~SL~* is assigned to each of the orifice's end nodes providing that
the node is not a storage unit nor has its head below the orifice
opening.

Dynamic wave analysis also needs a value for the derivative of a link's
flow rate with respect to head (*dQ/dH*) when updating the head for a
surcharged node connected to the link (see section 3.3.5). For submerged
headwater orifices that use Equation 6-3 to compute flow rate *Q*, this
derivative is:

  --------------------------------------------------------------------------
        $$\frac{dQ}{dH} = 0.5\frac{Q}{H_{e}}$$                      (6-16)
  ----- ----------------------------------------------------------- --------

  --------------------------------------------------------------------------

while for unsubmerged headwater orifices that use Equation 6-8 to
compute *Q* it is:

  ------------------------------------------------------------------------------
        $$\frac{dQ}{dH} = 1.5\frac{Q}{\left( H_{1} - Z_{O} \right)}$$   (6-17)
  ----- --------------------------------------------------------------- --------

  ------------------------------------------------------------------------------

### 6.2.6 Summary of Orifice Computations

The computational steps used to compute flow through an orifice link can
be summarized as follows. At the start of a time step:

1.  If the orifice setting has not yet reached its target value or the
    target value has been changed by a control rule then update the
    setting using Equation 6-4.

2.  If the orifice setting has changed then compute the effective area
    of its opening *A~O~*. For side weirs use Equation 6-7 to compute
    its critical head *H\** for weir behavior and Equation 6-9 to
    compute its equivalent weir constant *C~W~L*. For bottom weirs, use
    Equation 6-12 to find an equivalent weir length *L*, Equation 6-11
    to find the critical head *H\**, and set the equivalent weir
    constant to 3.33*L*.

For each iteration within a time step that requires computing flow
through the orifice:

1.  Let *H~1~* denote the most recently computed head at the orifice's
    upstream node and *H~2~* be the same at the downstream node. (For
    kinematic wave analysis *H~2~* is the downstream node's invert
    elevation.)

2.  If *H~1~ \< H~2~* reverse the values so that *H~1~* as the higher
    head and note that reverse flow will occur. If the orifice has a
    flap gate or *H~1~* is below the orifice opening then set its flow
    to 0.

3.  If the orifice is not submerged on its upstream side (*H1 \< H\**)
    then use Equation 6-8 to find its flow rate along with Equation 6-13
    to correct for any tailwater submergence. Otherwise use Equation 6-5
    (for side orifices) or 6-6 (for bottom orifices) to find the
    effective head *H~e~* on the orifice and then use Equation 6-3 to
    compute its flow rate.

4.  If the orifice has a flap gate then use Equation 6-14 to reduce its
    effective head and repeat the flow calculation of step 2.

5.  If the orifice has reverse flow then make the computed flow
    negative.

6.  Under dynamic wave analysis use Equation 6-15 to assign a surface
    area to the orifice and use Equation 6-16 (for side orifices) or
    6-17 (for bottom orifices) to compute $\frac{dQ}{dH}$ for the
    orifice.

## 6.3 Weirs

A transverse weir is a barrier with a cut-out placed across a conduit
perpendicular to the direction of flow. A side weir is a cut-out along
the side wall of a conduit parallel to the direction of flow. Flow
through a weir is proportional to the height of water above the weir's
crest raised to a power greater than one. Weirs are used for the same
types of reasons as orifices: to regulate flow out of storage
facilities, to regulate flow through channels, and to divert excess flow
from interceptor sewers to overflow structures. While orifices normally
operate with their inlet sides submerged, weirs normally maintain a free
surface above them.

### 6.3.1 Representation

SWMM represents a weir as a link between two nodes. For kinematic wave
analysis the inlet node must be a storage node since this is the only
type of node for which a true hydraulic head is computed. For dynamic
wave analysis it can be any type of node.

The properties of a weir link include:

- the height of its crest above the invert of its upstream node

- its orientation (transverse or side flow)

- the shape and dimensions of its opening

- the number of end contractions

- its effective weir coefficient

- whether or not it contains a flap gate that prevents reverse flow.

Figure 6-4 shows the different shapes of transverse weirs modeled by
SWMM. The only shape allowed for a side weir is rectangular.

<figure>
<img src="VolumeII/media/media/image49.png"
style="width:6.5in;height:3.18056in" alt="Weirs1.png" />
<figcaption><p><span id="_Toc484694738"
class="anchor"></span><strong>Figure 6‑4 Transverse weir
shapes</strong></p></figcaption>
</figure>

A suppressed rectangular weir has its opening extended across the entire
channel while a contracted weir does not. Weirs are also classified as
being sharp-crested or broad-crested. Sharp-crested weirs have a
relatively short crest thickness so that water springs clear of the
crest as it flows over the weir. The crest of a broad-crested weir is
thick enough so that the overflow remains in contact with the crest
surface.

The elevation of a weir's crest can be changed during a simulation by
having its setting adjusted using control rules. A weir's setting *ω* is
the fraction of its full height that remains open after it's crest is
moved up or down, as might occur with a downward opening weir gate or
inflatable dam. At a setting of 1 the weir's crest position is at its
lowest possible value and the full height of its opening is available
for flow. At a value of 0 the crest has been raised so that no opening
height remains and no flow can pass through the weir. At intermediate
settings the crest elevation equals its lowest possible value plus 1 -
*ω* times its full opening height.

### 6.3.2 Transverse Weirs

[General Equations]{.underline}

The general equation for free flow over a transverse rectangular weir is
(Brater et al., 1996):

  --------------------------------------------------------------------------
        $$Q = C_{W}L_{e}H_{e}^{3/2}$$                               (6-18)
  ----- ----------------------------------------------------------- --------

  --------------------------------------------------------------------------

and for a triangular weir is (Brater et al., 1996):

  ----------------------------------------------------------------------------
        $$Q = C_{W}\tan\left( \frac{\theta}{2} \right)H_{e}^{5/2}$$   (6-19)
  ----- ------------------------------------------------------------- --------

  ----------------------------------------------------------------------------

In these equations *Q* is the flow rate (cfs), *L~e~* is the effective
crest length (ft), *θ* is the slot angle of a triangular weir, *H~e~* is
the effective head seen by the inflow side of the weir (ft), and *C~W~*
is a weir coefficient (ft^1/2^/sec). A trapezoidal weir can be treated
as a combination of a rectangular weir and two half-triangular weirs
(Featherstone and Nalluri, 1982) leading to the equations:

  -------------------------------------------------------------------------
        $$Q = Q_{R} + Q_{T}$$                                     (6-20a)
  ----- --------------------------------------------------------- ---------

  -------------------------------------------------------------------------

  -------------------------------------------------------------------------
        $$Q_{R} = C_{WR}L_{e}H_{e}^{3/2}$$                        (6-20b)
  ----- --------------------------------------------------------- ---------

  -------------------------------------------------------------------------

  -------------------------------------------------------------------------
        $$Q_{T} = C_{WT}sH_{e}^{5/2}$$                            (6-20c)
  ----- --------------------------------------------------------- ---------

  -------------------------------------------------------------------------

where *s* is the slope (run / rise) of the trapezoidal side wall and
*C~WR~* and *C~WT~* are the coefficients that apply to the rectangular
and triangular portions of the weir, respectively.

[Effective Head]{.underline} (*H~e~*)

The effective head seen by a weir, accounting for its current setting,
is:

  --------------------------------------------------------------------------------
        $$H_{e} = H_{1} - \left( Z_{W} + (1 - \omega)Y_{full} \right)$$   (6-21)
  ----- ----------------------------------------------------------------- --------

  --------------------------------------------------------------------------------

where *H~1~* is the higher of the heads at the weir's end nodes, *Z~W~*
is the elevation of the weir's crest when fully open (i.e., when *ω* =
1), and *Y~full~* is the full height of the weir's opening. If *H~1~*
corresponds to the downstream node of the weir then reverse flow occurs
through the weir unless a flap gate is present in which case the flow is
0. Flow will also be 0 if $H_{e} \leq 0.$

[Effective Crest Length]{.underline} (*L~e~*)

The effective crest length of a rectangular weir is reduced by the
number of end contractions as follows (Mays, 2001):

  --------------------------------------------------------------------------
        $$L_{e} = L - 0.1nH_{e}$$                                   (6-22)
  ----- ----------------------------------------------------------- --------

  --------------------------------------------------------------------------

where *L* is the actual crest length and *n* = 1 if the weir is placed
away from one side wall, *n* = 2 if it is placed away from both side
walls and *n* = 0 if it occupies the entire width of the conduit (see
Figure 6-4).

When the setting *ω* for a triangular weir is less than 1 its opening
takes the shape of a trapezoidal weir. In this case the trapezoidal weir
equation 6-18 is used with both *C~WR~* and *C~WT~* set equal to the
weir's original coefficient, the side wall slope *s* set equal to
$\tan\left( \frac{\theta}{2} \right)$ and the effective length becomes:

  --------------------------------------------------------------------------
        $$L_{e} = 2s(1 - \omega)Y_{full}$$                          (6-23)
  ----- ----------------------------------------------------------- --------

  --------------------------------------------------------------------------

This equation is also used for a trapezoidal weir whose setting is less
than 1.

[Weir Coefficient]{.underline} (*C~W~*)

The standard weir coefficient *C~W~* for a sharp crested rectangular
weir is 3.33 ft^1/2^/sec (Mays, 2001). For
$\frac{H_{W}}{L} > \frac{1}{3}$ the coefficient has been found to vary
with effective head and weir sizing and placement (Bureau of
Reclamation, 2001). The Kindsvater-Carter method (Bureau of Reclamation,
2001) expresses this dependence with the following formula:

  --------------------------------------------------------------------------
        $$C_{W} = c1\left( \frac{H_{W}}{Z_{W}} \right) + c2$$       (6-24)
  ----- ----------------------------------------------------------- --------

  --------------------------------------------------------------------------

where the constants *c1* and *c2* vary with the ratio of the crest
length *L* to the full width *b* of the cross section in which the weir
is placed as listed in Table 6-2.

Broad crested weir behavior is considered to occur when the ratio of the
water level above the crest to the crest thickness exceeds a certain
limit. Limits of 1 to 2 have been proposed by Brater et al. (1996), 15
by French (1985), and 2 to 20 by the Bureau of Reclamation (2001). Table
6-3 is a compilation of broad-crested weir coefficients synthesized by
Brater and King (1976) from several different experimental studies. It
shows the dependence of the coefficient on both head and breadth of
crest. Above a ratio of about 2 the weir behaves as sharp-crested with a
coefficient of 3.32. For ratios below 0.5 the coefficient approaches
2.63.

  -----------------------------------------------------------------------
         ***L/b***        ***c1 (ft^1/2^/sec)***  ***c2 (ft^1/2^/sec)***
  ----------------------- ----------------------- -----------------------
            0.2                   -0.0087                  3.152

            0.4                   0.0317                   3.164

            0.5                   0.0612                   3.173

            0.6                   0.0995                   3.178

            0.7                   0.1602                   3.182

            0.8                   0.2376                   3.189

            0.9                   0.3447                   3.205

            1.0                   0.4000                   3.220
  -----------------------------------------------------------------------

  : []{#_Toc484690434 .anchor}**Table 6‑2 Kindsvater-Carter constants
  for rectangular weir coefficient**

The standard value for the triangular weir coefficient *C~W~* is 2.5
ft^1/2^/sec (Mays, 2001). Figure 6-5 shows the variation of *C~W~* (in
ft^1/2^/sec ) with head over the weir *H~W~* (in feet) presented by
Brater and King (1976). The range of coefficients is rather small, from
2.5 up to 2.8.

> ![CWT.png](VolumeII/media/media/image50.png){width="2.5626224846894137in"
> height="2.041411854768154in"}

[]{#_Toc484694739 .anchor}**Figure 6‑5 Coefficient for triangular weirs
(from Brater and King, 1976)**

+----------+--------------------------------------------------------------------------------------------------------------------------------------------------------------------+
| **Head** | **Breadth of Weir Crest (ft)**                                                                                                                                     |
|          |                                                                                                                                                                    |
| **(ft)** |                                                                                                                                                                    |
|          +--------------+--------------+--------------+--------------+--------------+--------------+--------------+--------------+--------------+--------------+--------------+
|          | **0.5**      | **0.75**     | **1.00**     | **1.5**      | **2.0**      | **2.5**      | **3.00**     | **4.00**     | **5.00**     | **10.00**    | **15.00**    |
+:========:+:============:+:============:+:============:+:============:+:============:+:============:+:============:+:============:+:============:+:============:+:============:+
| **0.2**  | 2.80         | 2.75         | 2.69         | 2.62         | 2.54         | 2.48         | 2.44         | 2.38         | 2.34         | 2.49         | 2.68         |
+----------+--------------+--------------+--------------+--------------+--------------+--------------+--------------+--------------+--------------+--------------+--------------+
| **0.4**  | 2.92         | 2.80         | 2.72         | 2.64         | 2.61         | 2.60         | 2.58         | 2.54         | 2.50         | 2.56         | 2.70         |
+----------+--------------+--------------+--------------+--------------+--------------+--------------+--------------+--------------+--------------+--------------+--------------+
| **0.6**  | 3.08         | 2.89         | 2.75         | 2.64         | 2.61         | 2.60         | 2.68         | 2.69         | 2.70         | 2.70         | 2.70         |
+----------+--------------+--------------+--------------+--------------+--------------+--------------+--------------+--------------+--------------+--------------+--------------+
| **0.8**  | 3.30         | 3.04         | 2.85         | 2.68         | 2.60         | 2.60         | 2.67         | 2.68         | 2.68         | 2.69         | 2.64         |
+----------+--------------+--------------+--------------+--------------+--------------+--------------+--------------+--------------+--------------+--------------+--------------+
| **1.0**  | 3.32         | 3.14         | 2.98         | 2.75         | 2.66         | 2.64         | 2.65         | 2.67         | 2.68         | 2.68         | 2.63         |
+----------+--------------+--------------+--------------+--------------+--------------+--------------+--------------+--------------+--------------+--------------+--------------+
| **1.2**  | 3.32         | 3.20         | 3.08         | 2.86         | 2.70         | 2.65         | 2.64         | 2.67         | 2.66         | 2.69         | 2.64         |
+----------+--------------+--------------+--------------+--------------+--------------+--------------+--------------+--------------+--------------+--------------+--------------+
| **1.4**  | 3.32         | 3.26         | 3.20         | 2.92         | 2.77         | 2.68         | 2.64         | 2.65         | 2.65         | 2.67         | 2.64         |
+----------+--------------+--------------+--------------+--------------+--------------+--------------+--------------+--------------+--------------+--------------+--------------+
| **1.6**  | 3.32         | 3.29         | 3.28         | 3.07         | 2.89         | 2.75         | 2.68         | 2.66         | 2.65         | 2.64         | 2.63         |
+----------+--------------+--------------+--------------+--------------+--------------+--------------+--------------+--------------+--------------+--------------+--------------+
| **1.8**  | 3.32         | 3.31         | 3.31         | 3.07         | 2.88         | 2.74         | 2.68         | 2.66         | 2.65         | 2.64         | 2.63         |
+----------+--------------+--------------+--------------+--------------+--------------+--------------+--------------+--------------+--------------+--------------+--------------+
| **2.0**  | 3.32         | 3.30         | 3.30         | 3.03         | 2.85         | 2.76         | 2.72         | 2.68         | 2.65         | 2.64         | 2.63         |
+----------+--------------+--------------+--------------+--------------+--------------+--------------+--------------+--------------+--------------+--------------+--------------+
| **2.5**  | 3.32         | 3.31         | 3.31         | 3.28         | 3.07         | 2.89         | 2.81         | 2.72         | 2.67         | 2.64         | 2.63         |
+----------+--------------+--------------+--------------+--------------+--------------+--------------+--------------+--------------+--------------+--------------+--------------+
| **3.0**  | 3.32         | 3.32         | 3.32         | 3.32         | 3.20         | 3.05         | 2.92         | 2.73         | 2.66         | 2.64         | 2.63         |
+----------+--------------+--------------+--------------+--------------+--------------+--------------+--------------+--------------+--------------+--------------+--------------+
| **3.5**  | 3.32         | 3.32         | 3.32         | 3.32         | 3.32         | 3.19         | 2.97         | 2.76         | 2.68         | 2.64         | 2.63         |
+----------+--------------+--------------+--------------+--------------+--------------+--------------+--------------+--------------+--------------+--------------+--------------+
| **4.0**  | 3.32         | 3.32         | 3.32         | 3.32         | 3.32         | 3.32         | 3.07         | 2.79         | 2.70         | 2.64         | 2.63         |
+----------+--------------+--------------+--------------+--------------+--------------+--------------+--------------+--------------+--------------+--------------+--------------+
| **4.5**  | 3.32         | 3.32         | 3.32         | 3.32         | 3.32         | 3.32         | 3.32         | 2.88         | 2.74         | 2.64         | 2.63         |
+----------+--------------+--------------+--------------+--------------+--------------+--------------+--------------+--------------+--------------+--------------+--------------+
| **5.0**  | 3.32         | 3.32         | 3.32         | 3.32         | 3.32         | 3.32         | 3.32         | 3.07         | 2.79         | 2.64         | 2.63         |
+----------+--------------+--------------+--------------+--------------+--------------+--------------+--------------+--------------+--------------+--------------+--------------+
| **5.5**  | 3.32         | 3.32         | 3.32         | 3.32         | 3.32         | 3.32         | 3.32         | 3.32         | 2.88         | 2.64         | 2.63         |
+----------+--------------+--------------+--------------+--------------+--------------+--------------+--------------+--------------+--------------+--------------+--------------+

: []{#_Toc484690435 .anchor}**Table 6‑3 Rectangular broad-crested weir
coefficients (ft^1/2^/sec)**

### 6.3.3 Rectangular Side Weirs

Flow through a rectangular side weir is a case of spatially varied flow
with decreasing discharge and varying flow depth with distance along the
weir. Mays (2001) cites a number of different studies that have
developed discharge equations for side weirs where both the head and
weir coefficient vary spatially. Unfortunately these approaches are too
complex to implement in a program like SWMM. The empirical Engels
equation (Metcalf & Eddy, Inc. 1972) is used instead:

  --------------------------------------------------------------------------
        $$Q = C_{W}L_{e}^{0.83}H_{e}^{1.67}$$                       (6-25)
  ----- ----------------------------------------------------------- --------

  --------------------------------------------------------------------------

Where flow *Q* is in cfs, length *L~e~* and head *H~e~* are in feet, and
*C~W~* is in ft^1/2^/sec. (It should be noted that previous versions of
SWMM used an incorrect form of this equation that had the exponent on
*L~e~* equal to 1.0)

Equation 6-25 applies to positive flow through the weir. For reverse
flow the standard rectangular weir equation 6-18 is used. *C~W~* was
assigned a value of 3.32 in the original Engels equation. Brunner (2014)
notes that side weir coefficients should be lower than the typical
values used for transverse weirs, and suggests a range of 1.5 to 2.6 for
weirs that model levees or roadways along natural channels.

### 6.3.4 Submerged Weir Flow 

As shown in Figure 6-6, submerged weir flow occurs when the water level
on the downstream side of the weir (*H~2~*) is above the crest elevation
(*Z~W~*). Under this condition weir flow is related not only to the head
on the upstream side of the weir (*H~1~*) but also to *H~2~* and *Z~W~*
(Brater et al., 1996). These effects are commonly accounted for by
applying an adjustment factor *f~S~* developed by Villemonte (1947) to
the flow computed using the free flow equation:

  ----------------------------------------------------------------------------------------------------------
        $$f_{S} = \left\lbrack 1 - \left( \frac{H_{2}}{H_{1}} \right)^{n} \right\rbrack^{0.385}$$   (6-26)
  ----- ------------------------------------------------------------------------------------------- --------

  ----------------------------------------------------------------------------------------------------------

where *n* is the exponent on head used in the weir flow equation and the
heads *H~1~* and *H~2~* are in feet. For transverse rectangular weirs
(Equation 6-18) it is 3/2, for side weirs (Equation 6-25) it is 1.67,
and for triangular weirs (Equation 6-19) it is 5/2. For trapezoidal
weirs separate submergence factors are computed for the rectangular flow
portion (*Q~R~* in Equation 6-20b with *n* = 3/2) and for the triangular
flow portion (*Q~T~* in Equation 6-20c with *n* = 5/2).

<figure>
<img src="VolumeII/media/media/image51.png"
style="width:4.87568in;height:2.33366in" alt="Weirs2b.png" />
<figcaption><p><span id="_Toc484694740"
class="anchor"></span><strong>Figure 6‑6 Definitions of submerged and
surcharged weir flow</strong></p></figcaption>
</figure>

### 6.3.5 Surcharged Weir Flow

As shown in Figure 6-4, the weirs modeled by SWMM assume that the top of
the flow opening extends to the top of the structure that houses the
weir. If this structure is an open channel then the highest head that
the weir can see is ${\omega Y}_{full}$ where *ω* reflects the weir's
current setting. If the structure encloses the weir from above, such as
in a sewer pipe, then the head on the upstream side of the weir can
exceed the structure's crown elevation causing the weir to become
surcharged (see Figure 6-6). In this case the weir acts as an orifice
and its flow should be evaluated using the equivalent of Equation 6-3:

  -----------------------------------------------------------------------------------
        $$Q = C_{d}A_{O}\sqrt{2gH_{e}} = C_{O}\sqrt{H_{e}}$$                 (6-27)
  ----- ------------------------------------------------------ ------------- --------

  -----------------------------------------------------------------------------------

where *C~O~* is an equivalent orifice constant with units of
ft^5/2^/sec.

*C~O~* can be evaluated by setting Equation 6-27 equal to the
appropriate weir equation (6-18, 6-19, 6-20, or 6-25 depending on weir
type) evaluated at a weir head $H_{e} = {\omega Y}_{full}$for which the
corresponding orifice head would be $\frac{{\omega Y}_{full}}{2}$. The
result is:

  ---------------------------------------------------------------------------------------------------------------------------
        $$C_{O} = \frac{Q_{W}\left( {\omega Y}_{full} \right)}{\sqrt{\frac{{\omega Y}_{full}}{2}}}$$                 (6-28)
  ----- ---------------------------------------------------------------------------------------------- ------------- --------

  ---------------------------------------------------------------------------------------------------------------------------

where $Q_{W}\left( {\omega Y}_{full} \right)$ is the flow in cfs from
the relevant weir equation for a head of ${\omega Y}_{full}$ feet. The
constant *C~O\ ~* is re-evaluated each time a weir's setting changes.

Thus if the user indicates that a weir is allowed to surcharge, then
whenever the upstream head *H~1~* is above *Z~W~ + Y~full~* its flow is
computed using Equation 6-27. The head *H~e~* to be used in this
equation is computed as follows. Let *H\** be the head corresponding to
the elevation at half of the weir's opening height, i.e.:

  -------------------------------------------------------------------------------------
        $$H^{*} = Z_{W} + (1 - \omega)Y_{full} + \frac{\omega Y_{full}}{2}$$   (6-29)
  ----- ---------------------------------------------------------------------- --------

  -------------------------------------------------------------------------------------

Then

+---+----------------------------------+--------------------------------------------+--------+
|   | $$H_{e} = \left\{ \begin{matrix} | for $H_{2} < H^{*}$                        | (6-30) |
|   | H_{1} - H^{*} \\                 |                                            |        |
|   | H_{1} - H_{2}                    | otherwise                                  |        |
|   | \end{matrix} \right.\ $$         |                                            |        |
+===+==================================+============================================+========+

In addition, the correction for weir submergence is not applied.

### 6.3.6 Flap Gate Head Loss Adjustment

When a weir has a flap gate it adds a small amount of head loss for flow
through the gate. The same Armco formula used for orifices is used to
compute this head loss for weirs:

  -------------------------------------------------------------------------------------------------------
        $$\mathrm{\Delta}H = \frac{4U^{2}}{g}\exp\left( - 1.15\frac{U}{\sqrt{H_{e}}} \right)$$   (6-31)
  ----- ---------------------------------------------------------------------------------------- --------

  -------------------------------------------------------------------------------------------------------

where *∆H* is the head loss added by the flap gate (ft) and *U* is the
velocity through the weir (ft/sec). To evaluate the velocity term in
this equation one needs to know the effective area *A~e~* of flow over
the weir. This area equals

+---+----------------------------------------------------------+----------------------------------+--------+
|   | $$A_{e} = \left\{ \begin{matrix}                         | for normal weir flow             | (6-32) |
|   | A\left( H_{W} + y_{C} \right) - A\left( y_{C} \right) \\ |                                  |        |
|   | A\left( Y_{full} \right) - A\left( y_{C} \right)         | for surcharged weir flow         |        |
|   | \end{matrix} \right.\ $$                                 |                                  |        |
+===+==========================================================+==================================+========+

where *Y~full~* is the full height of the weir opening, *y~C~* is the
distance that the weir crest has been raised due to the current setting
(equal to $(1 - \omega)Y_{full}$ ), and *A(y)* is the area of the weir
opening at flow depth *y*. The latter quantity is found using the
geometry functions described in Chapter 5 for either a rectangular,
triangular, or trapezoidal shape. Knowing *A~e~*, $U = \frac{Q}{A_{e}}$,
where the flow *Q* has been determined using the methods described in
the previous sections.

After the orifice's flow is first computed without this additional head
loss, *∆H* is computed with Equation 6-31 and subtracted from *H~e~*.
Then the flow is re-computed, this time using the adjusted value of
effective head.

### 6.3.7 Dynamic Wave Considerations

A weir does not contribute any surface area to its end nodes under
dynamic wave modeling. The derivative of its flow rate with respect to
head *(dQ/dH)*, used when updating the head of a surcharged end node
(see section 3.3.5), is computed using the formulas in Table 6-4.

+----------------------------------+-----------------------------------------------------------------------------------+
| **Weir Type**                    | **Flow Derivative *(dQ/dH)***                                                     |
+:=================================+===================================================================================+
| Transverse Rectangular           | $$1.5\frac{|Q|}{H_{e}}$$                                                          |
+----------------------------------+-----------------------------------------------------------------------------------+
| Side Flow Rectangular:           |                                                                                   |
+----------------------------------+-----------------------------------------------------------------------------------+
| > a\. $Q \geq 0$                 | $$1.67\frac{|Q|}{H_{e}}$$                                                         |
+----------------------------------+-----------------------------------------------------------------------------------+
| > b\. $Q < 0$                    | $$1.5\frac{|Q|}{H_{e}}$$                                                          |
+----------------------------------+-----------------------------------------------------------------------------------+
| Transverse Triangular:           |                                                                                   |
+----------------------------------+-----------------------------------------------------------------------------------+
| > a\. Fully open (*ω* = 1)       | $$2.5\frac{|Q|}{H_{e}}$$                                                          |
+----------------------------------+-----------------------------------------------------------------------------------+
| > b\. Partly open (*ω* \< 1)     | $$1.5\frac{\left| Q_{R} \right|}{H_{e} + 2.5\frac{\left| Q_{T} \right|}{H_{e}}}$$ |
+----------------------------------+-----------------------------------------------------------------------------------+
| Transverse Trapezoidal           | $$1.5\frac{\left| Q_{R} \right|}{H_{e} + 2.5\frac{\left| Q_{T} \right|}{H_{e}}}$$ |
+----------------------------------+-----------------------------------------------------------------------------------+

: []{#_Toc484690436 .anchor}**Table 6‑4 Formulas for flow derivatives of
various types of weirs**

Note: For trapezoidal openings, *Q~R~* is the flow through the central
rectangular portion and *Q~T~* is the flow through the triangular end
portions (see Equation 6-20).

### 6.3.8 Summary of Weir Computations

The computational steps used to compute flow through a weir link can be
summarized as follows. If the weir is allowed to surcharge and its
setting *ω* changes at the start of a time step then use Equation 6-28
to compute an equivalent orifice coefficient *C~O~* to use during
surcharge conditions. For each iteration within a time step that
requires computing flow through the weir:

1.  Let *H~1~* denote the most recently computed head at the weir's
    upstream node and *H~2~* be the same at the downstream node. (For
    kinematic wave analysis *H~2~* is the downstream node's invert
    elevation.)

2.  If *H~1~ \< H~2~* reverse the values so that *H~1~* as the higher
    head and note that reverse flow will occur. If the weir has a flap
    gate or *H~1~* is below the weir crest then set its flow to 0.

3.  If the head *H~1~* is above the top of the weir's opening and the
    weir is allowed to surcharge then use the equivalent orifice
    equation 6-27 to find its flow where the effective head is found
    from Equations 6-29 and 6-30.

4.  Otherwise use Equation 6-21 to find the effective head on the weir
    and either Equation 6-18, 6-19, 6-20, or 6-25, depending on weir
    type, to find its flow rate.

5.  If the weir has a flap gate then use Equation 6-31 to adjust its
    effective head and repeat the flow calculation of steps 3 and 4.

6.  If the weir is not surcharged use Equation 6-26 to correct the flow
    for any tailwater submergence.

7.  If the weir has reverse flow then make the computed flow negative.

8.  Under dynamic wave analysis use the formulas in Table 6-4 to compute
    $\frac{dQ}{dH}$ for the weir.

## 6.4 Outlets

SWMM's outlet link is a generic type of flow regulator with a user
defined rating curve that relates flow rate to effective head. It can be
used in cases where the head-flow relationships that SWMM uses for
orifice or weir links do not apply. Some examples would be:

- a side orifice using the Smith and Coleman weir equation, where flow
  rate varies with head raised to the 1.645 power (Metcalf & Eddy, Inc.,
  1972),

- a perforated riser pipe with a grate on top used as a detention pond
  outlet structure,

- a vortex-type flow regulator (Hydro International, 2009; Faram et
  al., 2010) that provides more precise flow control than do standard
  orifices (see Figure 6-7).

For kinematic wave analysis the outlet's upstream node must be a storage
node since this is the only type of node for which a true hydraulic head
is computed. For dynamic wave analysis it can be any type of node.

The properties of an outlet link include:

- the height of its offset above the invert of its upstream node

- a rating curve that defines the relationship between head and the
  resulting flow rate

- whether head is defined by just the water level at the upstream node
  of the link or by the difference in head between its upstream and
  downstream nodes

- whether or not it contains a flap gate that prevents reverse flow.

An outlet link can also have a flow setting between 0 and 1 that can be
modified by control rules. The setting serves as a multiplier applied to
the flow determined from the outlet's rating curve.

The rating curve can be defined either as an analytical power law
function or a tabular listing of points on the curve. The analytical
power function has the form:

  --------------------------------------------------------------------------
        $$Q = aH_{e}^{b}$$                                          (6-33)
  ----- ----------------------------------------------------------- --------

  --------------------------------------------------------------------------

where *Q* is flow rate (cfs), *H~e~* is the effective head (ft), and *a*
and *b* are user-supplied constants. The tabular rating curve consists
of pairs of head (*H~e~*) and flow (*Q*) values for points that the user
chooses to represent the shape of the outlet's rating curve.

<figure>
<img src="VolumeII/media/media/image52.png"
style="width:2.68793in;height:3.39631in" alt="hydra-brake4.png" />
<figcaption><p><span id="_Toc484694741"
class="anchor"></span><strong>Figure 6‑7 Rating curve for a vortex
device compared to an orifice</strong></p></figcaption>
</figure>

The following steps are used whenever the flow through an outlet link
must be computed:

1.  Let *H~1~* denote the most recently computed head at the outlet's
    upstream node and *H~2~* be the same at the downstream node. (For
    kinematic wave analysis *H~2~* is the downstream node's invert
    elevation.)

2.  If *H~1~ \< H~2~* reverse the values so that *H~1~* has the higher
    head and note that reverse flow will occur. If the outlet has a flap
    gate or *H~1~* is below the outlet's offset elevation then set its
    flow to 0.

3.  For dynamic wave modeling, if the outlet's rating curve is based on
    head difference then compute an effective head on the outlet as
    $H_{e} = H_{1} - max\left( H_{2},\ \ \ Z_{O} \right)$ where *Z~O~*
    is the outlet's offset elevation. Otherwise $H_{e} = H_{1} - Z_{O}$.

4.  For an analytical rating curve use Equation 6-33 to compute the
    outlet's flow rate *Q*. For a tabular rating curve find the adjacent
    head values in the table that bracket *H~e~* and use linear
    interpolation to find a corresponding flow rate *Q*. (If *H~e~* is
    below the first entry in the table then use the first entry's flow
    value. If it is above the last entry then use the last entry's flow
    value.)

5.  Multiply *Q* by whatever outlet setting is currently in effect and
    change its sign if reverse flow occurs.

# - Advanced Features

\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_

## 7.1 Evaporation and Seepage

### 7.1.1 Conduits

SWMM can model evaporation and seepage losses from conduits as a
uniformly distributed lateral outflow along the length of the conduit.
The following paragraphs explain how user-supplied evaporation and
seepage rates per unit area are converted into a distributed loss per
unit length for a conduit and how this loss rate is factored into the
solution of the governing equations for both dynamic and kinematic wave
modeling.

[Distributed Uniform Evaporation Rate]{.underline}

SWMM can use time varying evaporation data from several different types
of external sources. These include historical daily values from the
National Weather Service, values computed from an historical record of
daily temperatures, user-supplied monthly average values or a
user-supplied hourly time series. The details are described in Volume I
(Hydrology) of this reference manual. These data express the potential
evaporation rate over the entire study area as a volumetric loss per
unit of area per unit of time, which SWMM converts to internal units of
cfs/ft^2^. The following expression converts the rate per unit area into
a rate per unit length of channel (only open channels can evaporate)
over the time period *t* to *t+∆t*:

  -------------------------------------------------------------------------
        $$q_{E} = e_{t}W\left( \overline{\overline{Y}} \right)$$    (7-1)
  ----- ----------------------------------------------------------- -------

  -------------------------------------------------------------------------

where

  -----------------------------------------------------------------------------------------------
  $$q_{E}$$                     =   uniformly distributed evaporation rate along a channel
                                    (cfs/ft)
  ----------------------------- --- -------------------------------------------------------------
  $$e_{t}$$                     =   potential evaporation rate per unit area over the current
                                    time period (cfs/ft^2^)

  $$\overline{\overline{Y}}$$   =   average depth of flow in the channel over the current time
                                    period (ft)

  *W(Y)*                        =   width of water surface at depth of flow *Y* (ft).
  -----------------------------------------------------------------------------------------------

The program automatically extracts the appropriate rate *e~t~* from the
evaporation data source for the current time period being analyzed. The
water surface width *W* is computed using the procedures described in
Chapter 5 for a particular channel's cross sectional shape.

The average depth of flow in the channel is computed differently
depending on the hydraulic modeling procedure used. For kinematic wave
modeling,

  ------------------------------------------------------------------------------------------------------
        $$\overline{\overline{Y}} = \frac{\left( {Y(A}_{1}^{t}) + {Y(A}_{2}^{t} \right))}{2}$$   (7-2)
  ----- ---------------------------------------------------------------------------------------- -------

  ------------------------------------------------------------------------------------------------------

where $A_{1}^{t}$ is the flow area at the upstream end of the channel
previously computed for time *t*, $A_{2}^{t}$ is the same at the
downstream end of the channel, and *Y(A)* is the flow depth associated
with flow area *A* . The latter function is evaluated using the
procedures described in Chapter 5.

For dynamic wave modeling the average channel depth is computed as:

  ---------------------------------------------------------------------------------------------------------------------------------
        $$\overline{\overline{Y}} = \frac{\left( {\overline{Y}}^{t} + {\overline{Y}}^{t + \mathrm{\Delta}t} \right)}{2}$$   (7-3)
  ----- ------------------------------------------------------------------------------------------------------------------- -------

  ---------------------------------------------------------------------------------------------------------------------------------

where $\overline{Y} = \frac{\left( Y_{1} + Y_{2} \right)}{2}$. The
*Y~1~* and *Y~2~* values for ${\overline{Y}}^{t + \mathrm{\Delta}t}$ are
evaluated with Equation 3-16 for the most recently computed nodal head
solution *H^last^* in the iterative procedure used to solve the dynamic
wave equations. Thus $\overline{\overline{Y}}$ and therefore *q~E~* can
change as the dynamic wave iterations unfold within a time step.

[Distributed Uniform Seepage Rate]{.underline}

The seepage loss from a conduit could be due to infiltration into the
soil beneath an unlined or natural channel or result from a leaking or
perforated pipe. In theory the rate would depend upon such factors as
the flow depth in the conduit, the hydraulic conductivity of the
surrounding soil, the variation in moisture content of this soil and the
depth to groundwater. Rather than try to rigorously model the dynamics
of soil infiltration beneath the seeping conduit SWMM uses a
user-supplied constant seepage rate per unit area that can be different
for each conduit. At any given time period this rate is converted to a
uniformly distributed rate per length of conduit as follows:

  -------------------------------------------------------------------------
        $$q_{S} = sf_{c}W(\overline{\overline{Y}})$$                (7-4)
  ----- ----------------------------------------------------------- -------

  -------------------------------------------------------------------------

where

+-------------------------------------+------------------------------------+-------------------------------------------------------------+
| $$q_{S}$$                           | =                                  | uniformly distributed seepage rate per length of conduit    |
|                                     |                                    | (cfs/ft)                                                    |
+:============================+:======+===+================================+=============================================================+
| *s*                                 | =                                  | user-supplied seepage rate per unit area for the conduit    |
|                                     |                                    | (cfs/ft^2^)                                                 |
+-------------------------------------+------------------------------------+-------------------------------------------------------------+
| *f~c~*                              | =                                  | monthly climate adjustment factor for the current time step |
|                                     |                                    | (dimensionless)                                             |
+-----------------------------+-------+---+--------------------------------+-------------------------------------------------------------+
| $$\overline{\overline{Y}}$$ | =         | average depth of flow in the conduit over the current time period (ft).                      |
+-----------------------------+-----------+----------------------------------------------------------------------------------------------+

The monthly climate adjustment factors are a set of 12 user-supplied
constants that apply to the study area as a whole. They allow the
intensity of infiltration-based processes to vary on a seasonal basis.
The average flow depth is computed using Equation 7-2 or 7-3, depending
on the choice of flow routing method.

Equation 7-4 assumes that seepage occurs only in the vertical direction
so that the area over which it takes place is limited by the largest
horizontal extent of the flow cross section. Thus the average depth of
flow $\overline{\overline{Y}}$ is limited by the depth at which the
cross section width is a maximum. Table 7-1 lists the depth at maximum
width, as a fraction of full conduit depth, for several different cross
section shapes recognized by SWMM. For other shapes not listed the depth
at maximum width is as follows:

- For the Modified Basket Handle shape it is the height of the bottom
  rectangular portion of the shape (see Equation 5-15).

- For irregular channels and custom conduit shapes it is the entry in
  the table of width versus depth just prior to where width starts to
  decrease with depth. (If width always keeps increasing with depth then
  it is the full depth.)

- For all other shapes it is the full depth.

  -----------------------------------------------------------------------
  **Shape**            **Relative     **Shape**            **Relative
                         Depth**                             Depth**
  ----------------- ----------------- ----------------- -----------------
  Circular                0.50        Horseshoe               0.50

  Ellipsoid               0.48        Catenary                0.25

  Arch                    0.28        Gothic                  0.45

  Basket Handle           0.20        Semi-Circular           0.15

  Egg                     0.64        Semi-Elliptical         0.15
  -----------------------------------------------------------------------

  : []{#_Toc484690437 .anchor}**Table 7‑1 Relative depth at maximum
  width for select cross section shapes**

[Total Uniform Loss Rate]{.underline}

The total uniform outflow rate along a conduit *q~L~* is the sum of the
evaporative and seepage loss rates:

  -------------------------------------------------------------------------
        $$q_{L} = q_{E} + q_{S}$$                                   (7-5)
  ----- ----------------------------------------------------------- -------

  -------------------------------------------------------------------------

Over any given time step *∆t* the total volume lost to this outflow
cannot exceed the average volume contained in the conduit:

  -------------------------------------------------------------------------
        $$q_{L}L\mathrm{\Delta}t \leq \overline{\overline{A}}L$$    (7-6)
  ----- ----------------------------------------------------------- -------

  -------------------------------------------------------------------------

where $\overline{\overline{A}}$ is the average flow area over the time
step and *L* is the conduit length. Thus

  -----------------------------------------------------------------------------------------------------
        $$q_{L} = \min\left( q_{L},\frac{\overline{\overline{A}}}{\mathrm{\Delta}t} \right)$$   (7-7)
  ----- --------------------------------------------------------------------------------------- -------

  -----------------------------------------------------------------------------------------------------

For kinematic wave analysis the average flow area over the time step
from *t* to *t+∆t* is:

  --------------------------------------------------------------------------------------------
        $$\overline{\overline{A}} = \frac{\left( A_{1}^{t} + A_{2}^{t} \right)}{2}$$   (7-8)
  ----- ------------------------------------------------------------------------------ -------

  --------------------------------------------------------------------------------------------

where $A_{1}^{t}$ is the flow area at the upstream end of the conduit
computed for time *t* and $A_{2}^{t}$ is the same at the downstream end
of the conduit. For dynamic wave analysis:

  ---------------------------------------------------------------------------------------------------------------------------------
        $$\overline{\overline{A}} = \frac{\left( {\overline{A}}^{t} + {\overline{A}}^{t + \mathrm{\Delta}t} \right)}{2}$$   (7-9)
  ----- ------------------------------------------------------------------------------------------------------------------- -------

  ---------------------------------------------------------------------------------------------------------------------------------

where
${\overline{A}}^{t} = \frac{\left( A\left( Y_{1} \right) + A\left( Y_{2} \right) \right)}{2}$
for *Y~1~* and *Y~2~* computed at time *t*, with a similar expression
used for ${\overline{A}}^{t + \mathrm{\Delta}t}$. In the latter case the
flow depths *Y* are computed using the most recently computed nodal
heads (see Equation 3-16) as the iterative dynamic wave solution
unfolds.

An additional constraint on *q~L~* is that it cannot be greater than the
inflow rate $Q_{1}^{t + \Delta t}$ to the conduit under kinematic wave
analysis or the last computed flow *Q^last^* under dynamic wave
analysis.

[Dynamic Wave Modifications]{.underline}

For dynamic wave analysis, including a uniform loss rate adds an
additional term *∆Q~lateral~* to Equation 3-14 used to update a
conduit's flow rate over a time step. The revised equation is:

  ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
       $$Q_{t + \Delta t} = \frac{Q_{t} + {\mathrm{\Delta}Q}_{inertia} + {\mathrm{\Delta}Q}_{pressure} + \mathrm{\Delta}Q_{lateral}}{1 + {\mathrm{\Delta}Q}_{friction}}$$   (7-10)
  ---- -------------------------------------------------------------------------------------------------------------------------------------------------------------------- --------

  ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

where $\mathrm{\Delta}Q_{lateral} = 2.5\overline{U}q_{L}$ and all other
*∆Q* terms were defined previously in Section 3.2. See the sidebar below
for the derivation of this modified equation.

Another modification needed when including a uniform loss rate is to add
*q~L~L* (the total flow lost over the length of the conduit) to the
total outflow from the upstream node of a conduit with positive flow or
add it to the total inflow of the downstream node of a conduit with
negative flow. This modifies the $\sum_{}^{}Q^{t + \mathrm{\Delta}t}$
term (the net inflow minus outflow to a node) in Equation 3-15a which is
used to update nodal heads.

[Kinematic Wave Modifications]{.underline}

For kinematic wave analysis, adding a uniform loss term modifies the
original continuity equation 4-1 as follows:

  ------------------------------------------------------------------------------------------------------------
        $$\frac{\partial A}{\partial t} + \frac{\partial Q}{\partial x} + q_{L} = 0$$                 (7-11)
  ----- ------------------------------------------------------------------------------- ------------- --------

  ------------------------------------------------------------------------------------------------------------

Carrying the *q~L~* term over into the original finite difference form
of this equation (Equation 4-7) produces:

  -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
        $$\frac{(1 - \theta)\left( A_{1}^{t + \Delta t} - A_{1}^{t} \right) + \theta\left( A_{2}^{t + \Delta t} - A_{2}^{t} \right)}{\Delta t} + \frac{(1 - \phi)\left( Q_{2}^{t} - Q_{1}^{t} \right) + \phi\left( Q_{2}^{t + \Delta t} - Q_{1}^{t + \Delta t} \right)}{L} + q_{L} = 0$$   (7-12)
  ----- ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- --------

  -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

where all notation was defined previously in Section 4.2. After
substituting the Manning equation $Q = \beta\Psi(A)$ into this
expression the same non-linear equation for $A_{2}^{t + \Delta t}$
results as before (equation 4-8):

  ---------------------------------------------------------------------------------------------------
        $$\beta\Psi\left( A_{2}^{t + \Delta t} \right) + C1A_{2}^{t + \Delta t} + C2 = 0$$   (7-13)
  ----- ------------------------------------------------------------------------------------ --------

  ---------------------------------------------------------------------------------------------------

with *C1* given by Equation 4-9 and *C2* by Equation 4-10 but with the
additional term $q_{L}\frac{L}{\phi}$ added to it.

### 7.1.2 Storage Units

An open storage unit can evaporate water from its top surface and, if
unlined, can have a seepage loss due to water infiltrating into the soil
beneath its bottom and sloped sides. SWMM represents each of these
losses as a flow rate per unit area of surface exposed. The exposed
surface area (top for evaporation, bottom and sides for seepage) is
computed based on the storage unit's water depth at the start of each
computational time step. The combined total loss rate (in cfs) from both
processes is then subtracted from the net inflow to the storage unit so
that its new water depth at the end of the time step can be computed.

[Evaporation Loss]{.underline}

The evaporation loss rate from the surface of a storage unit during a
time period is based on the surface area in the unit at the start of the
time period using the following equation:

  --------------------------------------------------------------------------
        $$Q_{EN} = e_{t}f_{E}A_{SN}\left( Y^{t} \right)$$           (7-14)
  ----- ----------------------------------------------------------- --------

  --------------------------------------------------------------------------

where

  -----------------------------------------------------------------------------
  $$Q_{EN}$$   =   evaporation loss rate from a storage unit node (cfs)
  ------------ --- ------------------------------------------------------------
  *t*          =   starting time for the current computational time period
                   (sec)

  $$e_{t}$$    =   potential evaporation rate per unit area at time *t*
                   (cfs/ft^2^)

  *f~E~*       =   fraction of evaporation rate realized

  *Y^t^*       =   depth of stored water at time *t* (ft)

  *A~SN~(Y)*   =   storage unit surface area at water depth *Y* (ft).
  -----------------------------------------------------------------------------

The potential evaporation rate *e~t~* is the same quantity discussed in
the previous section on conduit evaporation and is automatically
retrieved from a study area's evaporation data source as the simulation
unfolds over time. The fraction of this rate realized, *f~E~*, is a
user-supplied value for each storage unit that allows the rate to be
adjusted for specific local conditions. It would normally be 1.0 but
could be 0 if the storage unit has a roof over it. The depth of stored
water *Y~t\ ~*is the difference between the water surface elevation
*H^t^* at time *t* and the storage unit's invert elevation *E*. The
*A(Y)* function represents the user-supplied curve of surface area
versus depth as described in Section 5.4.

[Seepage Loss]{.underline}

The seepage loss from a storage unit is modeled as infiltration of
ponded water into the native soil beneath the unit. The Green-Ampt soil
infiltration method is used to compute the rate of seepage per unit area
over time. Its fundamental formula is:

  ----------------------------------------------------------------------------------------------------------------------
        $$q_{SN} = K_{S}f_{C}\left\lbrack 1 + \frac{\left( \psi_{S} + d \right)\theta_{d}}{F} \right\rbrack$$   (7-15)
  ----- ------------------------------------------------------------------------------------------------------- --------

  ----------------------------------------------------------------------------------------------------------------------

where

  --------------------------------------------------------------------------------
  *q~SN~*        =   seepage rate per unit area from the storage unit node
                     (cfs/ft^2^)
  -------------- --- -------------------------------------------------------------
  *K~S~*         =   soil saturated hydraulic conductivity (ft/sec)

  *f~C~*         =   monthly climate adjustment factor for the current time step
                     (dimensionless)

  *d*            =   depth of stored water above the area undergoing seepage (ft)

  $$\psi_{S}$$   =   soil capillary suction head (ft)

  *θ~d~*         =   soil moisture deficit (dimensionless)

  *F*            =   cumulative depth of infiltrated water (ft).
  --------------------------------------------------------------------------------

The monthly seepage adjustment factor is the same user-supplied set of
multipliers used for conduit seepage described previously for equation
7-4. *K~S~, ψ~S~*, and the initial value of *θ~d~* are all parameters
associated with the Green-Ampt model. Both *θ~d~* and *F* are modified
by the model over time. Equation 7-15 makes the seepage rate dependent
on the ratio of stored water depth to cumulative infiltrated depth, both
of which will vary over time

The details of SWMM's implementation of the Green-Ampt infiltration
model are covered in Chapter 4 of Volume I (Hydrology) of this manual.
The only difference when using it for a storage unit is that the
quantity $\psi_{s}$ in the original formulation is replaced with
$\psi_{s} + d$. Volume I also provides guidance on selecting values of
*K~S~*, $\psi_{s}$, and an initial *θ~d\ ~* based on soil type. If
either $\psi_{s}$ or *θ~d\ ~*are 0 then SWMM assumes a constant seepage
rate equal to *K~S~* that is independent of storage depth. If *K~S~* is
0 then no seepage occurs.

The depth of water to use in the Green-Ampt formula will vary across the
top surface of a storage unit if it has sloped sides as shown in Figure
7-1. SWMM accounts for this by applying the Green-Ampt infiltration
method independently to two separate seepage areas -- one for water in
contact with the flat bottom portion of the unit and a second for water
in contact with the sloped sides. The total seepage loss rate *Q~SN~*
(in cfs) can be expressed as:

  ------------------------------------------------------------------------------------------------------------
        $$Q_{SN} = q_{btm}\left( d_{btm} \right)A_{btm} + q_{side}\left( d_{side} \right)A_{side}$$   (7-16)
  ----- --------------------------------------------------------------------------------------------- --------

  ------------------------------------------------------------------------------------------------------------

where:

  -----------------------------------------------------------------------------
  *d~btm~*    =   depth of stored water above bottom of unit (ft)
  ----------- --- -------------------------------------------------------------
  *q~btm~*    =   Green-Ampt infiltration rate based on *d = d~btm~*
                  (cfs/ft^2^)

  *A~btm~*    =   surface area over which bottom seepage occurs (ft^2^)

  *d~side~*   =   average depth of stored water above sloped sides of unit (ft)

  *q~side~*   =   Green-Ampt infiltration rate based on *d = d~side~*
                  (cfs/ft^2^)

  *A~side~*   =   surface area of sloped sides over which seepage occurs
                  (ft^2^).
  -----------------------------------------------------------------------------

<figure>
<img src="VolumeII/media/media/image53.png"
style="width:4.93688in;height:2.4372in" alt="StorageUnitSeepage.png" />
<figcaption><p><span id="_Toc484694742"
class="anchor"></span><strong>Figure 7‑1 Depths used for computing
seepage in storage units</strong></p></figcaption>
</figure>

As noted previously, the depth above the bottom of the storage unit
(*d~btm~*) is $Y^{t} = H^{t} - E$. The bottom surface area is found from
the unit's storage curve at a depth of 0 (see Section 5.4 for a
discussion of storage curves). The average depth above the sloped sides
is computed as:

+---+------------------------------------------------------+-------------------------------------+--------+
|   | $$d_{side} = \left\{ \begin{array}{r}                | for $Y^{t} < d_{\min}$              | (7-17) |
|   | 0 \\                                                 |                                     |        |
|   | \frac{\left( Y^{t} - d_{\min} \right)}{2} \\         | for                                 |        |
|   | Y^{t} - \frac{\left( d_{\max} - d_{\min} \right)}{2} | $d_{\min} \leq Y^{t} \leq d_{\max}$ |        |
|   | \end{array} \right.\ $$                              |                                     |        |
|   |                                                      | for $Y^{t} > d_{\max}$              |        |
+===+======================================================+=====================================+========+

where *d~min~* is the storage depth where the sloped sides begin and
*d~max~* is the depth where it ends (see Figure 7-1). Both of these
depths can be found from the storage unit's storage curve. The effective
area over which vertical seepage through the sloped sides occurs is
computed as:

  --------------------------------------------------------------------------------------------------------------
        $$A_{side} = min\left\{ A\left( Y^{t} \right),\ A\left( d_{\max} \right) \right\} - A_{btm}$$   (7-18)
  ----- ----------------------------------------------------------------------------------------------- --------

  --------------------------------------------------------------------------------------------------------------

[Total Storage Loss]{.underline}

The total loss rate from a storage unit node, *Q~LN~* (in cfs), over a
given time step is

  --------------------------------------------------------------------------
        $$Q_{LN} = Q_{EN} + Q_{SN}$$                                (7-19)
  ----- ----------------------------------------------------------- --------

  --------------------------------------------------------------------------

*Q~LN~* is not allowed exceed the volume of water in storage at the
start of the current time step:

  -----------------------------------------------------------------------------------------------------------
        $$Q_{LN} = min\left\{ Q_{LN},\frac{V_{N}\left( Y^{t} \right)}{\mathrm{\Delta}t} \right\}$$   (7-20)
  ----- -------------------------------------------------------------------------------------------- --------

  -----------------------------------------------------------------------------------------------------------

where *V~N~(Y)* is the storage unit's volume at depth *Y* (see section
5.4 for how it is computed) and *∆t* is the size of the current time
step.

For a given storage node, *Q~LN~* is computed once at the start of the
current time step based on the known stored water level. For dynamic
wave analysis it is subtracted from the
$\sum_{}^{}Q^{t + \mathrm{\Delta}t}$ term of Equation 3-15a (i.e., is
treated as a nodal outflow) each time the node's head is updated at step
4 of the solution procedure described in Section 3.2. For kinematic wave
analysis, after all link flows have been found, *Q~LN~* is added to the
node's total outflow at the end of the time step (see Equation 4-18)
which is used to update the node's volume and subsequently its hydraulic
head (see Section 4.3.5).

## 7.2 Minor Losses

Energy losses caused by rapid changes in magnitude or direction of
velocity are called minor or local losses. They can occur at bends,
contractions, or enlargements in conduit geometry and also be associated
with flows entering a conduit from a larger water body or flows exiting
a conduit to a larger water body. Table 7-2 lists the types of minor
losses most frequently considered in stormwater conveyance networks.

A minor loss is represented as the product of a loss coefficient and the
local velocity head for a specific location *i* along a conduit:

  --------------------------------------------------------------------------
        $$\mathrm{\Delta}H_{L} = K_{m,i}\frac{U_{i}^{2}}{2g}$$      (7-21)
  ----- ----------------------------------------------------------- --------

  --------------------------------------------------------------------------

where $\mathrm{\Delta}H_{L}$ is the minor head loss (ft), *K~m~,~i~* is
a loss coefficient, and *U~i~* is flow velocity (ft/sec). The location
index *i* is 1 for an entrance loss based on the conduit's upstream
velocity, 2 for an exit loss based on its downstream velocity, or 3 for
an average loss based on its average velocity.

Minor losses can be included in the St. Venant momentum equation for a
conduit by treating them as a loss per unit length, *h~L~*, in the same
way that the friction slope *S~f~* is treated. This modified version of
the momentum equation (originally equation 3-2 with uniform lateral
outflow rate *q~L~* included) is:

  -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
        $$\frac{\partial Q}{\partial t} + \frac{\partial\left( \frac{Q^{2}}{A} \right)}{\partial x} + gA\frac{\partial H}{\partial x} + gAS_{f} - U\frac{q_{L}}{2} + gAh_{L} = 0$$   (7-22)
  ----- ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------- --------

  -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

where $h_{L} = \frac{\sum_{i = 1}^{3}{K_{m,i}U_{i}^{2}}}{(2gL)}$ with
*L* being the conduit length.

+--------------------------+----------------+------------------+-------------+
| **Type of Loss**         | **Frequently** | **Occasionally** | **Rarely**  |
|                          |                |                  |             |
|                          | **Modeled**    | **Modeled**      | **Modeled** |
+:=========================+:==============:+:================:+:===========:+
| *Pipes (Full or          |                |                  |             |
| Partially Full)*         |                |                  |             |
+--------------------------+----------------+------------------+-------------+
| Entrance                 |                | x                |             |
+--------------------------+----------------+------------------+-------------+
| Exit                     |                | x                |             |
+--------------------------+----------------+------------------+-------------+
| Expansion and            |                | x                |             |
| Contraction              |                |                  |             |
+--------------------------+----------------+------------------+-------------+
| Inlet on branch          |                |                  | x           |
+--------------------------+----------------+------------------+-------------+
| Curves or bends          |                | x                |             |
+--------------------------+----------------+------------------+-------------+
| Outfall                  |                | x                |             |
+--------------------------+----------------+------------------+-------------+
| *Junctions (Full or      |                |                  |             |
| Partially Full)*         |                |                  |             |
+--------------------------+----------------+------------------+-------------+
| Flow through junction    | x              |                  |             |
+--------------------------+----------------+------------------+-------------+
| Bend within junction     | x              |                  |             |
+--------------------------+----------------+------------------+-------------+
| Junction with lateral    | x              |                  |             |
+--------------------------+----------------+------------------+-------------+
| Junction with inlet      |                |                  | x           |
+--------------------------+----------------+------------------+-------------+
| *Channels*               |                |                  |             |
+--------------------------+----------------+------------------+-------------+
| Expansion and            |                | x                |             |
| Contractions             |                |                  |             |
+--------------------------+----------------+------------------+-------------+
| Curves or bends          | x              | x                |             |
+--------------------------+----------------+------------------+-------------+
| Culvert entrance         | x              |                  |             |
+--------------------------+----------------+------------------+-------------+
| Culvert exit             | x              |                  |             |
+--------------------------+----------------+------------------+-------------+
| Outfall                  |                | x                |             |
+--------------------------+----------------+------------------+-------------+

: []{#_Toc484690438 .anchor}**Table 7‑2 Types of minor losses in
drainage systems (from Frost, 2006)**

For dynamic wave hydraulics the finite difference form of equation 7-22
can be found by following the same derivation used earlier in Section
3.2. This results in:

  -----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
        $$\frac{\mathrm{\Delta}Q}{\mathrm{\Delta}t} = 2\overline{U}\frac{\mathrm{\Delta}\overline{A}}{\mathrm{\Delta}t} + {\overline{U}}^{2}\frac{\left( A_{2} - A_{1} \right)}{L} - g\overline{A}\frac{\left( H_{2} - H_{1} \right)}{L} - g\eta^{2}\frac{Q\left| \overline{U} \right|}{{\overline{R}}^{4/3}} + 2.5\overline{U}q_{L} - \frac{Q\sum_{i = 1}^{3}{K_{m,i}\left| U_{i} \right|}}{2L}$$   (7-23)
  ----- -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- --------

  -----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

After re-arranging terms, the following revised form of the flow
updating equation 3-14 used in step 2 of the dynamic wave solution
procedure of Section 3.2 is:

  ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
       $$Q_{t + \Delta t} = \frac{Q_{t} + {\mathrm{\Delta}Q}_{inertia} + {\mathrm{\Delta}Q}_{pressure} + \mathrm{\Delta}Q_{lateral}}{1 + {\mathrm{\Delta}Q}_{friction} + \mathrm{\Delta}Q_{loss}}$$   (7-24)
  ---- ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- --------

  ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

where

  -----------------------------------------------------------------------------------------------------------------------
        $$\mathrm{\Delta}Q_{loss} = \frac{\mathrm{\Delta}t}{2L}\sum_{i = 1}^{3}{K_{m,i}\left| U_{i} \right|}$$   (7-25)
  ----- -------------------------------------------------------------------------------------------------------- --------

  -----------------------------------------------------------------------------------------------------------------------

and all other *∆Q* terms are as defined previously in Sections 3.2 and
7.1.1. Minor losses are not computed for kinematic wave analysis since
it uses a simplified version of the momentum equation that only accounts
for gravitational and friction forces. Frost (2006) provides guidance on
selecting values for the loss coefficient *K~m~*.

## 7.3 Force Mains

For dynamic wave modeling SWMM allows the user to designate particular
circular pipes as force mains. These pipes will use either the
Hazen-Williams or the Darcy-Weisbach equation to compute their friction
losses when pressurized conditions occur. For free surface flow the
Manning equation continues to be used.

### 7.3.1 Hazen-Williams Force Mains

The standard form of the Hazen-Williams equation in US units is (Clark
et al., 1977):

  --------------------------------------------------------------------------
        $$U = 1.318C_{HW}R_{full}^{0.63}S_{f}^{0.54}$$              (7-26)
  ----- ----------------------------------------------------------- --------

  --------------------------------------------------------------------------

where *U* is velocity (ft/sec), *R~full~* is the full pipe hydraulic
radius (ft), *S~f~* is the friction slope (head loss per unit length)
(ft/ft), and *C~HW~* is the user-supplied Hazen-Williams C-factor
coefficient. Typical values of the C-factor are listed in Table 7-3.

Solving Equation 7-26 for *S~f~* and putting the result in a form
similar to the Manning equation (see Equation 3-3) gives:

  -------------------------------------------------------------------------------------------
        $$S_{f} = \frac{0.6|U|^{0.852}Q}{C_{HW}^{1.852}A_{full}R_{full}^{1.667}}$$   (7-27)
  ----- ---------------------------------------------------------------------------- --------

  -------------------------------------------------------------------------------------------

+-----------------+----------------+-----------------+----------------+
| **Pipe          | **C-factor**   | **Pipe          | **C-Factor**   |
| Material**      |                | material**      |                |
+=================+:==============:+=================+:==============:+
| Asbestos Cement | 140            | Corrugated      | 60             |
|                 |                | Steel           |                |
+-----------------+----------------+-----------------+----------------+
| Brick Sewer     | 100            | Ductile Iron    | 140            |
+-----------------+----------------+-----------------+----------------+
| Cast Iron:      | 130            | Galvanized Iron | 120            |
|                 |                |                 |                |
| Unlined         | 100            |                 |                |
|                 |                |                 |                |
| Asphalt Coated  | 140            |                 |                |
|                 |                |                 |                |
| Cement Lined    |                |                 |                |
|                 |                +-----------------+----------------+
|                 |                | Plastic PVC     | 130            |
|                 |                +-----------------+----------------+
|                 |                | Polyethylene    | 140            |
|                 |                +-----------------+----------------+
|                 |                | Vitrified Clay  | 110            |
+-----------------+----------------+-----------------+----------------+
| Concrete        | 120            | Welded Steel    | 100            |
+-----------------+----------------+-----------------+----------------+

: []{#_Toc484690439 .anchor}**Table 7‑3 Hazen-Williams C-factors for
different pipe materials**

This expression replaces the Manning formula for *S~f~* when a force
main flows full. As a result, the friction term in the equation used to
update the conduit's flow in the iterative dynamic wave solution
procedure (Equation 3-14) becomes:

  --------------------------------------------------------------------------------------------------------------------------------------------------------------
        $${\mathrm{\Delta}Q}_{friction} = 0.6g\frac{\left| \overline{U} \right|^{0.852}\Delta t}{C_{HW}^{1.852}R_{full}^{1.667}}$$                      (7-28)
  ----- ---------------------------------------------------------------------------------------------------------------------------- ------------------ --------

  --------------------------------------------------------------------------------------------------------------------------------------------------------------

where $\overline{U} = \frac{Q^{last}}{A_{full}}$.

### 7.3.2 Darcy-Weisbach Force Mains

The standard form of the Darcy-Weisbach head loss equation is (Clark et
al., 1977):

  --------------------------------------------------------------------------
        $$S_{f} = \frac{fU^{2}}{2gD}$$                              (7-29)
  ----- ----------------------------------------------------------- --------

  --------------------------------------------------------------------------

where *S~f~* is the friction slope (head loss per unit length) (ft/ft),
*U* is flow velocity (ft/sec), *D* is pipe diameter (ft), and *f* is a
dimensionless friction factor. Noting that $D = 4R_{full}$ for a
circular pipe allows this equation to be expressed in a form similar to
the Manning formula:

  --------------------------------------------------------------------------
        $$S_{f} = \frac{f|U|Q}{8gA_{full}R_{full}}$$                (7-30)
  ----- ----------------------------------------------------------- --------

  --------------------------------------------------------------------------

As a result, the friction term in the equation used to update a
pressurized force main's flow in the iterative dynamic wave solution
procedure (Equation 3-14) becomes:

  ------------------------------------------------------------------------------------------------------------------------------
        $${\mathrm{\Delta}Q}_{friction} = \frac{f\left| \overline{U} \right|\Delta t}{8R_{full}}$$                      (7-31)
  ----- -------------------------------------------------------------------------------------------- ------------------ --------

  ------------------------------------------------------------------------------------------------------------------------------

The friction factor *f* can be determined graphically from the Moody
diagram as a function of the flow's Reynolds number (*Re*) and the
pipe's relative roughness (Bhave, 1991). For laminar flow (Re ≥ 2000)
the friction factor is:

  --------------------------------------------------------------------------
        $$f = \frac{64}{Re}$$                                       (7-32)
  ----- ---------------------------------------- ------------------ --------

  --------------------------------------------------------------------------

where $Re = \frac{D\left| \overline{U} \right|}{\mu}$ with *μ* being the
kinematic viscosity of water taken as 1.1×10^-5^ ft^2^/sec. For
transition and rough turbulent flow (*Re* ≥ 4000) the Swamee and Jain
approximation to the Colebrook-White formula is used (Bhave, 1991):

  ----------------------------------------------------------------------------------------------------------------------------------------------------------
        $$f = \frac{0.25}{\left\lbrack \log\left( \frac{\epsilon}{3.7D} + \frac{5.74}{{Re}^{0.9}} \right) \right\rbrack^{2}}$$                      (7-33)
  ----- ------------------------------------------------------------------------------------------------------------------------ ------------------ --------

  ----------------------------------------------------------------------------------------------------------------------------------------------------------

where $\epsilon$ is the equivalent surface roughness height (ft) of the
pipe wall as supplied by the user. This roughness height serves the same
purpose as the Manning roughness coefficient or the Hazen-Williams
C-factor. Typical values for different pipe materials are given in Table
7-4. For *Re* between 2000 and 4000 linear interpolation is used between
the friction factor at *Re* = 2000 (equal to 0.032) and that at *Re* =
4000 (which will also depend on $\frac{\epsilon}{D}$).

  -------------------------------------------------------------------------------
  **Material**       $\mathbf{\epsilon}$  **Material**       $\mathbf{\epsilon}$
                        **(inches)**                            **(inches)**
  ----------------- --------------------- ----------------- ---------------------
  Concrete              0.012 -- 0.12     Asphalted Cast           0.0048
                                          Iron              

  Cast Iron                 0.010         Welded Steel             0.0018

  Galvanized iron           0.006         PVC                      0.00006
  -------------------------------------------------------------------------------

  : []{#_Toc484690440 .anchor}**Table 7‑4 Darcy-Weisbach roughness
  heights for different pipe materials**

### 7.3.3 Equivalent Manning's n

For pipes designated as force mains a C-factor or roughness height will
be specified instead of a Manning roughness coefficient. Since the
Manning equation will still be used to analyze free surface flow when
the pipe is only partly full a method is needed to determine a Manning's
*n* that will be consistent with the other forms of pipe roughness. SWMM
does this by equating the Manning full pipe flow to either the
Hazen-Williams or Darcy-Weisbach flow computed under fully turbulent
conditions for a friction slope equal to the pipe's bottom slope.

When the Manning full normal flow is equated to the Hazen-Williams
formula flow the result is:

  ---------------------------------------------------------------------------------------------------------------------------------------------------------
        $$\left( \frac{1.486}{n} \right)^{2}R_{full}^{4/3}S_{O} = \left( 1.318C_{HW}R_{full}^{0.63}S_{O}^{0.54} \right)^{2}$$                      (7-34)
  ----- ----------------------------------------------------------------------------------------------------------------------- ------------------ --------

  ---------------------------------------------------------------------------------------------------------------------------------------------------------

where *S~O~* is the pipe's bottom slope (ft/ft), *R~full~* is in feet,
and *n* has units of sec/m^1/3^. Expressing *R~full~* as $\frac{D}{4}$
and solving for *n* gives:

  -----------------------------------------------------------------------------------------------------
        $$n = \frac{1.067\left( \frac{D}{S_{O}} \right)^{0.04}}{C_{HW}}$$                      (7-35)
  ----- ------------------------------------------------------------------- ------------------ --------

  -----------------------------------------------------------------------------------------------------

Doing the same for the Darcy-Weisbach formula produces:

  -----------------------------------------------------------------------------------------------------------------------------------
        $$\left( \frac{1.486}{n} \right)^{2}R_{full}^{4/3}S_{O} = \frac{2gDS_{O}}{f(\epsilon,\infty)}$$                      (7-36)
  ----- ------------------------------------------------------------------------------------------------- ------------------ --------

  -----------------------------------------------------------------------------------------------------------------------------------

where

  -------------------------------------------------------------------------------------------------------------------------------------------------
        $$f(\epsilon,\infty) = \frac{0.25}{\left\lbrack \log\left( \frac{\epsilon}{3.7D} \right) \right\rbrack^{2}}$$                      (7-37)
  ----- --------------------------------------------------------------------------------------------------------------- ------------------ --------

  -------------------------------------------------------------------------------------------------------------------------------------------------

Expressing *R~full~* as $\frac{D}{4}$ and solving for *n* gives:

  ----------------------------------------------------------------------------------------
        $$n = \sqrt{\frac{f(\epsilon,\infty)}{185}}D^{1/6}$$                      (7-38)
  ----- ------------------------------------------------------ ------------------ --------

  ----------------------------------------------------------------------------------------

To summarize, when flowing full under dynamic wave analysis, a pipe
designated as a force main uses Equation 7-28 (for Hazen-Williams) or
7-31 (for Darcy-Weisbach) in place of Equation 3-14c to evaluate
${\mathrm{\Delta}Q}_{friction}$ in the iterative flow updating Equation
3-14. For free surface flow it uses the Manning form of
${\mathrm{\Delta}Q}_{friction}$ (Equation 3-14c) with an *n*-value given
by Equation 7-35 (for Hazen-Williams) or 7-38 (for Darcy-Weisbach).

## 7.4 Culverts

Culverts are closed conduits that allow water from an open stream or
channel to flow under a road, railroad, trail, or similar obstruction
from one side to the other side (see Figure 7-2). A complete description
of culverts and their hydraulic performance is provided by the Federal
Highway Administration in their Hydraulic Design of Highway Culverts
manual (FHWA, 2012). The equations used by SWMM to model culverts are
taken from this publication.

<figure>
<img src="VolumeII/media/media/image54.png"
style="width:4.00056in;height:3.00042in"
alt="Figure 3-2. A photo of a concrete box culvert." />
<figcaption><p><span id="_Toc484694743"
class="anchor"></span><strong>Figure 7‑2 Concrete box culvert (from
FHWA, 2012)</strong></p></figcaption>
</figure>

Culvert flow can be controlled either by the inlet or the outlet. Inlet
control occurs when the conveyance capacity of the culvert's barrel is
higher than what the inlet will accept. Otherwise outlet control occurs,
with the possibility that flow may be limited by backwater effects.
Culverts are usually analyzed under a steady design flow condition to
determine if the resulting inlet water depth will be acceptable. However
for SWMM's unsteady dynamic wave analysis they are analyzed to find the
flow corresponding to known inlet and outlet depths. (Culvert analysis
is not made under kinematic wave analysis.)

Any SWMM conduit link can be designated as a culvert by assigning it one
of the code numbers associated with a particular shape, material, and
inlet configuration listed in Table H-1 in Appendix H. The choice made
from the table should be consistent with the conduit's designated shape
(circular, rectangular, ellipsoid, or arch). At any given time step of a
simulation the flow through the culvert is first determined using SWMM's
usual dynamic wave procedure. This flow represents the outlet control
condition. Then an inlet controlled flow is computed to see if it
becomes the limiting flow rate.

### 7.4.1 Inlet Control Flow

Under inlet control, a rating curve establishes the relationship between
culvert flow rate and inlet head. The shape of the curve varies
depending on the culvert's shape, material, and geometry of its inlet
opening. Figure 7-3 shows a typical inlet control rating curve in
normalized form, where inlet headwater depth (*Y~1~*) is normalized by
the full barrel depth (*Y~full~*) and flow rate (*Q*) is normalized by
$A_{full}\sqrt{Y_{full}}$ where *A~full~* is the full cross-section area
of the barrel. When the inlet is submerged it performs as an orifice
while when unsubmerged it performs as a weir.

<figure>
<img src="VolumeII/media/media/image55.png"
style="width:5.88468in;height:4.60359in"
alt="CulvertRatingCurve2.png" />
<figcaption><p><span id="_Toc484694744"
class="anchor"></span><strong>Figure 7‑3 Example of a culvert rating
curve (from FHWA, 2012)</strong></p></figcaption>
</figure>

Based on extensive experimental testing done by the National Bureau of
Standards, FHWA has developed inlet rating curves for a number of
different types of culverts and their inlet configurations. The curves
have been fitted to analytical functions that describe both their
submerged and unsubmerged portions. Table H-1 in Appendix H lists the
different types of culverts for which parameterized rating curves have
been developed. Table H-2 lists the parameter values used for both the
unsubmerged and submerged portions of each curve.

### 7.4.2 Unsubmerged Inlet Control Curves

The FHWA procedures identify two types of unsubmerged curves used to
compute inlet control for a culvert. The form 1 equation is:

  ------------------------------------------------------------------------------------------------------------------------------------------------------------------------
        $$\frac{H_{1} - Z_{1}}{Y_{full}} = \frac{E_{C}}{Y_{full}} + K_{I}\left\lbrack \frac{Q_{IC}}{A_{full}\sqrt{Y_{full}}} \right\rbrack^{M_{I}} + ScfS_{O}$$   (7-39)
  ----- --------------------------------------------------------------------------------------------------------------------------------------------------------- --------

  ------------------------------------------------------------------------------------------------------------------------------------------------------------------------

while the form 2 equation is:

  ------------------------------------------------------------------------------------------------------------------------------------
        $$\frac{H_{1} - Z_{1}}{Y_{full}} = K_{I}\left\lbrack \frac{Q_{IC}}{A_{full}\sqrt{Y_{full}}} \right\rbrack^{M_{I}}$$   (7-40)
  ----- --------------------------------------------------------------------------------------------------------------------- --------

  ------------------------------------------------------------------------------------------------------------------------------------

The following definitions apply to these equations:

  -----------------------------------------------------------------------------
  *H~1~*      =   hydraulic head at the inlet node of the culvert link (ft)
  ----------- --- -------------------------------------------------------------
  *Z~1~*      =   elevation of the culvert invert at its inlet end (ft)

  *Q~IC~*     =   inlet controlled flow rate through the culvert (cfs)

  *E~C~*      =   specific head at critical depth for flow *Q~IC~* (ft)

  *Y~full~*   =   full depth of the culvert barrel (ft)

  *A~full~*   =   full area of the culvert barrel cross section (ft^2^)

  *S~O~*      =   culvert barrel slope (ft/ft)

  *Scf*       =   slope correction factor (0.7 for mitered inlets and -0.5 for
                  all others)

  *K~I~,      =   constants from Table H-2 for corresponding culvert type in
  M~I~*           Table H-1.
  -----------------------------------------------------------------------------

Table H-2 also specifies which equation is used for each type of
culvert. It should be noted that *K~I~* has a factor of
$g^{\frac{- M_{I}}{2}}$ embedded in it so that equations 7-39 and 7-40
are dimensionally consistent.

The form 2 equation can be solved directly to determine *Q~IC~* for a
given inlet head *H~1~*:

  -------------------------------------------------------------------------------------------------------------------------
        $$Q_{IC} = A_{full}\sqrt{Y_{full}}\left( \frac{H_{1} - Z_{1}}{K_{I}Y_{full}} \right)^{\frac{1}{M_{I}}}$$   (7-41)
  ----- ---------------------------------------------------------------------------------------------------------- --------

  -------------------------------------------------------------------------------------------------------------------------

For the form 1 equation, the specific head at critical depth is defined
as:

  --------------------------------------------------------------------------
        $$E_{C} = Y_{C} + \frac{U_{C}^{2}}{2g}$$                    (7-42)
  ----- ----------------------------------------------------------- --------

  --------------------------------------------------------------------------

where *Y~C~* is the critical depth for flow *Q~IC~* and *U~C~* is the
velocity at this depth. From the definition of critical depth given by
Equation 5-28 in Section 5.5.1:

  -------------------------------------------------------------------------------------
        $$U_{C}^{2} = g\frac{A\left( Y_{C} \right)}{W\left( Y_{C} \right)}$$   (7-43)
  ----- ---------------------------------------------------------------------- --------

  -------------------------------------------------------------------------------------

and from Equation 5-29:

  --------------------------------------------------------------------------------------------------------------
        $$Q_{IC} = A\left( Y_{C} \right)\sqrt{g\frac{A\left( Y_{C} \right)}{W\left( Y_{C} \right)}}$$   (7-44)
  ----- ----------------------------------------------------------------------------------------------- --------

  --------------------------------------------------------------------------------------------------------------

The area *A* and top width *W* values in these equations are evaluated
at flow depth *Y~C~* using the methods described in Chapter 5 that
depend on the culvert's shape and dimensions.

Substituting these relations into the form 1 equation 7-39 results in
the following nonlinear equation in the single unknown *Y~C~* :

  --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
        $$\frac{Y_{C}}{Y_{full}} = \frac{H_{1} - Z_{1} - \frac{Y_{HC}}{2}}{Y_{full}} - K_{I}\left\lbrack \frac{A\left( Y_{C} \right)}{A_{full}}\sqrt{g\frac{Y_{HC}}{Y_{full}}} \right\rbrack^{M_{I}} - ScfS_{O}$$   (7-45)
  ----- ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- --------

  --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

where $Y_{HC}$ is the critical hydraulic depth defined as
$\frac{A\left( Y_{C} \right)}{W\left( Y_{C} \right)}$. This equation is
solved using Ridder's root finding method (see Appendix B) with an
initial bracket on *Y~C~* of 10 to 100 percent of *H~1~ -- Z~1~* and a
stopping tolerance of 0.001 ft. After *Y~C~* is found the corresponding
inlet control flow rate *Q~IC~* can be computed using Equation 7-44.

### 7.4.3 Submerged Inlet Control Curve

The FHWA equation for inlet control of a culvert whose inlet is
submerged is:

  ---------------------------------------------------------------------------------------------------------------------------------------------------
        $$\frac{H_{1} - Z_{1}}{Y_{full}} = c_{I}\left\lbrack \frac{Q_{IC}}{A_{full}\sqrt{Y_{full}}} \right\rbrack^{2} + y_{I} + ScfS_{O}$$   (7-46)
  ----- ------------------------------------------------------------------------------------------------------------------------------------ --------

  ---------------------------------------------------------------------------------------------------------------------------------------------------

where *c~I~* and *y~I~* are constants from Table H-2 for a particular
culvert type in Table H-1. The *c~I~* constant has a factor of
$\frac{1}{g}$ embedded in it so that equation 7-46 is dimensionally
consistent. Solving this expression for *Q~IC~* results in:

  --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
        $$Q_{IC} = \left\lbrack \left( \frac{1}{c_{I}} \right)\left( \frac{H_{1} - Z_{1}}{Y_{full}} - y_{I} - ScfS_{O} \right) \right\rbrack^{\frac{1}{2}}A_{full}\sqrt{Y_{full}}$$   (7-47)
  ----- ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------- --------

  --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

### 7.4.4 Inlet Control Transition Zone 

The FHWA procedure states that the submerged inlet control equation
should be used for values of
$\frac{Q_{IC}}{\left( A_{full}\sqrt{Y_{full}} \right)}$ above 4.0.
Converting this into a condition on *H~1~* results in:

  -------------------------------------------------------------------------------------------------
        $$H_{1} > {H_{IS} = Z}_{1} + Y_{full}\left( 16c_{I} + y_{I} + ScfS_{O} \right)$$   (7-48)
  ----- ---------------------------------------------------------------------------------- --------

  -------------------------------------------------------------------------------------------------

When this condition is satisfied SWMM uses the submerged portion of the
inlet control curve to compute the inlet control flow rate *Q~IC~*.

FHWA states that the unsubmerged inlet control equation applies for
$\frac{Q_{IC}}{\left( A_{full}\sqrt{Y_{full}} \right)}$ values below
3.5. This is difficult to convert to an a priori limit on *H~1~* because
of the *E~C~* term in the form 1 unsubmerged equation. Therefore SWMM
uses an arbitrary criterion of

  --------------------------------------------------------------------------
        $$H_{1} < {H_{IU} = Z}_{1} + 0.95Y_{full}$$                 (7-49)
  ----- ----------------------------------------------------------- --------

  --------------------------------------------------------------------------

to determine if the unsubmerged inlet control equations should be used.

When *H~1~* is between *H~IU~* and *H~IS~* linear interpolation is used
to compute *Q~IC~* as follows:

  --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
        $$Q_{IC} = Q_{IC}\left( H_{IU} \right) + \left( Q_{IC}\left( H_{IS} \right) - Q_{IC}\left( H_{IU} \right) \right)\frac{\left( H_{1} - H_{IU} \right)}{\left( H_{IS} - H_{IU} \right)}$$   (7-50)
  ----- ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- --------

  --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

where $Q_{IC}\left( H_{IU} \right)$ is the flow from the unsubmerged
equation for a head of *H~IU~* and $Q_{IC}\left( H_{IS} \right)$ is the
flow from the submerged equation for a head of *H~IS~*.

### 7.4.5 Flow Derivatives

SWMM's dynamic wave procedure needs to evaluate the derivative of a
conduit's flow rate with respect to head *(dQ/dH)* for use when updating
the head of a surcharged end node (see section 3.3.5). The derivatives
for the various methods of computing an inlet control flow limit are as
follows:

  ---------------------------------------------------------------------------------------------------------------
        $$\frac{dQ_{IC}}{dH_{1}} = \left\{ \begin{matrix}                                                (7-51)
        \frac{Q_{IC}}{M_{I}H_{1}} & unsubmerged \\                                                       
        \frac{0.5A_{full}^{2}}{c_{I}Q_{IC}} & submerged \\                                               
        \frac{Q_{IC}\left( H_{1S} \right) - Q_{IC}\left( H_{1U} \right)}{H_{1S} - H_{1U}} & transition   
        \end{matrix} \right.\ $$                                                                         
  ----- ------------------------------------------------------------------------------------------------ --------

  ---------------------------------------------------------------------------------------------------------------

### 7.4.6 Summary of Culvert Analysis

The following steps summarize how a culvert conduit is checked for inlet
control each time that a new flow is computed for it under dynamic wave
analysis:

1.  Equation 3-14 is used as usual to compute a first flow estimate *Q*.
    This represents an outlet control condition.

2.  If the conduit is not flowing full at both ends, a flow limit
    *Q~IC~* due to inlet control is computed. Equations 7-41 or 7-45 are
    used if the head *H~1~* at the culvert's inlet node is below
    *H~1U~*, Equation 7-47 is used if *H~1~* is above *H~1S~*, or
    Equation 7-50 is used if *H~1~* is in between these limits.

3.  If *Q~IC~* is less than *Q* then it is replaced with *Q~IC~* and
    Equation 7-51 is used to compute the conduit's flow derivative with
    respect to head.

## 7.5 Roadway Weirs

A culvert will become overtopped when the headwater rises to the
elevation of the roadway (see Figure 7-4). SWMM represents the flow
across the road with a special type of weir link designated as a roadway
weir. It is similar to a standard SWMM transverse rectangular weir but
has its own specific methods for computing a weir coefficient and
submergence factor based on road characteristics. Figure 7-5 shows how a
roadway weir would be configured with a culvert in a SWMM node-link
layout.

<figure>
<img src="VolumeII/media/media/image56.png"
style="width:5.37666in;height:2.33333in" alt="OvertoppedRoadway.png" />
<figcaption><p><span id="_Toc484694745"
class="anchor"></span><strong>Figure 7‑4 Roadway overtopping (from FHWA,
2012)</strong></p></figcaption>
</figure>

<figure>
<img src="VolumeII/media/media/image57.png"
style="width:5.35491in;height:1.82317in" alt="RoadwayWeir2.png" />
<figcaption><p><span id="_Toc484694746"
class="anchor"></span><strong>Figure 7‑5 SWMM node-link representation
of a culvert with a roadway weir</strong></p></figcaption>
</figure>

The standard transverse rectangular weir equation can be used to compute
the flow across a roadway weir as follows: (FHWA, 2012):

  --------------------------------------------------------------------------
        $$Q = {f_{S}C}_{W}LH^{3/2}$$                                (7-52)
  ----- ----------------------------------------------------------- --------

  --------------------------------------------------------------------------

where *Q* is the overtopping flow rate (cfs), *H* is the height of the
upstream water surface above the roadway crest (ft), *L* is the length
of the roadway crest (ft), *C~W~* is free flow weir discharge
coefficient (ft^1/2^/sec) and *f~S~* is a submergence adjustment factor.

Values for the flow coefficients *C~W~* and *f~S~* have been published
by the FHWA as functions of the headwater depth (*H*), tailwater depth
(*h~t~*), the width of the roadway (*L~r~*), and road surface material.
The functions are shown in graphical form in Figure 7-6.

<figure>
<img src="VolumeII/media/media/image58.png"
style="width:6.5in;height:6.70694in" alt="RoadwayWeirCoeffs.png" />
<figcaption><p><span id="_Toc484694747"
class="anchor"></span><strong>Figure 7‑6 Discharge coefficients for
roadway weirs (from FHWA, 2012)</strong></p></figcaption>
</figure>

To summarize, SWMM includes an additional type of weir, a roadway weir,
used to model flow over the top of a road that sits above a culvert or
possibly an embankment. Its properties include the following:

- crest elevation (typically the elevation of the road surface)

<!-- -->

- crest length (determined by the top width of the channel that the road
  crosses)

- width of the roadway (perpendicular to its crest length).

- whether the road surface is paved or gravel.

Unlike the other weirs discussed in Chapter 6, a roadway weir has
neither a control setting nor a flap gate. Its flow versus head relation
is given by equation 7-52, where the head *H* is the difference between
the head at its inlet node and its crest elevation, the tailwater head
*h~t~* is the difference between its outlet node head and its crest
elevation, and its flow coefficients are determined from the curves in
Figure 7-6.

## 7.6 Storm Drain Inlets

Storm drain inlets are structures that convey runoff from roadway
pavements into below ground storm sewers (see Figure 7-7). Inlet type,
sizing and spacing are normally chosen to meet limits on the spread and
depth of water across a roadway as set by local drainage agencies to
maintain public safety. The U.S. Department of Transportation Federal
Highway Administration's Urban Drainage Design Manual (HEC-22) (FHWA,
2009) contains experimentally derived equations for computing the amount
of flow captured by different types of inlets. These equations are
widely used throughout North America and have been incorporated into
SWMM's flow routing routines.

  ---------------------------------------------------------------------------------------------------------------------------------------------------
  ![Grate.png](VolumeII/media/media/image59.png){width="2.4482589676290463in"   ![CurbOpening.png](VolumeII/media/media/image60.png){width="3.0569444444444445in"
  height="2.068134295713036in"}                                          height="1.8127537182852143in"}
  ---------------------------------------------------------------------- ----------------------------------------------------------------------------

  ---------------------------------------------------------------------------------------------------------------------------------------------------

Figure 7-7 Examples of storm drain inlets.

Figure 7-8 depicts the different types of street inlet structures whose
hydraulic performance is computed using the HEC-22 procedures. In
addition to these standard inlet types, a custom inlet can also be
deployed. Its performance is defined by a user-supplied rating curve of
captured flow as a function of flow depth, a diversion curve of captured
flow as a function of flow upstream of the inlet, or both curves (see
Figure 7-9). These curves are defined by a tabular listing of their data
points.

> ![Fig2a.png](VolumeII/media/media/image61.png){width="4.448537839020123in"
> height="3.146272965879265in"}

Figure 7-8 Standard types of curb and gutter inlets

![](VolumeII/media/media/image62.png){width="2.75in" height="1.8125in"}

Figure 7-9 Performance curve for a custom inlet

### 7.6.1 Model Setup

To add storm drain inlet modeling into SWMM a site is represented as a
dual drainage system consisting of both street conduits along the ground
surface and sewer conduits below it as shown in Figure 7-10. An inlet
structure will divert some portion of the street flow it sees into a
designated node of the sewer system with the rest being bypassed to
downstream streets. When an inlet's sewer node reaches its full depth
any excess flow that would cause it to flood is sent back through the
inlet and onto the street.

![DualDrainage.png](VolumeII/media/media/image63.png){width="4.908233814523185in"
height="3.2929975940507434in"}

Figure 7-10 Representation of a dual drainage system

The HEC-22 procedures assume that the curb and gutter inlets shown in
Figure 7-8 are placed in conduits that have a Street cross-section shape
as described in Section 5.4. Even if a street conduit does not contain
inlets, it should still be assigned a Street cross-section if the spread
and depth of surface water across it needs to be reported. The Street
cross-sections can be either single sided (have a single section that
slopes downward from the street crown) or dual sided (have mirror image
sloping sections on either side of the street crown) as required.

Streets can be assigned any number of a specific inlet design (grate,
curb opening, combination, or slotted drain). If the cross-section is
dual sided then each side receives the replicate number of inlets. If a
street needs to use a mix of inlet designs then it needs to be divided
into separate street conduits where each utilizes just a single type of
design.

As shown in Figure 7-10, inlets can be located either on a continuous
sloping section of roadway (on-grade) or at a low point where flow tends
to pool (on-sag). The HEC-22 procedures make flow capture for on-grade
inlets a function of the approach flow rate they see while flow capture
for on-sag inlets depends on the depth of water that pools above them.

Because there is no physical link in the model, such as a shared
manhole, weir or orifice, that connects the street and sewer system to
one another, there is no need to have the rim elevation of a sewer
manhole match the invert elevation of the street node that sends inlet
flow into it.

### 7.6.2 Computational Scheme

To account for the flow capture and diversion provided by inlets, at
each flow routing time step SWMM adjusts the lateral inflows seen at the
downstream nodes of street conduits with inlets and at the sewer nodes
designated to receive inlet flow. These lateral flows contribute to the
node flow continuity equation 3-5 for dynamic wave routing and to the
link flow continuity equation 4-8 for kinematic wave routing. The steps
involved can be summarized as follows:

1.  Compute the flow captured at each street inlet using the HEC-22
    procedures for standard inlets or table lookup for custom inlets,
    using current values of street flow rates and flow depths.

2.  Add each inlet's captured flow to the lateral inflow that enters the
    inlet's assigned sewer node and subtract that same flow from the
    lateral inflow seen by the downstream node of the inlet's street
    conduit.

3.  Add any current overflow (i.e., flooding) that an inlet's sewer node
    experiences onto the lateral inflow for the inlet's street node
    instead of having it leave the system as it normally would. This
    allows for a two-way flow exchange between the street and sewer once
    the water level in the sewer node reaches the ground elevation.

4.  Apply the usual flow routing step normally taken by SWMM's routing
    methods.

In Step 1, because each side of a two-sided street has the same
cross-section geometry and number of inlets, flow capture is computed
for only one side using half the total street flow as the approach flow
seen by its inlets. The one-sided flow capture is then doubled to
determine the full flow capture for the entire street.

### 7.6.3 Flow Capture for On-Grade Inlets

The flow capture efficiency of an inlet placed on-grade is affected by:

- inlet type and dimensions

- approach flow rate, velocity, and spread of water on the street

- street cross slope and curb depression

- longitudinal street slope and surface roughness.

[Grate Inlets]{.underline}

For a standard street grate located on-grade the HEC-22 equation for
flow capture is:

  ----------------------------------------------------------------------------
        $$Q_{c} = Q\left\{ R_{f}E_{0} + R_{s}(1 - E_{0}) \right\}$$   (7-53)
  ----- ------------------------------------------------------------- --------

  ----------------------------------------------------------------------------

where:

  ----------------------------------------------------------------------------
  *Q~c~*    =  captured flow (cfs)
  -------- --- ---------------------------------------------------------------
  *Q*       =  approach flow (cfs)

  *E~0~*    =  ratio of flow over the grate's width to total flow

  *R~f~*    =  frontal capture efficiency

  *R~s~*    =  side capture efficiency
  ----------------------------------------------------------------------------

The frontal capture efficiency *R~f~* is:

  --------------------------------------------------------------------------
        $$R_{f} = 1 - 0.09\ max(0,\ V - V_{o})$$                    (7-54)
  ----- ----------------------------------------------------------- --------

  --------------------------------------------------------------------------

while the side capture efficiency *R~s~* is:

  ------------------------------------------------------------------------------
        $$R_{s} = 1/\left\{ 1 + 0.15V^{1.8}/(S_{x}L^{2.3}) \right\}$$   (7-55)
  ----- --------------------------------------------------------------- --------

  ------------------------------------------------------------------------------

with:

  ----------------------------------------------------------------------------
  *V*       =  velocity of flow over the grate (ft/sec)
  -------- --- ---------------------------------------------------------------
  *V~0~*    =  velocity at which water begins to splash over the inlet
               (ft/sec)

  *S~x~*    =  street cross slope (ft/ft)

  *L*       =  grate length (ft)
  ----------------------------------------------------------------------------

FHWA (2009) contains curves showing how the splash-over velocity *V~0~*
increases with increasing grate length *L* for the common grate designs
listed in Table 7-5. Table 7-6 contains polynomial expressions that were
fit to these curves by UDFCD (2010) that are used by SWMM. For grates
that do not conform to one of the listed designs the splash-over
velocity must be supplied by the user.

**Table 7-5 Description of grate inlet types^1^**

+------------+-------------------------------------------------------------+---------------------------------------------+
| **Grate    | **Layout**                                                  | **Description**                             |
| Type**     |                                                             |                                             |
+============+:============================================================+:============================================+
| P-50       | ![](VolumeII/media/media/image64.png){width="0.4270833333333333in" | Parallel bar grate with 1-7/8" bar spacing  |
|            | height="0.3541666666666667in"}                              | on center                                   |
+------------+-------------------------------------------------------------+---------------------------------------------+
| P-50x100   | ![](VolumeII/media/media/image65.emf){width="0.5104166666666666in" | Parallel bar grate with 1-7/8" bar spacing  |
|            | height="0.34375in"}                                         | on center and 3/8" diameter lateral rods    |
|            |                                                             | spaced at 4" on center                      |
+------------+-------------------------------------------------------------+---------------------------------------------+
| P-30       | ![](VolumeII/media/media/image66.emf){width="0.4583333333333333in" | Parallel bar grate with 1-1/8" bar spacing  |
|            | height="0.34375in"}                                         | on center                                   |
+------------+-------------------------------------------------------------+---------------------------------------------+
| Curved     | ![](VolumeII/media/media/image67.png){width="0.9686286089238845in" | Curved vane grate with 3-1/4" longitudinal  |
| Vane       | height="0.6457524059492563in"}                              | bar and 4-1/4" transverse bar spacing on    |
|            |                                                             | center                                      |
+------------+-------------------------------------------------------------+---------------------------------------------+
| 45° Tilt   | ![](VolumeII/media/media/image68.png){width="0.9477985564304462in" | 45° tilt-bar grate with 3-1/4" longitudinal |
| Bar        | height="0.635336832895888in"}                               | bar and 4" transverse bar spacing on center |
+------------+-------------------------------------------------------------+---------------------------------------------+
| 30° Tilt   | ![](VolumeII/media/media/image69.png){width="0.9269674103237096in" | 30° tilt-bar grate with 3-1/4" longitudinal |
| Bar        | height="0.6457524059492563in"}                              | bar and 4"                                  |
|            |                                                             |                                             |
|            |                                                             | transverse bar spacing on center            |
+------------+-------------------------------------------------------------+---------------------------------------------+
| Reticuline | ![](VolumeII/media/media/image70.png){width="0.6769991251093613in" | Honeycomb pattern of lateral bars and       |
|            | height="0.39578412073490815in"}                             | longitudinal bearing bars                   |
+------------+-------------------------------------------------------------+---------------------------------------------+

^1^See FHWA (2009) for more detailed descriptions and pictures.

**Table 7-6 Splash-over velocity for different types of grate
inlets^1^**

  -----------------------------------------------------------------------
  **Grate Type**   **Splash over velocity *V~0~* (ft/s) as a function of
                   grate length *L* (ft)**
  ---------------- ------------------------------------------------------
  P-50             $$V_{0} = 2.22 + 4.03L - 0.65L^{2} + 0.06L^{3}$$

  P-50x100         $$V_{0} = 0.74 + 2.44L - 0.27L^{2} + 0.02L^{3}$$

  P-30             $$V_{0} = 1.76 + 3.12L - 0.45L^{2} + 0.03L^{3}$$

  Curved Vane      $$V_{0} = 0.30 + 4.85L - 1.31L^{2} + 0.15L^{3}$$

  45° Tilt Bar     $$V_{0} = 0.99 + 2.64L - 0.36L^{2} + 0.03L^{3}$$

  30° Tilt Bar     $$V_{0} = 0.51 + 2.34L - 0.20L^{2} + 0.01L^{3}$$

  Reticuline       $$V_{0} = 0.28 + 2.28L - 0.18L^{2} + 0.01L^{3}$$
  -----------------------------------------------------------------------

^1^ Source: Denver UDFCD (2010).

[Curb Opening Inlets]{.underline}

For a curb opening inlet located on-grade, the HEC-22 equation for flow
capture is:

  -----------------------------------------------------------------------------------
        $$Q_{c} = Q\left\{ 1 - {(1 - min(1,\ L/L_{T})\ )}^{1.8} \right\}$$   (7-56)
  ----- -------------------------------------------------------------------- --------

  -----------------------------------------------------------------------------------

*L* is now the length of the curb opening and *L~T~* is the length at
which complete flow capture occurs. The latter quantity is computed as:

  --------------------------------------------------------------------------
        $$L_{T} = 0.6Q^{0.42}S_{L}^{0.3}{(nS_{e})}^{- 0.6}$$        (7-57)
  ----- ----------------------------------------------------------- --------

  --------------------------------------------------------------------------

where:

  ---------------------------------------------------------------------------
  *S~L~*    =  longitudinal street slope (ft/ft)
  -------- --- --------------------------------------------------------------
  *n*       =  Manning's roughness coefficient for the street surface

  *S~e~*    =  $$S_{x} + (\frac{a}{W)E_{0}}$$

  *a*       =  curb depression (ft)

  *W*       =  depressed gutter width (ft)

  *E~0~*    =  ratio of flow over depressed gutter width to total flow
  ---------------------------------------------------------------------------

If *L \> L~T~* then complete capture is obtained.

[Computing *E~0~*]{.underline}

The on-grade flow capture formulas for grate and curb opening inlets
need to know *E~o~*, the fraction of total street flow *Q* within a
distance *W* from the curb or as depicted in Figure 7-11, the ratio of
*Q~W~* to *Q*. For grates this distance is the width of the grate. For
curb openings it is the width of the depressed gutter (if present).

![CompositeStreetSection2.png](VolumeII/media/media/image71.png){width="6.290880358705162in"
height="2.593426290463692in"}

Figure 7-11 Street cross-section divided into gutter and roadway flow

HEC-22 bases its determination of *E~0~* on Izzard's form of the Manning
equation that relates flow spread *T* to flow rate *Q* for a triangular
cross-section. It is derived from the standard Manning equation by
integrating the hydraulic radius across successive increments of street
width. The result for US standard units is:

  --------------------------------------------------------------------------
        $$Q = (\frac{0.56}{n)S_{x}^{1.67}S_{L}^{0.5}T^{2.67}}$$     (7-58)
  ----- ----------------------------------------------------------- --------

  --------------------------------------------------------------------------

(Note: the standard Manning equation has the same form except with the
constant being 0.47.)

Solving for T as a function of Q gives:

  --------------------------------------------------------------------------------------------------
        $$T = \left\lbrack \frac{Qn}{0.56S_{x}^{1.67}S_{L}^{0.5}} \right\rbrack^{0.375}$$   (7-59)
  ----- ----------------------------------------------------------------------------------- --------

  --------------------------------------------------------------------------------------------------

If the street has a uniform cross slope ($a = 0$ in Figure 7-12) then
Equation 7-59 can be used to derive the following expression for*E~0~*:

  --------------------------------------------------------------------------
        $$E_{0} = 1 - {(1 - \frac{W}{T})}^{2.67}$$                  (7-60)
  ----- ----------------------------------------------------------- --------

  --------------------------------------------------------------------------

where *T* is evaluated at a given flow rate *Q*.

For a compound street cross-section with depressed curb ($a > 0$ in
Figure 7-12), HEC-22 provides the following equation for *E~0~*:

  ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------
        $$E_{0} = \frac{1}{1 + \frac{\frac{S_{W}}{S_{X}}}{\left\lbrack 1 + \frac{\frac{S_{W}}{S_{X}}}{\left( \frac{T}{W} - 1 \right)} \right\rbrack^{2.67} - 1}}$$   (7-61)
  ----- ------------------------------------------------------------------------------------------------------------------------------------------------------------ --------

  ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------

where $S_{W} = S_{X} + \frac{a}{W}$. It is not possible to solve
directly for *E~0~* since Equation 7-59 for *T(Q)* applies only to a
triangular section of uniform slope.

To solve equation 7-61 let *Q~X~* and *T~X~* denote the flow and spread,
respectively, across the non-depressed triangular portion of the
street's cross section. Then the following relations apply:

  --------------------------------------------------------------------------
        $$Q_{X} = Q(1 - E_{0})$$                                      (7-62)
  ----- ----------------------------------------------------------- --------
        $$T_{X} = T - W$$                                             (7-63)

        $$\frac{T}{W} - 1 = \frac{T_{X}}{W}$$                         (7-64)
  --------------------------------------------------------------------------

These can be used in the following iterative procedure to find *E~0~*
for a particular flow rate *Q*:

1.  Assume a value for *T~X~*.

2.  Use Equation 7-61 to compute *E~0~*, with *T~X~/W* substituted for
    *T/W -- 1*.

3.  Compute *Q~X~* from Equation 7-62.

4.  Compute a new value for *T~X~* using Equation 7-59 with *Q~X~* as
    the flow rate.

5.  If there is negligible change in *T~X~* then stop with the last
    value of *E~0~* as the solution. Otherwise return to Step 2.

In cases where the width of the grate is smaller than the width *W* of
the depressed gutter section, *E~0~* is adjusted by the ratio of the
flow area over the grate's width to the flow area over the depressed
gutter width.

[Combination Inlets]{.underline}

A combination inlet consists of both a grate and curb opening placed
together at the same location. Its on-grade flow capture equals that of
the grate plus any flow captured by the portion of the curb opening that
extends upstream of the grate's length. The latter flow capture is
computed first and is subtracted from the approach flow used to
determine the grate's flow capture.

[Slotted Inlets]{.underline}

The flow capture capability of a slotted inlet located on-grade is the
same as that of a curb opening inlet of equal length.

[Custom Inlets]{.underline}

If an on-grade custom inlet is supplied with a flow diversion curve
(captured flow v. approach flow) then that curve will be used to
determine its flow capture. If it only has a flow rating curve (captured
flow v. water depth) then that curve will be used in conjunction with
the water surface depth at the downstream end of the conduit containing
the inlet.

### 7.6.4 On-Sag Inlet Flow Capture

HEC-22 has flow capture efficiency for an inlet in a sag location depend
on the size of the inlet's opening and the depth of water that collects
next to it at the street curb. At low flow depths the inlet acts as a
weir with

  --------------------------------------------------------------------------
        $$Q_{c} = C_{W}L_{W}d^{1.5}$$                               (7-66)
  ----- ----------------------------------------------------------- --------

  --------------------------------------------------------------------------

while at higher depths it acts as an orifice with

  --------------------------------------------------------------------------
        $$Q_{c} = C_{O}A_{O}\sqrt{2gd}$$                            (7-67)
  ----- ----------------------------------------------------------- --------

  --------------------------------------------------------------------------

In these equations:

  ---------------------------------------------------------------------------
  *C~W~*    =  weir coefficient (ft^0.5^/sec)
  -------- --- --------------------------------------------------------------
  *C~O~*    =  orifice coefficient

  *g*       =  acceleration of gravity (ft/sec^2^)

  *L~W~*    =  effective length of inlet (ft)

  *A~O~*    =  open area of inlet (ft^2^)

  *d*       =  effective depth of water seen by the inlet (ft).
  ---------------------------------------------------------------------------

[Grate Inlets]{.underline}

For grate inlets the following values are used in Equations 7-66 and
7-67:

  ---------------------------------------------------------------------------
  *C~W~*    =  3.0
  -------- --- --------------------------------------------------------------
  *C~O~*    =  0.67

  *Lw*      =  *L + 2W*

  *A~O~*    =  $$LWf_{O}$$

  *d*       =  $$d_{i} - (\frac{W}{2)S_{W}}$$
  ---------------------------------------------------------------------------

where *L* is the grate's length, *W* its width, *f~O~* the ratio of open
area to full area, and *d~i~* is the depth of water at the downstream
node of the conduit containing the inlet. Opening ratios for several
types of grate designs are listed in Table 7-5.

HEC-22 does not provide clear guidance on what depth causes a switch
from weir flow to orifice flow for grates. Therefore it is assumed the
switch occurs at a depth *d* where Equation 7-66 equals Equation 7-67.
This results in weir flow for depths below $1.79\frac{A_{O}}{L_{W}}$ and
orifice flow for depths above it.

[Curb Opening Inlets]{.underline}

For streets with uniform cross slope or for openings greater than 12
feet in length, the values used in Equation 7-66 for weir flow are
*C~W~* = 3.0 and *L~W~* = opening length. Otherwise, *C~W~* = 2.3 and
$L_{W} = L + 1.8W$ where *L* = opening length and *W* = width of the
depressed gutter. The values used in Equation 7-67 for orifice flow are
$C_{O} = 0.67$ and $A_{O} = hL$ where *h* is the height of the opening.
The effective depth *d* seen by the curb opening inlet under orifice
flow depends on the orientation of the opening's throat relative to the
street surface as shown in Table 7-7.

Table 7-7 Effective depth for curb opening inlets under orifice flow

+---------------------------------------------------------------------------------------------------------+-------------------------------------+
| **Throat Angle**                                                                                        | **Effective Depth**                 |
+:======================+:===============================================================================:+:====================================+
| Horizontal            | ![HorizontalThroat.bmp](VolumeII/media/media/image72.png){width="1.4685662729658793in" | $$d = d_{i} - \frac{h}{2}$$         |
|                       | height="0.6769991251093613in"}                                                  |                                     |
+-----------------------+---------------------------------------------------------------------------------+-------------------------------------+
| Inclined              | ![InclinedThroat.bmp](VolumeII/media/media/image73.png){width="1.4477362204724409in"   | $$d = d_{i} - 0.7071(\frac{h}{2})$$ |
|                       | height="0.6978291776027996in"}                                                  |                                     |
+-----------------------+---------------------------------------------------------------------------------+-------------------------------------+
| Vertical              | ![VerticalThroat.bmp](VolumeII/media/media/image74.png){width="1.4789818460192476in"   | $$d = d_{i}$$                       |
|                       | height="0.6978291776027996in"}                                                  |                                     |
+-----------------------+---------------------------------------------------------------------------------+-------------------------------------+

HEC-22 states that weir flow for on-sag curb openings occurs at
effective depths below *h* while orifice flow occurs at depths greater
than *1.4h*. For depths in between these SWMM uses the following
interpolation formula:

  --------------------------------------------------------------------------
        $$Q_{c} = (1 - r)Q_{weir} + rQ_{orif}$$                     (7-68)
  ----- ----------------------------------------------------------- --------

  --------------------------------------------------------------------------

where *Q~weir~* is weir flow capture at depth *h*, *Q~orif~* is orifice
flow capture at depth *1.4h* and $r = \frac{(d - h)}{(0.4h)}$.

[Slotted Inlets]{.underline}

For slotted inlets the variables in the weir and orifice equations 7-66
and 7-67 are as follows:

  ---------------------------------------------------------------------------
  *C~W~*    =  2.48
  -------- --- --------------------------------------------------------------
  *C~O~*    =  0.8

  *Lw*      =  *L*

  *A~O~*    =  $$LW$$

  *d~i~*    =  $$d$$
  ---------------------------------------------------------------------------

where *L* = inlet length and *W* = inlet width. Weir flow holds for
$d \leq 0.2$ feet while orifice flow occurs for $d \geq 0.4$ feet. In
between these values flow capture is computed using Equation 7-68 with
*Q~weir~* as weir flow capture at depth 0.2, *Q~orif~* as orifice flow
capture at depth 0.4 and $r = \frac{(d - 0.2)}{0.2}$.

[Custom Inlets]{.underline}

If an on-sag custom inlet is supplied with a flow rating curve (captured
flow v. water depth), then that curve is used to determine its flow
capture. The depth supplied to the curve's lookup table is the depth of
the downstream node of the conduit containing the inlet. If the inlet
was only assigned a diversion curve (captured flow v. approach flow)
then that curve is used thus essentially treating the inlet as if it
were on-grade.

### 7.6.5 Drop Inlets

Drop inlets, pictured in Figure 7-12, are used to drain water from
roadside ditches, swales and flat bottom channels. SWMM allows these
structures to be placed in open channels that have either a rectangular
or trapezoidal cross-section. Model set-up for utilizing these inlets is
the same as described in section 7.6.2 and the same computational scheme
applies. The methods used to compute their flow capture efficiencies are
described in the following paragraphs.

> ![Fig2b.png](VolumeII/media/media/image75.png){width="4.469374453193351in"
> height="1.5731364829396326in"}

Figure 7-12 Types of channel drop inlets

[Drop Grate Inlets]{.underline}

The flow capture equation for a drop grate inlet located on-grade is the
same as for a street grate (see equation 7-53) except that the ratio
*E~0~* of flow over the grate to total cross-section flow *Q* is given
by:

  ---------------------------------------------------------------------------------------
        $$E_{0} = \frac{1.486\sqrt{S_{L}}{(yW)}^{1.67}}{nQ{(W + 2y)}^{0.67}}$$   (7-69)
  ----- ------------------------------------------------------------------------ --------

  ---------------------------------------------------------------------------------------

where

  ---------------------------------------------------------------------------
  *W*       =  side length of grate parallel to flow direction (ft)
  -------- --- --------------------------------------------------------------
  *y*       =  flow depth in the channel (ft)

  *n*       =  channel Manning's roughness coefficient

  *S~L~*    =  channel longitudinal slope (ft/ft)
  ---------------------------------------------------------------------------

A cross slope *S~X~* of 1% is assumed unless the grate extends across
the entire bottom width of a trapezoidal channel. In that case *S~X~* is
taken as the slope of the channel's side wall.

Drop grates located on-sag use the same weir and orifice equations (7-66
and 7-67) as do street grates with the only difference being that for
weir flow the effective length of the inlet's sides (*L~W~* in equation
7-66) is the sum of the lengths of all four sides.

[Drop Curb Inlets]{.underline}

Flow capture for drop curb inlets is computed the same as for curb
opening inlets located on sag. The only difference is that the effective
length of the opening is the total length of all four sides and the open
area is the height of the opening times the total length of all four
sides.

### 7.6.6 Additional Considerations

[]{#_Toc73193124 .anchor}

# Appendix

\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_

#### Newton-Raphson-Bisection Root Finding Method

The following Newton-Raphson procedure adapted from Press et al. (1992)
is used to solve the equation $f(x) = 0$ over the interval
$\left\lbrack x_{LOW},x_{HIGH} \right\rbrack$ that is known to bracket
the solution with initial estimate *x* and convergence tolerance *ε* :

# 

#### Ridder's Root Finding Method

Ridder's method uses the following iterative procedure adapted from
Press et al. (1992) to solve the equation $f(x) = 0$ over the interval
$\left\lbrack x_{1},x_{2} \right\rbrack$ that is known to bracket the
solution with a convergence tolerance of *ε*:

#### Section Properties of Circular Pipes

  ----------------------------------------------------------------------------------------------------------------------------------------------------
    ***Y/Y~full~***   ***A/A~full~***   ***W/W~max~***   ***R/R~full~***          ***Y/Y~full~***   ***A/A~full~***   ***W/W~max~***   ***R/R~full~***
  ----------------- ----------------- ---------------- ----------------- ------ ----------------- ----------------- ---------------- -----------------
               0.00           0.00000          0.00000           0.01000                     0.52           0.52550          0.99920           1.02400

               0.02           0.00471          0.28000           0.05280                     0.54           0.55093          0.99680           1.04800

               0.04           0.01340          0.39190           0.10480                     0.56           0.57630          0.99280           1.07000

               0.06          0.024446          0.47500           0.15560                     0.58           0.60135          0.98710           1.09120

               0.08           0.03740          0.54260           0.20520                     0.60           0.62640          0.97980           1.11000

               0.10           0.05208          0.60000           0.25400                     0.62           0.65126          0.97080           1.12720

               0.12           0.06800          0.64990           0.30160                     0.64           0.67580          0.96000           1.14400

               0.14           0.08505          0.69400           0.34840                     0.66           0.70015          0.94740           1.15960

               0.16           0.10330          0.73320           0.39440                     0.68           0.72410          0.93300           1.17400

               0.18           0.12236          0.76840           0.43880                     0.70           0.74764          0.91650           1.18480

               0.20           0.14230          0.80000           0.48240                     0.72           0.77080          0.89800           1.19400

               0.22           0.16310          0.82850           0.52480                     0.74           0.79335          0.87730           1.20240

               0.24           0.18450          0.85420           0.56640                     0.76           0.81540          0.85420           1.21000

               0.26           0.20665          0.87730           0.60640                     0.78           0.83690          0.82850           1.21480

               0.28           0.22920          0.89800           0.64560                     0.80           0.85760          0.80000           1.21700

               0.30           0.25236          0.91650           0.68360                     0.82           0.87764          0.76840           1.21720

               0.32           0.27590          0.93300           0.72040                     0.84           0.89670          0.73320           1.21500

               0.34           0.29985          0.94740           0.75640                     0.86           0.91495          0.69400           1.21040

               0.36           0.32420          0.96000           0.79120                     0.88           0.93200          0.64990           1.20300

               0.38           0.34874          0.97080           0.82440                     0.90           0.94792          0.60000           1.19200

               0.40           0.37360          0.97980           0.85680                     0.92           0.96260          0.54260           1.17800

               0.42           0.39878          0.98710           0.88800                     0.94           0.97555          0.47500           1.15840

               0.44           0.42370          0.99280           0.91760                     0.96           0.98660          0.39190           1.13200

               0.46           0.44907          0.99680           0.94640                     0.98           0.99516          0.28000           1.09400

               0.48           0.47450          0.99920           0.97360                     1.00           1.00000          0.00000           1.00000

               0.50           0.50000          1.00000           1.00000                                                             
  ----------------------------------------------------------------------------------------------------------------------------------------------------

  : []{#_Toc484695150 .anchor}**Table C‑1 Circular section properties as
  function of depth**

  ----------------------------------------------------------------------------------------------------------------------
    ***A/A~full~***   ***Y/Y~full~***   ***Ψ/Ψ~full~***              ***A/A~full~***   ***Y/Y~full~***   ***Ψ/Ψ~full~***
  ----------------- ----------------- ----------------- ---------- ----------------- ----------------- -----------------
               0.00            0.0000           0.00000                         0.52           0.51572           0.52658

               0.02           0.05236           0.00529                         0.54           0.53146           0.55354

               0.04           0.08369           0.01432                         0.56           0.54723           0.58064

               0.06           0.11025           0.02559                         0.58           0.56305           0.60777

               0.08           0.13423           0.03859                         0.60           0.57892           0.63499

               0.10           0.15643           0.05304                         0.62           0.59487           0.66232

               0.12           0.17755           0.06877                         0.64           0.61093           0.68995

               0.14           0.19772           0.08551                         0.66           0.62710           0.71770

               0.16           0.21704           0.10326                         0.68           0.64342           0.74538

               0.18           0.23581           0.12195                         0.70           0.65991           0.77275

               0.20           0.25412           0.14144                         0.72           0.67659           0.79979

               0.22           0.27194           0.16162                         0.74           0.69350           0.82658

               0.24           0.28948           0.18251                         0.76           0.71068           0.85320

               0.26           0.30653           0.20410                         0.78           0.72816           0.87954

               0.28           0.32349           0.22636                         0.80           0.74602           0.90546

               0.30           0.34017           0.24918                         0.82           0.76424           0.93095

               0.32           0.35666           0.27246                         0.84           0.78297           0.95577

               0.34           0.37298           0.29614                         0.86           0.80235           0.97976

               0.36           0.38915           0.32027                         0.88           0.82240           1.00291

               0.38           0.40521           0.34485                         0.90           0.84353           1.02443

               0.40           0.42117           0.36989                         0.92           0.86563           1.04465

               0.42           0.43704           0.39531                         0.94           0.88970           1.06135

               0.44           0.45284           0.42105                         0.96           0.91444           1.08208

               0.46           0.46858           0.44704                         0.98           0.94749           1.07662

               0.48           0.48430           0.47329                         1.00            1.0000           1.00000

               0.50           0.50000           0.49980                                                
  ----------------------------------------------------------------------------------------------------------------------

  : []{#_Toc484690441 .anchor}**Table C‑2 Circular section properties as
  function of area**

#### Section Properties of Elliptical Pipes

[]{#_Toc484695151 .anchor}

  --------------------------------------------------------------------------
   **Code**    **Minor Axis    **Major Axis    ***A~full~*     ***R~full~*
                  (in)**          (in)**        (ft^2^)**        (ft)**
  ---------- ---------------- -------------- --------------- ---------------
      1             14              23            1.80            0.367

      2             19              30            3.30            0.490

      3             22              34            4.10            0.546

      4             24              38            5.10            0.613

      5             27              42            6.30            0.686

      6             29              45            7.40            0.736

      7             32              49            8.80            0.812

      8             34              53            10.20           0.875

      9             38              60            12.90           0.969

      10            43              68            16.60           1.106

      11            48              76            20.50           1.229

      12            53              83            24.80           1.352

      13            58              91            29.50           1.475

      14            63              98            34.60           1.598

      15            68             106            40.10           1.721

      16            72             113            46.10           1.845

      17            77             121            52.40           1.967

      18            82             128            59.20           2.091

      19            87             136            66.40           2.215

      20            92             143            74.00           2.340

      21            97             151            82.00           2.461

      22           106             166            99.20           2.707

      23           116             180           118.60           2.968
  --------------------------------------------------------------------------

  : **Table D‑1 Standard elliptical pipe sizes**

Note: The Minor Axis is the maximum width for a vertical ellipse and the
full depth for a horizontal ellipse while the Major Axis is the maximum
width for a horizontal ellipse and the full depth for a vertical
ellipse.

Source: American Concrete Pipe Association (2011).

+-----------------+----------------------------------------------------+------+----------------------------------------------------+
| ***Y/Y~full~*** | **Horizontal Ellipse**                             |      | **Vertical Ellipse**                               |
|                 +-----------------+----------------+-----------------+------+-----------------+----------------+-----------------+
|                 | ***A/A~full~*** | ***W/W~max~*** | ***R/R~full~*** |      | ***A/A~full~*** | ***W/W~max~*** | ***R/R~full~*** |
+================:+================:+===============:+================:+=====:+================:+===============:+================:+
| 0.00            | 0.000           | 0.0000         | 0.0100          |      | 0.000           | 0.0000         | 0.0100          |
+-----------------+-----------------+----------------+-----------------+------+-----------------+----------------+-----------------+
| 0.04            | 0.015           | 0.3919         | 0.0764          |      | 0.010           | 0.3919         | 0.1250          |
+-----------------+-----------------+----------------+-----------------+------+-----------------+----------------+-----------------+
| 0.08            | 0.040           | 0.5426         | 0.1726          |      | 0.040           | 0.5426         | 0.2436          |
+-----------------+-----------------+----------------+-----------------+------+-----------------+----------------+-----------------+
| 0.12            | 0.650           | 0.6499         | 0.2389          |      | 0.070           | 0.6499         | 0.3536          |
+-----------------+-----------------+----------------+-----------------+------+-----------------+----------------+-----------------+
| 0.16            | 0.950           | 0.7332         | 0.3274          |      | 0.100           | 0.7332         | 0.4474          |
+-----------------+-----------------+----------------+-----------------+------+-----------------+----------------+-----------------+
| 0.20            | 0.130           | 0.8000         | 0.4191          |      | 0.140           | 0.8000         | 0.5484          |
+-----------------+-----------------+----------------+-----------------+------+-----------------+----------------+-----------------+
| 0.24            | 0.165           | 0.8542         | 0.5120          |      | 0.185           | 0.8542         | 0.6366          |
+-----------------+-----------------+----------------+-----------------+------+-----------------+----------------+-----------------+
| 0.28            | 0.205           | 0.8980         | 0.5983          |      | 0.230           | 0.8980         | 0.7155          |
+-----------------+-----------------+----------------+-----------------+------+-----------------+----------------+-----------------+
| 0.32            | 0.250           | 0.9330         | 0.6757          |      | 0.280           | 0.9330         | 0.7768          |
+-----------------+-----------------+----------------+-----------------+------+-----------------+----------------+-----------------+
| 0.36            | 0.300           | 0.9600         | 0.7630          |      | 0.330           | 0.9600         | 0.8396          |
+-----------------+-----------------+----------------+-----------------+------+-----------------+----------------+-----------------+
| 0.40            | 0.355           | 0.9798         | 0.8326          |      | 0.380           | 0.9798         | 0.8969          |
+-----------------+-----------------+----------------+-----------------+------+-----------------+----------------+-----------------+
| 0.44            | 0.415           | 0.9928         | 0.9114          |      | 0.430           | 0.9928         | 0.9480          |
+-----------------+-----------------+----------------+-----------------+------+-----------------+----------------+-----------------+
| 0.48            | 0.480           | 0.9992         | 0.9702          |      | 0.480           | 0.9992         | 0.9925          |
+-----------------+-----------------+----------------+-----------------+------+-----------------+----------------+-----------------+
| 0.52            | 0.520           | 0.9992         | 1.030           |      | 0.520           | 0.9992         | 1.023           |
+-----------------+-----------------+----------------+-----------------+------+-----------------+----------------+-----------------+
| 0.56            | 0.585           | 0.9928         | 1.091           |      | 0.570           | 0.9928         | 1.053           |
+-----------------+-----------------+----------------+-----------------+------+-----------------+----------------+-----------------+
| 0.60            | 0.645           | 0.9798         | 1.146           |      | 0.620           | 0.9798         | 1.084           |
+-----------------+-----------------+----------------+-----------------+------+-----------------+----------------+-----------------+
| 0.64            | 0.700           | 0.9600         | 1.185           |      | 0.670           | 0.9600         | 1.107           |
+-----------------+-----------------+----------------+-----------------+------+-----------------+----------------+-----------------+
| 0.68            | 0.750           | 0.9330         | 1.225           |      | 0.720           | 0.9330         | 1.130           |
+-----------------+-----------------+----------------+-----------------+------+-----------------+----------------+-----------------+
| 0.72            | 0.795           | 0.8980         | 1.257           |      | 0.770           | 0.8980         | 1.154           |
+-----------------+-----------------+----------------+-----------------+------+-----------------+----------------+-----------------+
| 0.76            | 0.835           | 0.8542         | 1.274           |      | 0.815           | 0.8542         | 1.170           |
+-----------------+-----------------+----------------+-----------------+------+-----------------+----------------+-----------------+
| 0.80            | 0.870           | 0.8000         | 1.290           |      | 0.860           | 0.8000         | 1.177           |
+-----------------+-----------------+----------------+-----------------+------+-----------------+----------------+-----------------+
| 0.84            | 0.905           | 0.7332         | 1.282           |      | 0.900           | 0.7332         | 1.177           |
+-----------------+-----------------+----------------+-----------------+------+-----------------+----------------+-----------------+
| 0.88            | 0.935           | 0.6499         | 1.274           |      | 0.930           | 0.6499         | 1.170           |
+-----------------+-----------------+----------------+-----------------+------+-----------------+----------------+-----------------+
| 0.92            | 0.960           | 0.5426         | 1.257           |      | 0.960           | 0.5426         | 1.162           |
+-----------------+-----------------+----------------+-----------------+------+-----------------+----------------+-----------------+
| 0.96            | 0.985           | 0.3919         | 1.185           |      | 0.990           | 0.3919         | 1.122           |
+-----------------+-----------------+----------------+-----------------+------+-----------------+----------------+-----------------+
| 1.00            | 1.000           | 0.0000         | 1.000           |      | 1.000           | 0.0000         | 1.000           |
+-----------------+-----------------+----------------+-----------------+------+-----------------+----------------+-----------------+

: []{#_Toc484695152 .anchor}**Table D‑2 Elliptical section properties as
function of depth**

#### Section Properties of Arch Pipes

[]{#_Toc484695153 .anchor}

+-------------------------------+-------------------------------+-------------------------------+---------------+---------------+
| **Code**                      | **Rise (*Y~full~*) (in)**     | **Span (*W~max~*) (in)**      | ***A~full~*   | ***R~full~*   |
|                               |                               |                               | (in^2^)**     | (in)**        |
+===============+===============+===============+===============+===============+===============+===============+===============+
| **Concrete**                                                                                                                  |
+-------------------------------+-------------------------------+-------------------------------+---------------+---------------+
| 1                             | 11                            | 18                            | 1.1           | 0.25          |
+-------------------------------+-------------------------------+-------------------------------+---------------+---------------+
| 2                             | 13.5                          | 22                            | 1.65          | 0.30          |
+-------------------------------+-------------------------------+-------------------------------+---------------+---------------+
| 3                             | 15.5                          | 26                            | 2.2           | 0.36          |
+-------------------------------+-------------------------------+-------------------------------+---------------+---------------+
| 4                             | 18                            | 28.5                          | 2.8           | 0.45          |
+-------------------------------+-------------------------------+-------------------------------+---------------+---------------+
| 5                             | 22.5                          | 36.25                         | 4.4           | 0.56          |
+-------------------------------+-------------------------------+-------------------------------+---------------+---------------+
| 6                             | 26.625                        | 43.75                         | 6.4           | 0.68          |
+-------------------------------+-------------------------------+-------------------------------+---------------+---------------+
| 7                             | 31.3125                       | 51.125                        | 8.8           | 0.80          |
+-------------------------------+-------------------------------+-------------------------------+---------------+---------------+
| 8                             | 36                            | 58.5                          | 11.4          | 0.90          |
+-------------------------------+-------------------------------+-------------------------------+---------------+---------------+
| 9                             | 40                            | 65                            | 14.3          | 1.01          |
+-------------------------------+-------------------------------+-------------------------------+---------------+---------------+
| 10                            | 45                            | 73                            | 17.7          | 1.13          |
+-------------------------------+-------------------------------+-------------------------------+---------------+---------------+
| 11                            | 54                            | 88                            | 25.6          | 1.35          |
+-------------------------------+-------------------------------+-------------------------------+---------------+---------------+
| 12                            | 62                            | 102                           | 34.6          | 1.57          |
+-------------------------------+-------------------------------+-------------------------------+---------------+---------------+
| 13                            | 72                            | 115                           | 44.5          | 1.77          |
+-------------------------------+-------------------------------+-------------------------------+---------------+---------------+
| 14                            | 77.5                          | 122                           | 51.7          | 1.92          |
+-------------------------------+-------------------------------+-------------------------------+---------------+---------------+
| 15                            | 87.125                        | 138                           | 66.0          | 2.17          |
+-------------------------------+-------------------------------+-------------------------------+---------------+---------------+
| 16                            | 96.875                        | 154                           | 81.8          | 2.42          |
+-------------------------------+-------------------------------+-------------------------------+---------------+---------------+
| 17                            | 106.5                         | 168.75                        | 99.1          | 2.65          |
+-------------------------------+-------------------------------+-------------------------------+---------------+---------------+
| **Corrugated Steel, 2-2/3 x 1/2\" Corrugation**                                                                               |
+---------------+-------------------------------+-------------------------------+-------------------------------+---------------+
| 18            | 13                            | 17                            | 1.1                           | 0.324         |
+---------------+-------------------------------+-------------------------------+-------------------------------+---------------+
| 19            | 15                            | 21                            | 1.6                           | 0.374         |
+---------------+-------------------------------+-------------------------------+-------------------------------+---------------+
| 20            | 18                            | 24                            | 2.2                           | 0.449         |
+---------------+-------------------------------+-------------------------------+-------------------------------+---------------+
| 21            | 20                            | 28                            | 2.9                           | 0.499         |
+---------------+-------------------------------+-------------------------------+-------------------------------+---------------+
| 22            | 24                            | 35                            | 4.5                           | 0.598         |
+---------------+-------------------------------+-------------------------------+-------------------------------+---------------+
| 23            | 29                            | 42                            | 6.5                           | 0.723         |
+---------------+-------------------------------+-------------------------------+-------------------------------+---------------+
| 24            | 33                            | 49                            | 8.9                           | 0.823         |
+---------------+-------------------------------+-------------------------------+-------------------------------+---------------+
| 25            | 38                            | 57                            | 11.6                          | 0.947         |
+---------------+-------------------------------+-------------------------------+-------------------------------+---------------+
| 26            | 43                            | 64                            | 14.7                          | 1.072         |
+---------------+-------------------------------+-------------------------------+-------------------------------+---------------+
| 27            | 47                            | 71                            | 18.1                          | 1.171         |
+---------------+-------------------------------+-------------------------------+-------------------------------+---------------+
| 28            | 52                            | 77                            | 21.9                          | 1.296         |
+---------------+-------------------------------+-------------------------------+-------------------------------+---------------+
| 29            | 57                            | 83                            | 26.0                          | 1.421         |
+---------------+-------------------------------+-------------------------------+-------------------------------+---------------+

: **Table E‑1 Standard arch pipe sizes**

+-------------------+-------------------+-------------------+-------------------+-------------------+
| **Code**          | **Rise            | **Span (*W~max~*) | ***A~full~*       | ***R~full~*       |
|                   | (*Y~full~*)       | (in)**            | (in^2^)**         | (in)**            |
|                   | (in)**            |                   |                   |                   |
+===================+===================+===================+===================+===================+
| **Corrugated Steel, 3 x 1\" Corrugation**                                                         |
+-------------------+-------------------+-------------------+-------------------+-------------------+
| 30                | 31                | 40                | 7.0               | 0.773             |
+-------------------+-------------------+-------------------+-------------------+-------------------+
| 31                | 36                | 46                | 9.4               | 0.773             |
+-------------------+-------------------+-------------------+-------------------+-------------------+
| 32                | 41                | 53                | 12.3              | 1.022             |
+-------------------+-------------------+-------------------+-------------------+-------------------+
| 33                | 46                | 60                | 15.6              | 1.147             |
+-------------------+-------------------+-------------------+-------------------+-------------------+
| 34                | 51                | 66                | 19.3              | 1.271             |
+-------------------+-------------------+-------------------+-------------------+-------------------+
| 35                | 55                | 73                | 23.2              | 1.371             |
+-------------------+-------------------+-------------------+-------------------+-------------------+
| 36                | 59                | 81                | 27.4              | 1.471             |
+-------------------+-------------------+-------------------+-------------------+-------------------+
| 37                | 63                | 87                | 32.1              | 1.570             |
+-------------------+-------------------+-------------------+-------------------+-------------------+
| 38                | 67                | 95                | 37.0              | 1.670             |
+-------------------+-------------------+-------------------+-------------------+-------------------+
| 39                | 71                | 103               | 42.4              | 1.770             |
+-------------------+-------------------+-------------------+-------------------+-------------------+
| 40                | 75                | 112               | 48.0              | 1.869             |
+-------------------+-------------------+-------------------+-------------------+-------------------+
| 41                | 79                | 117               | 54.2              | 1.969             |
+-------------------+-------------------+-------------------+-------------------+-------------------+
| 42                | 83                | 128               | 60.5              | 2.069             |
+-------------------+-------------------+-------------------+-------------------+-------------------+
| 43                | 87                | 137               | 67.4              | 2.168             |
+-------------------+-------------------+-------------------+-------------------+-------------------+
| 44                | 91                | 142               | 74.5              | 2.268             |
+-------------------+-------------------+-------------------+-------------------+-------------------+
| **Structural Plate, 18\" Corner Radius**                                                          |
+-------------------+-------------------+-------------------+-------------------+-------------------+
| 45                | 55                | 73                | 22                | 1.371             |
+-------------------+-------------------+-------------------+-------------------+-------------------+
| 46                | 57                | 76                | 24                | 1.421             |
+-------------------+-------------------+-------------------+-------------------+-------------------+
| 47                | 59                | 81                | 26                | 1.471             |
+-------------------+-------------------+-------------------+-------------------+-------------------+
| 48                | 61                | 84                | 28                | 1.520             |
+-------------------+-------------------+-------------------+-------------------+-------------------+
| 49                | 63                | 87                | 31                | 1.570             |
+-------------------+-------------------+-------------------+-------------------+-------------------+
| 50                | 65                | 92                | 33                | 1.620             |
+-------------------+-------------------+-------------------+-------------------+-------------------+
| 51                | 67                | 95                | 35                | 1.670             |
+-------------------+-------------------+-------------------+-------------------+-------------------+
| 52                | 69                | 98                | 38                | 1.720             |
+-------------------+-------------------+-------------------+-------------------+-------------------+
| 53                | 71                | 103               | 40                | 1.770             |
+-------------------+-------------------+-------------------+-------------------+-------------------+
| 54                | 73                | 106               | 43                | 1.820             |
+-------------------+-------------------+-------------------+-------------------+-------------------+
| 55                | 75                | 112               | 46                | 1.869             |
+-------------------+-------------------+-------------------+-------------------+-------------------+
| 56                | 77                | 114               | 49                | 1.919             |
+-------------------+-------------------+-------------------+-------------------+-------------------+
| 57                | 79                | 117               | 52                | 1.969             |
+-------------------+-------------------+-------------------+-------------------+-------------------+
| 58                | 81                | 123               | 55                | 2.019             |
+-------------------+-------------------+-------------------+-------------------+-------------------+
| 59                | 83                | 128               | 58                | 2.069             |
+-------------------+-------------------+-------------------+-------------------+-------------------+
| 60                | 85                | 131               | 61                | 2.119             |
+-------------------+-------------------+-------------------+-------------------+-------------------+
| 61                | 87                | 137               | 64                | 2.168             |
+-------------------+-------------------+-------------------+-------------------+-------------------+
| 62                | 89                | 139               | 67                | 2.218             |
+-------------------+-------------------+-------------------+-------------------+-------------------+
| 63                | 91                | 142               | 71                | 2.268             |
+-------------------+-------------------+-------------------+-------------------+-------------------+
| 64                | 93                | 148               | 74                | 2.318             |
+-------------------+-------------------+-------------------+-------------------+-------------------+
| 65                | 95                | 150               | 78                | 2.368             |
+-------------------+-------------------+-------------------+-------------------+-------------------+
| 66                | 97                | 152               | 81                | 2.418             |
+-------------------+-------------------+-------------------+-------------------+-------------------+

: **Table E‑1 Continued**

+-------------------+-------------------+-------------------+-------------------+-------------------+
| **Code**          | **Rise            | **Span (*W~max~*) | ***A~full~*       | ***R~full~*       |
|                   | (*Y~full~*)       | (in)**            | (in^2^)**         | (in)**            |
|                   | (in)**            |                   |                   |                   |
+===================+===================+===================+===================+===================+
| **Structural Plate, 18\" Corner Radius**                                                          |
+-------------------+-------------------+-------------------+-------------------+-------------------+
| 67                | 100               | 154               | 85                | 2.493             |
+-------------------+-------------------+-------------------+-------------------+-------------------+
| 68                | 101               | 161               | 89                | 2.517             |
+-------------------+-------------------+-------------------+-------------------+-------------------+
| 69                | 103               | 167               | 93                | 2.567             |
+-------------------+-------------------+-------------------+-------------------+-------------------+
| 70                | 105               | 169               | 97                | 2.617             |
+-------------------+-------------------+-------------------+-------------------+-------------------+
| 71                | 107               | 171               | 101               | 2.667             |
+-------------------+-------------------+-------------------+-------------------+-------------------+
| 72                | 109               | 178               | 105               | 2.717             |
+-------------------+-------------------+-------------------+-------------------+-------------------+
| 73                | 111               | 184               | 109               | 2.767             |
+-------------------+-------------------+-------------------+-------------------+-------------------+
| 74                | 113               | 186               | 113               | 2.817             |
+-------------------+-------------------+-------------------+-------------------+-------------------+
| 75                | 115               | 188               | 118               | 2.866             |
+-------------------+-------------------+-------------------+-------------------+-------------------+
| 76                | 118               | 190               | 122               | 2.941             |
+-------------------+-------------------+-------------------+-------------------+-------------------+
| 77                | 119               | 197               | 126               | 2.966             |
+-------------------+-------------------+-------------------+-------------------+-------------------+
| 78                | 121               | 199               | 131               | 3.016             |
+-------------------+-------------------+-------------------+-------------------+-------------------+
| **Structural Plate, 31\" Corner Radius**                                                          |
+-------------------+-------------------+-------------------+-------------------+-------------------+
| 79                | 112               | 159               | 97                | 2.792             |
+-------------------+-------------------+-------------------+-------------------+-------------------+
| 80                | 114               | 162               | 102               | 2.841             |
+-------------------+-------------------+-------------------+-------------------+-------------------+
| 81                | 116               | 168               | 105               | 2.891             |
+-------------------+-------------------+-------------------+-------------------+-------------------+
| 82                | 118               | 170               | 109               | 2.941             |
+-------------------+-------------------+-------------------+-------------------+-------------------+
| 83                | 120               | 173               | 114               | 2.991             |
+-------------------+-------------------+-------------------+-------------------+-------------------+
| 84                | 122               | 179               | 118               | 3.041             |
+-------------------+-------------------+-------------------+-------------------+-------------------+
| 85                | 124               | 184               | 123               | 3.091             |
+-------------------+-------------------+-------------------+-------------------+-------------------+
| 86                | 126               | 187               | 127               | 3.141             |
+-------------------+-------------------+-------------------+-------------------+-------------------+
| 87                | 128               | 190               | 132               | 3.190             |
+-------------------+-------------------+-------------------+-------------------+-------------------+
| 88                | 130               | 195               | 137               | 3.240             |
+-------------------+-------------------+-------------------+-------------------+-------------------+
| 89                | 132               | 198               | 142               | 3.290             |
+-------------------+-------------------+-------------------+-------------------+-------------------+
| 90                | 134               | 204               | 146               | 3.340             |
+-------------------+-------------------+-------------------+-------------------+-------------------+
| 91                | 136               | 206               | 151               | 3.390             |
+-------------------+-------------------+-------------------+-------------------+-------------------+
| 92                | 138               | 209               | 157               | 3.440             |
+-------------------+-------------------+-------------------+-------------------+-------------------+
| 93                | 140               | 215               | 161               | 3.490             |
+-------------------+-------------------+-------------------+-------------------+-------------------+
| 94                | 142               | 217               | 167               | 3.539             |
+-------------------+-------------------+-------------------+-------------------+-------------------+
| 95                | 144               | 223               | 172               | 3.589             |
+-------------------+-------------------+-------------------+-------------------+-------------------+
| 96                | 146               | 225               | 177               | 3.639             |
+-------------------+-------------------+-------------------+-------------------+-------------------+
| 97                | 148               | 231               | 182               | 3.689             |
+-------------------+-------------------+-------------------+-------------------+-------------------+
| 98                | 150               | 234               | 188               | 3.739             |
+-------------------+-------------------+-------------------+-------------------+-------------------+
| 99                | 152               | 236               | 194               | 3.789             |
+-------------------+-------------------+-------------------+-------------------+-------------------+
| 100               | 154               | 239               | 200               | 3.838             |
+-------------------+-------------------+-------------------+-------------------+-------------------+
| 101               | 156               | 245               | 205               | 3.888             |
+-------------------+-------------------+-------------------+-------------------+-------------------+
| 102               | 158               | 247               | 211               | 3.938             |
+-------------------+-------------------+-------------------+-------------------+-------------------+

: **Table E‑1 Continued**

Source: American Iron and Steel Institute (1999).

[]{#_Toc484695154 .anchor}**Table E‑2 Arch pipe section properties as
function of depth**

  -----------------------------------------------------------------------
   ***Y/Y~full~***   ***A/A~full~***   ***W/W~max~***    ***R/R~full~***
  ----------------- ----------------- ----------------- -----------------
        0.00              0.000            0.0000            0.0100

        0.04              0.020            0.6272            0.0983

        0.08              0.060            0.8521            0.1965

        0.12              0.100            0.9243            0.2948

        0.16              0.140            0.9645            0.3940

        0.20              0.190            0.9846            0.4962

        0.24              0.240            0.9964            0.5911

        0.28              0.290            0.9988            0.6796

        0.32              0.340            0.9917            0.7615

        0.36              0.390            0.9811            0.8364

        0.40              0.440            0.9680            0.9044

        0.44              0.490            0.9515            0.9640

        0.48              0.540            0.9314             1.018

        0.52              0.590            0.9101             1.065

        0.56              0.640            0.8864             1.106

        0.60              0.690            0.8592             1.142

        0.64              0.735            0.8284             1.170

        0.68              0.780            0.7917             1.192

        0.72              0.820            0.7527             1.208

        0.76              0.860            0.7065             1.217

        0.80              0.895            0.6544             1.220

        0.84              0.930            0.5953             1.213

        0.88              0.960            0.5231             1.196

        0.92              0.985            0.4355             1.168

        0.96              0.995            0.3195             1.112

        1.00              1.000             0.000             1.000
  -----------------------------------------------------------------------

#### Section Properties of Masonry Sewers

[]{#_Toc484695155 .anchor}

+--------------------+-----------------------------------------------------------+
| ***Y/Y~full~***    | ***A/A~full~***                                           |
|                    +-------------------+-------------------+-------------------+
|                    | **Basket Handle** | **Egg**           | **Horseshoe**     |
+:==================:+:=================:+:=================:+:=================:+
| 0.00               | 0.0000            | 0.000             | 0.0000            |
+--------------------+-------------------+-------------------+-------------------+
| 0.04               | 0.0173            | 0.015             | 0.0181            |
+--------------------+-------------------+-------------------+-------------------+
| 0.08               | 0.0457            | 0.040             | 0.0508            |
+--------------------+-------------------+-------------------+-------------------+
| 0.12               | 0.0828            | 0.055             | 0.0908            |
+--------------------+-------------------+-------------------+-------------------+
| 0.16               | 0.1271            | 0.085             | 0.1326            |
+--------------------+-------------------+-------------------+-------------------+
| 0.20               | 0.1765            | 0.120             | 0.1757            |
+--------------------+-------------------+-------------------+-------------------+
| 0.24               | 0.2270            | 0.155             | 0.2201            |
+--------------------+-------------------+-------------------+-------------------+
| 0.28               | 0.2775            | 0.190             | 0.2655            |
+--------------------+-------------------+-------------------+-------------------+
| 0.32               | 0.3280            | 0.225             | 0.3118            |
+--------------------+-------------------+-------------------+-------------------+
| 0.36               | 0.3780            | 0.275             | 0.3587            |
+--------------------+-------------------+-------------------+-------------------+
| 0.40               | 0.4270            | 0.320             | 0.4064            |
+--------------------+-------------------+-------------------+-------------------+
| 0.44               | 0.4765            | 0.370             | 0.4542            |
+--------------------+-------------------+-------------------+-------------------+
| 0.48               | 0.5260            | 0.420             | 0.5023            |
+--------------------+-------------------+-------------------+-------------------+
| 0.52               | 0.5740            | 0.470             | 0.5506            |
+--------------------+-------------------+-------------------+-------------------+
| 0.56               | 0.6220            | 0.515             | 0.5987            |
+--------------------+-------------------+-------------------+-------------------+
| 0.60               | 0.6690            | 0.570             | 0.6462            |
+--------------------+-------------------+-------------------+-------------------+
| 0.64               | 0.7160            | 0.620             | 0.6931            |
+--------------------+-------------------+-------------------+-------------------+
| 0.68               | 0.7610            | 0.680             | 0.7387            |
+--------------------+-------------------+-------------------+-------------------+
| 0.72               | 0.8030            | 0.730             | 0.7829            |
+--------------------+-------------------+-------------------+-------------------+
| 0.76               | 0.8390            | 0.780             | 0.8253            |
+--------------------+-------------------+-------------------+-------------------+
| 0.80               | 0.8770            | 0.835             | 0.8652            |
+--------------------+-------------------+-------------------+-------------------+
| 0.84               | 0.9110            | 0.885             | 0.9022            |
+--------------------+-------------------+-------------------+-------------------+
| 0.88               | 0.9410            | 0.925             | 0.9356            |
+--------------------+-------------------+-------------------+-------------------+
| 0.92               | 0.9680            | 0.955             | 0.9645            |
+--------------------+-------------------+-------------------+-------------------+
| 0.96               | 0.9880            | 0.980             | 0.9873            |
+--------------------+-------------------+-------------------+-------------------+
| 1.00               | 1.0000            | 1.000             | 1.0000            |
+--------------------+-------------------+-------------------+-------------------+

: **Table F‑1 Area of masonry sewers as function of depth**

+--------------------+-----------------------------------------------------------+
| ***Y/Y~full~***    | ***W/W~max~***                                            |
|                    +-------------------+-------------------+-------------------+
|                    | **Basket Handle** | **Egg**           | **Horseshoe**     |
+:==================:+:=================:+:=================:+:=================:+
| 0.00               | 0.000             | 0.000             | 0.0000            |
+--------------------+-------------------+-------------------+-------------------+
| 0.04               | 0.490             | 0.298             | 0.5878            |
+--------------------+-------------------+-------------------+-------------------+
| 0.08               | 0.667             | 0.433             | 0.8772            |
+--------------------+-------------------+-------------------+-------------------+
| 0.12               | 0.820             | 0.508             | 0.8900            |
+--------------------+-------------------+-------------------+-------------------+
| 0.16               | 0.930             | 0.582             | 0.9028            |
+--------------------+-------------------+-------------------+-------------------+
| 0.20               | 1.000             | 0.642             | 0.9156            |
+--------------------+-------------------+-------------------+-------------------+
| 0.24               | 1.000             | 0.696             | 0.9284            |
+--------------------+-------------------+-------------------+-------------------+
| 0.28               | 1.000             | 0.746             | 0.9412            |
+--------------------+-------------------+-------------------+-------------------+
| 0.32               | 0.997             | 0.791             | 0.9540            |
+--------------------+-------------------+-------------------+-------------------+
| 0.36               | 0.994             | 0.836             | 0.9668            |
+--------------------+-------------------+-------------------+-------------------+
| 0.40               | 0.988             | 0.866             | 0.9798            |
+--------------------+-------------------+-------------------+-------------------+
| 0.44               | 0.982             | 0.896             | 0.9928            |
+--------------------+-------------------+-------------------+-------------------+
| 0.48               | 0.967             | 0.926             | 0.9992            |
+--------------------+-------------------+-------------------+-------------------+
| 0.52               | 0.948             | 0.956             | 0.9992            |
+--------------------+-------------------+-------------------+-------------------+
| 0.56               | 0.928             | 0.970             | 0.9928            |
+--------------------+-------------------+-------------------+-------------------+
| 0.60               | 0.904             | 0.985             | 0.9798            |
+--------------------+-------------------+-------------------+-------------------+
| 0.64               | 0.874             | 1.000             | 0.9600            |
+--------------------+-------------------+-------------------+-------------------+
| 0.68               | 0.842             | 0.985             | 0.9330            |
+--------------------+-------------------+-------------------+-------------------+
| 0.72               | 0.798             | 0.970             | 0.8980            |
+--------------------+-------------------+-------------------+-------------------+
| 0.76               | 0.750             | 0.940             | 0.8542            |
+--------------------+-------------------+-------------------+-------------------+
| 0.80               | 0.697             | 0.896             | 0.8000            |
+--------------------+-------------------+-------------------+-------------------+
| 0.84               | 0.637             | 0.836             | 0.7332            |
+--------------------+-------------------+-------------------+-------------------+
| 0.88               | 0.567             | 0.764             | 0.6499            |
+--------------------+-------------------+-------------------+-------------------+
| 0.92               | 0.467             | 0.642             | 0.5426            |
+--------------------+-------------------+-------------------+-------------------+
| 0.96               | 0.342             | 0.310             | 0.3919            |
+--------------------+-------------------+-------------------+-------------------+
| 1.00               | 0.000             | 0.000             | 0.0000            |
+--------------------+-------------------+-------------------+-------------------+

: []{#_Toc484695156 .anchor}**Table F‑2 Width of masonry sewers as
function of depth - I**

+-----------------+-----------------------------------------------------------------------------+
| ***Y/Y~full~*** | ***W/W~max~***                                                              |
|                 +-----------------+-----------------+---------------------+-------------------+
|                 | **Gothic**      | **Catenary**    | **Semi-Elliptical** | **Semi-Circular** |
+:===============:+:===============:+:===============:+:===================:+:=================:+
| 0               | 0.000           | 0.0000          | 0.00                | 0.0000            |
+-----------------+-----------------+-----------------+---------------------+-------------------+
| 0.05            | 0.286           | 0.6667          | 0.70                | 0.5488            |
+-----------------+-----------------+-----------------+---------------------+-------------------+
| 0.10            | 0.643           | 0.8222          | 0.98                | 0.8537            |
+-----------------+-----------------+-----------------+---------------------+-------------------+
| 0.15            | 0.762           | 0.9111          | 1.00                | 1.0000            |
+-----------------+-----------------+-----------------+---------------------+-------------------+
| 0.20            | 0.833           | 0.9778          | 1.00                | 1.0000            |
+-----------------+-----------------+-----------------+---------------------+-------------------+
| 0.25            | 0.905           | 1.0000          | 1.00                | 0.9939            |
+-----------------+-----------------+-----------------+---------------------+-------------------+
| 0.30            | 0.952           | 1.0000          | 0.99                | 0.9878            |
+-----------------+-----------------+-----------------+---------------------+-------------------+
| 0.35            | 0.976           | 0.9889          | 0.98                | 0.9756            |
+-----------------+-----------------+-----------------+---------------------+-------------------+
| 0.40            | 0.976           | 0.9778          | 0.96                | 0.9634            |
+-----------------+-----------------+-----------------+---------------------+-------------------+
| 0.45            | 1.000           | 0.9556          | 0.94                | 0.9451            |
+-----------------+-----------------+-----------------+---------------------+-------------------+
| 0.50            | 1.000           | 0.9333          | 0.91                | 0.9207            |
+-----------------+-----------------+-----------------+---------------------+-------------------+
| 0.55            | 0.976           | 0.8889          | 0.88                | 0.8902            |
+-----------------+-----------------+-----------------+---------------------+-------------------+
| 0.60            | 0.976           | 0.8444          | 0.84                | 0.8537            |
+-----------------+-----------------+-----------------+---------------------+-------------------+
| 0.65            | 0.952           | 0.8000          | 0.80                | 0.8171            |
+-----------------+-----------------+-----------------+---------------------+-------------------+
| 0.70            | 0.905           | 0.7556          | 0.75                | 0.7683            |
+-----------------+-----------------+-----------------+---------------------+-------------------+
| 0.75            | 0.833           | 0.7000          | 0.70                | 0.7073            |
+-----------------+-----------------+-----------------+---------------------+-------------------+
| 0.80            | 0.762           | 0.6333          | 0.64                | 0.6463            |
+-----------------+-----------------+-----------------+---------------------+-------------------+
| 0.85            | 0.667           | 0.5556          | 0.56                | 0.5732            |
+-----------------+-----------------+-----------------+---------------------+-------------------+
| 0.90            | 0.524           | 0.4444          | 0.46                | 0.4756            |
+-----------------+-----------------+-----------------+---------------------+-------------------+
| 0.95            | 0.357           | 0.3333          | 0.34                | 0.3354            |
+-----------------+-----------------+-----------------+---------------------+-------------------+
| 1.00            | 0.000           | 0.0000          | 0.00                | 0.0000            |
+-----------------+-----------------+-----------------+---------------------+-------------------+

: []{#_Toc484695157 .anchor}**Table F‑3 Width of masonry sewers as
function of depth - II**

+--------------------+-----------------------------------------------------------+
| ***Y/Y~full~***    | ***R/R~full~***                                           |
|                    +-------------------+-------------------+-------------------+
|                    | **Basket Handle** | **Egg**           | **Horseshoe**     |
+:==================:+:=================:+:=================:+:=================:+
| 0.00               | 0.010             | 0.010             | 0.0100            |
+--------------------+-------------------+-------------------+-------------------+
| 0.04               | 0.0952            | 0.097             | 0.1040            |
+--------------------+-------------------+-------------------+-------------------+
| 0.08               | 0.189             | 0.216             | 0.2065            |
+--------------------+-------------------+-------------------+-------------------+
| 0.12               | 0.273             | 0.302             | 0.3243            |
+--------------------+-------------------+-------------------+-------------------+
| 0.16               | 0.369             | 0.386             | 0.4322            |
+--------------------+-------------------+-------------------+-------------------+
| 0.20               | 0.463             | 0.465             | 0.5284            |
+--------------------+-------------------+-------------------+-------------------+
| 0.24               | 0.560             | 0.536             | 0.6147            |
+--------------------+-------------------+-------------------+-------------------+
| 0.28               | 0.653             | 0.611             | 0.6927            |
+--------------------+-------------------+-------------------+-------------------+
| 0.32               | 0.743             | 0.676             | 0.7636            |
+--------------------+-------------------+-------------------+-------------------+
| 0.36               | 0.822             | 0.735             | 0.8268            |
+--------------------+-------------------+-------------------+-------------------+
| 0.40               | 0.883             | 0.791             | 0.8873            |
+--------------------+-------------------+-------------------+-------------------+
| 0.44               | 0.949             | 0.854             | 0.9417            |
+--------------------+-------------------+-------------------+-------------------+
| 0.48               | 0.999             | 0.904             | 0.9905            |
+--------------------+-------------------+-------------------+-------------------+
| 0.52               | 1.055             | 0.941             | 1.036             |
+--------------------+-------------------+-------------------+-------------------+
| 0.56               | 1.095             | 1.008             | 1.077             |
+--------------------+-------------------+-------------------+-------------------+
| 0.60               | 1.141             | 1.045             | 1.113             |
+--------------------+-------------------+-------------------+-------------------+
| 0.64               | 1.161             | 1.076             | 1.143             |
+--------------------+-------------------+-------------------+-------------------+
| 0.68               | 1.188             | 1.115             | 1.169             |
+--------------------+-------------------+-------------------+-------------------+
| 0.72               | 1.206             | 1.146             | 1.189             |
+--------------------+-------------------+-------------------+-------------------+
| 0.76               | 1.206             | 1.162             | 1.202             |
+--------------------+-------------------+-------------------+-------------------+
| 0.80               | 1.206             | 1.186             | 1.208             |
+--------------------+-------------------+-------------------+-------------------+
| 0.84               | 1.205             | 1.193             | 1.206             |
+--------------------+-------------------+-------------------+-------------------+
| 0.88               | 1.196             | 1.186             | 1.195             |
+--------------------+-------------------+-------------------+-------------------+
| 0.92               | 1.168             | 1.162             | 1.170             |
+--------------------+-------------------+-------------------+-------------------+
| 0.96               | 1.127             | 1.107             | 1.126             |
+--------------------+-------------------+-------------------+-------------------+
| 1.00               | 1.000             | 1.000             | 1.000             |
+--------------------+-------------------+-------------------+-------------------+

: []{#_Toc484695158 .anchor}**Table F‑4 Hydraulic radius of masonry
sewers as function of depth**

+--------------------+-----------------------------------------------------------+
| ***A/A~full~***    | ***Y/Y~full~***                                           |
|                    +-------------------+-------------------+-------------------+
|                    | **Basket Handle** | **Egg**           | **Horseshoe**     |
+:==================:+:=================:+:=================:+:=================:+
| 0.00               | 0.00000           | 0.00000           | 0.00000           |
+--------------------+-------------------+-------------------+-------------------+
| 0.02               | 0.04112           | 0.04912           | 0.04146           |
+--------------------+-------------------+-------------------+-------------------+
| 0.04               | 0.07380           | 0.08101           | 0.07033           |
+--------------------+-------------------+-------------------+-------------------+
| 0.06               | 0.10000           | 0.11128           | 0.09098           |
+--------------------+-------------------+-------------------+-------------------+
| 0.08               | 0.12236           | 0.14161           | 0.10962           |
+--------------------+-------------------+-------------------+-------------------+
| 0.10               | 0.14141           | 0.16622           | 0.12921           |
+--------------------+-------------------+-------------------+-------------------+
| 0.12               | 0.15857           | 0.18811           | 0.14813           |
+--------------------+-------------------+-------------------+-------------------+
| 0.14               | 0.17462           | 0.21356           | 0.16701           |
+--------------------+-------------------+-------------------+-------------------+
| 0.16               | 0.18946           | 0.23742           | 0.18565           |
+--------------------+-------------------+-------------------+-------------------+
| 0.18               | 0.20315           | 0.25742           | 0.20401           |
+--------------------+-------------------+-------------------+-------------------+
| 0.20               | 0.21557           | 0.27742           | 0.22211           |
+--------------------+-------------------+-------------------+-------------------+
| 0.22               | 0.22833           | 0.29741           | 0.23998           |
+--------------------+-------------------+-------------------+-------------------+
| 0.24               | 0.24230           | 0.31742           | 0.25769           |
+--------------------+-------------------+-------------------+-------------------+
| 0.26               | 0.25945           | 0.33742           | 0.27524           |
+--------------------+-------------------+-------------------+-------------------+
| 0.28               | 0.27936           | 0.35747           | 0.29265           |
+--------------------+-------------------+-------------------+-------------------+
| 0.30               | 0.30000           | 0.37364           | 0.30990           |
+--------------------+-------------------+-------------------+-------------------+
| 0.32               | 0.32040           | 0.40000           | 0.32704           |
+--------------------+-------------------+-------------------+-------------------+
| 0.34               | 0.34034           | 0.41697           | 0.34406           |
+--------------------+-------------------+-------------------+-------------------+
| 0.36               | 0.35892           | 0.43372           | 0.36101           |
+--------------------+-------------------+-------------------+-------------------+
| 0.38               | 0.37595           | 0.45000           | 0.37790           |
+--------------------+-------------------+-------------------+-------------------+
| 0.40               | 0.39214           | 0.46374           | 0.39471           |
+--------------------+-------------------+-------------------+-------------------+
| 0.42               | 0.40802           | 0.47747           | 0.41147           |
+--------------------+-------------------+-------------------+-------------------+
| 0.44               | 0.42372           | 0.49209           | 0.42818           |
+--------------------+-------------------+-------------------+-------------------+
| 0.46               | 0.43894           | 0.50989           | 0.44484           |
+--------------------+-------------------+-------------------+-------------------+
| 0.48               | 0.45315           | 0.53015           | 0.46147           |
+--------------------+-------------------+-------------------+-------------------+
| 0.50               | 0.46557           | 0.55000           | 0.47807           |
+--------------------+-------------------+-------------------+-------------------+

: []{#_Toc484695159 .anchor}**Table F‑5 Depth of masonry sewers as
function of area - I**

+--------------------+-----------------------------------------------------------+
| ***A/A~full~***    | ***Y/Y~full~***                                           |
|                    +-------------------+-------------------+-------------------+
|                    | **Basket Handle** | **Egg**           | **Horseshoe**     |
+:==================:+:=================:+:=================:+:=================:+
| 0.52               | 0.47833           | 0.56429           | 0.49468           |
+--------------------+-------------------+-------------------+-------------------+
| 0.54               | 0.49230           | 0.57675           | 0.51134           |
+--------------------+-------------------+-------------------+-------------------+
| 0.56               | 0.50945           | 0.58834           | 0.52803           |
+--------------------+-------------------+-------------------+-------------------+
| 0.58               | 0.52936           | 0.60000           | 0.54474           |
+--------------------+-------------------+-------------------+-------------------+
| 0.60               | 0.55000           | 0.61441           | 0.56138           |
+--------------------+-------------------+-------------------+-------------------+
| 0.62               | 0.57000           | 0.62967           | 0.57804           |
+--------------------+-------------------+-------------------+-------------------+
| 0.64               | 0.59000           | 0.64582           | 0.59478           |
+--------------------+-------------------+-------------------+-------------------+
| 0.66               | 0.61023           | 0.66368           | 0.61171           |
+--------------------+-------------------+-------------------+-------------------+
| 0.68               | 0.63045           | 0.68209           | 0.62881           |
+--------------------+-------------------+-------------------+-------------------+
| 0.70               | 0.65000           | 0.70000           | 0.64609           |
+--------------------+-------------------+-------------------+-------------------+
| 0.72               | 0.66756           | 0.71463           | 0.66350           |
+--------------------+-------------------+-------------------+-------------------+
| 0.74               | 0.68413           | 0.72807           | 0.68111           |
+--------------------+-------------------+-------------------+-------------------+
| 0.76               | 0.70000           | 0.74074           | 0.69901           |
+--------------------+-------------------+-------------------+-------------------+
| 0.78               | 0.71481           | 0.75296           | 0.71722           |
+--------------------+-------------------+-------------------+-------------------+
| 0.80               | 0.72984           | 0.76500           | 0.73583           |
+--------------------+-------------------+-------------------+-------------------+
| 0.82               | 0.74579           | 0.77784           | 0.75490           |
+--------------------+-------------------+-------------------+-------------------+
| 0.84               | 0.76417           | 0.79212           | 0.77447           |
+--------------------+-------------------+-------------------+-------------------+
| 0.86               | 0.78422           | 0.80945           | 0.79471           |
+--------------------+-------------------+-------------------+-------------------+
| 0.88               | 0.80477           | 0.82936           | 0.81564           |
+--------------------+-------------------+-------------------+-------------------+
| 0.90               | 0.82532           | 0.85000           | 0.83759           |
+--------------------+-------------------+-------------------+-------------------+
| 0.92               | 0.85000           | 0.86731           | 0.86067           |
+--------------------+-------------------+-------------------+-------------------+
| 0.94               | 0.88277           | 0.88769           | 0.88557           |
+--------------------+-------------------+-------------------+-------------------+
| 0.96               | 0.91500           | 0.91400           | 0.91159           |
+--------------------+-------------------+-------------------+-------------------+
| 0.98               | 0.95000           | 0.95000           | 0.94520           |
+--------------------+-------------------+-------------------+-------------------+
| 1.00               | 1.00000           | 1.00000           | 1.00000           |
+--------------------+-------------------+-------------------+-------------------+

: **Table F-5 Continued**

+-----------------+-----------------------------------------------------------------------------+
| ***A/A~full~*** | ***Y/Y~full~***                                                             |
|                 +-----------------+-----------------+-------------------+---------------------+
|                 | **Catenary**    | **Gothic**      | **Semi-Circular** | **Semi-Elliptical** |
+:===============:+:===============:+:===============:+:=================:+:===================:+
| 0.00            | 0.00000         | 0.00000         | 0.00000           | 0.00000             |
+-----------------+-----------------+-----------------+-------------------+---------------------+
| 0.02            | 0.02974         | 0.04522         | 0.04102           | 0.03075             |
+-----------------+-----------------+-----------------+-------------------+---------------------+
| 0.04            | 0.06439         | 0.07825         | 0.07407           | 0.05137             |
+-----------------+-----------------+-----------------+-------------------+---------------------+
| 0.06            | 0.08433         | 0.10646         | 0.10000           | 0.07032             |
+-----------------+-----------------+-----------------+-------------------+---------------------+
| 0.08            | 0.10549         | 0.12645         | 0.11769           | 0.09000             |
+-----------------+-----------------+-----------------+-------------------+---------------------+
| 0.10            | 0.12064         | 0.14645         | 0.13037           | 0.11323             |
+-----------------+-----------------+-----------------+-------------------+---------------------+
| 0.12            | 0.13952         | 0.16787         | 0.14036           | 0.13037             |
+-----------------+-----------------+-----------------+-------------------+---------------------+
| 0.14            | 0.15560         | 0.18641         | 0.15000           | 0.14519             |
+-----------------+-----------------+-----------------+-------------------+---------------------+
| 0.16            | 0.17032         | 0.20129         | 0.16546           | 0.15968             |
+-----------------+-----------------+-----------------+-------------------+---------------------+
| 0.18            | 0.18512         | 0.22425         | 0.18213           | 0.18459             |
+-----------------+-----------------+-----------------+-------------------+---------------------+
| 0.20            | 0.20057         | 0.24129         | 0.20000           | 0.19531             |
+-----------------+-----------------+-----------------+-------------------+---------------------+
| 0.22            | 0.21995         | 0.25624         | 0.22018           | 0.21354             |
+-----------------+-----------------+-----------------+-------------------+---------------------+
| 0.24            | 0.24011         | 0.27344         | 0.24030           | 0.22694             |
+-----------------+-----------------+-----------------+-------------------+---------------------+
| 0.26            | 0.25892         | 0.29097         | 0.25788           | 0.23947             |
+-----------------+-----------------+-----------------+-------------------+---------------------+
| 0.28            | 0.27595         | 0.30529         | 0.27216           | 0.25296             |
+-----------------+-----------------+-----------------+-------------------+---------------------+
| 0.30            | 0.29214         | 0.32607         | 0.28500           | 0.26500             |
+-----------------+-----------------+-----------------+-------------------+---------------------+
| 0.32            | 0.30802         | 0.33755         | 0.29704           | 0.27784             |
+-----------------+-----------------+-----------------+-------------------+---------------------+
| 0.34            | 0.32372         | 0.35073         | 0.30892           | 0.29212             |
+-----------------+-----------------+-----------------+-------------------+---------------------+
| 0.36            | 0.33894         | 0.36447         | 0.32128           | 0.30970             |
+-----------------+-----------------+-----------------+-------------------+---------------------+
| 0.38            | 0.35315         | 0.37558         | 0.33476           | 0.32982             |
+-----------------+-----------------+-----------------+-------------------+---------------------+
| 0.40            | 0.36557         | 0.40000         | 0.35000           | 0.35000             |
+-----------------+-----------------+-----------------+-------------------+---------------------+
| 0.42            | 0.37833         | 0.41810         | 0.36927           | 0.36738             |
+-----------------+-----------------+-----------------+-------------------+---------------------+
| 0.44            | 0.39230         | 0.43648         | 0.38963           | 0.38390             |
+-----------------+-----------------+-----------------+-------------------+---------------------+
| 0.46            | 0.40970         | 0.45374         | 0.41023           | 0.40000             |
+-----------------+-----------------+-----------------+-------------------+---------------------+
| 0.48            | 0.42982         | 0.46805         | 0.43045           | 0.41667             |
+-----------------+-----------------+-----------------+-------------------+---------------------+
| 0.50            | 0.45000         | 0.48195         | 0.45000           | 0.43333             |
+-----------------+-----------------+-----------------+-------------------+---------------------+

: []{#_Toc484695160 .anchor}**Table F‑6 Depth of masonry sewers as
function of area - II**

+-----------------+-----------------------------------------------------------------------------+
| ***A/A~full~*** | ***Y/Y~full~***                                                             |
|                 +-----------------+-----------------+-------------------+---------------------+
|                 | **Catenary**    | **Gothic**      | **Semi-Circular** | **Semi-Elliptical** |
+:===============:+:===============:+:===============:+:=================:+:===================:+
| 0.52            | 0.46769         | 0.49626         | 0.46769           | 0.45000             |
+-----------------+-----------------+-----------------+-------------------+---------------------+
| 0.54            | 0.48431         | 0.51352         | 0.48431           | 0.46697             |
+-----------------+-----------------+-----------------+-------------------+---------------------+
| 0.56            | 0.50000         | 0.53190         | 0.50000           | 0.48372             |
+-----------------+-----------------+-----------------+-------------------+---------------------+
| 0.58            | 0.51466         | 0.55000         | 0.51443           | 0.50000             |
+-----------------+-----------------+-----------------+-------------------+---------------------+
| 0.60            | 0.52886         | 0.56416         | 0.52851           | 0.51374             |
+-----------------+-----------------+-----------------+-------------------+---------------------+
| 0.62            | 0.54292         | 0.57787         | 0.54271           | 0.52747             |
+-----------------+-----------------+-----------------+-------------------+---------------------+
| 0.64            | 0.55729         | 0.59224         | 0.55774           | 0.54209             |
+-----------------+-----------------+-----------------+-------------------+---------------------+
| 0.66            | 0.57223         | 0.60950         | 0.57388           | 0.55950             |
+-----------------+-----------------+-----------------+-------------------+---------------------+
| 0.68            | 0.58780         | 0.62941         | 0.59101           | 0.57941             |
+-----------------+-----------------+-----------------+-------------------+---------------------+
| 0.70            | 0.60428         | 0.65000         | 0.60989           | 0.60000             |
+-----------------+-----------------+-----------------+-------------------+---------------------+
| 0.72            | 0.62197         | 0.67064         | 0.63005           | 0.62000             |
+-----------------+-----------------+-----------------+-------------------+---------------------+
| 0.74            | 0.64047         | 0.69055         | 0.65000           | 0.64000             |
+-----------------+-----------------+-----------------+-------------------+---------------------+
| 0.76            | 0.65980         | 0.70721         | 0.66682           | 0.66000             |
+-----------------+-----------------+-----------------+-------------------+---------------------+
| 0.78            | 0.67976         | 0.72031         | 0.68318           | 0.68000             |
+-----------------+-----------------+-----------------+-------------------+---------------------+
| 0.80            | 0.70000         | 0.73286         | 0.70000           | 0.70000             |
+-----------------+-----------------+-----------------+-------------------+---------------------+
| 0.82            | 0.71731         | 0.74632         | 0.71675           | 0.71843             |
+-----------------+-----------------+-----------------+-------------------+---------------------+
| 0.84            | 0.73769         | 0.76432         | 0.73744           | 0.73865             |
+-----------------+-----------------+-----------------+-------------------+---------------------+
| 0.86            | 0.76651         | 0.78448         | 0.76651           | 0.76365             |
+-----------------+-----------------+-----------------+-------------------+---------------------+
| 0.88            | 0.80000         | 0.80421         | 0.80000           | 0.79260             |
+-----------------+-----------------+-----------------+-------------------+---------------------+
| 0.90            | 0.82090         | 0.82199         | 0.82090           | 0.82088             |
+-----------------+-----------------+-----------------+-------------------+---------------------+
| 0.92            | 0.84311         | 0.84363         | 0.84311           | 0.85000             |
+-----------------+-----------------+-----------------+-------------------+---------------------+
| 0.94            | 0.87978         | 0.87423         | 0.87978           | 0.88341             |
+-----------------+-----------------+-----------------+-------------------+---------------------+
| 0.96            | 0.91576         | 0.90617         | 0.91576           | 0.90998             |
+-----------------+-----------------+-----------------+-------------------+---------------------+
| 0.98            | 0.95000         | 0.93827         | 0.95000           | 0.93871             |
+-----------------+-----------------+-----------------+-------------------+---------------------+
| 1.00            | 1.00000         | 1.00000         | 1.00000           | 1.00000             |
+-----------------+-----------------+-----------------+-------------------+---------------------+

: **Table F-6 Continued**

+--------------------+-----------------------------------------------------------+
| ***A/A~full~***    | ***Ψ/Ψ~full~***                                           |
|                    +-------------------+-------------------+-------------------+
|                    | **Basket Handle** | **Egg**           | **Horseshoe**     |
+:==================:+:=================:+:=================:+:=================:+
| 0.00               | 0.00000           | 0.00000           | 0.00000           |
+--------------------+-------------------+-------------------+-------------------+
| 0.02               | 0.00758           | 0.00295           | 0.00467           |
+--------------------+-------------------+-------------------+-------------------+
| 0.04               | 0.01812           | 0.01331           | 0.01237           |
+--------------------+-------------------+-------------------+-------------------+
| 0.06               | 0.03000           | 0.02629           | 0.02268           |
+--------------------+-------------------+-------------------+-------------------+
| 0.08               | 0.03966           | 0.04000           | 0.03515           |
+--------------------+-------------------+-------------------+-------------------+
| 0.10               | 0.04957           | 0.05657           | 0.04943           |
+--------------------+-------------------+-------------------+-------------------+
| 0.12               | 0.06230           | 0.07500           | 0.06525           |
+--------------------+-------------------+-------------------+-------------------+
| 0.14               | 0.07849           | 0.09432           | 0.08212           |
+--------------------+-------------------+-------------------+-------------------+
| 0.16               | 0.09618           | 0.11473           | 0.10005           |
+--------------------+-------------------+-------------------+-------------------+
| 0.18               | 0.11416           | 0.13657           | 0.11891           |
+--------------------+-------------------+-------------------+-------------------+
| 0.20               | 0.13094           | 0.15894           | 0.13856           |
+--------------------+-------------------+-------------------+-------------------+
| 0.22               | 0.14808           | 0.18030           | 0.15896           |
+--------------------+-------------------+-------------------+-------------------+
| 0.24               | 0.16583           | 0.20036           | 0.18004           |
+--------------------+-------------------+-------------------+-------------------+
| 0.26               | 0.18381           | 0.22000           | 0.20172           |
+--------------------+-------------------+-------------------+-------------------+
| 0.28               | 0.20294           | 0.23919           | 0.22397           |
+--------------------+-------------------+-------------------+-------------------+
| 0.30               | 0.22500           | 0.25896           | 0.24677           |
+--------------------+-------------------+-------------------+-------------------+
| 0.32               | 0.25470           | 0.28000           | 0.27006           |
+--------------------+-------------------+-------------------+-------------------+
| 0.34               | 0.28532           | 0.30504           | 0.29380           |
+--------------------+-------------------+-------------------+-------------------+
| 0.36               | 0.31006           | 0.33082           | 0.31790           |
+--------------------+-------------------+-------------------+-------------------+
| 0.38               | 0.32804           | 0.35551           | 0.34237           |
+--------------------+-------------------+-------------------+-------------------+
| 0.40               | 0.34555           | 0.37692           | 0.36720           |
+--------------------+-------------------+-------------------+-------------------+
| 0.42               | 0.36944           | 0.39809           | 0.39239           |
+--------------------+-------------------+-------------------+-------------------+
| 0.44               | 0.40032           | 0.42000           | 0.41792           |
+--------------------+-------------------+-------------------+-------------------+
| 0.46               | 0.43203           | 0.44625           | 0.44374           |
+--------------------+-------------------+-------------------+-------------------+
| 0.48               | 0.46004           | 0.47321           | 0.46984           |
+--------------------+-------------------+-------------------+-------------------+
| 0.50               | 0.47849           | 0.50000           | 0.49619           |
+--------------------+-------------------+-------------------+-------------------+

: []{#_Toc484695161 .anchor}**Table F‑7 Section factor for masonry
sewers as function of area - I**

+--------------------+-----------------------------------------------------------+
| ***A/A~full~***    | ***Ψ/Ψ~full~***                                           |
|                    +-------------------+-------------------+-------------------+
|                    | **Basket Handle** | **Egg**           | **Horseshoe**     |
+:==================:+:=================:+:=================:+:=================:+
| 0.52               | 0.49591           | 0.52255           | 0.52276           |
+--------------------+-------------------+-------------------+-------------------+
| 0.54               | 0.51454           | 0.54481           | 0.54950           |
+--------------------+-------------------+-------------------+-------------------+
| 0.56               | 0.53810           | 0.56785           | 0.57640           |
+--------------------+-------------------+-------------------+-------------------+
| 0.58               | 0.56711           | 0.59466           | 0.60345           |
+--------------------+-------------------+-------------------+-------------------+
| 0.60               | 0.60000           | 0.62485           | 0.63065           |
+--------------------+-------------------+-------------------+-------------------+
| 0.62               | 0.64092           | 0.65518           | 0.65795           |
+--------------------+-------------------+-------------------+-------------------+
| 0.64               | 0.68136           | 0.68181           | 0.68531           |
+--------------------+-------------------+-------------------+-------------------+
| 0.66               | 0.71259           | 0.70415           | 0.71271           |
+--------------------+-------------------+-------------------+-------------------+
| 0.68               | 0.73438           | 0.72585           | 0.74009           |
+--------------------+-------------------+-------------------+-------------------+
| 0.70               | 0.75500           | 0.74819           | 0.76738           |
+--------------------+-------------------+-------------------+-------------------+
| 0.72               | 0.78625           | 0.77482           | 0.79451           |
+--------------------+-------------------+-------------------+-------------------+
| 0.74               | 0.81880           | 0.80515           | 0.82144           |
+--------------------+-------------------+-------------------+-------------------+
| 0.76               | 0.85000           | 0.83534           | 0.84814           |
+--------------------+-------------------+-------------------+-------------------+
| 0.78               | 0.86790           | 0.86193           | 0.87450           |
+--------------------+-------------------+-------------------+-------------------+
| 0.80               | 0.88483           | 0.88465           | 0.90057           |
+--------------------+-------------------+-------------------+-------------------+
| 0.82               | 0.90431           | 0.90690           | 0.92652           |
+--------------------+-------------------+-------------------+-------------------+
| 0.84               | 0.93690           | 0.93000           | 0.95244           |
+--------------------+-------------------+-------------------+-------------------+
| 0.86               | 0.97388           | 0.95866           | 0.97724           |
+--------------------+-------------------+-------------------+-------------------+
| 0.88               | 1.00747           | 0.98673           | 0.99988           |
+--------------------+-------------------+-------------------+-------------------+
| 0.90               | 1.03300           | 1.01238           | 1.02048           |
+--------------------+-------------------+-------------------+-------------------+
| 0.92               | 1.05000           | 1.03396           | 1.03989           |
+--------------------+-------------------+-------------------+-------------------+
| 0.94               | 1.05464           | 1.05000           | 1.05698           |
+--------------------+-------------------+-------------------+-------------------+
| 0.96               | 1.06078           | 1.06517           | 1.07694           |
+--------------------+-------------------+-------------------+-------------------+
| 0.98               | 1.05500           | 1.05380           | 1.07562           |
+--------------------+-------------------+-------------------+-------------------+
| 1.00               | 1.00000           | 1.00000           | 1.00000           |
+--------------------+-------------------+-------------------+-------------------+

: **Table F-7 Continued**

+-----------------+-----------------------------------------------------------------------------+
| ***A/A~full~*** | ***Ψ/Ψ~full~***                                                             |
|                 +-----------------+-----------------+-------------------+---------------------+
|                 | **Catenary**    | **Gothic**      | **Semi-Circular** | **Semi-Elliptical** |
+:===============:+:===============:+:===============:+:=================:+:===================:+
| 0.00            | 0.00000         | 0.00000         | 0.00000           | 0.00000             |
+-----------------+-----------------+-----------------+-------------------+---------------------+
| 0.02            | 0.00605         | 0.00500         | 0.00757           | 0.00438             |
+-----------------+-----------------+-----------------+-------------------+---------------------+
| 0.04            | 0.01455         | 0.01740         | 0.01815           | 0.01227             |
+-----------------+-----------------+-----------------+-------------------+---------------------+
| 0.06            | 0.02540         | 0.03098         | 0.03000           | 0.02312             |
+-----------------+-----------------+-----------------+-------------------+---------------------+
| 0.08            | 0.03863         | 0.04272         | 0.03580           | 0.03638             |
+-----------------+-----------------+-----------------+-------------------+---------------------+
| 0.10            | 0.05430         | 0.05500         | 0.04037           | 0.05145             |
+-----------------+-----------------+-----------------+-------------------+---------------------+
| 0.12            | 0.07127         | 0.06980         | 0.04601           | 0.06783             |
+-----------------+-----------------+-----------------+-------------------+---------------------+
| 0.14            | 0.08778         | 0.08620         | 0.05500           | 0.08500             |
+-----------------+-----------------+-----------------+-------------------+---------------------+
| 0.16            | 0.10372         | 0.10461         | 0.07475           | 0.10093             |
+-----------------+-----------------+-----------------+-------------------+---------------------+
| 0.18            | 0.12081         | 0.12463         | 0.09834           | 0.11752             |
+-----------------+-----------------+-----------------+-------------------+---------------------+
| 0.20            | 0.14082         | 0.14500         | 0.12500           | 0.13530             |
+-----------------+-----------------+-----------------+-------------------+---------------------+
| 0.22            | 0.16375         | 0.16309         | 0.15570           | 0.15626             |
+-----------------+-----------------+-----------------+-------------------+---------------------+
| 0.24            | 0.18779         | 0.18118         | 0.18588           | 0.17917             |
+-----------------+-----------------+-----------------+-------------------+---------------------+
| 0.26            | 0.21157         | 0.20000         | 0.20883           | 0.20296             |
+-----------------+-----------------+-----------------+-------------------+---------------------+
| 0.28            | 0.23478         | 0.22181         | 0.22300           | 0.22654             |
+-----------------+-----------------+-----------------+-------------------+---------------------+
| 0.30            | 0.25818         | 0.24487         | 0.23472           | 0.24962             |
+-----------------+-----------------+-----------------+-------------------+---------------------+
| 0.32            | 0.28244         | 0.26888         | 0.24667           | 0.27269             |
+-----------------+-----------------+-----------------+-------------------+---------------------+
| 0.34            | 0.30741         | 0.29380         | 0.26758           | 0.29568             |
+-----------------+-----------------+-----------------+-------------------+---------------------+
| 0.36            | 0.33204         | 0.31901         | 0.29346           | 0.31848             |
+-----------------+-----------------+-----------------+-------------------+---------------------+
| 0.38            | 0.35505         | 0.34389         | 0.32124           | 0.34152             |
+-----------------+-----------------+-----------------+-------------------+---------------------+
| 0.40            | 0.37465         | 0.36564         | 0.35000           | 0.36500             |
+-----------------+-----------------+-----------------+-------------------+---------------------+
| 0.42            | 0.39404         | 0.38612         | 0.37720           | 0.38941             |
+-----------------+-----------------+-----------------+-------------------+---------------------+
| 0.44            | 0.41426         | 0.40720         | 0.40540           | 0.41442             |
+-----------------+-----------------+-----------------+-------------------+---------------------+
| 0.46            | 0.43804         | 0.43000         | 0.43541           | 0.44000             |
+-----------------+-----------------+-----------------+-------------------+---------------------+
| 0.48            | 0.46531         | 0.45868         | 0.46722           | 0.46636             |
+-----------------+-----------------+-----------------+-------------------+---------------------+
| 0.50            | 0.49357         | 0.48895         | 0.50000           | 0.49309             |
+-----------------+-----------------+-----------------+-------------------+---------------------+

: []{#_Toc484695162 .anchor}**Table F‑8 Section factor for masonry
sewers as function of area - II**

+-----------------+-----------------------------------------------------------------------------+
| ***A/A~full~*** | ***Ψ/Ψ~full~***                                                             |
|                 +-----------------+-----------------+-------------------+---------------------+
|                 | **Catenary**    | **Gothic**      | **Semi-Circular** | **Semi-Elliptical** |
+:===============:+:===============:+:===============:+:=================:+:===================:+
| 0.52            | 0.52187         | 0.52000         | 0.53532           | 0.52000             |
+-----------------+-----------------+-----------------+-------------------+---------------------+
| 0.54            | 0.54925         | 0.55032         | 0.56935           | 0.54628             |
+-----------------+-----------------+-----------------+-------------------+---------------------+
| 0.56            | 0.57647         | 0.58040         | 0.60000           | 0.57285             |
+-----------------+-----------------+-----------------+-------------------+---------------------+
| 0.58            | 0.60321         | 0.61000         | 0.61544           | 0.60000             |
+-----------------+-----------------+-----------------+-------------------+---------------------+
| 0.60            | 0.62964         | 0.63762         | 0.62811           | 0.62949             |
+-----------------+-----------------+-----------------+-------------------+---------------------+
| 0.62            | 0.65639         | 0.66505         | 0.64170           | 0.65877             |
+-----------------+-----------------+-----------------+-------------------+---------------------+
| 0.64            | 0.68472         | 0.69290         | 0.66598           | 0.68624             |
+-----------------+-----------------+-----------------+-------------------+---------------------+
| 0.66            | 0.71425         | 0.72342         | 0.70010           | 0.71017             |
+-----------------+-----------------+-----------------+-------------------+---------------------+
| 0.68            | 0.74303         | 0.75467         | 0.73413           | 0.73304             |
+-----------------+-----------------+-----------------+-------------------+---------------------+
| 0.70            | 0.76827         | 0.78500         | 0.76068           | 0.75578             |
+-----------------+-----------------+-----------------+-------------------+---------------------+
| 0.72            | 0.79168         | 0.81165         | 0.78027           | 0.77925             |
+-----------------+-----------------+-----------------+-------------------+---------------------+
| 0.74            | 0.81500         | 0.83654         | 0.80000           | 0.80368             |
+-----------------+-----------------+-----------------+-------------------+---------------------+
| 0.76            | 0.84094         | 0.86000         | 0.82891           | 0.83114             |
+-----------------+-----------------+-----------------+-------------------+---------------------+
| 0.78            | 0.86707         | 0.88253         | 0.85964           | 0.85950             |
+-----------------+-----------------+-----------------+-------------------+---------------------+
| 0.80            | 0.89213         | 0.90414         | 0.89000           | 0.88592             |
+-----------------+-----------------+-----------------+-------------------+---------------------+
| 0.82            | 0.91607         | 0.92500         | 0.91270           | 0.90848             |
+-----------------+-----------------+-----------------+-------------------+---------------------+
| 0.84            | 0.94000         | 0.94486         | 0.93664           | 0.93000             |
+-----------------+-----------------+-----------------+-------------------+---------------------+
| 0.86            | 0.96604         | 0.96475         | 0.96677           | 0.95292             |
+-----------------+-----------------+-----------------+-------------------+---------------------+
| 0.88            | 0.99000         | 0.98567         | 1.00000           | 0.97481             |
+-----------------+-----------------+-----------------+-------------------+---------------------+
| 0.90            | 1.00714         | 1.00833         | 1.02661           | 0.99374             |
+-----------------+-----------------+-----------------+-------------------+---------------------+
| 0.92            | 1.02158         | 1.03000         | 1.04631           | 1.01084             |
+-----------------+-----------------+-----------------+-------------------+---------------------+
| 0.94            | 1.03814         | 1.05360         | 1.05726           | 1.02858             |
+-----------------+-----------------+-----------------+-------------------+---------------------+
| 0.96            | 1.05000         | 1.06500         | 1.06637           | 1.04543             |
+-----------------+-----------------+-----------------+-------------------+---------------------+
| 0.98            | 1.05000         | 1.05500         | 1.06000           | 1.05000             |
+-----------------+-----------------+-----------------+-------------------+---------------------+
| 1.00            | 1.00000         | 1.00000         | 1.00000           | 1.00000             |
+-----------------+-----------------+-----------------+-------------------+---------------------+

: **Table F-8 Continued**

#### Manning's Roughness Coefficients

[]{#_Toc484695163 .anchor}

+------------------------------------------+--------------------+--------------------+--------------------+
| **Type of Channel and Description**      | **Minimum**        | **Normal**         | **Maximum**        |
+:=========================================+:===================+:===================+:===================+
|                                                                                                         |
+---------------------------------------------------------------------------------------------------------+
| **1. Natural streams - minor streams (top width at flood stage \< 100 ft) **                            |
+------------------------------------------+--------------------+--------------------+--------------------+
| >   a. clean, straight, full stage, no   | 0.025              | 0.030              | 0.033              |
| > rifts or deep pools                    |                    |                    |                    |
+------------------------------------------+--------------------+--------------------+--------------------+
| >   b. same as above, but more stones    | 0.030              | 0.035              | 0.040              |
| > and weeds                              |                    |                    |                    |
+------------------------------------------+--------------------+--------------------+--------------------+
| >   c. clean, winding, some pools and    | 0.033              | 0.040              | 0.045              |
| > shoals                                 |                    |                    |                    |
+------------------------------------------+--------------------+--------------------+--------------------+
| >   d. same as above, but some weeds and | 0.035              | 0.045              | 0.050              |
| > stones                                 |                    |                    |                    |
+------------------------------------------+--------------------+--------------------+--------------------+
| >   e. same as above, lower stages, more | 0.040              | 0.048              | 0.055              |
| > ineffective\                           |                    |                    |                    |
| >   slopes and sections                  |                    |                    |                    |
+------------------------------------------+--------------------+--------------------+--------------------+
| >   f. same as \"d\" with more stones    | 0.045              | 0.050              | 0.060              |
+------------------------------------------+--------------------+--------------------+--------------------+
| >   g. sluggish reaches, weedy, deep     | 0.050              | 0.070              | 0.080              |
| > pools                                  |                    |                    |                    |
+------------------------------------------+--------------------+--------------------+--------------------+
| >   h. very weedy reaches, deep pools,   | 0.075              | 0.100              | 0.150              |
| > or floodways\                          |                    |                    |                    |
| >   with heavy stand of timber and       |                    |                    |                    |
| > underbrush                             |                    |                    |                    |
+------------------------------------------+--------------------+--------------------+--------------------+
| **2. Mountain streams, no vegetation in channel, banks usually steep, trees and brush along banks       |
| submerged at high stages**                                                                              |
+------------------------------------------+--------------------+--------------------+--------------------+
| >   a. bottom: gravels, cobbles, and few | 0.030              | 0.040              | 0.050              |
| > boulders                               |                    |                    |                    |
+------------------------------------------+--------------------+--------------------+--------------------+
| >   b. bottom: cobbles with large        | 0.040              | 0.050              | 0.070              |
| > boulders                               |                    |                    |                    |
+------------------------------------------+--------------------+--------------------+--------------------+
| **3. Floodplains**                       |                    |                    |                    |
+------------------------------------------+--------------------+--------------------+--------------------+
| >   a. Pasture, no brush                 |                    |                    |                    |
+------------------------------------------+--------------------+--------------------+--------------------+
| > 1.short grass                          | 0.025              | 0.030              | 0.035              |
+------------------------------------------+--------------------+--------------------+--------------------+
| > 2\. high grass                         | 0.030              | 0.035              | 0.050              |
+------------------------------------------+--------------------+--------------------+--------------------+
| >    b. Cultivated areas                 |                    |                    |                    |
+------------------------------------------+--------------------+--------------------+--------------------+
| > 1. no crop                             | 0.020              | 0.030              | 0.040              |
+------------------------------------------+--------------------+--------------------+--------------------+
| > 2\. mature row crops                   | 0.025              | 0.035              | 0.045              |
+------------------------------------------+--------------------+--------------------+--------------------+
| > 3\. mature field crops                 | 0.030              | 0.040              | 0.050              |
+------------------------------------------+--------------------+--------------------+--------------------+

: **Table G‑1 Manning's roughness coefficient n for open channels**

+------------------------------------------+-------------+------------+-------------+
| **Type of Channel and Description**      | **Minimum** | **Normal** | **Maximum** |
+:=========================================+:===========:+:==========:+:===========:+
|                                          |             |            |             |
+------------------------------------------+-------------+------------+-------------+
| **3. Floodplains**                       |             |            |             |
+------------------------------------------+-------------+------------+-------------+
| >     c. Brush                           |             |            |             |
+------------------------------------------+-------------+------------+-------------+
| > 1\. scattered brush, heavy weeds       | 0.035       | 0.050      | 0.070       |
+------------------------------------------+-------------+------------+-------------+
| > 2\. light brush and trees, in winter   | 0.035       | 0.050      | 0.060       |
+------------------------------------------+-------------+------------+-------------+
| > 3\. light brush and trees, in summer   | 0.040       | 0.060      | 0.080       |
+------------------------------------------+-------------+------------+-------------+
| > 4\. medium to dense brush, in winter   | 0.045       | 0.070      | 0.110       |
+------------------------------------------+-------------+------------+-------------+
| > 5\. medium to dense brush, in summer   | 0.070       | 0.100      | 0.160       |
+------------------------------------------+-------------+------------+-------------+
| >     d. Trees                           |             |            |             |
+------------------------------------------+-------------+------------+-------------+
| > 1\. dense willows, summer, straight    | 0.110       | 0.150      | 0.200       |
+------------------------------------------+-------------+------------+-------------+
| > 2\. cleared land with tree stumps, no  | 0.030       | 0.040      | 0.050       |
| > sprouts                                |             |            |             |
+------------------------------------------+-------------+------------+-------------+
| > 3\. same as above, but with heavy      | 0.050       | 0.060      | 0.080       |
| > growth of sprouts                      |             |            |             |
+------------------------------------------+-------------+------------+-------------+
| > 4\. heavy stand of timber, a few down  | 0.080       | 0.100      | 0.120       |
| > trees, little\                         |             |            |             |
| >   undergrowth, flood stage below       |             |            |             |
| > branches                               |             |            |             |
+------------------------------------------+-------------+------------+-------------+
| > 5\. same as 4. with flood stage        | 0.100       | 0.120      | 0.160       |
| > reaching  branches                     |             |            |             |
+------------------------------------------+-------------+------------+-------------+
| **4. Excavated or Dredged Channels**     |             |            |             |
+------------------------------------------+-------------+------------+-------------+
| > a\. Earth, straight, and uniform       |             |            |             |
+------------------------------------------+-------------+------------+-------------+
| > 1\. clean, recently completed          | 0.016       | 0.018      | 0.020       |
+------------------------------------------+-------------+------------+-------------+
| > 2\. clean, after weathering            | 0.018       | 0.022      | 0.025       |
+------------------------------------------+-------------+------------+-------------+
| > 3\. gravel, uniform section, clean     | 0.022       | 0.025      | 0.030       |
+------------------------------------------+-------------+------------+-------------+
| > 4\. with short grass, few weeds        | 0.022       | 0.027      | 0.033       |
+------------------------------------------+-------------+------------+-------------+
| > b\. Earth winding and sluggish         |             |            |             |
+------------------------------------------+-------------+------------+-------------+
| > 1\.  no vegetation                     | 0.023       | 0.025      | 0.030       |
+------------------------------------------+-------------+------------+-------------+
| > 2\. grass, some weeds                  | 0.025       | 0.030      | 0.033       |
+------------------------------------------+-------------+------------+-------------+
| > 3\. dense weeds or aquatic plants in   | 0.030       | 0.035      | 0.040       |
| > deep channels                          |             |            |             |
+------------------------------------------+-------------+------------+-------------+
| > 4\. earth bottom and rubble sides      | 0.028       | 0.030      | 0.035       |
+------------------------------------------+-------------+------------+-------------+
| > 5\. stony bottom and weedy banks       | 0.025       | 0.035      | 0.040       |
+------------------------------------------+-------------+------------+-------------+
| > 6\. cobble bottom and clean sides      | 0.030       | 0.040      | 0.050       |
+------------------------------------------+-------------+------------+-------------+
| > c\. Dragline-excavated or dredged      |             |            |             |
+------------------------------------------+-------------+------------+-------------+
| > 1.  no vegetation                      | 0.025       | 0.028      | 0.033       |
+------------------------------------------+-------------+------------+-------------+
| > 2\. light brush on banks               | 0.035       | 0.050      | 0.060       |
+------------------------------------------+-------------+------------+-------------+

: **Table G-1 Continued**

+------------------------------------------+-------------+------------+-------------+
| **Type of Channel and Description**      | **Minimum** | **Normal** | **Maximum** |
+:=========================================+:===========:+:==========:+:===========:+
|                                          |             |            |             |
+------------------------------------------+-------------+------------+-------------+
| > d\. Rock cuts                          |             |            |             |
+------------------------------------------+-------------+------------+-------------+
| > 1. smooth and uniform                  | 0.025       | 0.035      | 0.040       |
+------------------------------------------+-------------+------------+-------------+
| > 2\. jagged and irregular               | 0.035       | 0.040      | 0.050       |
+------------------------------------------+-------------+------------+-------------+
| > e\. Channels not maintained, weeds and |             |            |             |
| > brush uncut                            |             |            |             |
+------------------------------------------+-------------+------------+-------------+
| > 1\. dense weeds, high as flow depth    | 0.050       | 0.080      | 0.120       |
+------------------------------------------+-------------+------------+-------------+
| > 2\. clean bottom, brush on sides       | 0.040       | 0.050      | 0.080       |
+------------------------------------------+-------------+------------+-------------+
| > 3\. same as above, highest stage of    | 0.045       | 0.070      | 0.110       |
| > flow                                   |             |            |             |
+------------------------------------------+-------------+------------+-------------+
| > 4\. dense brush, high stage            | 0.080       | 0.100      | 0.140       |
+------------------------------------------+-------------+------------+-------------+
| **5. Lined or Constructed Channels**     |             |            |             |
+------------------------------------------+-------------+------------+-------------+
| > a\. Cement                             |             |            |             |
+------------------------------------------+-------------+------------+-------------+
| > 1\.  neat surface                      | 0.010       | 0.011      | 0.013       |
+------------------------------------------+-------------+------------+-------------+
| > 2\. mortar                             | 0.011       | 0.013      | 0.015       |
+------------------------------------------+-------------+------------+-------------+
| > b\. Wood                               |             |            |             |
+------------------------------------------+-------------+------------+-------------+
| > 1. planed, untreated                   | 0.010       | 0.012      | 0.014       |
+------------------------------------------+-------------+------------+-------------+
| > 2\.  planed, creosoted                 | 0.011       | 0.012      | 0.015       |
+------------------------------------------+-------------+------------+-------------+
| > 3\. unplaned                           | 0.011       | 0.013      | 0.015       |
+------------------------------------------+-------------+------------+-------------+
| > 4\. plank with battens                 | 0.012       | 0.015      | 0.018       |
+------------------------------------------+-------------+------------+-------------+
| > 5\. lined with roofing paper           | 0.010       | 0.014      | 0.017       |
+------------------------------------------+-------------+------------+-------------+
| > c\. Concrete                           |             |            |             |
+------------------------------------------+-------------+------------+-------------+
| > 1\. trowel finish                      | 0.011       | 0.013      | 0.015       |
+------------------------------------------+-------------+------------+-------------+
| > 2\. float finish                       | 0.013       | 0.015      | 0.016       |
+------------------------------------------+-------------+------------+-------------+
| > 3\. finished, with gravel on bottom    | 0.015       | 0.017      | 0.020       |
+------------------------------------------+-------------+------------+-------------+
| > 4\. unfinished                         | 0.014       | 0.017      | 0.020       |
+------------------------------------------+-------------+------------+-------------+
| > 5\. gunite, good section               | 0.016       | 0.019      | 0.023       |
+------------------------------------------+-------------+------------+-------------+
| > 6\. gunite, wavy section               | 0.018       | 0.022      | 0.025       |
+------------------------------------------+-------------+------------+-------------+
| > 7\. on good excavated rock             | 0.017       | 0.020      |             |
+------------------------------------------+-------------+------------+-------------+
| > 8\. on irregular excavated rock        | 0.022       | 0.027      |             |
+------------------------------------------+-------------+------------+-------------+

: **Table G-1 Continued**

+------------------------------------------+-------------+------------+-------------+
| **Type of Channel and Description**      | **Minimum** | **Normal** | **Maximum** |
+:=========================================+:===========:+:==========:+:===========:+
|                                          |             |            |             |
+------------------------------------------+-------------+------------+-------------+
| > d\. Concrete bottom float finish with  |             |            |             |
| > sides of:                              |             |            |             |
+------------------------------------------+-------------+------------+-------------+
| > 1\. dressed stone in mortar            | 0.015       | 0.017      | 0.020       |
+------------------------------------------+-------------+------------+-------------+
| > 2\. random stone in mortar             | 0.017       | 0.020      | 0.024       |
+------------------------------------------+-------------+------------+-------------+
| > 3\. cement rubble masonry, plastered   | 0.016       | 0.020      | 0.024       |
+------------------------------------------+-------------+------------+-------------+
| > 4\. cement rubble masonry              | 0.020       | 0.025      | 0.030       |
+------------------------------------------+-------------+------------+-------------+
| > 5\. dry rubble or riprap               | 0.020       | 0.030      | 0.035       |
+------------------------------------------+-------------+------------+-------------+
| > e\. Gravel bottom with sides of:       |             |            |             |
+------------------------------------------+-------------+------------+-------------+
| > 1\. formed concrete                    | 0.017       | 0.020      | 0.025       |
+------------------------------------------+-------------+------------+-------------+
| > 2\. random stone mortar                | 0.020       | 0.023      | 0.026       |
+------------------------------------------+-------------+------------+-------------+
| > 3\. dry rubble or riprap               | 0.023       | 0.033      | 0.036       |
+------------------------------------------+-------------+------------+-------------+
| > f\. Brick                              |             |            |             |
+------------------------------------------+-------------+------------+-------------+
| > 1\. glazed                             | 0.011       | 0.013      | 0.015       |
+------------------------------------------+-------------+------------+-------------+
| > 2\. in cement mortar                   | 0.012       | 0.015      | 0.018       |
+------------------------------------------+-------------+------------+-------------+
| > g\. Masonry                            |             |            |             |
+------------------------------------------+-------------+------------+-------------+
| > 1\. cemented rubble                    | 0.017       | 0.025      | 0.030       |
+------------------------------------------+-------------+------------+-------------+
| > 2\. dry rubble                         | 0.023       | 0.032      | 0.035       |
+------------------------------------------+-------------+------------+-------------+
| > h\. Dressed ashlar/stone paving        | 0.013       | 0.015      | 0.017       |
+------------------------------------------+-------------+------------+-------------+
| > i\. Asphalt                            |             |            |             |
+------------------------------------------+-------------+------------+-------------+
| > 1\. smooth                             | 0.013       | 0.013      |             |
+------------------------------------------+-------------+------------+-------------+
| 2\. rough                                | 0.016       | 0.016      |             |
+------------------------------------------+-------------+------------+-------------+
| > j\. Vegetal lining                     | 0.030       |            | 0.500       |
+------------------------------------------+-------------+------------+-------------+

: **Table G-1 Continued**

Source: Chow, 1959.

>  

 

+--------------------------------------+-------------+------------+-------------+
| > **Type of Conduit and              | **Minimum** | **Normal** | **Maximum** |
| > Description**                      |             |            |             |
+======================================+:===========:+:==========:+:===========:+
| > **1. Brass, smooth:**              | 0.009       | 0.010      | 0.013       |
+--------------------------------------+-------------+------------+-------------+
| > **2. Steel:**                      |             |            |             |
+--------------------------------------+-------------+------------+-------------+
| > Lockbar and welded                 | 0.010       | 0.012      | 0.014       |
+--------------------------------------+-------------+------------+-------------+
| > Riveted and spiral                 | 0.013       | 0.016      | 0.017       |
+--------------------------------------+-------------+------------+-------------+
| > **3. Cast Iron:**                  |             |            |             |
+--------------------------------------+-------------+------------+-------------+
| > Coated                             | 0.010       | 0.013      | 0.014       |
+--------------------------------------+-------------+------------+-------------+
| > Uncoated                           | 0.011       | 0.014      | 0.016       |
+--------------------------------------+-------------+------------+-------------+
| > **4. Wrought Iron:**               |             |            |             |
+--------------------------------------+-------------+------------+-------------+
| > Black                              | 0.012       | 0.014      | 0.015       |
+--------------------------------------+-------------+------------+-------------+
| > Galvanized                         | 0.013       | 0.016      | 0.017       |
+--------------------------------------+-------------+------------+-------------+
| > **5. Corrugated Metal:**           |             |            |             |
+--------------------------------------+-------------+------------+-------------+
| > Subdrain                           | 0.017       | 0.019      | 0.021       |
+--------------------------------------+-------------+------------+-------------+
| > Stormdrain                         | 0.021       | 0.024      | 0.030       |
+--------------------------------------+-------------+------------+-------------+
| > **6. Cement:**                     |             |            |             |
+--------------------------------------+-------------+------------+-------------+
| > Neat Surface                       | 0.010       | 0.011      | 0.013       |
+--------------------------------------+-------------+------------+-------------+
| > Mortar                             | 0.011       | 0.013      | 0.015       |
+--------------------------------------+-------------+------------+-------------+
| > **7. Concrete:**                   |             |            |             |
+--------------------------------------+-------------+------------+-------------+
| > Culvert, straight and free of      | 0.010       | 0.011      | 0.013       |
| > debris                             |             |            |             |
+--------------------------------------+-------------+------------+-------------+
| > Culvert with bends, connections,   | 0.011       | 0.013      | 0.014       |
| > and some debris                    |             |            |             |
+--------------------------------------+-------------+------------+-------------+
| > Finished                           | 0.011       | 0.012      | 0.014       |
+--------------------------------------+-------------+------------+-------------+
| > Sewer with manholes, inlet, etc.,  | 0.013       | 0.015      | 0.017       |
| > straight                           |             |            |             |
+--------------------------------------+-------------+------------+-------------+
| > Unfinished, steel form             | 0.012       | 0.013      | 0.014       |
+--------------------------------------+-------------+------------+-------------+
| > Unfinished, smooth wood form       | 0.012       | 0.014      | 0.016       |
+--------------------------------------+-------------+------------+-------------+
| > Unfinished, rough wood form        | 0.015       | 0.017      | 0.020       |
+--------------------------------------+-------------+------------+-------------+
| > **8. Wood:**                       |             |            |             |
+--------------------------------------+-------------+------------+-------------+
| > Stave                              | 0.010       | 0.012      | 0.014       |
+--------------------------------------+-------------+------------+-------------+
| > Laminated, treated                 | 0.015       | 0.017      | 0.020       |
+--------------------------------------+-------------+------------+-------------+

: []{#_Toc484695164 .anchor}**Table G‑2 Manning's roughness coefficient
n for closed conduits**

>  
>
>  

+--------------------------------------+-------------+------------+-------------+
| > **Type of Conduit and              | **Minimum** | **Normal** | **Maximum** |
| > Description**                      |             |            |             |
+======================================+:===========:+:==========:+:===========:+
| > **9. Clay:**                       |             |            |             |
+--------------------------------------+-------------+------------+-------------+
| > Common drainage tile               | 0.011       | 0.013      | 0.017       |
+--------------------------------------+-------------+------------+-------------+
| > Vitrified sewer                    | 0.011       | 0.014      | 0.017       |
+--------------------------------------+-------------+------------+-------------+
| > Vitrified sewer with manholes,     | 0.013       | 0.015      | 0.017       |
| > inlet, etc.                        |             |            |             |
+--------------------------------------+-------------+------------+-------------+
| > Vitrified subdrain with open joint | 0.014       | 0.016      | 0.018       |
+--------------------------------------+-------------+------------+-------------+
| > **10. Brickwork:**                 |             |            |             |
+--------------------------------------+-------------+------------+-------------+
| > Glazed                             | 0.011       | 0.013      | 0.015       |
+--------------------------------------+-------------+------------+-------------+
| > Lined with cement mortar           | 0.012       | 0.015      | 0.017       |
+--------------------------------------+-------------+------------+-------------+
| > Sanitary sewers coated with sewage | 0.012       | 0.013      | 0.016       |
| > slime with bends and connections   |             |            |             |
+--------------------------------------+-------------+------------+-------------+
| > Paved invert, sewer, smooth bottom | 0.016       | 0.019      | 0.020       |
+--------------------------------------+-------------+------------+-------------+
| > Rubble masonry, cemented           | 0.018       | 0.025      | 0.030       |
+--------------------------------------+-------------+------------+-------------+

: **Table G-2 Continued**

Source: Chow, 1959.

+-------------------------------------------------------+--------------+
|   **Type of Pipe, Diameter and Corrugation            | **n**        |
| Dimension**                                           |              |
+=======================================================+:============:+
| **  1. Annular 2.67 x 1/2 inch (all diameters)**      | 0.024        |
+-------------------------------------------------------+--------------+
| **  2. Helical 1.50 x 1/4 inch**                      |              |
+-------------------------------------------------------+--------------+
| > 8\" diameter                                        | 0.012        |
+-------------------------------------------------------+--------------+
| > 10\" diameter                                       | 0.014        |
+-------------------------------------------------------+--------------+
| **  3. Helical 2.67 x 1/2 inch**                      |              |
+-------------------------------------------------------+--------------+
| > 12\" diameter                                       | 0.011        |
+-------------------------------------------------------+--------------+
| > 18\" diameter                                       | 0.014        |
+-------------------------------------------------------+--------------+
| > 24\" diameter                                       | 0.016        |
+-------------------------------------------------------+--------------+
| > 36\" diameter                                       | 0.019        |
+-------------------------------------------------------+--------------+
| > 48\" diameter                                       | 0.020        |
+-------------------------------------------------------+--------------+
| > 60\" diameter                                       | 0.021        |
+-------------------------------------------------------+--------------+
| **  4. Annular 3x1 inch (all diameters)**             | 0.027        |
+-------------------------------------------------------+--------------+
| **  5. Helical 3x1 inch**                             |              |
+-------------------------------------------------------+--------------+
| > 48\" diameter                                       | 0.023        |
+-------------------------------------------------------+--------------+
| > 54\" diameter                                       | 0.023        |
+-------------------------------------------------------+--------------+
| > 60\" diameter                                       | 0.024        |
+-------------------------------------------------------+--------------+
| > 66\" diameter                                       | 0.025        |
+-------------------------------------------------------+--------------+
| > 72\" diameter                                       | 0.026        |
+-------------------------------------------------------+--------------+
| > 78\" diameter and larger                            | 0.027        |
+-------------------------------------------------------+--------------+
| **  6. Corrugations 6x2 inches**                      |              |
+-------------------------------------------------------+--------------+
| > 60\" diameter                                       | 0.033        |
+-------------------------------------------------------+--------------+
| > 72\" diameter                                       | 0.032        |
+-------------------------------------------------------+--------------+
| > 120\" diameter                                      | 0.030        |
+-------------------------------------------------------+--------------+
| > 180\" diameter                                      | 0.028        |
+-------------------------------------------------------+--------------+

: []{#_Toc484695165 .anchor}**Table G‑3 Manning's roughness coefficient
n for corrugated steel pipe**

 Source: American Iron and Steel Institute, 1999.

 

#### Culvert Coefficients

[]{#_Toc484695166 .anchor}

+----------------------------+---------------------------------+----------+
| **Culvert Shape and        | **Inlet Configuration**         | **Code** |
| Material**                 |                                 |          |
+:===========================+:================================+:========:+
| Circular Concrete          | Square edge with headwall       | 1        |
|                            +---------------------------------+----------+
|                            | Groove end with headwall        | 2        |
|                            +---------------------------------+----------+
|                            | Groove end projecting           | 3        |
+----------------------------+---------------------------------+----------+
| Circular Corrugated Metal  | Headwall                        | 4        |
| Pipe                       |                                 |          |
|                            +---------------------------------+----------+
|                            | Mitered to slope                | 5        |
|                            +---------------------------------+----------+
|                            | Projecting                      | 6        |
+----------------------------+---------------------------------+----------+
| Circular Pipe, Beveled     | 45 deg. bevels                  | 7        |
| Ring Entrance              |                                 |          |
|                            +---------------------------------+----------+
|                            | 33.7 deg. bevels                | 8        |
+----------------------------+---------------------------------+----------+
| Rectangular Box; Flared    | 30-75 deg. wingwall flares      | 9        |
| Wingwalls                  |                                 |          |
|                            +---------------------------------+----------+
|                            | 90 or 15 deg. wingwall flares   | 10       |
|                            +---------------------------------+----------+
|                            | 0 deg. wingwall flares          | 11       |
|                            | (straight sides)                |          |
+----------------------------+---------------------------------+----------+
| Rectangular Box;Flared     | 45 deg flare; 0.43D top edge    | 12       |
| Wingwalls and Top Edge     | bevel                           |          |
| Bevel                      |                                 |          |
|                            +---------------------------------+----------+
|                            | 18-33.7 deg. flare; 0.083D top  | 13       |
|                            | edge bevel                      |          |
+----------------------------+---------------------------------+----------+
| Rectangular Box, 90-deg    | Chamfered 3/4-in.               | 14       |
| Headwall, Chamfered /      |                                 |          |
| Beveled Inlet Edges        |                                 |          |
|                            +---------------------------------+----------+
|                            | Beveled 1/2-in/ft at 45 deg     | 15       |
|                            | (1:1)                           |          |
|                            +---------------------------------+----------+
|                            | Beveled 1-in/ft at 33.7 deg     | 16       |
|                            | (1:1.5)                         |          |
+----------------------------+---------------------------------+----------+
| Rectangular Box, Skewed    | 3/4\" chamfered edge, 45 deg    | 17       |
| Headwall, Chamfered /      | skewed headwall                 |          |
| Beveled Inlet Edges        |                                 |          |
|                            +---------------------------------+----------+
|                            | 3/4\" chamfered edge, 30 deg    | 18       |
|                            | skewed headwall                 |          |
|                            +---------------------------------+----------+
|                            | 3/4\" chamfered edge, 15 deg    | 19       |
|                            | skewed headwall                 |          |
|                            +---------------------------------+----------+
|                            | 45 deg beveled edge, 10-45 deg  | 20       |
|                            | skewed headwall                 |          |
+----------------------------+---------------------------------+----------+
| Rectangular Box,           | 45 deg (1:1) wingwall flare     | 21       |
| Non-offset Flared          |                                 |          |
| Wingwalls, 3/4\" Chamfer   |                                 |          |
| at Top of Inlet            |                                 |          |
|                            +---------------------------------+----------+
|                            | 8.4 deg (3:1) wingwall flare    | 22       |
|                            +---------------------------------+----------+
|                            | 18.4 deg (3:1) wingwall flare,  | 23       |
|                            | 30 deg inlet skew               |          |
+----------------------------+---------------------------------+----------+

: **Table H‑1 Culvert codes**

+----------------------------+---------------------------------+----------+
| **Culvert Shape and        | **Inlet Configuration**         | **Code** |
| Material**                 |                                 |          |
+:===========================+:================================+:========:+
| Rectangular Box, Offset    | 45 deg (1:1) flare, 0.042D top  | 24       |
| Flared Wingwalls, Beveled  | edge bevel                      |          |
| Edge at Inlet Top          |                                 |          |
|                            +---------------------------------+----------+
|                            | 33.7 deg (1.5:1) flare, 0.083D  | 25       |
|                            | top edge bevel                  |          |
|                            +---------------------------------+----------+
|                            | 18.4 deg (3:1) flare, 0.083D    | 26       |
|                            | top edge bevel                  |          |
+----------------------------+---------------------------------+----------+
| Corrugated Metal Box       | 90 deg headwall                 | 27       |
|                            +---------------------------------+----------+
|                            | Thick wall projecting           | 28       |
|                            +---------------------------------+----------+
|                            | Thin wall projecting            | 29       |
+----------------------------+---------------------------------+----------+
| Horizontal Ellipse         | Square edge with headwall       | 30       |
| Concrete                   |                                 |          |
|                            +---------------------------------+----------+
|                            | Grooved end with headwall       | 31       |
|                            +---------------------------------+----------+
|                            | Grooved end projecting          | 32       |
+----------------------------+---------------------------------+----------+
| Vertical Ellipse Concrete  | Square edge with headwall       | 33       |
|                            +---------------------------------+----------+
|                            | Grooved end with headwall       | 34       |
|                            +---------------------------------+----------+
|                            | Grooved end projecting          | 35       |
+----------------------------+---------------------------------+----------+
| Pipe Arch, 18\" Corner     | 90 deg headwall                 | 36       |
| Radius, Corrugated Metal   |                                 |          |
|                            +---------------------------------+----------+
|                            | Mitered to slope                | 37       |
|                            +---------------------------------+----------+
|                            | Projecting                      | 38       |
+----------------------------+---------------------------------+----------+
| Pipe Arch, 18\" Corner     | Projecting                      | 39       |
| Radius, Corrugated Metal   |                                 |          |
|                            +---------------------------------+----------+
|                            | No bevels                       | 40       |
|                            +---------------------------------+----------+
|                            | 33.7 deg bevels                 | 41       |
+----------------------------+---------------------------------+----------+
| Pipe Arch, 31\" Corner     | Projecting                      | 42       |
| Radius,Corrugated Metal    |                                 |          |
|                            +---------------------------------+----------+
|                            | No bevels                       | 43       |
|                            +---------------------------------+----------+
|                            | 33.7 deg. bevels                | 44       |
+----------------------------+---------------------------------+----------+
| Arch, Corrugated Metal     | 90 deg headwall                 | 45       |
|                            +---------------------------------+----------+
|                            | Mitered to slope                | 46       |
|                            +---------------------------------+----------+
|                            | Thin wall projecting            | 47       |
+----------------------------+---------------------------------+----------+
| Circular Culvert           | Smooth tapered inlet throat     | 48       |
|                            +---------------------------------+----------+
|                            | Rough tapered inlet throat      | 49       |
+----------------------------+---------------------------------+----------+

: **Table H-1 Continued**

**\
Table H-1 Continued**

+----------------------------+---------------------------------+----------+
| **Culvert Shape and        | **Inlet Configuration**         | **Code** |
| Material**                 |                                 |          |
+:===========================+:================================+:========:+
| Elliptical Inlet Face      | Tapered inlet, beveled edges    | 50       |
|                            +---------------------------------+----------+
|                            | Tapered inlet, square edges     | 51       |
|                            +---------------------------------+----------+
|                            | Tapered inlet, thin edge        | 52       |
|                            | projecting                      |          |
+----------------------------+---------------------------------+----------+
| Rectangular                | Tapered inlet throat            | 53       |
+----------------------------+---------------------------------+----------+
| Rectangular Concrete       | Side tapered, less favorable    | 54       |
|                            | edges                           |          |
|                            +---------------------------------+----------+
|                            | Side tapered, more favorable    | 55       |
|                            | edges                           |          |
|                            +---------------------------------+----------+
|                            | Slope tapered, less favorable   | 56       |
|                            | edges                           |          |
|                            +---------------------------------+----------+
|                            | Slope tapered, more favorable   | 57       |
|                            | edges                           |          |
+----------------------------+---------------------------------+----------+

+----------+----------+-------------+-------------+-----------+-----------+
| Culvert  | Equation | Unsubmerged | Unsubmerged | Submerged | Submerged |
|          |          |             |             |           |           |
| Code     | Form     | K           | M           | c         | y         |
+:=========+:========:+:===========:+:===========:+:=========:+:=========:+
| 1        | 1        | 0.0098      | 2.00        | 0.0398    | 0.67      |
+----------+----------+-------------+-------------+-----------+-----------+
| 2        | 1        | 0.0018      | 2.00        | 0.0292    | 0.74      |
+----------+----------+-------------+-------------+-----------+-----------+
| 3        | 1        | 0.0045      | 2.00        | 0.0317    | 0.69      |
+----------+----------+-------------+-------------+-----------+-----------+
| 4        | 1        | 0.0078      | 2.00        | 0.0379    | 0.69      |
+----------+----------+-------------+-------------+-----------+-----------+
| 5        | 1        | 0.0210      | 1.33        | 0.0463    | 0.75      |
+----------+----------+-------------+-------------+-----------+-----------+
| 6        | 1        | 0.0340      | 1.50        | 0.0553    | 0.54      |
+----------+----------+-------------+-------------+-----------+-----------+
| 7        | 1        | 0.0018      | 2.50        | 0.0300    | 0.74      |
+----------+----------+-------------+-------------+-----------+-----------+
| 8        | 1        | 0.0018      | 2.50        | 0.0243    | 0.83      |
+----------+----------+-------------+-------------+-----------+-----------+
| 9        | 1        | 0.026       | 1.0         | 0.0347    | 0.81      |
+----------+----------+-------------+-------------+-----------+-----------+
| 10       | 1        | 0.061       | 0.75        | 0.0400    | 0.80      |
+----------+----------+-------------+-------------+-----------+-----------+
| 11       | 1        | 0.061       | 0.75        | 0.0423    | 0.82      |
+----------+----------+-------------+-------------+-----------+-----------+
| 12       | 2        | 0.510       | 0.667       | 0.0309    | 0.80      |
+----------+----------+-------------+-------------+-----------+-----------+
| 13       | 2        | 0.486       | 0.667       | 0.0249    | 0.83      |
+----------+----------+-------------+-------------+-----------+-----------+
| 14       | 2        | 0.515       | 0.667       | 0.0375    | 0.79      |
+----------+----------+-------------+-------------+-----------+-----------+
| 15       | 2        | 0.495       | 0.667       | 0.0314    | 0.82      |
+----------+----------+-------------+-------------+-----------+-----------+
| 16       | 2        | 0.486       | 0.667       | 0.0252    | 0.865     |
+----------+----------+-------------+-------------+-----------+-----------+
| 17       | 2        | 0.545       | 0.667       | 0.04505   | 0.73      |
+----------+----------+-------------+-------------+-----------+-----------+
| 18       | 2        | 0.533       | 0.667       | 0.0425    | 0.705     |
+----------+----------+-------------+-------------+-----------+-----------+
| 19       | 2        | 0.522       | 0.667       | 0.0402    | 0.68      |
+----------+----------+-------------+-------------+-----------+-----------+
| 20       | 2        | 0.498       | 0.667       | 0.0327    | 0.75      |
+----------+----------+-------------+-------------+-----------+-----------+
| 21       | 2        | 0.497       | 0.667       | 0.0339    | 0.803     |
+----------+----------+-------------+-------------+-----------+-----------+
| 22       | 2        | 0.493       | 0.667       | 0.0361    | 0.806     |
+----------+----------+-------------+-------------+-----------+-----------+
| 23       | 2        | 0.495       | 0.667       | 0.0386    | 0.71      |
+----------+----------+-------------+-------------+-----------+-----------+
| 24       | 2        | 0.497       | 0.667       | 0.0302    | 0.835     |
+----------+----------+-------------+-------------+-----------+-----------+
| 25       | 2        | 0.495       | 0.667       | 0.0252    | 0.881     |
+----------+----------+-------------+-------------+-----------+-----------+
| 26       | 2        | 0.493       | 0.667       | 0.0227    | 0.887     |
+----------+----------+-------------+-------------+-----------+-----------+
| 27       | 1        | 0.0083      | 2.00        | 0.0379    | 0.69      |
+----------+----------+-------------+-------------+-----------+-----------+
| 28       | 1        | 0.0145      | 1.75        | 0.0419    | 0.64      |
+----------+----------+-------------+-------------+-----------+-----------+
| 29       | 1        | 0.0340      | 1.50        | 0.0496    | 0.57      |
+----------+----------+-------------+-------------+-----------+-----------+
| 30       | 1        | 0.0100      | 2.00        | 0.0398    | 0.67      |
+----------+----------+-------------+-------------+-----------+-----------+
| 31       | 1        | 0.0018      | 2.50        | 0.0292    | 0.74      |
+----------+----------+-------------+-------------+-----------+-----------+
| 32       | 1        | 0.0045      | 2.00        | 0.0317    | 0.69      |
+----------+----------+-------------+-------------+-----------+-----------+

: []{#_Toc484695167 .anchor}**Table H‑2 Culvert coefficients**

+----------+----------+-------------+-------------+-----------+-----------+
| Culvert  | Equation | Unsubmerged | Unsubmerged | Submerged | Submerged |
|          |          |             |             |           |           |
| Code     | Form     | K           | M           | c         | y         |
+:=========+:========:+:===========:+:===========:+:=========:+:=========:+
| 33       | 1        | 0.0100      | 2.00        | 0.0398    | 0.67      |
+----------+----------+-------------+-------------+-----------+-----------+
| 34       | 1        | 0.0018      | 2.50        | 0.0292    | 0.74      |
+----------+----------+-------------+-------------+-----------+-----------+
| 35       | 1        | 0.0095      | 2.00        | 0.0317    | 0.69      |
+----------+----------+-------------+-------------+-----------+-----------+
| 36       | 1        | 0.0083      | 2.00        | 0.0379    | 0.69      |
+----------+----------+-------------+-------------+-----------+-----------+
| 37       | 1        | 0.0300      | 1.00        | 0.0463    | 0.75      |
+----------+----------+-------------+-------------+-----------+-----------+
| 38       | 1        | 0.0340      | 1.50        | 0.0496    | 0.57      |
+----------+----------+-------------+-------------+-----------+-----------+
| 39       | 1        | 0.0300      | 1.50        | 0.0496    | 0.57      |
+----------+----------+-------------+-------------+-----------+-----------+
| 40       | 1        | 0.0088      | 2.00        | 0.0368    | 0.68      |
+----------+----------+-------------+-------------+-----------+-----------+
| 41       | 1        | 0.0030      | 2.00        | 0.0269    | 0.77      |
+----------+----------+-------------+-------------+-----------+-----------+
| 42       | 1        | 0.0300      | 1.50        | 0.0496    | 0.57      |
+----------+----------+-------------+-------------+-----------+-----------+
| 43       | 1        | 0.0088      | 2.00        | 0.0368    | 0.68      |
+----------+----------+-------------+-------------+-----------+-----------+
| 44       | 1        | 0.0030      | 2.00        | 0.0269    | 0.77      |
+----------+----------+-------------+-------------+-----------+-----------+
| 45       | 1        | 0.0083      | 2.00        | 0.0379    | 0.69      |
+----------+----------+-------------+-------------+-----------+-----------+
| 46       | 1        | 0.0300      | 1.00        | 0.0473    | 0.75      |
+----------+----------+-------------+-------------+-----------+-----------+
| 47       | 1        | 0.0340      | 1.50        | 0.0496    | 0.57      |
+----------+----------+-------------+-------------+-----------+-----------+
| 48       | 2        | 0.534       | 0.555       | 0.0196    | 0.90      |
+----------+----------+-------------+-------------+-----------+-----------+
| 49       | 2        | 0.519       | 0.640       | 0.0210    | 0.90      |
+----------+----------+-------------+-------------+-----------+-----------+
| 50       | 2        | 0.536       | 0.622       | 0.0368    | 0.83      |
+----------+----------+-------------+-------------+-----------+-----------+
| 51       | 2        | 0.5035      | 0.719       | 0.0478    | 0.80      |
+----------+----------+-------------+-------------+-----------+-----------+
| 52       | 2        | 0.547       | 0.800       | 0.0598    | 0.75      |
+----------+----------+-------------+-------------+-----------+-----------+
| 53       | 2        | 0.475       | 0.667       | 0.0179    | 0.97      |
+----------+----------+-------------+-------------+-----------+-----------+
| 54       | 2        | 0.560       | 0.667       | 0.0446    | 0.85      |
+----------+----------+-------------+-------------+-----------+-----------+
| 55       | 2        | 0.560       | 0.667       | 0.0378    | 0.87      |
+----------+----------+-------------+-------------+-----------+-----------+
| 56       | 2        | 0.500       | 0.667       | 0.0446    | 0.65      |
+----------+----------+-------------+-------------+-----------+-----------+
| 57       | 2        | 0.500       | 0.667       | 0.0378    | 0.71      |
+----------+----------+-------------+-------------+-----------+-----------+

: **Table H-2 Continued**

# References

\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_

American Concrete Pipe Association, *Concrete Pipe Design Manual*,
American Concrete Pipe Association, <http://www.concrete-pipe.org>,
2011.

American Iron and Steel Institute, *Modern Sewer Design*, American Iron
and Steel Institute, 1999.

Armco, *Handbook of Drainage and Construction Products*, Armco Drainage
& Metal Products, Inc. 1978. (Out of print; see
[http://www.hydrogate.com/sites/hydrogate.com/files/Flap Gates
Brochure.pdf](http://www.hydrogate.com/sites/hydrogate.com/files/Flap%20Gates%20Brochure.pdf)
instead.)

Ascher, U.M. and Petzold, L.R., *Computer Methods for Ordinary
Differential Equations and Differential-Algebraic Equations*, SIAM,
Philadelphia, 1998.

Atkinson, K.E., *An Introduction to Numerical Analysis* (2nd ed.), John
Wiley & Sons, New York 1989.

Bhave, P.R., *Analysis of Flow in Water Distribution Networks*,
Technomic Publishing, Lancaster, PA, 1991.

Brater, E.F. and King, H.W., *Handbook of Hydraulics*, (6^th^ edition),
McGraw Hill Book Company, New York, NY, 1976.

Brater, E.F., King, H.W., Lindel, J.E., and Wei, C.Y., *Handbook of
Hydraulics* (7^th^ edition), McGraw-Hill, New York, NY, 1996.

Brunner,G.W., "Combined 1D and 2D Modeling with HEC-RAS". U.S. Army
Corps of Engineers Hydrologic Engineering Center, October, 2014.

<http://www.hec.usace.army.mil/misc/files/ras/Combined_1D_and_2D_Modeling_with_HEC-RAS.pdf>

Bureau of Reclamation, *Water Measurement Manual*, U.S. Department of
the Interior, Washington, DC, 2001.

Chow, V.T., *Open-Channel Hydraulics*, McGraw-Hill, NY, 1959.

Clark, J.W., Viessman, W. Jr., Hammer, M.J., *Water Supply and Pollution
Control*, (Third Edition), IEP, New York, NY, 1977.

Cormen, T.H., Leiserson, C.E., Rivest, R.L. and Stein, C. *Introduction
to Algorithms* (3rd edition), MIT Press and McGraw-Hill, 2009.

Cunge, J.A., Holly, F.M. Jr., and Verwey, A., *Practical Aspects of
Computational River Hydraulics,* Pitman, London, 1980.

Cunge, J.A. and Wegner, M. "Numerical integration of Barré de
Saint-Venant\'s flow equations by means of an implicit scheme of finite
differences", *La Houille Blanche*, Number 1, 1964.

Davis, C.V., *Handbook of Applied Hydraulics*, Second Edition,
McGraw-Hill, New York, 1952.

de Almeida, G.A.M. and Bates, P., "Applicability of the local inertial
approximation of the shallow water equations to flood modeling", *Water
Resour. Res*., 49, 4833--4844, 2013.

Faram, M.G., Stephenson, A.G. and Andoh, R.Y.G., "Vortex flow controls:
state of the art review and application (from the catchbasin to the
dam)", *Proceedings of the 7^th^ International Conference on Sustainable
Techniques and Strategies in Urban Water Management* (Novatech 2010),
Lyon, France, 27 June -- 1 July, 2010.

Featherstone, R.E. and Nalluri, C., *Civil Engineering Hydraulics*,
Granada Publishing, Bungay, Great Britain, 1982.

Federal Highway Administration (FHWA), *Urban Drainage Design Manual*,
Third Edition, Hydraulic Engineering Circular No. 22, FHWA-NHI-10-009,
U.S. Department of Transportation, 2009.

Federal Highway Administration (FHWA), *Hydraulic Design of Culverts*,
Third Edition, Hydraulic Design Series Number 5, FHWA-HIF-12-026, U.S.
Department of Transportation, April, 2012.

Fread, D.L., Jin, M. and Lewis, J.M., "An LPI numerical solution for
unsteady mixed flow simulation", Proceedings North American Water and
Environment Congress '96, American Society of Civil Engineers, Anaheim,
California, 1996.

French, R.H., *Open-Channel Hydraulics*, McGraw-Hill, NY, 1985.

Frost, W.H., "Minor Loss Coefficients for Storm Drain Modeling with
SWMM", *Journal of Water Management Modeling*, R225-23, doi:
10.14796/JWMM.R225-23, 2006.

(<http://www.chijournal.org>)

Heaney, J.P., Huber, W.C., Sheikhv, H., Medina, M.A., Doyle, J.R.,
Peltz, W.A., and Darling, J.E., "Urban Stormwater Management Modeling
and Decision Making", EPA-670/2-75-022 (NTIS PB-242290), U.S.
Environmental Protection Agency, Cincinnati, OH, 1975.

Henderson, F.M., *Open Channel Flow*, MacMillan Publishing, NY, 1966.

Huber, W.C., J.P. Heaney, M.A. Medina, W.A. Peltz, H. Sheikh, and G.F.
Smith, "Storm Water Management Model User's Manual ­ Version II,"
EPA-670/2-75-01· (NTIS PB-257809), U.S. Environmental Protection Agency,
Cincinnati, OH, March 1975.

Huber, W.C., Heaney,J.P., Nix, S.J., Dickinson, R.E. and Polmann, D.J.,
"Storm Water Management Model User's Manual, Version III,"
EPA-600/2-84-109a (NTIS PB84-198423), U.S. Environmental Protection
Agency, Cincinnati, OH, November 1981.

Huber, W.C., and Dickinson, R.E., "Storm Water Management Model, Version
4, User\'s Manual"*,* EPA/600/3-88/001a (NTIS PB88-236641/AS), U.S.
Environmental Protection Agency, Athens, GA, 1988.

Huber, W.C. and Roesner, L., \"The History and Evolution of the EPA
SWMM\" in Fifty Years of Watershed Modeling - Past, Present And Future,
A.S. Donigian and R. Field, eds., ECI Symposium Series, Volume P20,
2013. <http://dc.engconfintl.org/watershed/29>

Hydro International, "Reg-U-Flo^®^ Vortex Valves Modeling in SWMM 5.0",
June, 2009.

<http://www.hydrointernational.biz>

Kibler, D.F., Monser, J.R. and Roesner, L.A., "San Francisco Stormwater
Model, User's Manual and Program Documentation", prepared for the
Division of Sanitary Engineering City and County of San Francisco, Water
Resources Engineers, Walnut Creek, CA, 1975.

Mays, L.W., "Storm and Combined Sewer Overflow: Flow Regulation and
Control", Chapter 18 in *Stormwater Collection Systems Design Handbook*,
L.W. Mays editor, McGraw-Hill, NY, 2001.

Metcalf, L. and Eddy, H.P., *American Sewerage Practice, Design of
Sewers*, Volume 1, McGraw-Hill, New York, 1914.

Metcalf & Eddy, Inc. *Wastewater Engineering*, McGraw-Hill, New York,
NY, 1972.

Metcalf and Eddy, Inc., University of Florida, and Water Resources
Engineers, Inc., "Storm Water Management Model, Volume I ­ Final Report,"
EPA Report 11024 DOC 07/71 (NTIS PB-203289), U.S. Environmental
Protection Agency, Washington, DC, July 1971a.

Metcalf and Eddy, Inc., University of Florida, and Water Resources
Engineers, Inc., "Storm Water Management Model, Volume II ­ Verification
and Testing," EPA Report 11024 DOC 08/71 (NTIS PB-203290), U.S.
Environmental Protection Agency, Washington, DC, August 1971b.

Metcalf and Eddy, Inc., University of Florida, and Water Resources
Engineers, Inc., "Storm Water Management Model, Volume III ­ User's
Manual," EPA-11024 DOC 09/71 (NTIS PB-203291), U.S. Environmental
Protection Agency, Washington, DC, September 1971c.

Metcalf and Eddy, Inc., University of Florida, and Water Resources
Engineers, Inc., "Storm Water Management Model, Volume IV ­ Program
Listing," EPA Report 11024 DOC 10/71 (NTIS PB-203292), U.S.
Environmental Protection Agency, Washington, DC, October 1971d.

Miller, J.E., "Basic Concepts of Kinematic-Wave Models", U.S. Geological
Survey Professional Paper 1302, U.S. Geological Survey, Department of
the Interior, Alexandria, VA, 1984.

O'Brien, G.G., Hyman, M.A., and Kaplan, S. "A Study of the Numerical
Solution of Partial Differential Equations", *Journal Math. and
Physics*, No. 29, pp. 223-251, 1951.

Ponce, V.M., "The Kinematic Wave Controversy", *Journal of Hydraulic
Engineering*, Vol. 117, No. 4, April, 1991.

Ponce, V. M., Li, R.M. and Simons, D.B., \"Applicability of Kinematic
and Diffusion Models.\" *Journal of the Hydraulics Division, ASCE*, Vol.
104, No. HY3, March, pp. 353-360, 1978.

Press, W.H., Teukolsky, S.A., Vetterling, W.T., and Flannery, B.P.,
*Numerical Recipes in C*, Second Edition, Cambridge University Press,
1992.

Roesner, L.A., Kassem, A.M., and Wisner, P.E., "Improvements in EXTRAN",
*Proceedings Stormwater Management Model (SWMM) Users Group Meeting 10
-- 11 January* 1980, EPA-600/9-80-017, U.S. Environmental Protection
Agency, Washington, DC, March 1980.

Roesner, L.A., Shubinski, R.P. and Aldrich, J.A., "Stormwater Management
Model User's Manual Version III, Addendum I EXTRAN", EPA-600/2-84-109b,
Municipal Environmental Research Laboratory, U.S. Environmental
Protection Agency, Cincinnati, OH, 1983.

Roesner, L.A., Aldrich, J.A. and Dickinson, R.E., "Storm Water
Management Model, Version 4, User\'s Manual: Extran Addendum",
EPA/600/3-88/001b (NTIS PB88-236658/AS), Environmental Protection
Agency, Athens, GA, 1992.

Sanks, R.L. et al., *Pumping Station Design*, Second Edition,
Butterworth, London, UK, 1998.

Shubinski, R.P., McCarty, J.C., and Lindorf, M.R., \"Computer Simulation
of Estuarial Networks\", *Journal of the Hydraulics Division, ASCE*,
vol. 91, HY5 , September 1965.

Singh, V.P., ed., *Computer Models of Watershed Hydrology*, Water
Resources Publications, Highlands Ranch, CO, 1995.

Sjőberg, A., "Sewer Network Models DAGVL-A and DAGVL-DIFF" in *Urban
Stormwater Hydraulics and Hydrology*, B.C. Yen, ed., Water Resources
Publications, Highlands Ranch, CO, 1982.

Smith, G.D., *Numerical Solution of Partial Differential Equations:
Finite Difference Methods*, Second Edition, Clarendon Press, Oxford,
1978.

Strelkoff, T. "One-Dimensional Equations of Open-Channel Flow", *Journal
of the Hydraulics Division*, ASCE, Vol. 95, No. HY3, May, 1969.

Swamee, P.K., "Critical depth equations for irrigation canals", *Journal
of Irrigation and Drainage Engineering*, Vol. 119, No. 2, pp. 400--409,
1993.

Toro, E.F., *Shock-Capturing Methods for Free-Surface Shallow Flows*,
John Wiley, Chichester, U.K., 2001.

U.S. Environmental Protection Agency (US EPA), "SWMM 5 Applications
Manual", EPA/600/R-09/000, National Risk Management Research Laboratory,
Office of Research and Development, Cincinnati, OH, 2009.

U.S. Environmental Protection Agency (US EPA), "SWMM 5 User's Manual",
EPA/600/R-05/040, National Risk Management Research Laboratory, Office
of Research and Development, Cincinnati, OH, 2010.

Villemonte, J.R., "Submerged-weir Discharge Studies", *Engineering News
Record*, p. 866, Dec 25, 1947.

Yen, B.C., "Hydraulics of Sewer Systems", Chapter 6 in *Stormwater
Collection Systems Design Handbook*, L.W. Mays editor, McGraw-Hill, NY,
2001.
