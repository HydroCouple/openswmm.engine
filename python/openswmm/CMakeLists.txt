# ============================================================================
# openswmm package — top-level Cython extension + subpackage delegation
#
# Builds the root _openswmm stub (if ever re-added) and delegates to:
#   engine/    — new 6.x bindings
#   legacy/    — legacy 5.x solver + output reader
#
# Shared libraries installed via CMake are placed next to each extension:
#   openswmm/engine/libopenswmm.engine.*
#   openswmm/legacy/engine/libopenswmm.legacy.engine.*
#   openswmm/legacy/output/libopenswmm.legacy.output.*
# ============================================================================

add_subdirectory(engine)
add_subdirectory(legacy)

# ---------------------------------------------------------------------------
# Install shared libraries alongside the Cython extensions so that
# @loader_path / $ORIGIN RPATH entries resolve at runtime.
#
# In pre-built mode the targets are IMPORTED; cmake forbids install(TARGETS)
# on imported targets, so we resolve the path via IMPORTED_LOCATION instead.
# ---------------------------------------------------------------------------
if(NOT OPENSWMM_USE_PREBUILT_ENGINE)
    # On Windows, gather the transitive runtime DLL closure of every engine
    # library into a single RUNTIME_DEPENDENCY_SET so it gets bundled into
    # the wheel alongside the .pyd files.  Without this, `pip install .` on
    # Windows produces a wheel where the engine DLL is present but its
    # dependencies (OpenMP vcomp140.dll, vcpkg-installed runtime libs, etc.)
    # are absent — Python's loader then fails with
    #     "DLL load failed while importing _solver: The specified module
    #      could not be found."
    # The cibuildwheel path uses delvewheel, which performs the same bundling
    # at wheel-repair time; this CMake-level rule mirrors that behaviour for
    # the `pip install .` (no delvewheel) path.
    set(_openswmm_runtime_dep_set "")
    if(WIN32)
        set(_openswmm_runtime_dep_set
            RUNTIME_DEPENDENCY_SET openswmm_runtime_deps)
    endif()

    install(TARGETS openswmm_engine
        ${_openswmm_runtime_dep_set}
        LIBRARY DESTINATION openswmm/engine
        ARCHIVE DESTINATION openswmm/engine
        RUNTIME DESTINATION openswmm/engine
    )
    install(TARGETS openswmm_legacy_engine
        ${_openswmm_runtime_dep_set}
        LIBRARY DESTINATION openswmm/legacy/engine
        ARCHIVE DESTINATION openswmm/legacy/engine
        RUNTIME DESTINATION openswmm/legacy/engine
    )
    install(TARGETS openswmm_legacy_output
        ${_openswmm_runtime_dep_set}
        LIBRARY DESTINATION openswmm/legacy/output
        ARCHIVE DESTINATION openswmm/legacy/output
        RUNTIME DESTINATION openswmm/legacy/output
    )

    if(WIN32)
        install(RUNTIME_DEPENDENCY_SET openswmm_runtime_deps
            DESTINATION openswmm/engine
            # Skip OS-supplied DLLs that ship with every Windows installation
            # (Python loader finds them in System32 anyway, and shipping them
            # into a wheel would conflict with the loaded copies).
            PRE_EXCLUDE_REGEXES
                "api-ms-.*\\.dll"
                "ext-ms-.*\\.dll"
                "[Kk]ernel32\\.dll"
                "[Nn]tdll\\.dll"
                "[Mm]svcrt\\.dll"
                "ucrtbase\\.dll"
                "vcruntime[0-9]+\\.dll"
                "msvcp[0-9]+\\.dll"
                "concrt[0-9]+\\.dll"
                "[Pp]ython[0-9]+\\.dll"
            POST_EXCLUDE_REGEXES
                ".*[Ww]indows[/\\\\][Ss]ystem32.*"
                ".*[Ww]indows[/\\\\][Ss]ys[Ww][Oo][Ww]64.*"
        )
    endif()
else()
    foreach(_entry
        "OpenSWMMEngine::openswmm_engine|openswmm/engine"
        "OpenSWMMEngine::openswmm_legacy_engine|openswmm/legacy/engine"
        "OpenSWMMEngine::openswmm_legacy_output|openswmm/legacy/output"
    )
        string(REPLACE "|" ";" _pair "${_entry}")
        list(GET _pair 0 _tgt)
        list(GET _pair 1 _dest)

        get_target_property(_loc "${_tgt}" IMPORTED_LOCATION)
        if(NOT _loc)
            get_target_property(_loc "${_tgt}" IMPORTED_LOCATION_RELEASE)
        endif()
        if(_loc)
            install(FILES "${_loc}" DESTINATION "${_dest}")
        else()
            message(WARNING "IMPORTED_LOCATION not found for ${_tgt} — dylib missing from wheel")
        endif()

        if(WIN32)
            get_target_property(_imp "${_tgt}" IMPORTED_IMPLIB)
            if(NOT _imp)
                get_target_property(_imp "${_tgt}" IMPORTED_IMPLIB_RELEASE)
            endif()
            if(_imp)
                install(FILES "${_imp}" DESTINATION "${_dest}")
            endif()
        endif()
    endforeach()
endif()
