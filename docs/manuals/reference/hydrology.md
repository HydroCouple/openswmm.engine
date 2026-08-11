@page hydrology_reference_manual OpenSWMM Hydrology Reference Manual

<center>
OpenSWMM Hydrology Reference Manual
=====================================
</center>

<center>

See @ref authors for the full list of authors and contributors.

</center>

## DISCLAIMER

This software is provided on an "as is" basis and the user assumes responsibility for its use. Although a reasonable effort has been made to assure that the results obtained are correct, the authors are not responsible and assume no liability whatsoever for any results or any use made of the results obtained from these programs, nor for any damages or litigation that result from the use of these programs for any purpose.

## ACKNOWLEDGEMENTS

This reference manual was originally prepared by **Lewis A. Rossman** and **Wayne C. Huber**, whose foundational work on the SWMM hydrology model and its documentation is gratefully acknowledged.

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

## Acronyms and Abbreviations

AASHTO American Association of State Highway and Transportation
Officials

ADC areal depletion curve

ADT average daily traffic

AMC antecedent moisture condition

ASCE American Society of Civil Engineers

AWND average daily wind speed

BES Bureau of Environmental Services

BMP best management practice

BWF base wastewater flow

CDO Climate Data Online

CFS cubic feet per second

CMS cubic meters per second

CSO combined sewer overflow

DCIA directly connected impervious area

EIA effective impervious area

EPA Environmental Protection Agency

ET evapotranspiration

EVAP daily pan evaporation

FTP file transfer protocol

GHCN Global Historical Climatology Network

GIS geographic information system

GPM gallons per minute

GWI groundwater infiltration

HELP Hydrological Evaluation of Landfill Performance

HSPF Hydrologic Simulation Program - Fortran

IDF intensity- duration-frequency

ILLUDAS Illinois Urban Drainage Area Simulator

LID low impact development

LPS liters per second

MGD million gallons per day

MLD million liters per day

NCDC National Climatic Data Center

NOAA National Oceanic and Atmospheric Administration

NRCS Natural Resources Conservation Service

NWS National Weather Service

PRMS Precipitation-Runoff Modeling System

RDII rainfall dependent inflow and infiltration

SCF Snow Catch Factor

SCS Soil Conservation Service

SFWMD South Florida Water Management District

SPAW Soil-Plant-Air-Water

STORM Storage, Treatment, Overflow, Runoff Model

SWMM Storm Water Management Model

TMAX maximum daily temperature

TMIN minimum daily temperature

TVA Tennessee Valley Authority

UDFCD Urban Drainage and Flood Control District

UH unit hydrograph

USCS Unified Soil Classification System

USDA United States Department of Agriculture

USGS United States Geological Survey

WDMV 24-hour wind movement

WE water equivalent

## List of Figures

- **Figure 1-1** Elements of a typical urban drainage system.
- **Figure 1-2** SWMM's conceptual model of a stormwater drainage system.
- **Figure 1-3** Processes modeled by SWMM.
- **Figure 1-4** Block diagram of SWMM's state transition process.
- **Figure 1-5** Flow chart of SWMM's simulation procedure.
- **Figure 1-6** Interpolation of reported values from computed values.

- **Figure 2-1** Sinusoidal interpolation of hourly temperatures.

- **Figure 3-1** Idealized representation of a subcatchment.
- **Figure 3-2** Nonlinear reservoir model of a subcatchment.
- **Figure 3-3** Types of subareas within a subcatchment.
- **Figure 3-4** Idealized subcatchment partitioning for overland flow.
- **Figure 3-5** Re-routing of overland flow.
- **Figure 3-6** Fisk B catchment, Portland, Oregon.
- **Figure 3-7** Detailed view of two Fisk B subcatchments.
- **Figure 3-8** Idealized representation of a subcatchment.
- **Figure 3-9** Rectangular subcatchments for illustration of shape and width effects.
- **Figure 3-10** Subcatchment hydrographs for different shapes of Figure 3-9.
- **Figure 3-11** Irregular subcatchment shape for width calculations.
- **Figure 3-12** Runoff results for illustrative example.
- **Figure 3-13** SCS (NRCS) triangular unit hydrograph.

- **Figure 4-1** Physical properties for Woodburn silt loam, Benton County, Oregon.
- **Figure 4-2** The Horton infiltration curve.
- **Figure 4-3** Cumulative infiltration F as the area under the Horton curve.
- **Figure 4-4** Regeneration (recovery) of infiltration capacity during dry time steps.
- **Figure 4-5** Two-zone representation of the Green-Ampt infiltration model.
- **Figure 4-6** Illustration of infiltration capacity as function of cumulative infiltration for the Green-Ampt method.
- **Figure 4-7** Green-Ampt recovery parameters as functions of hydraulic conductivity.
- **Figure 4-8** Infiltration rates produced by different methods for a 2-inch rainfall event.

- **Figure 5-1** Definitional sketch of the two-zone groundwater model.
- **Figure 5-2** Heights used to compute lateral groundwater flow rate.
- **Figure 5-3** Relation between soil moisture limits and soil texture class.
- **Figure 5-4** SPAW's soil water characteristics calculator.
- **Figure 5-5** Measured hydraulic conductivity for three soils.
- **Figure 5-6** Fitting SWMM's hydraulic conductivity equation to a power law equation.
- **Figure 5-7** Definition sketch for Dupuit-Forcheimer seepage to an adjacent channel.
- **Figure 5-8** Definition sketch for Hooghoudt's method for flow to circular drains.
- **Figure 5-9** Surface runoff and groundwater flow for the illustrative groundwater example.

- **Figure 6-1** Typical gage catch deficiency correction.
- **Figure 6-2** Subcatchment partitionings used for snowmelt and runoff.
- **Figure 6-3** Seasonal variation of melt coefficients.
- **Figure 6-4** Typical areal depletion curve for natural area and temporary curve for new snow.
- **Figure 6-5** Effect of snow cover on areal depletion curves.
- **Figure 6-6** Schematic of liquid water routing through snow pack.
- **Figure 6-7** Continuous air temperature for illustrative snowmelt example.
- **Figure 6-8** Precipitation amounts for illustrative snowmelt example.
- **Figure 6-9** Snow pack depth for illustrative snowmelt example.
- **Figure 6-10** Runoff time series for illustrative snowmelt example.

- **Figure 7-1** Components of wet-weather wastewater flow.
- **Figure 7-2** Example of an RDII triangular unit hydrograph.
- **Figure 7-3** Application of a unit hydrograph to a storm event.
- **Figure 7-4** Use of three unit hydrographs to represent RDII.
- **Figure 7-5** Sewershed delineation.
- **Figure 7-6** Extracting RDII flow from a continuous flow monitor.
- **Figure 7-7** Fitting unit hydrographs to an RDII flow record.
- **Figure 7-8** Unit hydrographs used for the illustrative RDII example.
- **Figure 7-9** Time series of RDII flows for the illustrative RDII example.
- **Figure 7-10** Excerpt from the RDII interface file for the illustrative RDII example.
- **Figure 7-11** Emergent seasonal behavior of the exponential-decay initial abstraction model.

## List of Tables

- **Table 1-1** Development history of SWMM
- **Table 1-2** SWMM's modeling objects
- **Table 1-3** State variables used by SWMM
- **Table 1-4** Units of expression used by SWMM

- **Table 2-1** 15-minute precipitation data from NCDC Climate Data Online
- **Table 2-2** 15-minute precipitation data in NCDC FTP file format
- **Table 2-3** 15-minute precipitation data in comma-delimited format
- **Table 2-4** 15-minute precipitation data in space-delimited format
- **Table 2-5** 15-minute precipitation data in fixed-length format
- **Table 2-6** Record layout of Canadian HYL0 and HLY21 hourly precipitation files
- **Table 2-7** Record layout of Canadian FIF21 15-minute precipitation files
- **Table 2-8** Contents of an NCDC GHCN-Daily climate file
- **Table 2-9** Contents of an NCDC DS3200 climate file
- **Table 2-10** Layout of the ID portion of an NCDC DS3200 climate file record
- **Table 2-11** Layout of the data portion of an NCDC DS3200 climate file record
- **Table 2-12** Record layout of Canadian DLY daily climatologic files
- **Table 2-13** Example user-prepared climate file
- **Table 2-14** Time zones and standard meridians (degrees west longitude)
- **Table 2-15** Monthly climate adjustments available in the `[ADJUSTMENTS]` section

- **Table 3-1** Impervious area as a percentage of land use.
- **Table 3-2** Coefficients for Southerland's EIA equations.
- **Table 3-3** Data for example of effect of subcatchment width.
- **Table 3-4** Width computations for Portland example.
- **Table 3-5** Estimates of Manning's roughness coefficient for overland flow
- **Table 3-6** Sensitivity of runoff volume and peak flow to surface runoff parameters.
- **Table 3-7** Parameters used for illustrative runoff example
- **Table 3-8** Contents of a typical Routing Interface file

- **Table 4-1** Hydrologic soil group meanings
- **Table 4-2** Horton parameters for selected Georgia soils
- **Table 4-3** Horton parameters provided by Horton
- **Table 4-4** Values of f~∞~ for Hydrologic Soil Groups
- **Table 4-5** Rate of decay of infiltration capacity for different values of k~d~
- **Table 4-6** Representative values for f~0~
- **Table 4-7** Green-Ampt parameters for different soil classes
- **Table 4-8** Typical values of *θ*~dmax~ for various soil types.
- **Table 4-9** Runoff curve numbers for selected land uses
- **Table 4-10** Parameters used in example comparison of infiltration methods

- **Table 5-1** Volumetric moisture content at field capacity and wilting point
- **Table 5-2** Volumetric moisture content at field capacity and wilting point
- **Table 5-3** Average moisture limits and saturated hydraulic conductivity for different soil types
- **Table 5-4** Default properties of low-density soils used in the EPA HELP model
- **Table 5-5** Default properties of moderate-density soils used in the EPA HELP model
- **Table 5-6** Soil texture abbreviations
- **Table 5-7** Regression equations for soil moisture limits
- **Table 5-8** Regression estimates of soil moisture limits from the SPAW calculator*
- **Table 5-9** Estimated HCO for different soil types
- **Table 5-10** DET (in feet) for different soil types and land cover
- **Table 5-11** Parameters used in groundwater example

- **Table 6-1** Guidelines for level of service in snow and ice control
- **Table 6-2** Summary of snowmelt parameters (in US customary units)
- **Table 6-3** Typical areal depletion curve for natural areas
- **Table 6-4** Subcatchment and snow pack parameters for illustrative snowmelt example
- **Table 6-5** Daily temperatures for illustrative snowmelt example
- **Table 6-6** Periods of precipitation for illustrative snowmelt example

- **Table 7-1** Rainfall time series for the illustrative RDII example
- **Table 7-2** Parameters of the `[RDII_DECAY]` section
- **Table 7-3** Typical parameter ranges for the exponential-decay model

## Manual Contents

- @subpage hydrology_ref_ch1_overview — Chapter 1: Overview
- @subpage hydrology_ref_ch2_meteorology — Chapter 2: Meteorology
- @subpage hydrology_ref_ch3_surface_runoff — Chapter 3: Surface Runoff
- @subpage hydrology_ref_ch4_infiltration — Chapter 4: Infiltration
- @subpage hydrology_ref_ch5_groundwater — Chapter 5: Groundwater
- @subpage hydrology_ref_ch6_snowmelt — Chapter 6: Snowmelt
- @subpage hydrology_ref_ch7_rdii — Chapter 7: Rainfall Dependent Inflow and Infiltration
- @subpage hydrology_ref_glossary — Glossary
- @subpage hydrology_ref_references — References
