# Fuzz targets for untrusted engine inputs (OPENSWMM_BUILD_FUZZERS).
#
# With clang and libFuzzer available the targets link -fsanitize=fuzzer and
# run as ordinary libFuzzer binaries:
#     fuzz_inp_open -max_total_time=600 -timeout=10 corpus_dir/ seeds_dir/
# Otherwise (Apple clang, GCC, MSVC) each target builds with a standalone
# main() that runs the files named on its command line once, which is how
# the seed corpus is checked under ASan/UBSan locally.
#
# Sanitizers are not chosen here: pass them in CMAKE_C_FLAGS/CMAKE_CXX_FLAGS
# (e.g. -fsanitize=address,undefined) so the engine library is instrumented
# too. -fsanitize=fuzzer-no-link must also be in those flags for libFuzzer to
# get coverage feedback from the engine code, not just from the target.

include(CheckCXXSourceCompiles)

set(CMAKE_REQUIRED_FLAGS "-fsanitize=fuzzer")
check_cxx_source_compiles(
    "#include <cstdint>
     #include <cstddef>
     extern \"C\" int LLVMFuzzerTestOneInput(const uint8_t*, size_t) { return 0; }"
    OPENSWMM_HAVE_LIBFUZZER)
unset(CMAKE_REQUIRED_FLAGS)

function(openswmm_add_fuzzer NAME)
    add_executable(${NAME} ${ARGN})
    target_link_libraries(${NAME} PRIVATE openswmm_engine)
    target_include_directories(${NAME} PRIVATE ${PROJECT_SOURCE_DIR}/include)
    set_target_properties(${NAME} PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
    if(OPENSWMM_HAVE_LIBFUZZER)
        target_link_options(${NAME} PRIVATE -fsanitize=fuzzer)
    else()
        target_compile_definitions(${NAME} PRIVATE OPENSWMM_FUZZ_STANDALONE)
    endif()
endfunction()

openswmm_add_fuzzer(fuzz_inp_open fuzz_inp_open.cpp)

if(OPENSWMM_HAVE_LIBFUZZER)
    message(STATUS "Fuzzers: libFuzzer targets (fuzz_inp_open)")
else()
    message(STATUS "Fuzzers: no libFuzzer; building standalone seed runners (fuzz_inp_open)")
endif()
