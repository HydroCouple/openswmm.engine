#
# tests/benchmarks/CMakeLists.txt
#
# Google Benchmark performance tests.
# Enabled by -DOPENSWMM_BUILD_BENCHMARKS=ON.
#
# Benchmarks:
#   bench_engine_vs_legacy      — wall-time comparison: new engine vs legacy
#   bench_timeseries_lookup     — cursor-based vs linear scan for time series
#   bench_hydraulics            — SIMD vs scalar routing kernel
#
# See: docs/MASTER_IMPLEMENTATION_PLAN.md Phase 9
#

cmake_minimum_required(VERSION 3.21)
find_package(benchmark CONFIG REQUIRED)

function(add_benchmark BENCH_NAME SOURCE_FILE)
    add_executable(${BENCH_NAME} ${SOURCE_FILE})

    target_link_libraries(${BENCH_NAME}
        PRIVATE
            benchmark::benchmark
            benchmark::benchmark_main
            openswmm_engine
            openswmm_legacy_engine
    )

    target_include_directories(${BENCH_NAME}
        PRIVATE
            ${PROJECT_SOURCE_DIR}/include
    )

    set_target_properties(${BENCH_NAME} PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
    )

    add_custom_command(TARGET ${BENCH_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy
            $<TARGET_FILE:${BENCH_NAME}>
            ${CMAKE_BINARY_DIR}/bin/$<CONFIG>/$<TARGET_FILE_NAME:${BENCH_NAME}>
    )
endfunction()

add_benchmark(bench_engine_vs_legacy      bench_engine_vs_legacy.cpp)
add_benchmark(bench_timeseries_lookup     bench_timeseries_lookup.cpp)
add_benchmark(bench_hydraulics            bench_hydraulics.cpp)
add_benchmark(bench_spectral_correction   bench_spectral_correction.cpp)
add_benchmark(bench_2d_spectral_precond   bench_2d_spectral_precond.cpp)
target_include_directories(bench_2d_spectral_precond
    PRIVATE ${PROJECT_SOURCE_DIR}/src/engine
)
if(OPENSWMM_BUILD_2D)
    target_compile_definitions(bench_2d_spectral_precond PRIVATE OPENSWMM_HAS_2D=1)
endif()

add_benchmark(bench_2d_spectral_rom       bench_2d_spectral_rom.cpp)
target_include_directories(bench_2d_spectral_rom
    PRIVATE ${PROJECT_SOURCE_DIR}/src/engine
)
if(OPENSWMM_BUILD_2D)
    target_compile_definitions(bench_2d_spectral_rom PRIVATE OPENSWMM_HAS_2D=1)
endif()

# Standalone demo: SpectralROM sidecar with temporal uncertainty output.
# Not a benchmark (no timing harness) — runs once and prints a table.
add_executable(demo_2d_spectral_rom demo_2d_spectral_rom.cpp)
target_link_libraries(demo_2d_spectral_rom
    PRIVATE
        openswmm_engine
        openswmm_legacy_engine
)
target_include_directories(demo_2d_spectral_rom
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/src/engine
)
set_target_properties(demo_2d_spectral_rom PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)
add_custom_command(TARGET demo_2d_spectral_rom POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
        $<TARGET_FILE:demo_2d_spectral_rom>
        ${CMAKE_BINARY_DIR}/bin/$<CONFIG>/$<TARGET_FILE_NAME:demo_2d_spectral_rom>
)
if(OPENSWMM_BUILD_2D)
    target_compile_definitions(demo_2d_spectral_rom PRIVATE OPENSWMM_HAS_2D=1)
endif()

# Standalone demo: 1D SpectralROM (DCT-II eigenbasis, no mesh dependency).
add_executable(demo_1d_spectral_rom demo_1d_spectral_rom.cpp)
set_target_properties(demo_1d_spectral_rom PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)
add_custom_command(TARGET demo_1d_spectral_rom POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
        $<TARGET_FILE:demo_1d_spectral_rom>
        ${CMAKE_BINARY_DIR}/bin/$<CONFIG>/$<TARGET_FILE_NAME:demo_1d_spectral_rom>
)

# Standalone demo: 1D kinematic wave wet weather confidence bands (city_04 parameters).
add_executable(demo_1d_wet_event_bands demo_1d_wet_event_bands.cpp)
set_target_properties(demo_1d_wet_event_bands PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)
add_custom_command(TARGET demo_1d_wet_event_bands POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
        $<TARGET_FILE:demo_1d_wet_event_bands>
        ${CMAKE_BINARY_DIR}/bin/$<CONFIG>/$<TARGET_FILE_NAME:demo_1d_wet_event_bands>
)
