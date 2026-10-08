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

# D1 (program plan §C.1) --- the D-C6 guard.
#
#   cmake -DROOTS=<dir;dir…> [-DFILES=<file;file…>] [-DEXCLUDE=<dir;dir…>]
#         -P cmake/CheckNoHydroCoupleSDK.cmake
#
# ROOTS are scanned recursively; FILES are scanned individually (the top-level
# CMakeLists.txt, which must not drag the whole repository into a recursive
# walk: tests/output alone is ~118k files, none of them target source).
#
# D-C6: the engine, `src/couplers/*` and SWMMVis link ONLY the header-only
# `HydroCouple` interfaces. No `HydroCoupleSDK`, anywhere — recorded as not
# re-openable by a later mounting of the SDK. A rule nobody checks is a rule
# that holds only until someone is in a hurry, so this makes it a test.
#
# What counts as a violation — the two ways a dependency actually arrives:
#   1. an `#include` of anything under `hydrocouplesdk/` (any case, either
#      quote style) in a C/C++ source or header;
#   2. `find_package(HydroCoupleSDK …)`, or `HydroCoupleSDK::` / `hydrocouplesdk`
#      named inside `target_link_libraries(…)`, in a CMake file.
#
# Known limitation, accepted: a CMake STRING literal that happens to contain
# `find_package(HydroCoupleSDK` or a `target_link_libraries(` span naming it
# is reported, because only comments are stripped. Stripping quoted strings
# would also hide a real, quoted link argument. Outside this script no such
# string exists; if one ever needs to, exclude that file explicitly.
#
# What does NOT count, deliberately: the WORD. The files that state this rule
# (this one, FetchHydroCouple.cmake, the option's docstring) necessarily name
# the SDK in prose, and a guard that fails on its own documentation would be
# switched off within a week. Matching the include and the link, not the
# name, is what lets the guard stay on.
#
# Written as a CMake script rather than Python so it runs on every CI leg
# with nothing installed beyond the build tool itself.

cmake_minimum_required(VERSION 3.21)

if(NOT DEFINED ROOTS AND NOT DEFINED FILES)
    message(FATAL_ERROR "CheckNoHydroCoupleSDK: give -DROOTS=<dir;…> and/or -DFILES=<file;…>")
endif()
set(_roots "")
foreach(_r IN LISTS ROOTS)
    if(NOT IS_DIRECTORY "${_r}")
        # A missing root is a configuration error, not a pass: a guard that
        # silently scans nothing is worse than no guard.
        message(FATAL_ERROR "CheckNoHydroCoupleSDK: root '${_r}' does not exist")
    endif()
    get_filename_component(_ra "${_r}" ABSOLUTE)
    list(APPEND _roots "${_ra}")
endforeach()

set(_excl "")
foreach(_e IN LISTS EXCLUDE)
    get_filename_component(_ea "${_e}" ABSOLUTE)
    list(APPEND _excl "${_ea}")
endforeach()

set(_sources "")
set(_cmake "")
foreach(_r IN LISTS _roots)
    file(GLOB_RECURSE _s LIST_DIRECTORIES FALSE
        "${_r}/*.h" "${_r}/*.hh" "${_r}/*.hpp" "${_r}/*.hxx"
        "${_r}/*.c" "${_r}/*.cc" "${_r}/*.cpp" "${_r}/*.cxx"
        "${_r}/*.inl" "${_r}/*.ipp")
    file(GLOB_RECURSE _c LIST_DIRECTORIES FALSE
        "${_r}/CMakeLists.txt" "${_r}/*.cmake" "${_r}/*.cmake.in")
    list(APPEND _sources ${_s})
    list(APPEND _cmake ${_c})
endforeach()
foreach(_f IN LISTS FILES)
    if(NOT EXISTS "${_f}")
        message(FATAL_ERROR "CheckNoHydroCoupleSDK: file '${_f}' does not exist")
    endif()
    get_filename_component(_fa "${_f}" ABSOLUTE)
    if(_fa MATCHES "(CMakeLists\\.txt|\\.cmake|\\.cmake\\.in)$")
        list(APPEND _cmake "${_fa}")
    else()
        list(APPEND _sources "${_fa}")
    endif()
endforeach()

function(_is_excluded _path _out)
    set(${_out} FALSE PARENT_SCOPE)
    foreach(_x IN LISTS _excl)
        string(FIND "${_path}" "${_x}/" _pos)
        if(_pos EQUAL 0)
            set(${_out} TRUE PARENT_SCOPE)
            return()
        endif()
    endforeach()
    # Build trees and fetched dependencies are not ours to police.
    if(_path MATCHES "/(build|_deps|\\.git)/")
        set(${_out} TRUE PARENT_SCOPE)
    endif()
    # …nor is this script. It is the one file that must SPELL the patterns it
    # forbids — its violation messages are CMake string literals naming
    # `find_package(HydroCoupleSDK …)` — and comment-stripping does not strip
    # strings. The first run over the real tree failed on exactly that: the
    # guard reported itself, twice. Found only because the real-tree case is
    # a gate of its own, not an afterthought to the fixtures.
    if(_path STREQUAL "${CMAKE_CURRENT_LIST_FILE}")
        set(${_out} TRUE PARENT_SCOPE)
    endif()
endfunction()

set(_violations "")
set(_scanned 0)

foreach(_f IN LISTS _sources)
    _is_excluded("${_f}" _skip)
    if(_skip)
        continue()
    endif()
    math(EXPR _scanned "${_scanned} + 1")
    file(STRINGS "${_f}" _hits
         REGEX "^[ \t]*#[ \t]*include[ \t]*[<\"][^>\"]*[Hh][Yy][Dd][Rr][Oo][Cc][Oo][Uu][Pp][Ll][Ee][Ss][Dd][Kk]/")
    foreach(_h IN LISTS _hits)
        string(STRIP "${_h}" _h)
        list(APPEND _violations "${_f}: ${_h}")
    endforeach()
endforeach()

foreach(_f IN LISTS _cmake)
    _is_excluded("${_f}" _skip)
    if(_skip)
        continue()
    endif()
    math(EXPR _scanned "${_scanned} + 1")
    file(READ "${_f}" _text)
    # Strip CMake line comments first: prose that names the SDK is allowed,
    # a call that uses it is not.
    string(REGEX REPLACE "#[^\n]*" "" _code "${_text}")
    string(TOLOWER "${_code}" _lc)
    if(_lc MATCHES "find_package[ \t\n]*\\([ \t\n]*hydrocouplesdk")
        list(APPEND _violations "${_f}: find_package(HydroCoupleSDK …)")
    endif()
    if(_lc MATCHES "target_link_libraries[ \t\n]*\\([^)]*hydrocouplesdk")
        list(APPEND _violations "${_f}: target_link_libraries(… HydroCoupleSDK …)")
    endif()
endforeach()

if(_violations)
    list(LENGTH _violations _n)
    list(JOIN _violations "\n  " _pretty)
    message(FATAL_ERROR
        "D-C6 violated: ${_n} HydroCoupleSDK dependenc(ies)\n  ${_pretty}\n"
        "The engine may use only the header-only HydroCouple interfaces "
        "(program plan decision D-C6). This is not re-openable.")
endif()

message(STATUS "D-C6 guard: ${_scanned} files scanned, no HydroCoupleSDK include or link")
