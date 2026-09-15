#
# src/tools/o4_differential/CMakeLists.txt
#
# A diagnostic driver for O4 — the API/CLI hydrology divergence.
# Mirrors src/cli/CMakeLists.txt so the two link identically; the whole
# point of the tool is that it differs from the CLI only in the sequence
# it makes, not in how it was built.
#

add_executable(o4_differential
    main.cpp
)

target_link_libraries(o4_differential
    PRIVATE
        openswmm_engine
        openswmm::common
)

target_include_directories(o4_differential
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include/openswmm/engine
)

set_target_properties(o4_differential
    PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        OUTPUT_NAME "o4_differential"
)

# Land it beside the CLI in the build tree. Without this the binary stays in
# src/tools/o4_differential/ while `openswmm` is in bin/<config>/, and the
# protocol's own commands read as though the two sit together. This is the
# only part of src/cli/CMakeLists.txt worth mirroring beyond the link line —
# the install, RPATH and bundling rules there are for a shipped artefact and
# this is a debugging instrument that is OFF by default.
add_custom_command(TARGET o4_differential POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
        $<TARGET_FILE:o4_differential>
        ${CMAKE_BINARY_DIR}/bin/$<CONFIG>/$<TARGET_FILE_NAME:o4_differential>
)

# Windows finds its DLLs beside the exe or not at all; the CLI stages them the
# same way. Untested here — this round ran on macOS only.
if(WIN32)
    add_custom_command(TARGET o4_differential POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:o4_differential>
            $<TARGET_RUNTIME_DLLS:openswmm_engine>
            $<TARGET_FILE_DIR:o4_differential>
        COMMAND_EXPAND_LISTS
    )
endif()
