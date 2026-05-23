# CMake configuration for openswmm.engine python output library
#
# Created by: Caleb Buahin (EPA/ORD/CESER/WID)
# Created on: 2024-11-19

add_cython_target(_output _output.pyx LANGUAGE CXX PY3)

# Add Cython target
add_library(_output MODULE ${_output})

# Add library
target_link_libraries(
    _output
    openswmm::common
    openswmm::legacy::output
)

# Specify that this is a Python extension module
python_extension_module(_output)

# RPATH — same strategy as _solver: wheel, conda/venv env lib, and
# CMAKE_INSTALL_RPATH_USE_LINK_PATH covers the pre-built/editable case.
if(APPLE)
    set(INSTALL_LIB_ROOT "@loader_path;@loader_path/../../../lib")
elseif(UNIX)
    set(INSTALL_LIB_ROOT "$ORIGIN;$ORIGIN/../../../lib")
endif()

if(NOT WIN32)
    set_target_properties(_output PROPERTIES
        INSTALL_RPATH "${INSTALL_LIB_ROOT}"
    )
endif()

# Install the target
install(TARGETS _output LIBRARY DESTINATION openswmm/output)

# Include directories
target_include_directories(
    _output
    PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)