# CMake configuration for openswmm.legacy.engine (SWMM 5.x solver bindings)
#
# Originally: openswmm/solver/CMakeLists.txt

# Add Cython target
add_cython_target(_solver _solver.pyx CXX PY3)

# Add library
add_library(_solver MODULE ${_solver})

# Link to SWMM and Python libraries
target_link_libraries(
    _solver
    OpenSWMMCore::openswmmcore
    OpenSWMMCore::openswmmcore_solver
)

# Specify that this is a Python extension module
python_extension_module(_solver)

if(APPLE)
    set(INSTALL_LIB_ROOT "@loader_path;@rpath;@loader_path/../../../../..;${SOLVER_BUILD_DIR}")
    set(BUILD_LIB_ROOT "@loader_path;@rpath;${SOLVER_BUILD_DIR}")
elseif(UNIX)
    set(INSTALL_LIB_ROOT "$ORIGIN;$ORIGIN/../../../../..;${SOLVER_BUILD_DIR}")
    set(BUILD_LIB_ROOT "$ORIGIN;${SOLVER_BUILD_DIR}")
endif()

# Set up rpath
set_target_properties(_solver
    PROPERTIES
        BUILD_RPATH "${BUILD_LIB_ROOT}"
        INSTALL_RPATH "${INSTALL_LIB_ROOT}"
        BUILD_WITH_INSTALL_RPATH True
)

# Install the target into the new namespace
install(TARGETS _solver LIBRARY DESTINATION openswmm/legacy/engine)

# Include directories
target_include_directories(
    _solver
    PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)
