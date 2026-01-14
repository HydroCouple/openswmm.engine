## Stormwater Management Model (SWMM) Open Source Initiative

Stormwater Management Model (SWMM) SWMM is a dynamic hydrology-hydraulic water quality simulation model. 
It is used for single event or long-term (continuous) simulation of runoff quantity and quality from primarily 
urban areas. SWMM was originally developed by the U.S. Environmental Protection Agency (EPA) is being advanced
as an open source project by the community to ensure its long-term sustainability.

Recent advancements to the SWMM computational engine include a modernized codebase with improved modularity and computational
efficiency, a well-documented application programming interface (API) for easier integration with third-party applications,
and Python bindings for enhanced accessibility and usability by the broader community.

New process formulations including a spatially explicit overland flow solver, a groundwater transport model, 
and a new low impact development (LID) module have also been added and are being tested.

The SWMM source code is written in the C/C++ Programming Language. 

## Build Status
[![Unit Testing](https://github.com/HydroCouple/OpenSWMMCore/actions/workflows/unit_testing.yml/badge.svg?branch=bug_fixes)](https://github.com/HydroCouple/OpenSWMMCore/actions/workflows/unit_testing.yml?query=branch%3Abug_fixes)
[![Build and Regression Testing](https://github.com/HydroCouple/OpenSWMMCore/actions/workflows/regression_testing.yml/badge.svg?branch=bug_fixes)](https://github.com/HydroCouple/OpenSWMMCore/actions/workflows/regression_testing.yml?query=branch%3Abug_fixes)
[![Documentation](https://github.com/HydroCouple/OpenSWMMCore/actions/workflows/documentation.yml/badge.svg?branch=bug_fixes)](https://github.com/HydroCouple/OpenSWMMCore/actions/workflows/documentation.yml?query=branch%3Abug_fixes)
[![Deployment](https://github.com/HydroCouple/OpenSWMMCore/actions/workflows/deployment.yml/badge.svg?branch=bug_fixes)](https://github.com/HydroCouple/OpenSWMMCore/actions/workflows/deployment.yml?query=branch%3Abug_fixes)
[![Issues](https://img.shields.io/github/issues/HydroCouple/OpenSWMMCore)](https://github.com/HydroCouple/OpenSWMMCore/issues)

## Python Binding
[![PyPi](https://img.shields.io/pypi/v/openswmm.svg)](https://pypi.org/project/openswmm)
[![PythonVersion](https://img.shields.io/pypi/pyversions/openswmm.svg)](https://pypi.org/project/openswmm)
[![Wheel](https://img.shields.io/pypi/wheel/openswmm.svg)](https://pypi.org/project/openswmm)
[![Downloads](https://pepy.tech/badge/openswmm)](https://pepy.tech/project/openswmm)
[![Downloads](https://pepy.tech/badge/openswmm/month)](https://pepy.tech/project/openswmm)
[![Downloads](https://pepy.tech/badge/openswmm/week)](https://pepy.tech/project/openswmm)

## Introduction

This repository hosts a community-driven, open source initiative to continue development of the SWMM computational engine. 
The project aims to preserve and advance the rich legacy of SWMM by developing high-quality, QA/QC'd, and peer-reviewed 
code while the EPA Office of Research and Development (ORD) is being dissolved. The goal is to build an active community 
around the codebase so it can be sustainably maintained and improved, and to continue collaborating with the agency 
toward future official releases when feasible. This community is actively working with organizations such as
ASCE's Environmental and Water Resources Institute (EWRI) and Water Environment Federation (WEF) to ensure the
long-term sustainability of the SWMM codebase.

SWMM is a dynamic hydrology-hydraulic water quality simulation model. It is used for single event or long-term 
(continuous) simulation of runoff quantity and quality from primarily urban areas. SWMM source code is written 
in the C Programming Language and released in the Public Domain.

## Build Instructions

The 'src' folder of this repository contains the C source code for
the current version of Storm Water Management Model's computational
engine. The code can be compiled into both a shared
object library and a command line executable. Under Windows, the 
library file (openswmmcore.dll) is used to power SWMM's graphical user
interface.

Also included is a python interface for the SWMM computational engine and output 
post-processing application programming interfaces located in the python folder.

### Computational Engine

The 'CMakeLists.txt' file is a script used by CMake (https://cmake.org/)
to build the SWMM binaries. CMake is a cross-platform build tool
that generates platform native build systems for many compilers. To
check if the required version is installed on your system, enter from 
a console window and check that the version is 3.15 or higher.

```bash
cmake --version
```

To build the SWMM engine and related libraries:

1. Open a console window and navigate to the directory where this
   Readme file resides (which should have 'src' as a sub-directory
   underneath it).

2. Then the following CMake commands to build the binaries. Where
   <platform> can either be Windows, Linux, or Darwin. The confurations
   for the platforms can be modified in the CMakePresets.json file.

``` bash
cmake ---preset=<platform>
cmake --build build --target package
```

### Python Bindings

Python bindings for the SWMM API have been developed. _**These bindings are still under development and testing**_. 
The python bindings can be built and installed locally using the following command.

```bash
cd python
python -m pip install -r requirements.txt
python -m pip install . 
```
Users may also build python wheels for installation or distribution.  Example usage of python bindings can be 
found below. More extensive documentation will be provided once cleared.

```python

from openswmmcore import solver
from openswmmcore.solver import Solver 
from openswmmcore.output import Output

# Alternative 1 to run SWMM

with Solver(inp_file="input_file.inp") as swmm_solver:
   
   # Open swmm file and starts the simulation
   swmm_solver.start()

   # Set initialization parameters e.g., time step stride, start date, end da.te etc.
   swmm_solver.time_stride = 600 

   for elapsed_time, current_datetime in swmm_solver:

      # Get and set attributes per timestep
      print(current_datetime)

      swmm_solver.set_value(
         object_type=solver.SWMMObjects.RAIN_GAGE,
         property_type=solver.SWMMRainGageProperties.GAGE_RAINFALL,
         index="RG1",
         value=3.6
      )

# Alternative 2 to run SWMM
swmm_solver = Solver(inp_file="input_file.inp")

# Open and start the simulation
swmm_solver.initialize()

for elapsed_time, current_datetime in swmm_solver:
   # Get and set attributes per timestep
   print(current_datetime)


swmm_solver.finalize()
# or
# swmm_solver.end()
# swmm_solver.report()
# swmm_solver.close()

# Alternative 3 to run SWMM
swmm_solver = Solver(inp_file="input_file.inp")
swmm_solver.execute()

# To read output file

swmm_output = Output(output_file='output_file.out')

# Dict[datetime, float]
link_timeseries = swmm_output.get_link_timeseries(
   element_index="C1",
   attribute=output.LinkAttribute.FLOW_RATE,
)

```

## Unit and Regression Testing

Unit tests and regression tests have been developed for both the natively compiled SWMM computational engine and output toolkit as well as their respective python bindings. Unit tests for the natively compiled toolkits use the Boost 1.67.0 library and can be compiled by adding DBUILD_TESTS=ON flag during the cmake build phase as shown below:

```bash
ctest --test-dir .  -DBUILD_TESTS=ON --config Debug --output-on-failure
```

Unit testing on the python bindings may be executed using the following command after installation.

```bash
cd python\tests
pytest .
```

Regression tests are executed using the python bindings using the pytest and pytest-regressions extension using the following commands.

```bash
cd ci
pytest --data-dir <path-to-regression-testing-files> --atol <absolute-tolerance> --rtol <relative-tolerance> --benchmark-compare --benchmark-json=PATH
```

## Find Out More

A live web version of the SWMM documentation of the API and user manuals can be found on the [SWMM GitHub Pages website](https://hydrocouple.github.io/OpenSWMMCore). Note that this documentation is experimental and maintained by the community; it has yet to go through formal agency quality assurance review. The project welcomes contributions, review, and collaboration from the community and from agency partners toward future official releases.
