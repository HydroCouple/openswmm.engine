# Native solver feasibility evidence; no benchmark/solver package dependency.
add_executable(fv_implicit_baseline driver.cpp)
target_link_libraries(fv_implicit_baseline PRIVATE
    $<IF:$<BOOL:${MSVC}>,openswmm_engine_internal,openswmm_engine>)
target_include_directories(fv_implicit_baseline PRIVATE
    ${PROJECT_SOURCE_DIR}/src/engine ${PROJECT_SOURCE_DIR}/include)
set_target_properties(fv_implicit_baseline PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/engine_library_$<CONFIG>.txt"
    CONTENT "$<TARGET_FILE:openswmm_engine>\n")

add_executable(fv_pressure_qualification ../fv_implicit_phase05/qualification.cpp)
target_link_libraries(fv_pressure_qualification PRIVATE
    $<IF:$<BOOL:${MSVC}>,openswmm_engine_internal,openswmm_engine>)
target_include_directories(fv_pressure_qualification PRIVATE
    ${PROJECT_SOURCE_DIR}/src/engine ${PROJECT_SOURCE_DIR}/include)
set_target_properties(fv_pressure_qualification PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")

add_executable(fv_implicit_reach ../fv_implicit_phase1/driver.cpp)
target_link_libraries(fv_implicit_reach PRIVATE
    $<IF:$<BOOL:${MSVC}>,openswmm_engine_internal,openswmm_engine>)
target_include_directories(fv_implicit_reach PRIVATE
    ${PROJECT_SOURCE_DIR}/src/engine ${PROJECT_SOURCE_DIR}/include)
set_target_properties(fv_implicit_reach PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")

add_executable(fv_implicit_network ../fv_implicit_phase1/network_driver.cpp)
target_link_libraries(fv_implicit_network PRIVATE
    $<IF:$<BOOL:${MSVC}>,openswmm_engine_internal,openswmm_engine>)
target_include_directories(fv_implicit_network PRIVATE
    ${PROJECT_SOURCE_DIR}/src/engine ${PROJECT_SOURCE_DIR}/include)
set_target_properties(fv_implicit_network PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")
