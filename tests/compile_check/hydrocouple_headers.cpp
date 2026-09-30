// SPDX-License-Identifier: Apache-2.0
//
// Copyright 2026 Caleb Buahin
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/**
 * @file hydrocouple_headers.cpp
 * @brief D1 (program plan §C.1) — compile-check of every HydroCouple interface
 *        header with the engine's toolchain, and the pin's ABI as a
 *        static_assert.
 *
 * Built only with OPENSWMM_WITH_HYDROCOUPLE=ON (the headers do not exist
 * otherwise). If this file compiles, two things are true:
 *
 *  1. every public HydroCouple header is self-contained and compiles as C++20
 *     under this compiler — a later `src/couplers/` failure is then not "the
 *     headers", and
 *  2. the headers found are the ABI the pin stands for. The build resolves a
 *     local install before the pinned fetch, so this assert is the only thing
 *     that makes the pin binding on a developer machine: without it, a
 *     sibling checkout at a different ABI compiles silently.
 */

#include <hydrocouple.h>
#include <hydrocoupledistributed.h>
#include <hydrocouplehelpers.h>
#include <hydrocouplespatial.h>
#include <hydrocouplespatialwkb.h>
#include <hydrocouplespatiotemporal.h>
#include <hydrocoupletemporal.h>

#ifndef OPENSWMM_HYDROCOUPLE_EXPECTED_ABI
#error "OPENSWMM_HYDROCOUPLE_EXPECTED_ABI must be defined by the build (cmake/FetchHydroCouple.cmake)"
#endif

static_assert(HydroCouple::HYDROCOUPLE_ABI_VERSION == OPENSWMM_HYDROCOUPLE_EXPECTED_ABI,
              "The HydroCouple headers found by this build declare a different "
              "HYDROCOUPLE_ABI_VERSION than the pinned commit "
              "(OPENSWMM_HYDROCOUPLE_EXPECTED_ABI). A local install or sibling "
              "checkout is shadowing the pin. Either point CMAKE_PREFIX_PATH at an "
              "install of the pinned commit, disable the local copy with "
              "-DCMAKE_DISABLE_FIND_PACKAGE_HydroCouple=ON so the pin is fetched, or "
              "bump OPENSWMM_HYDROCOUPLE_GIT_TAG and OPENSWMM_HYDROCOUPLE_EXPECTED_ABI "
              "together.");

// Referenced so the object is not empty on toolchains that warn about it.
int openswmm_hydrocouple_header_check() { return HydroCouple::HYDROCOUPLE_ABI_VERSION; }
