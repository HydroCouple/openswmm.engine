#
# BundleRuntimeDeps.cmake.in
#
# Configured per-target by openswmm_bundle_runtime_deps() at top-level
# CMakeLists.txt. Runs at install time as part of the per-directory
# cmake_install.cmake script. Probes the already-installed executable
# (so RPATH rewrites from install_name_tool are in effect) and copies
# every shared library it depends on — except OS-provided ones filtered
# by the PRE/POST_EXCLUDE_REGEXES — next to the exe.
#
# @-substituted placeholders: see configure_file() call at the helper.
#

cmake_policy(PUSH)
cmake_policy(SET CMP0011 NEW)  # POLICY scope, regular variable scope rules
# NB: regexes contain '\.' and '\+' which CMake's quoted-string parser would
#     otherwise flag under CMP0010 (bad variable reference syntax). Using a
#     template file keeps the literal backslashes out of the install(CODE)
#     interpolation path.

set(_exe "${CMAKE_INSTALL_PREFIX}/bin/openswmm-legacy")
if(NOT EXISTS "${_exe}")
    message(WARNING "openswmm_bundle_runtime_deps: ${_exe} does not exist yet — install ordering bug?")
    cmake_policy(POP)
    return()
endif()

message(STATUS "Bundling runtime deps for openswmm_legacy into bin")

# Why the set + ${...} dance:
#   The @-substituted value is wrapped as [==[a;b;c]==] (bracket-quoted to
#   preserve literal backslashes in the regexes through configure_file).
#   Passing [==[a;b;c]==] DIRECTLY to file(GET_RUNTIME_DEPENDENCIES ...
#   PRE_EXCLUDE_REGEXES ...) passes it as ONE giant single regex that
#   matches nothing — verified by direct testing of file(GET_RUNTIME_DEPENDENCIES).
#   Capturing into a regular CMake variable first, then dereferencing with
#   ${...}, triggers CMake's list-aware expansion that splits on `;` into
#   separate arguments. Each regex is then applied individually as intended.
set(_pre_excludes  [==[api-ms-.*;ext-ms-.*;^([Aa][Cc][Ll][Uu][Ii]|[Aa][Dd][Vv][Aa][Pp][Ii]32|[Bb][Cc][Rr][Yy][Pp][Tt]|[Bb][Cc][Rr][Yy][Pp][Tt][Pp][Rr][Ii][Mm][Ii][Tt][Ii][Vv][Ee][Ss]|[Cc][Oo][Mm][Bb][Aa][Ss][Ee]|[Cc][Oo][Mm][Cc][Tt][Ll]32|[Cc][Oo][Mm][Dd][Ll][Gg]32|[Cc][Rr][Yy][Pp][Tt]32|[Cc][Rr][Yy][Pp][Tt][Bb][Aa][Ss][Ee]|[Cc][Rr][Yy][Pp][Tt][Ss][Pp]|[Dd][Bb][Gg][Cc][Oo][Rr][Ee]|[Dd][Bb][Gg][Hh][Ee][Ll][Pp]|[Dd][Nn][Ss][Aa][Pp][Ii]|[Dd][Ww][Mm][Aa][Pp][Ii]|[Dd][Ww][Rr][Ii][Tt][Ee]|[Ff][Ww][Pp][Uu][Cc][Ll][Nn][Tt]|[Gg][Dd][Ii]32|[Gg][Dd][Ii]32[Ff][Uu][Ll][Ll]|[Ii][Mm][Aa][Gg][Ee][Hh][Ll][Pp]|[Ii][Mm][Mm]32|[Ii][Pp][Hh][Ll][Pp][Aa][Pp][Ii]|[Kk][Ee][Rr][Nn][Ee][Ll]32|[Kk][Ee][Rr][Nn][Ee][Ll][Bb][Aa][Ss][Ee]|[Mm][Pp][Rr]|[Mm][Ss][Aa][Ss][Nn]1|[Mm][Ss][Cc][Oo][Rr][Ee][Ee]|[Mm][Ss][Ii]|[Mm][Ss][Vv][Cc][Pp]_[Ww][Ii][Nn]|[Mm][Ss][Vv][Cc][Rr][Tt]|[Mm][Ss][Ww][Ss][Oo][Cc][Kk]|[Nn][Ee][Tt][Aa][Pp][Ii]32|[Nn][Ee][Tt][Uu][Tt][Ii][Ll][Ss]|[Nn][Oo][Rr][Mm][Aa][Ll][Ii][Zz]|[Nn][Tt][Dd][Ll][Ll]|[Nn][Tt][Mm][Aa][Rr][Tt][Aa]|[Oo][Ll][Ee]32|[Oo][Ll][Ee][Aa][Cc][Cc]|[Oo][Ll][Ee][Aa][Uu][Tt]32|[Pp][Oo][Ww][Rr][Pp][Rr][Oo][Ff]|[Pp][Rr][Oo][Ff][Aa][Pp][Ii]|[Pp][Rr][Oo][Pp][Ss][Yy][Ss]|[Pp][Ss][Aa][Pp][Ii]|[Rr][Pp][Cc][Rr][Tt]4|[Ss][Aa][Mm][Cc][Ll][Ii]|[Ss][Ee][Cc][Hh][Oo][Ss][Tt]|[Ss][Ee][Cc][Uu][Rr]32|[Ss][Ee][Tt][Uu][Pp][Aa][Pp][Ii]|[Ss][Ff][Cc]|[Ss][Ff][Cc]_[Oo][Ss]|[Ss][Hh][Cc][Oo][Rr][Ee]|[Ss][Hh][Ee][Ll][Ll]32|[Ss][Hh][Ll][Ww][Aa][Pp][Ii]|[Ss][Rr][Vv][Cc][Ll][Ii]|[Uu][Ss][Ee][Rr]32|[Uu][Ss][Ee][Rr][Ee][Nn][Vv]|[Uu][Ss][Pp]10|[Uu][Xx][Tt][Hh][Ee][Mm][Ee]|[Vv][Ee][Rr][Ss][Ii][Oo][Nn]|[Ww][Ee][Rr]|[Ww][Ii][Nn]32[Uu]|[Ww][Ii][Nn][Ii][Nn][Ee][Tt]|[Ww][Ii][Nn][Mm][Mm]|[Ww][Ii][Nn][Nn][Ss][Ii]|[Ww][Ii][Nn][Tt][Rr][Uu][Ss][Tt]|[Ww][Kk][Ss][Cc][Ll][Ii]|[Ww][Ll][Dd][Aa][Pp]32|[Ww][Ss]2_32|[Ww][Ss][Oo][Cc][Kk]32|[Ww][Tt][Ss][Aa][Pp][Ii]32|[Zz][Ll][Ii][Bb][Ww][Aa][Pp][Ii])\.[Dd][Ll][Ll];^/usr/lib/.*;^/lib/.*;^/lib64/.*;^/System/Library/.*;^/usr/lib/system/.*;^libc\..*;^libm\..*;^libdl\..*;^libpthread\..*;^librt\..*;^libutil\..*;^libresolv\..*;^libnsl\..*;^libstdc\+\+\..*;^libgcc_s\..*;^libGL\..*;^libGLX\..*;^libEGL\..*;^libGLdispatch\..*;^opengl32\.dll;^glu32\.dll]==])
set(_post_excludes [==[.*[/\\][Ss]ystem32[/\\].*\.dll;.*[/\\][Ss]ys[Ww][Oo][Ww]64[/\\].*\.dll]==])
# Extra search paths captured at configure time — typically vcpkg's
# installed/<triplet>/bin where SUNDIALS, HDF5, etc. live. The list is
# `;`-separated; ${_extra_dirs} dereference splits it into separate args.
set(_extra_dirs    "")

file(GET_RUNTIME_DEPENDENCIES
    RESOLVED_DEPENDENCIES_VAR   _resolved
    UNRESOLVED_DEPENDENCIES_VAR _unresolved
    EXECUTABLES "${_exe}"
    DIRECTORIES
        "${CMAKE_INSTALL_PREFIX}/bin"
        "${CMAKE_INSTALL_PREFIX}/lib"
        ${_extra_dirs}
    PRE_EXCLUDE_REGEXES  ${_pre_excludes}
    POST_EXCLUDE_REGEXES ${_post_excludes}
)

file(REAL_PATH "${CMAKE_INSTALL_PREFIX}" _prefix_real)
set(_bundled_basename_to_oldpath "")  # map of basename → original absolute path
foreach(_dep IN LISTS _resolved)
    file(REAL_PATH "${_dep}" _dep_real)
    # Skip anything that already lives under the install prefix — those
    # files are installed by their own install(TARGETS …) rule and copying
    # again would just duplicate them.
    string(FIND "${_dep_real}" "${_prefix_real}/" _in_prefix)
    if(_in_prefix EQUAL 0)
        continue()
    endif()

    if(_dep MATCHES "\\.framework/")
        # Copy the whole .framework directory, not just the inner binary.
        string(REGEX REPLACE "(.*\\.framework)/.*" "\\1" _fwk "${_dep}")
        file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/bin"
            TYPE DIRECTORY FILES "${_fwk}" USE_SOURCE_PERMISSIONS)
        message(STATUS "  bundled framework: ${_fwk}")
    else()
        file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/bin"
            TYPE SHARED_LIBRARY FILES "${_dep}" FOLLOW_SYMLINK_CHAIN)
        message(STATUS "  bundled: ${_dep}")
        get_filename_component(_bn "${_dep}" NAME)
        list(APPEND _bundled_basename_to_oldpath "${_bn}=${_dep}")
    endif()
endforeach()

foreach(_dep IN LISTS _unresolved)
    message(WARNING "  unresolved runtime dep: ${_dep}")
endforeach()

# ---- macOS install_name rewrite -----------------------------------------
# On macOS, bundling a dylib doesn't help if consumers still reference its
# ORIGINAL absolute path (LC_LOAD_DYLIB). Each bundled lib gets its install
# name flipped to @rpath/<basename>, and every dylib / executable in the
# install tree gets its references to the original path rewritten the same
# way. The dyld loader then resolves @rpath via each binary's LC_RPATH set,
# which we've configured to include @loader_path / @loader_path/../bin /
# @loader_path/../lib (see project-wide RPATH config in src/engine).
if(APPLE)
    find_program(_install_name_tool install_name_tool REQUIRED)

    # 1. Set each bundled lib's own install_name to @rpath/<basename> so any
    #    binary that links it after rewriting picks the @rpath entry.
    foreach(_entry IN LISTS _bundled_basename_to_oldpath)
        string(REGEX REPLACE "^([^=]+)=.*$" "\\1" _bn "${_entry}")
        set(_installed "${CMAKE_INSTALL_PREFIX}/bin/${_bn}")
        if(EXISTS "${_installed}")
            execute_process(
                COMMAND "${_install_name_tool}" -id "@rpath/${_bn}" "${_installed}"
                RESULT_VARIABLE _rc
            )
            if(NOT _rc EQUAL 0)
                message(WARNING "    install_name_tool -id failed on ${_installed}")
            endif()
        endif()
    endforeach()

    # 2. Walk every Mach-O in the install tree and rewrite LC_LOAD_DYLIB
    #    references that still point at a bundled lib's original abs path.
    file(GLOB_RECURSE _machos
        "${CMAKE_INSTALL_PREFIX}/bin/*"
        "${CMAKE_INSTALL_PREFIX}/lib/*"
    )
    foreach(_macho IN LISTS _machos)
        if(IS_SYMLINK "${_macho}")
            continue()
        endif()
        if(_macho MATCHES "\\.(a|h|hpp|cmake)$")
            continue()
        endif()
        foreach(_entry IN LISTS _bundled_basename_to_oldpath)
            string(REGEX REPLACE "^([^=]+)=(.*)$" "\\1" _bn "${_entry}")
            string(REGEX REPLACE "^([^=]+)=(.*)$" "\\2" _oldpath "${_entry}")
            execute_process(
                COMMAND "${_install_name_tool}" -change "${_oldpath}" "@rpath/${_bn}" "${_macho}"
                RESULT_VARIABLE _rc
                ERROR_QUIET
            )
        endforeach()
    endforeach()
endif()

cmake_policy(POP)
