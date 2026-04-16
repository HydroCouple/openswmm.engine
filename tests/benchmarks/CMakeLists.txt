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
add_benchmark(bench_operator_snapshot  bench_operator_snapshot.cpp)
