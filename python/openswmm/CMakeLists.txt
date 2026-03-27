# Description: CMake configuration for openswmmcore python library
# Created by: Caleb Buahin 
# Created on: 2024-11-19

# Add Cython targets for openswmmcore
add_cython_target(_openswmm _openswmm.pyx CXX PY3)

# Create Python extension modules
add_library(_openswmm MODULE ${_openswmm})

set_target_properties(_openswmm
    PROPERTIES
        VERSION ${LIBRARY_VERSION}
        SOVERSION ${LIBRARY_SOVERSION}
)

python_extension_module(_openswmm)

set_target_properties(_openswmm
    PROPERTIES
        CXX_VISIBILITY_PRESET default
        C_VISIBILITY_PRESET default
        VISIBILITY_INLINES_HIDDEN OFF
)

# Keep PyInit__openswmm exported with global hidden visibility flags.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(_openswmm PRIVATE -fvisibility=default)
endif()

# Install the openswmmcore module
install(TARGETS _openswmm LIBRARY DESTINATION openswmm)

# Include the current source directory
target_include_directories(
    _openswmm
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

# Add subdirectories
add_subdirectory(legacy)
add_subdirectory(engine)

# ---------------------------------------------------------------------------
# Install shared libraries alongside the Cython extensions so that
# @loader_path / $ORIGIN RPATH entries resolve at runtime.
# ---------------------------------------------------------------------------
install(TARGETS openswmm_engine
    LIBRARY DESTINATION openswmm/engine
    RUNTIME DESTINATION openswmm/engine   # Windows DLLs
)
install(TARGETS openswmm_legacy_engine
    LIBRARY DESTINATION openswmm/legacy/engine
    RUNTIME DESTINATION openswmm/legacy/engine
)
install(TARGETS openswmm_legacy_output
    LIBRARY DESTINATION openswmm/legacy/output
    RUNTIME DESTINATION openswmm/legacy/output
)