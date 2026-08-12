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

add_benchmark(bench_engine_vs_legacy   bench_engine_vs_legacy.cpp)
add_benchmark(bench_timeseries_lookup  bench_timeseries_lookup.cpp)
add_benchmark(bench_hydraulics         bench_hydraulics.cpp)

# ---------------------------------------------------------------------------
# bench_model_load — open()/initialize()/start() timings over the synthetic
# scaling corpus (see plans/MODEL_LOAD_INIT_OPTIMIZATION_PLAN_2026-08-12.md).
#
# Not built with add_benchmark(): it discovers its models at run time and so
# needs its own main(), which would clash with benchmark::benchmark_main.
# ---------------------------------------------------------------------------
add_executable(bench_model_load bench_model_load.cpp)

target_link_libraries(bench_model_load
    PRIVATE
        benchmark::benchmark
        openswmm_engine
)

target_include_directories(bench_model_load
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
)

set_target_properties(bench_model_load PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)

add_custom_command(TARGET bench_model_load POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
        $<TARGET_FILE:bench_model_load>
        ${CMAKE_BINARY_DIR}/bin/$<CONFIG>/$<TARGET_FILE_NAME:bench_model_load>
)

# parity_probe and the two load-optimization ctest gates are defined in
# tests/CMakeLists.txt, not here: they need no Google Benchmark, and this
# directory is only added when OPENSWMM_BUILD_BENCHMARKS is ON — a build that
# has enable_testing() off, where add_test() registers nothing.
