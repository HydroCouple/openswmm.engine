#
# CMakeLists.txt - CMake configuration file for swmm-solver/run
#
# Created:  July 11, 2019
# Modified: February 26, 2025
#
# Modified: Michael E. Tryby
#           US EPA ORD/NRMRL
#
#           Caleb Buahin
#           US EPA ORD/CESER
#

# Creates the EPANET command line executable
add_executable(openswmm
    main.c
)

target_include_directories(openswmm
    PUBLIC
        include
)

target_link_libraries(openswmm
    LINK_PUBLIC
        OpenSWMMCore::Solver
)

if(APPLE)
    set(LIB_ROOT "@loader_path;@rpath;@executable_path;@loader_path/../lib;@rpath/../lib;@executable_path/../lib")
elseif(UNIX)
    set(LIB_ROOT "$ORIGIN;$ORIGIN/../lib")
endif()

# Set up rpath for openswmm inside install package
set_target_properties(openswmm
    PROPERTIES
        INSTALL_RPATH "${LIB_ROOT}"
)

install(
    TARGETS
        openswmm
    DESTINATION
        bin
)


# copy openswmm to build tree for testing
add_custom_command(TARGET openswmm POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
        $<TARGET_FILE:openswmm>
        ${CMAKE_BINARY_DIR}/bin/$<CONFIG>/$<TARGET_FILE_NAME:openswmm>
)
