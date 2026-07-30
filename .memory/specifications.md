---
project: SWMM6_2
type: specification
version: 0.1
updated: 2026-05-23
---

## Goal
Build and distribute the OpenSWMM GUI (SWMMVis) as installable packages on Mac (Intel), Ubuntu, and Windows 11.

## Requirements
- Produce a working .dmg on macOS (Intel x86_64)
- Produce a working .tar.gz / AppImage on Ubuntu
- Produce a working NSIS .exe installer on Windows 11
- GUI must link against openswmm.engine (fetched automatically by CMake)

## Constraints
- Mac host is Intel (x86_64), not Apple Silicon — Darwin preset targets x86_64 which is compatible
- Qt 6.7.x required (qtcharts module must be included); Qt 5 not acceptable
- vcpkg manages C++ deps (GDAL, HDF5, OpenSSL, sundials, nanoflann) — first build compiles them from source (~45 min)
- Windows build requires MSVC (Visual Studio 2022), not MinGW
- Ubuntu and Windows builds run in VMs

## Stack
- C++20: primary language for both engine and GUI
- Qt 6.7: GUI framework (Widgets, OpenGL, Charts, QML, Network, Concurrent, SVG)
- CMake 3.21+: build system with platform presets (Darwin / Linux / Windows)
- vcpkg: C++ dependency manager (pinned baseline d5ec528)
- aqtinstall: pip-based Qt downloader (no Qt account needed)
- Ninja: fast build runner invoked by CMake
- CPack: packaging (dmg / tar.gz+AppImage / NSIS)

## Repos
- GUI: https://github.com/HydroCouple/openswmm.gui  branch: swmm6_gui
- Engine: https://github.com/HydroCouple/openswmm.engine  branch: develop (auto-fetched by GUI CMake)

## Out of Scope
- Python bindings (Cython) for the engine
- Code signing / notarization (macOS) for now
- Qt account or commercial Qt license
- Building engine as a standalone artifact (GUI CMake handles it)
