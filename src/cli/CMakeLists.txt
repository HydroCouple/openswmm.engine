#
# src/cli/CMakeLists.txt
#
# OpenSWMM Engine 6.0 command-line executable.
# Links against the new engine library (openswmm_engine).
#
# The legacy CLI (EPA SWMM 5.x) is now at src/legacy/cli/.
#

add_executable(openswmm
    main.cpp
)

target_link_libraries(openswmm
    PRIVATE
        openswmm_engine
        openswmm::common
)

target_include_directories(openswmm
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include/openswmm/engine
)

set_target_properties(openswmm
    PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        OUTPUT_NAME "openswmm"
)

if(APPLE)
    # @loader_path      — co-located (single-dir installs)
    # @loader_path/../lib — standard prefix layout (bin/ → ../lib/)
    set(LIB_ROOT "@loader_path;@loader_path/../lib")
elseif(UNIX)
    set(LIB_ROOT "$ORIGIN;$ORIGIN/../lib")
endif()

if(NOT WIN32)
    set_target_properties(openswmm PROPERTIES
        INSTALL_RPATH "${LIB_ROOT}"
    )
endif()

# Install the executable plus the full closure of its runtime dependencies
# (SUNDIALS, HDF5, libomp, sqlite3, GLEW/GLFW/ANGLE etc.) into bin/ so the
# packaged tarball is self-contained on download. The bundling step probes
# the already-installed executable so it follows the same RPATH the loader
# uses at runtime — see openswmm_bundle_runtime_deps() at top-level for why
# we can't use install(TARGETS ... RUNTIME_DEPENDENCY_SET ...) here.
install(TARGETS openswmm
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
openswmm_bundle_runtime_deps(openswmm ${CMAKE_INSTALL_BINDIR})

add_custom_command(TARGET openswmm POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
        $<TARGET_FILE:openswmm>
        ${CMAKE_BINARY_DIR}/bin/$<CONFIG>/$<TARGET_FILE_NAME:openswmm>
)

# Stage runtime DLLs / dylibs next to the build-tree copy of the exe so
# `./build/bin/<config>/openswmm` runs without LD/DYLD/PATH overrides.
if(WIN32)
    add_custom_command(TARGET openswmm POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:openswmm>
            $<TARGET_RUNTIME_DLLS:openswmm_engine>
            $<TARGET_RUNTIME_DLLS:openswmm_legacy_engine>
            $<TARGET_FILE_DIR:openswmm>
        COMMAND_EXPAND_LISTS
    )
endif()
