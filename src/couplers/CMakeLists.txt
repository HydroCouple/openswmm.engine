# SPDX-License-Identifier: Apache-2.0
find_package(nlohmann_json CONFIG REQUIRED)
add_library(openswmm_model_component MODULE OpenSWMMModelComponent.cpp)
target_link_libraries(openswmm_model_component PRIVATE openswmm::engine HydroCouple::HydroCouple nlohmann_json::nlohmann_json)
set_target_properties(openswmm_model_component PROPERTIES
    CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/components")
install(TARGETS openswmm_model_component LIBRARY DESTINATION lib/hydrocouple/components)

# Generic component; the existing FVQual module remains independently available.
add_library(openswmm_engine_component MODULE OpenSWMMEngineComponent.cpp)
target_link_libraries(openswmm_engine_component PRIVATE openswmm::engine HydroCouple::HydroCouple nlohmann_json::nlohmann_json)
set_target_properties(openswmm_engine_component PROPERTIES
    CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/components")
install(TARGETS openswmm_engine_component LIBRARY DESTINATION lib/hydrocouple/components)
