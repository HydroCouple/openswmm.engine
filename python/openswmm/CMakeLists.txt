# Description: CMake configuration for openswmmcore python library
# Created by: Caleb Buahin (EPA/ORD/CESER/WID)
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

# Install the openswmmcore module
install(TARGETS _openswmm LIBRARY DESTINATION openswmm)

# Include the current source directory
target_include_directories(
    _openswmm
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

# Add subdirectories
add_subdirectory(solver)
add_subdirectory(output)