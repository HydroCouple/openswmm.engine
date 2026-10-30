# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Caleb Buahin
# Configure-only fixture: exercise the real identity helper without optional engine dependencies.
cmake_minimum_required(VERSION 3.21)
project(OpenSWMMEngineAbiFixture LANGUAGES CXX)
include(GNUInstallDirs)
if(NOT ENGINE_ROOT)
    message(FATAL_ERROR "ENGINE_ROOT is required")
endif()
include("${ENGINE_ROOT}/cmake/EngineAbi.cmake")
openswmm_configure_engine_abi()
