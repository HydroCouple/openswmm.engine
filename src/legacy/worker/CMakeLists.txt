#
# src/legacy/worker/CMakeLists.txt
#
# Legacy SWMM engine worker process for parallel simulation execution.
# Spawned by openswmm.gui's SimulationRunner for concurrent legacy simulations.
#
# Uses plain C++ (no Qt dependency) for maximum portability.
#

add_executable(openswmm_legacy_worker
    main.cpp
)

target_link_libraries(openswmm_legacy_worker
    PRIVATE
        openswmm_legacy_engine
)

target_include_directories(openswmm_legacy_worker
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}
)

set_target_properties(openswmm_legacy_worker
    PROPERTIES
        OUTPUT_NAME "openswmm-legacy-worker"
)

if(APPLE)
    set(LIB_ROOT "@loader_path;@rpath;@loader_path/../lib;@rpath/../lib")
    set_target_properties(openswmm_legacy_worker PROPERTIES INSTALL_RPATH "${LIB_ROOT}")
elseif(UNIX)
    set(LIB_ROOT "$ORIGIN;$ORIGIN/../lib")
    set_target_properties(openswmm_legacy_worker PROPERTIES INSTALL_RPATH "${LIB_ROOT}")
endif()

install(TARGETS openswmm_legacy_worker DESTINATION bin)

add_custom_command(TARGET openswmm_legacy_worker POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
        $<TARGET_FILE:openswmm_legacy_worker>
        ${CMAKE_BINARY_DIR}/bin/$<CONFIG>/$<TARGET_FILE_NAME:openswmm_legacy_worker>
)
