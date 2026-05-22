#
# src/legacy/cli/CMakeLists.txt
#
# Legacy SWMM 5.x command-line executable.
# Links against the legacy solver library (openswmm_legacy_engine).
#

add_executable(openswmm_legacy
    main.c
)

target_link_libraries(openswmm_legacy
    PRIVATE
        openswmm_legacy_engine
)

target_include_directories(openswmm_legacy
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include/openswmm/legacy/engine
)

if(APPLE)
    set(LIB_ROOT "@loader_path;@loader_path/../lib")
elseif(UNIX)
    set(LIB_ROOT "$ORIGIN;$ORIGIN/../lib")
endif()

set_target_properties(openswmm_legacy PROPERTIES OUTPUT_NAME "openswmm-legacy")
if(NOT WIN32)
    set_target_properties(openswmm_legacy PROPERTIES INSTALL_RPATH "${LIB_ROOT}")
endif()

install(TARGETS openswmm_legacy DESTINATION bin)

add_custom_command(TARGET openswmm_legacy POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
        $<TARGET_FILE:openswmm_legacy>
        ${CMAKE_BINARY_DIR}/bin/$<CONFIG>/$<TARGET_FILE_NAME:openswmm_legacy>
)
