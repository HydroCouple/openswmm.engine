#
# tests/legacy/CMakeLists.txt
#
# Legacy SWMM test subdirectories:
#   engine/ — tests for openswmm_legacy_engine (solver API)
#   output/ — tests for openswmm_legacy_output (binary output reader)
#

cmake_minimum_required(VERSION 3.21)

add_subdirectory(engine)
add_subdirectory(output)
