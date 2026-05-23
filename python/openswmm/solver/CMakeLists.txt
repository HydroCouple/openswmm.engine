# CMake configuration for openswmm.engine python solver library
#
# Created by: Caleb Buahin (EPA/ORD/CESER/WID)
# Created on: 2024-11-19
#

# Add Cython target
add_cython_target(_solver _solver.pyx CXX PY3)

# Add library
add_library(_solver MODULE ${_solver})

# Link to SWMM and Python libraries
target_link_libraries(
    _solver
    openswmm::common
    openswmm::legacy::engine
)

# Specify that this is a Python extension module
python_extension_module(_solver)

# RPATH — three layouts covered:
#   @loader_path / $ORIGIN          — wheel (dylib bundled next to .so by delocate/auditwheel)
#   ../../../lib (3 levels up)      — conda/venv env lib/  from site-packages/openswmm/engine/
# CMAKE_INSTALL_RPATH_USE_LINK_PATH appends the linked library's install dir
# (prefix/lib for a pre-built engine, build dir for an editable build).
if(APPLE)
    set(INSTALL_LIB_ROOT "@loader_path;@loader_path/../../../lib")
elseif(UNIX)
    set(INSTALL_LIB_ROOT "$ORIGIN;$ORIGIN/../../../lib")
endif()

if(NOT WIN32)
    set_target_properties(_solver PROPERTIES
        INSTALL_RPATH "${INSTALL_LIB_ROOT}"
    )
endif()

# Install the target
install(TARGETS _solver LIBRARY DESTINATION openswmm/solver)



# Include directories
target_include_directories(
    _solver
    PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

