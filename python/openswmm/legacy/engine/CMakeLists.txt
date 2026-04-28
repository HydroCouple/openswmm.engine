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
    openswmm::common
    openswmm::legacy::engine
)

# Specify that this is a Python extension module
python_extension_module(_solver)

set_target_properties(_solver
    PROPERTIES
        CXX_VISIBILITY_PRESET default
        C_VISIBILITY_PRESET default
        VISIBILITY_INLINES_HIDDEN OFF
)

# Ensure PyInit__solver is exported when global flags include
# -fvisibility=hidden.  -fno-lto prevents LTO from stripping it.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(_solver PRIVATE -fvisibility=default -fno-lto)
    target_link_options(_solver PRIVATE -fno-lto)
endif()

# libopenswmm.legacy.engine.6.dylib is installed alongside this extension
# in openswmm/legacy/engine/ so @loader_path / $ORIGIN (same directory) is
# sufficient.  Stale build-tree paths and the circular @rpath are removed.
if(APPLE)
    set(INSTALL_LIB_ROOT "@loader_path")
    set(BUILD_LIB_ROOT "@loader_path")
elseif(UNIX)
    set(INSTALL_LIB_ROOT "$ORIGIN")
    set(BUILD_LIB_ROOT "$ORIGIN")
endif()

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
