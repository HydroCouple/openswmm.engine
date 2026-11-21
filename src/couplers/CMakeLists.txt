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
if(OPENSWMM_BUILD_2D AND OPENSWMM_2D_HDF5_OUTPUT)
    target_compile_definitions(openswmm_engine_component PRIVATE OPENSWMM_COMPONENT_2D_RESULTS=1)
endif()
target_link_libraries(openswmm_engine_component PRIVATE openswmm::engine HydroCouple::HydroCouple nlohmann_json::nlohmann_json)
set_target_properties(openswmm_engine_component PROPERTIES
    CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/components")
install(TARGETS openswmm_engine_component LIBRARY DESTINATION lib/hydrocouple/components)

# Optional collective service contract; no HydroCoupleSDK dependency.
install(FILES "${PROJECT_SOURCE_DIR}/include/openswmm/components/openswmm_component_recovery.h"
    DESTINATION include/openswmm/components)
