# Changelog

All notable changes to the OpenSWMM project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

---

## [6.0.0-alpha.1] - 2026-03-25

### Overview
OpenSWMM 6.0.0 represents a major architectural transformation from EPA SWMM 5.x, introducing a modern C++20 codebase while preserving the proven EPA SWMM 5.2.4 engine functionality. This alpha release focuses on establishing the foundation for a modular, extensible stormwater modeling platform.

### Added
- **Modern C++20 Architecture**: Complete refactoring of the SWMM engine to C++20 with improved memory management, type safety, and performance
- **Plugin System**: New plugin SDK for extensible I/O, solvers, and analysis tools
- **Cross-Platform Build System**: CMake-based build with vcpkg dependency management for Windows, macOS, and Linux
- **Python Bindings**: Native Python wrapper using Cython for seamless integration with scientific Python ecosystem
- **Modular Design**: Separation into distinct libraries:
  - `openswmm_legacy_engine`: Preserved EPA SWMM 5.2.4 solver (unmodified)
  - `openswmm_legacy_output`: EPA SWMM 5.x binary output reader
  - `openswmm_engine`: New C++20 refactored engine (in development)
  - `openswmm_plugin_sdk`: Header-only plugin development kit
- **Enhanced CLI**: Modern command-line interface with improved error reporting
- **Comprehensive Testing**: Google Test framework for unit and regression testing
- **Performance Benchmarks**: Google Benchmark integration for performance tracking
- **API Documentation**: Doxygen-based documentation generation

### Changed
- Project renamed from EPA SWMM to OpenSWMM to reflect open-source community development model
- Version numbering reset to 6.0.0 to indicate major architectural changes
- Build system migrated from Visual Studio project files to CMake
- Code organization restructured into `src/legacy/`, `src/engine/`, `src/cli/`, and `src/plugin_sdk/`

### Fixed
- All bug fixes from EPA SWMM 5.2.4 are included in the legacy engine (see EPA SWMM 5.2.4 release notes)
- Cross-platform compatibility issues with file I/O (`off_t` vs `__int64`)
- CMake configuration for proper library aliasing and installation
- Python package build and deployment issues
- RPATH handling for macOS dynamic libraries

### Technical Debt & Known Issues
- Legacy engine remains as-is for validation; gradual migration to C++20 engine in progress
- Some Python bindings require adjustment for updated API signatures
- Documentation migration from CHM to web-based format ongoing
- Additional work needed on Windows MSVC compilation

### Development
- Established continuous integration pipeline
- Added comprehensive unit test suite for new C++ components
- Created migration strategy documentation (`docs/MASTER_IMPLEMENTATION_PLAN.md`)

---

## Historical EPA SWMM Releases Summary

### [5.2.4] - 2023-07-15

**Key Engine Updates:**
- Fixed pollutant mass balance reporting discrepancies between surface runoff and wet weather inflow
- Corrected water flux calculations for Bio-Retention, Permeable Pavement, and Infiltration Trench LIDs
- Fixed Street cross-section geometry with depressed gutters
- Improved curb inlet hydraulic calculations
- Limited conduit evaporation/seepage to available volume under dynamic wave routing
- Fixed Inertial Damping and Variable Time Step default values

**Key GUI Updates:**
- Fixed map panning flicker
- Eliminated Access Violation errors in Storage Unit polygon editing
- Improved subcatchment/storage node centroid positioning
- Fixed scrollbar range errors for long simulations

### [5.2.3] - 2023-02-12

**Key Updates:**
- Fixed double counting of moisture volume in green roof drainage mat
- Fixed backdrop image loading and Storage Unit centroid updates in GUI

### [5.2.2] - 2022-12-01

**Key Engine Updates:**
- Eliminated excessive run times when external time series exceeded simulation duration
- Fixed math expression parser for exponent operations (a*b^c)
- Added validation for Modified Baskethandle and Round-Rectangular dimensions
- Enhanced Street Flow Summary with additional performance statistics
- Corrected storage unit evaporation/exfiltration reporting
- Changed default threads for dynamic wave routing to 1

**Key GUI Updates:**
- Storage units can now be drawn as polygons
- Added grayscale backdrop image option
- Fixed Citrix compatibility issues with progress indicators

### [5.2.1] - 2022-08-01

**Key Engine Updates:**
- Made Normal Flow Limited feature optional for dynamic wave routing
- Fixed refactoring bug causing slow control rule execution
- Corrected Egg-shaped cross-section geometry
- Improved pollutant concentration handling at dry nodes
- Fixed F_OFF definition for non-MS compilers
- Reduced excessive warning messages

**Key GUI Updates:**
- Added task bar progress indicator during simulation
- Improved Welcome Screen behavior
- Enhanced DXF export functionality

### [5.2.0] - 2021-11-01

**Major Features Added:**
- **Street Runoff Capture**: New Street cross-sections, Inlet objects, and HEC-22 capture analysis
- **Type 5 Variable Speed Pump**: Affinity law-based pump modeling
- **Storage Curve Shapes**: Pre-defined analytical shapes (cylinders, paraboloids, cones, pyramids)
- **Enhanced Control Rules**: Named variables, math expressions, and new condition properties
- **Rain Barrel Improvements**: Added covered/uncovered parameter
- **API Expansion**: Numerous new functions added to SWMM 5 API
- **Large File Support**: Binary output files >2 GB now supported

**Key GUI Updates:**
- Added Welcome page
- Map results theming capability
- Street and Inlet editors
- Improved culvert code selection dialog
- Enhanced keyboard shortcuts

### [5.1.015] - 2020-05-01
- **Mixed Infiltration Methods**: Different infiltration methods can be used within a project
- Added variable routing time step frequency table to Status Report
- Fixed average statistics reporting for delayed reporting start dates
- GUI improvements for 4K monitors and mouse wheel zooming

### [5.1.014] - 2020-03-01
- Fixed rainfall time series bug affecting RDII Unit Hydrographs
- Fixed LID-related crashes and mass balance issues
- Corrected street sweeping period handling
- Fixed conduit evaporation/seepage adjustments under dynamic wave routing

### [5.1.013] - 2018-05-10
- **Monthly Adjustments**: Added monthly patterns for depression storage, surface roughness, and conductivity
- **LID Pervious Treatment**: LIDs can now treat pervious area runoff
- **Enhanced Underdrains**: Auto-open/close depths and control curves for LID drains
- **LID Pollutant Removal**: Removal percentages for underdrain flow
- **Preissmann Slot Option**: Alternative surcharge method for dynamic wave routing
- **Pressurized Storage**: Storage units can pressurize up to specified depth
- **Weir Curves**: Variable discharge coefficients with head
- **Reporting Options**: Average values over reporting intervals

### [5.1.012] - 2017-03-14
- Fixed direct.h header inclusion for non-Windows platforms
- Corrected rain garden LID surface infiltration limits
- Fixed Engels flow equation for side flow weirs
- Corrected culvert Slope Correction Factor for mitered inlets
- Fixed Modified Basket Handle cross-section depth formula

### [5.1.011] - 2016-08-22
- **Event-Based Routing**: Restrict detailed routing to pre-defined event periods
- Enhanced API with swmm_getError() and swmm_getWarnings()
- Updated NCDC precipitation file format support
- Fixed LID underdrain offset limitations
- Improved regulator link offset handling

### [5.1.010] - 2015-08-05
- **Modified Green-Ampt**: New infiltration option without moisture redistribution
- **Roadway Weirs**: FHWA HDS-5 method for roadway overtopping
- **Rule Enhancements**: Time-based conditions (link open/closed duration)
- Added unsaturated hydraulic conductivity to groundwater equations
- **Parallelization**: OpenMP support for dynamic wave routing speedup
- Added daily potential evapotranspiration (PET) as output variable

### [5.1.009] - 2015-04-30
- Fixed simulations longer than 68 years
- Corrected LID runon for fully-occupied subcatchments
- Fixed control rule variable comparison parsing
- Improved dry node definition for quality routing

### [5.1.008] - 2015-04-02
- **Groundwater Enhancements**: Monthly conductivity adjustments, custom flow equations
- **Rooftop Disconnection LID**: New LID practice for explicit roof runoff modeling
- **Permeable Pavement Soil Layer**: Optional sand filter/bedding layer
- **Outfall Irrigation**: Outfalls can discharge onto subcatchments
- **Parallelization**: Multi-threaded dynamic wave routing (major performance improvement)
- **Minimum Time Step Control**: User-definable minimum variable time step
- **LID Runon Distribution**: Improved distribution across non-LID areas
- Major refactoring of pollutant washoff routines

### [5.1.007] - 2014-09-15
- **Climate Adjustments**: Monthly adjustments for temperature, evaporation, and rainfall
- **GHCN-Daily Support**: Reading new GHCN climate data files
- **Deep Groundwater Flow**: Custom equations for seepage to deeper aquifer
- **Weir Surcharge Control**: Parameter to enable/disable orifice equation surcharging
- Revised Modified Horton recovery formula
- Fixed Green-Ampt initial cumulative infiltration
- Corrected Bio-Retention Cell and Permeable Pavement infiltration with zero storage layer

### [5.1.006] - 2014-05-19
- Fixed LID report timing and off-by-one errors
- Corrected soil water availability for LID evaporation
- Fixed permeable pavement infiltration equation parenthesis error
- Fixed pollutant concentration units conversion for direct precipitation

### [5.1.005] - 2014-04-23
- Fixed hot start file reading for hydraulic results

### [5.1.004] - 2014-04-14
- Added Ignore RDII analysis option support
- Fixed numerical precision preferences bug

### [5.1.003] - 2014-04-08
- **Aquifer Upper Zone Evap Pattern**: Monthly adjustment for aquifer evaporation
- Fixed binary RDII file read/write
- Fixed non-US regional settings compatibility

### [5.1.002] - 2014-03-31
- Fixed hot start file reading with new format
- Corrected surface area calculation for surcharge algorithm
- Fixed memory leaks in grid editor

### [5.1.001] - 2014-03-24
- **Modified Horton Infiltration**: New cumulative-based state variable method
- **Binary RDII Files**: Reduced storage space for interface files
- **Green Roofs**: New LID category with Drainage Mat layer
- **Rain Gardens**: New LID category for simplified configuration
- **User Groundwater Equations**: Custom outflow equations
- **Channel Evaporation**: Water loss from open channels
- **Conduit Seepage**: Uniform seepage along conduit surfaces
- **Storage Seepage**: Renamed from infiltration for consistency
- **Improved Link Capacity**: Redefined as fraction of full area for conduits
- **Flow Volume Tracking**: Replaced Froude number as view variable
- **Link Pollutant Load Summary**: New summary table
- Enhanced convergence reporting for dynamic wave routing

### [5.0.022] - 2011-09-01
- Fixed LID drain problems and Green-Ampt infiltration issues
- Corrected time series external inflow
- Fixed rule-based pump control
- Improved reporting for negative inflows

### [5.0.021] - 2011-05-24
- Fixed ponded depth continuation errors
- Corrected Hot Start file reporting
- Fixed Steady Flow routing issues

### [5.0.020] - 2010-12-15
- Fixed quality routing instability in long simulations
- Corrected storage node initial depth application
- Fixed snow melt accounting errors

### [5.0.019] - 2010-11-18
- Added Hot Start file capability
- Fixed subcatchment runoff coefficient calculations
- Corrected LID performance reporting

### [5.0.018] - 2010-09-15
- Fixed Green-Ampt infiltration recovery
- Corrected rainfall file reading
- Fixed pump rating curve interpolation

### [5.0.017] - 2010-07-20
- Fixed groundwater flow calculations
- Corrected Curve Number infiltration
- Fixed control rule evaluation timing

### [5.0.016] - 2010-06-01
- Fixed Force Main equations
- Corrected pollutant buildup/washoff
- Fixed storage curve extrapolation

### [5.0.015] - 2010-03-25
- Fixed orifice flow calculations
- Corrected weir flow for low heads
- Fixed culvert inlet control

### [5.0.014] - 2009-12-22
- **Low Impact Development (LID)**: First implementation of LID controls
- Added Bio-Retention Cells, Rain Barrels, Infiltration Trenches, Porous Pavement
- Fixed dynamic wave routing stability issues

### [5.0.013] - 2009-10-01
- Fixed control rule precedence
- Corrected pollutant decay rates
- Fixed reporting time step alignment

### [5.0.012] - 2009-07-15
- Fixed variable time step calculations
- Corrected storage unit geometry
- Fixed RDII hydrograph generation

### [5.0.011] - 2009-04-30
- Fixed surcharge algorithm convergence
- Corrected pump startup/shutoff logic
- Fixed transect geometry processing

### [5.0.010] - 2009-02-10
- Fixed rainfall intensity calculations
- Corrected conduit flow reversals
- Fixed snowmelt computations

### [5.0.009] - 2008-11-12
- Fixed control rule evaluation order
- Corrected treatment function calculations
- Fixed outfall boundary conditions

### [5.0.008] - 2008-08-20
- Fixed subcatchment routing lag time
- Corrected conduit flow classification
- Fixed pollutant mass balance

### [5.0.007] - 2008-06-05
- Fixed Kinematic Wave routing instabilities
- Corrected detention storage calculations
- Fixed groundwater lateral flow

### [5.0.006] - 2008-03-18
- Fixed time series interpolation
- Corrected cross-section geometry for irregular shapes
- Fixed snowpack redistribution

### [5.0.005] - 2007-12-10
- Fixed control rule actions
- Corrected pump curves with shutoff head
- Fixed status report formatting

### [5.0.004] - 2005-06-15
- Fixed Curve Number initial abstraction
- Corrected street sweeping calculations
- Fixed water quality treatment

### [5.0.003] - 2004-11-10
- Fixed rounded cross-section stability
- Added error for excessive binary file size (>2.1 GB)
- Corrected RDII units for metric
- Fixed weir crest control

### [5.0.002] - 2004-11-01
- Modified Picard method for dynamic wave routing

### [5.0.001] - 2004-10-29
- **Initial Release**: First official release of EPA SWMM 5
- Complete rewrite from SWMM 4 with modern architecture
- Dynamic wave flow routing with variable time stepping
- Integrated runoff and routing simulation
- Enhanced control rules and real-time controls
- Comprehensive GUI for Windows

---

## Version Numbering

- **6.x.x**: OpenSWMM - Modern C++20 architecture with plugin system
- **5.2.x**: EPA SWMM - Final releases with street/inlet modeling
- **5.1.x**: EPA SWMM - Added LID enhancements, parallelization, and numerous features
- **5.0.x**: EPA SWMM - Initial SWMM 5 releases with LID introduction

[6.0.0-alpha.1]: https://github.com/hydrocouple/OpenSWMMCore/releases/tag/v6.0.0-alpha.1
[5.2.4]: https://www.epa.gov/water-research/storm-water-management-model-swmm
[5.2.3]: https://www.epa.gov/water-research/storm-water-management-model-swmm
[5.2.2]: https://www.epa.gov/water-research/storm-water-management-model-swmm
[5.2.1]: https://www.epa.gov/water-research/storm-water-management-model-swmm
[5.2.0]: https://www.epa.gov/water-research/storm-water-management-model-swmm
[5.1.015]: https://www.epa.gov/water-research/storm-water-management-model-swmm
[5.1.014]: https://www.epa.gov/water-research/storm-water-management-model-swmm
[5.1.013]: https://www.epa.gov/water-research/storm-water-management-model-swmm

