# CMake generated Testfile for 
# Source directory: /tmp/workspace/HydroCouple/openswmm.engine/tests
# Build directory: /tmp/workspace/HydroCouple/openswmm.engine/build_test/tests
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test([=[test_legacy_solver_api]=] "/tmp/workspace/HydroCouple/openswmm.engine/build_test/bin/Release/test_solver_api")
set_tests_properties([=[test_legacy_solver_api]=] PROPERTIES  LABELS "unit" WORKING_DIRECTORY "/tmp/workspace/HydroCouple/openswmm.engine/tests/unit/legacy/engine/data" _BACKTRACE_TRIPLES "/tmp/workspace/HydroCouple/openswmm.engine/tests/CMakeLists.txt;116;add_test;/tmp/workspace/HydroCouple/openswmm.engine/tests/CMakeLists.txt;0;")
add_test([=[test_legacy_solver_errors]=] "/tmp/workspace/HydroCouple/openswmm.engine/build_test/bin/Release/test_solver_errors")
set_tests_properties([=[test_legacy_solver_errors]=] PROPERTIES  LABELS "unit" WORKING_DIRECTORY "/tmp/workspace/HydroCouple/openswmm.engine/tests/unit/legacy/engine/data" _BACKTRACE_TRIPLES "/tmp/workspace/HydroCouple/openswmm.engine/tests/CMakeLists.txt;119;add_test;/tmp/workspace/HydroCouple/openswmm.engine/tests/CMakeLists.txt;0;")
add_test([=[test_legacy_solver_hotstart]=] "/tmp/workspace/HydroCouple/openswmm.engine/build_test/bin/Release/test_solver_hotstart")
set_tests_properties([=[test_legacy_solver_hotstart]=] PROPERTIES  LABELS "unit" WORKING_DIRECTORY "/tmp/workspace/HydroCouple/openswmm.engine/tests/unit/legacy/engine/data" _BACKTRACE_TRIPLES "/tmp/workspace/HydroCouple/openswmm.engine/tests/CMakeLists.txt;122;add_test;/tmp/workspace/HydroCouple/openswmm.engine/tests/CMakeLists.txt;0;")
add_test([=[test_legacy_solver_shapes]=] "/tmp/workspace/HydroCouple/openswmm.engine/build_test/bin/Release/test_solver_shapes")
set_tests_properties([=[test_legacy_solver_shapes]=] PROPERTIES  LABELS "unit" WORKING_DIRECTORY "/tmp/workspace/HydroCouple/openswmm.engine/tests/unit/legacy/engine/data" _BACKTRACE_TRIPLES "/tmp/workspace/HydroCouple/openswmm.engine/tests/CMakeLists.txt;125;add_test;/tmp/workspace/HydroCouple/openswmm.engine/tests/CMakeLists.txt;0;")
add_test([=[test_legacy_solver_expanded_api]=] "/tmp/workspace/HydroCouple/openswmm.engine/build_test/bin/Release/test_solver_expanded_api")
set_tests_properties([=[test_legacy_solver_expanded_api]=] PROPERTIES  LABELS "unit" WORKING_DIRECTORY "/tmp/workspace/HydroCouple/openswmm.engine/tests/unit/legacy/engine/data" _BACKTRACE_TRIPLES "/tmp/workspace/HydroCouple/openswmm.engine/tests/CMakeLists.txt;128;add_test;/tmp/workspace/HydroCouple/openswmm.engine/tests/CMakeLists.txt;0;")
add_test([=[test_legacy_output]=] "/tmp/workspace/HydroCouple/openswmm.engine/build_test/bin/Release/test_output")
set_tests_properties([=[test_legacy_output]=] PROPERTIES  LABELS "unit" WORKING_DIRECTORY "/tmp/workspace/HydroCouple/openswmm.engine/tests/unit/legacy/output/data" _BACKTRACE_TRIPLES "/tmp/workspace/HydroCouple/openswmm.engine/tests/CMakeLists.txt;131;add_test;/tmp/workspace/HydroCouple/openswmm.engine/tests/CMakeLists.txt;0;")
subdirs("unit")
