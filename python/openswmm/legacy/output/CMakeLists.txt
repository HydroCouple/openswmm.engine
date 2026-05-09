# CMake configuration for openswmm.legacy.output (SWMM 5.x output reader)
#
# Originally: openswmm/output/CMakeLists.txt

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

set_target_properties(_output
    PROPERTIES
        CXX_VISIBILITY_PRESET default
        C_VISIBILITY_PRESET default
        VISIBILITY_INLINES_HIDDEN OFF
)

# Ensure PyInit__output is exported when global flags include
# -fvisibility=hidden.  -fno-lto prevents LTO from stripping it.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(_output PRIVATE -fvisibility=default -fno-lto)
    target_link_options(_output PRIVATE -fno-lto)
endif()

# libopenswmm.legacy.output.6.dylib is installed alongside this extension
# in openswmm/legacy/output/ so @loader_path / $ORIGIN is sufficient.
if(APPLE)
    set(INSTALL_LIB_ROOT "@loader_path")
    set(BUILD_LIB_ROOT "@loader_path")
elseif(UNIX)
    set(INSTALL_LIB_ROOT "$ORIGIN")
    set(BUILD_LIB_ROOT "$ORIGIN")
endif()

set_target_properties(_output
    PROPERTIES
        BUILD_RPATH "${BUILD_LIB_ROOT}"
        INSTALL_RPATH "${INSTALL_LIB_ROOT}"
        BUILD_WITH_INSTALL_RPATH True
)

# Install the target into the new namespace
install(TARGETS _output LIBRARY DESTINATION openswmm/legacy/output)

# Include directories
target_include_directories(
    _output
    PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${NUMPY_INCLUDE_DIR}
)
