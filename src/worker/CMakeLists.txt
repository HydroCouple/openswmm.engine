#
# src/worker/CMakeLists.txt
#
# SWMMVis worker process for EPA SWMM 5.2.4 — the subprocess the GUI spawns
# to run a model on this engine, reporting progress as JSON lines on stdout.
# Mirrors openswmm.engine/src/legacy/worker/CMakeLists.txt (the 5.3.0 worker).
#
# Plain C++17, no Qt. Links the shared swmm5 built alongside; the GUI itself
# compiles src/solver/*.c + this main.cpp statically instead (it never runs
# this CMake), so keep the worker free of anything but swmm5.h.
#

add_executable(openswmm_legacy_worker_524
    main.cpp
    worker_progress.h
)

target_link_libraries(openswmm_legacy_worker_524
    PRIVATE
        swmm5
)

target_compile_features(openswmm_legacy_worker_524 PRIVATE cxx_std_17)

set_target_properties(openswmm_legacy_worker_524
    PROPERTIES
        OUTPUT_NAME "openswmm-legacy-worker-5.2.4"
)

if(NOT WIN32)
    if(APPLE)
        set(LIB_ROOT "@loader_path;@loader_path/../lib")
    else()
        set(LIB_ROOT "$ORIGIN;$ORIGIN/../lib")
    endif()
    set_target_properties(openswmm_legacy_worker_524 PROPERTIES INSTALL_RPATH "${LIB_ROOT}")
endif()

install(TARGETS openswmm_legacy_worker_524
    DESTINATION "${TOOL_DIST}"
)

# copy the worker to the build tree beside swmm5 and runswmm for testing
add_custom_command(TARGET openswmm_legacy_worker_524 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
        $<TARGET_FILE:openswmm_legacy_worker_524>
        ${CMAKE_BINARY_DIR}/bin/$<CONFIGURATION>/$<TARGET_FILE_NAME:openswmm_legacy_worker_524>
)
