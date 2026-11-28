// SPDX-License-Identifier: Apache-2.0
//
// Fuzz target: an arbitrary byte string as a SWMM input file.
//
// The bytes are written to a private temporary .inp and taken through the
// public C API exactly as a user's deck is: create, open (parse, post-parse
// resolution and validation), initialize, destroy. No routing step is taken,
// so a valid deck with a long simulation period cannot stall the fuzzer, and
// the target exercises only what an untrusted input file can reach before a
// run starts: the tokenizer, every section handler, the cross-reference
// resolver and the network validation.
//
// Built two ways (tests/fuzz/CMakeLists.txt):
//   * with libFuzzer (clang -fsanitize=fuzzer): the fuzzer supplies main();
//   * without it: STANDALONE main() below runs every file named on the
//     command line once, so the target and its seed corpus can be checked
//     under ASan/UBSan with a compiler that ships no libFuzzer (Apple clang).
//
// Any crash, sanitizer report or hang is a finding. A non-zero error code
// from open/initialize is NOT: rejecting malformed input is the job.

#include <openswmm/engine/openswmm_engine.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#ifdef _WIN32
#include <process.h>
#define OPENSWMM_FUZZ_PID _getpid()
#else
#include <unistd.h>
#define OPENSWMM_FUZZ_PID getpid()
#endif

namespace fs = std::filesystem;

namespace {

// One scratch directory per process; the fuzzer reuses it for every input.
const fs::path& scratch_dir() {
    static const fs::path dir = [] {
        fs::path d = fs::temp_directory_path() /
                     ("openswmm_fuzz_" + std::to_string(OPENSWMM_FUZZ_PID));
        std::error_code ec;
        fs::create_directories(d, ec);
        return d;
    }();
    return dir;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // Decks above 1 MiB add parse time without reaching new code.
    if (size > (1u << 20))
        return 0;

    const fs::path inp = scratch_dir() / "input.inp";
    const fs::path rpt = scratch_dir() / "input.rpt";
    const fs::path out = scratch_dir() / "input.out";
    {
        std::ofstream f(inp, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    }

    SWMM_Engine engine = swmm_engine_create();
    if (!engine)
        return 0;
    if (swmm_engine_open(engine, inp.string().c_str(), rpt.string().c_str(),
                         out.string().c_str(), nullptr) == SWMM_OK) {
        (void)swmm_engine_initialize(engine);
    }
    swmm_engine_destroy(engine);
    return 0;
}

#ifdef OPENSWMM_FUZZ_STANDALONE
#include <iterator>
#include <vector>

int main(int argc, char** argv) {
    int ran = 0;
    for (int i = 1; i < argc; ++i) {
        std::ifstream f(argv[i], std::ios::binary);
        if (!f) {
            std::fprintf(stderr, "cannot read %s\n", argv[i]);
            return 2;
        }
        std::vector<char> buf((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        LLVMFuzzerTestOneInput(reinterpret_cast<const uint8_t*>(buf.data()), buf.size());
        ++ran;
    }
    std::fprintf(stderr, "fuzz_inp_open: ran %d input(s) without a crash\n", ran);
    return 0;
}
#endif
