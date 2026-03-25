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
    set(LIB_ROOT "@loader_path;@rpath;@loader_path/../lib;@rpath/../lib")
elseif(UNIX)
    set(LIB_ROOT "$ORIGIN;$ORIGIN/../lib")
endif()

set_target_properties(openswmm
    PROPERTIES
        INSTALL_RPATH "${LIB_ROOT}"
        BUILD_RPATH "${LIB_ROOT}"
        BUILD_WITH_INSTALL_RPATH TRUE
)

install(TARGETS openswmm DESTINATION bin)

add_custom_command(TARGET openswmm POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
        $<TARGET_FILE:openswmm>
        ${CMAKE_BINARY_DIR}/bin/$<CONFIG>/$<TARGET_FILE_NAME:openswmm>
)
