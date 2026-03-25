#
# src/legacy/CMakeLists.txt
#
# Registers legacy library subdirectories.
# Both libraries contain unmodified EPA SWMM 5.x code — do NOT edit.
#
add_subdirectory(engine)   # → openswmm_legacy_engine (openswmm.legacy.engine)
add_subdirectory(output)   # → openswmm_legacy_output (openswmm.legacy.output)
add_subdirectory(cli)      # → openswmm_legacy (openswmm-legacy executable)
