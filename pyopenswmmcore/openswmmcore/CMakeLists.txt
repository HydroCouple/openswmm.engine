# Description: CMake configuration for openswmmcore python library
# Created by: Caleb Buahin (EPA/ORD/CESER/WID)
# Created on: 2024-11-19

# Add Cython targets for openswmmcore
add_cython_target(_openswmmcore _openswmmcore.pyx CXX PY3)

# Create Python extension modules
add_library(_openswmmcore MODULE ${_openswmmcore})

set_target_properties(_openswmmcore
    PROPERTIES
        VERSION ${LIBRARY_VERSION}
        SOVERSION ${LIBRARY_SOVERSION}
)

python_extension_module(_openswmmcore)

# Install the openswmmcore module
install(TARGETS _openswmmcore LIBRARY DESTINATION openswmmcore)

# Include the current source directory
target_include_directories(
    _openswmmcore
    PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# Add subdirectories
add_subdirectory(solver)
add_subdirectory(output)