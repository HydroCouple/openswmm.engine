# SPDX-License-Identifier: Apache-2.0
#
# Copyright 2026 Caleb Buahin
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# HC-1 --- the HydroCouple interface headers, and nothing else.
#
# Program plan D-C6: the engine, `src/couplers/*` and SWMMVis link ONLY the
# header-only `HydroCouple` interfaces. No `HydroCoupleSDK`, anywhere, and
# that decision is recorded as not re-openable by a later mounting of the SDK.
# `HydroCouple` is an INTERFACE library exporting `HydroCouple::HydroCouple`,
# so "link" here costs an include directory and `cxx_std_20` — no objects, no
# runtime dependency, nothing to ship.
#
# THE PIN IS THE POINT. HydroCouple 2.0.0 renumbered `WorkflowStatus` after
# tagging, without a release: a build against a moving ref compiles fine today
# and silently changes the meaning of a status enum tomorrow. The program plan's
# instruction is to pin a commit, so `OPENSWMM_HYDROCOUPLE_GIT_TAG` defaults to
# a full SHA rather than a branch or a tag, and a tag is accepted only because
# a caller may deliberately want one.
#
# Resolution order:
#   1. An already-defined `HydroCouple::HydroCouple` target (superbuild).
#   2. `find_package(HydroCouple CONFIG)` --- a system/vcpkg install, or the
#      sibling checkout via `CMAKE_PREFIX_PATH` / `HydroCouple_ROOT`. Preferred
#      in development, because it uses the tree the developer already has.
#   3. `FetchContent` at the pinned commit.
#
# Step 3 is deliberately last: silently downloading a second copy of interfaces
# the developer already has checked out is how two versions of an ABI end up in
# one build.

include_guard(GLOBAL)

set(OPENSWMM_HYDROCOUPLE_GIT_REPOSITORY
    "https://github.com/hydrocouple/HydroCouple.git"
    CACHE STRING "Git remote for the HydroCouple interface headers")

# bef95cb --- "fix(abi): factory-component instances carry their ownership in
# the type" (2026-09-04). ABI 2. On origin/dev since 2026-09-29.
#
# The FULL 40-character SHA, not the abbreviation: an abbreviated SHA is only
# resolvable after the clone has fetched enough history to disambiguate it,
# and the full one cannot become ambiguous as the repository grows.
set(OPENSWMM_HYDROCOUPLE_GIT_TAG
    "bef95cb19310c6560e7f35158515bf27ec57ac29"
    CACHE STRING
    "Pinned HydroCouple commit. A SHA, not a branch: WorkflowStatus was \
renumbered without a release, so a moving ref is a silent ABI change.")

function(_openswmm_hydrocouple_report _how)
    if(NOT OPENSWMM_HYDROCOUPLE_QUIET)
        message(STATUS "HydroCouple interfaces: ${_how}")
    endif()
endfunction()

# --- 1. already satisfied -----------------------------------------------------
if(TARGET HydroCouple::HydroCouple)
    _openswmm_hydrocouple_report("using the target already defined in this build")
    return()
endif()

# --- 2. an installed or sibling copy ----------------------------------------
find_package(HydroCouple CONFIG QUIET)
if(HydroCouple_FOUND AND TARGET HydroCouple::HydroCouple)
    _openswmm_hydrocouple_report(
        "found ${HydroCouple_VERSION} via CONFIG at ${HydroCouple_DIR}")
    return()
endif()

# --- 3. the pinned fetch -----------------------------------------------------
#
# History worth keeping: on 2026-09-27 this path was verified BROKEN, because
# the pin was 6 commits ahead of origin/dev and unpushed — the ABI-2 work the
# engine targets existed only in the developer's checkout. FetchContent's own
# message for that is the unhelpful "Failed to checkout tag", which sends the
# reader looking for a typo. HydroCouple was pushed on 2026-09-29 and this
# path now resolves; see the verification in the HC-1 commit.
#
# What the pre-flight below can and cannot do. `git ls-remote` asks the remote
# about REFS, so it can prove a branch or tag name is wrong. It cannot ask
# about an arbitrary commit, so for the SHA pin actually in use an unpushed or
# mistyped commit still surfaces as FetchContent's opaque checkout error. If
# you see "Failed to checkout tag" with a SHA pin: check that the commit has
# been pushed to OPENSWMM_HYDROCOUPLE_GIT_REPOSITORY before anything else.
include(FetchContent)

# Ask the remote whether the pin is even reachable before cloning, so the
# failure is one message instead of a clone plus a checkout error.
find_package(Git QUIET)
if(GIT_EXECUTABLE)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} ls-remote --exit-code
                ${OPENSWMM_HYDROCOUPLE_GIT_REPOSITORY} ${OPENSWMM_HYDROCOUPLE_GIT_TAG}
        RESULT_VARIABLE _hc_lsremote_rc
        OUTPUT_QUIET ERROR_QUIET)
    # A SHA is not a ref, so ls-remote failing is expected for one and
    # meaningful for the other. Only a ref-shaped pin is checked here.
    if(NOT OPENSWMM_HYDROCOUPLE_GIT_TAG MATCHES "^[0-9a-fA-F]+$"
       AND NOT _hc_lsremote_rc EQUAL 0)
        message(FATAL_ERROR
            "OPENSWMM_HYDROCOUPLE_GIT_TAG '${OPENSWMM_HYDROCOUPLE_GIT_TAG}' is "
            "not a ref on ${OPENSWMM_HYDROCOUPLE_GIT_REPOSITORY}.")
    endif()
endif()

# HydroCouple's own CMakeLists offers tests; we want the interface target only.
set(HYDROCOUPLE_BUILD_TESTS OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    HydroCouple
    GIT_REPOSITORY ${OPENSWMM_HYDROCOUPLE_GIT_REPOSITORY}
    GIT_TAG        ${OPENSWMM_HYDROCOUPLE_GIT_TAG}
    GIT_SHALLOW    FALSE   # a SHA cannot be fetched shallowly from all remotes
    EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(HydroCouple)

if(NOT TARGET HydroCouple::HydroCouple)
    message(FATAL_ERROR
        "HydroCouple was fetched at ${OPENSWMM_HYDROCOUPLE_GIT_TAG} but did "
        "not define HydroCouple::HydroCouple. The interface target's name "
        "changed, or the pin points at a commit that predates it.")
endif()

_openswmm_hydrocouple_report("fetched at pinned ${OPENSWMM_HYDROCOUPLE_GIT_TAG}")
