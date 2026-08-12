// SPDX-License-Identifier: Apache-2.0
//
// Copyright 2026 Caleb Buahin
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/**
 * @file parity_probe.cpp
 * @brief Emits the three artifacts the load-optimization parity gate compares.
 *
 * @details The plan's gate (Phase 0.4) diffs, for each model, the report minus
 *          its timing lines, the binary output byte-for-byte, and the
 *          written-back `.inp`. The shipped CLI produces the first two but has
 *          no write-back flag, so this probe drives all three from one open:
 *
 *              parity_probe <model.inp> <out-dir> [stem]
 *
 *          writes `<out-dir>/<stem>.rpt`, `.out` and `.writeback.inp`.
 *
 *          The write-back happens straight after open() — before initialize()
 *          — so it captures the parsed+resolved model rather than post-run
 *          state. That is what makes it a parse/resolve parity check, which is
 *          exactly the surface Phases 1 and 2 disturb.
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @license  Apache-2.0
 */

#include <cstdio>
#include <filesystem>
#include <string>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_model.h>

namespace fs = std::filesystem;

namespace {

int fail(const char* what, int rc) {
    std::fprintf(stderr, "parity_probe: %s failed (rc=%d)\n", what, rc);
    return rc != 0 ? rc : 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr,
                     "usage: parity_probe <model.inp> <out-dir> [stem]\n");
        return 2;
    }

    const std::string inp = argv[1];
    const fs::path    dir = argv[2];
    const std::string stem =
        (argc > 3) ? argv[3] : fs::path(inp).stem().string();

    std::error_code ec;
    fs::create_directories(dir, ec);

    const std::string rpt = (dir / (stem + ".rpt")).string();
    const std::string out = (dir / (stem + ".out")).string();
    const std::string wb  = (dir / (stem + ".writeback.inp")).string();

    SWMM_Engine eng = swmm_engine_create();
    if (eng == nullptr) return fail("create", 1);

    int rc = swmm_engine_open(eng, inp.c_str(), rpt.c_str(), out.c_str(),
                              nullptr);
    if (rc != SWMM_OK) { swmm_engine_destroy(eng); return fail("open", rc); }

    // Write-back before initialize(): the parse/resolve product, not run state.
    rc = swmm_model_write(eng, wb.c_str());
    if (rc != SWMM_OK) {
        swmm_engine_close(eng);
        swmm_engine_destroy(eng);
        return fail("model_write", rc);
    }

    rc = swmm_engine_initialize(eng);
    if (rc != SWMM_OK) {
        swmm_engine_close(eng);
        swmm_engine_destroy(eng);
        return fail("initialize", rc);
    }

    rc = swmm_engine_start(eng, 1);
    if (rc != SWMM_OK) {
        swmm_engine_close(eng);
        swmm_engine_destroy(eng);
        return fail("start", rc);
    }

    // Run to completion so the .out is a full result set, not a header.
    double elapsed_days = 0.0;
    long   guard        = 0;
    do {
        rc = swmm_engine_step(eng, &elapsed_days);
        if (rc != SWMM_OK) {
            swmm_engine_close(eng);
            swmm_engine_destroy(eng);
            return fail("step", rc);
        }
    } while (elapsed_days > 0.0 && ++guard < 100000000L);

    rc = swmm_engine_end(eng);
    if (rc == SWMM_OK) rc = swmm_engine_report(eng);
    swmm_engine_close(eng);
    swmm_engine_destroy(eng);
    if (rc != SWMM_OK) return fail("end/report", rc);

    std::printf("parity_probe: wrote %s{.rpt,.out,.writeback.inp}\n",
                (dir / stem).string().c_str());
    return 0;
}
