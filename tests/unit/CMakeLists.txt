#
# tests/unit/CMakeLists.txt
#
# Unit test subdirectories:
#   legacy/  — tests for the legacy openswmm_legacy_engine and openswmm_legacy_output libraries
#   engine/  — unit tests for the new openswmm_engine library
#   output/  — unit tests for the new openswmm_engine output layer
#

cmake_minimum_required(VERSION 3.21)

add_subdirectory(legacy)
add_subdirectory(engine)
add_subdirectory(output)
