#----------------------------------------------------------------
# Generated CMake target import file for configuration "Release".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "OpenSWMMEngine::openswmm_legacy_engine" for configuration "Release"
set_property(TARGET OpenSWMMEngine::openswmm_legacy_engine APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
set_target_properties(OpenSWMMEngine::openswmm_legacy_engine PROPERTIES
  IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/lib/libopenswmm.legacy.engine.so.6.0.0"
  IMPORTED_SONAME_RELEASE "libopenswmm.legacy.engine.so.6"
  )

list(APPEND _cmake_import_check_targets OpenSWMMEngine::openswmm_legacy_engine )
list(APPEND _cmake_import_check_files_for_OpenSWMMEngine::openswmm_legacy_engine "${_IMPORT_PREFIX}/lib/libopenswmm.legacy.engine.so.6.0.0" )

# Import target "OpenSWMMEngine::openswmm_legacy_output" for configuration "Release"
set_property(TARGET OpenSWMMEngine::openswmm_legacy_output APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
set_target_properties(OpenSWMMEngine::openswmm_legacy_output PROPERTIES
  IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/lib/libopenswmm.legacy.output.so.6.0.0"
  IMPORTED_SONAME_RELEASE "libopenswmm.legacy.output.so.6"
  )

list(APPEND _cmake_import_check_targets OpenSWMMEngine::openswmm_legacy_output )
list(APPEND _cmake_import_check_files_for_OpenSWMMEngine::openswmm_legacy_output "${_IMPORT_PREFIX}/lib/libopenswmm.legacy.output.so.6.0.0" )

# Import target "OpenSWMMEngine::openswmm_engine" for configuration "Release"
set_property(TARGET OpenSWMMEngine::openswmm_engine APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
set_target_properties(OpenSWMMEngine::openswmm_engine PROPERTIES
  IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/lib/libopenswmm.engine.so.6.0.0"
  IMPORTED_SONAME_RELEASE "libopenswmm.engine.so.6"
  )

list(APPEND _cmake_import_check_targets OpenSWMMEngine::openswmm_engine )
list(APPEND _cmake_import_check_files_for_OpenSWMMEngine::openswmm_engine "${_IMPORT_PREFIX}/lib/libopenswmm.engine.so.6.0.0" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
