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
    install(TARGETS openswmm_engine
        LIBRARY DESTINATION openswmm/engine
        ARCHIVE DESTINATION openswmm/engine
        RUNTIME DESTINATION openswmm/engine
    )
    install(TARGETS openswmm_legacy_engine
        LIBRARY DESTINATION openswmm/legacy/engine
        ARCHIVE DESTINATION openswmm/legacy/engine
        RUNTIME DESTINATION openswmm/legacy/engine
    )
    install(TARGETS openswmm_legacy_output
        LIBRARY DESTINATION openswmm/legacy/output
        ARCHIVE DESTINATION openswmm/legacy/output
        RUNTIME DESTINATION openswmm/legacy/output
    )
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
