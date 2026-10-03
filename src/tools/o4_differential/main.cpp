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
 * @file main.cpp
 * @brief `o4_differential` — the plain A/B driver O4's protocol §3 calls for.
 *
 * @details **O4:** the same parity deck delivered **7.25 in** to the ground
 *          through `lifecycle_open_model` + `lifecycle_run_simulation` where
 *          the CLI delivered **12.98 in**, from an identical 12.000 in of
 *          precipitation — the packs barely melted. Three CLI builds agree
 *          with each other and disagree with that run, **including one built
 *          at the API run's own commit**, so the commit range is controlled
 *          for and the execution path is the variable left standing.
 *
 * @par What this tool is FOR, and the variable it removes
 *      Those two names are **MCP tool names, not engine entry points.** There
 *      is no `lifecycle_run_simulation` anywhere in `src/`. So the measurement
 *      was taken through the MCP server, and the server is a *third* variable
 *      — its own stepping, its working directory, its process lifetime across
 *      `close_model`/reopen.
 *
 *      This driver removes it. It runs the deck through the C API **in one
 *      process, from one binary**, twice:
 *
 *        - **A** — the CLI's exact sequence, byte for byte
 *          (`src/cli/main.cpp:76-131`).
 *        - **B** — the same sequence with the differences a session driver
 *          plausibly introduces, selectable so each can be tested alone.
 *
 *      If A and B agree, the C API is not the variable and the MCP server is
 *      — which is a finding, and a much narrower place to look.
 *
 * @par What it deliberately does NOT do
 *      It does not instrument `dhm`, `season` or `last_melt_doy_`. O4 §4 is
 *      explicit that those three come *after* localisation, because the
 *      hypothesis that motivated them is the one §2 eliminated. **Reading
 *      them first is how a plain differential becomes a targeted hunt for a
 *      cause that has already been ruled out.**
 *
 * @par Where to look in the output it produces
 *      Each variant writes its own `.rpt` and `.out`. **The tool does not
 *      parse or compare them** — it deliberately leaves that to `cmp` and
 *      `diff`, which is the protocol's step 4, because a comparison written
 *      into the instrument is a comparison nobody can check independently.
 *
 *      The `.rpt` is where O4's signature lives: the runoff continuity block
 *      says *which reservoir* the water went to, and the divergence is
 *      specifically `Snow Cover` against `Infiltration`. A binary `.out`
 *      diff says only *that* two runs parted.
 *
 * @see plans/transport/O4_API_CLI_DIFFERENTIAL_2026-08-22.md
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>

#include "openswmm_engine.h"

namespace {

/// The ways a session driver can differ from the CLI while still making the
/// same C API calls. Each is separately selectable: the point of a plain
/// differential is that **one variable moves at a time**.
struct Variant {
    const char* name;
    bool        chdir_elsewhere;   ///< run with cwd != the deck's directory
    bool        reopen_first;      ///< open/close/open before stepping
    bool        save_results;      ///< `start`'s only argument
    bool        report_before_end; ///< call report() before end()
};

/// Restores the previous working directory on scope exit, so one variant
/// cannot leak its cwd into the next and turn a one-variable differential
/// into a two-variable one.
class ScopedCwd {
public:
    explicit ScopedCwd(const std::filesystem::path& to)
        : prev_(std::filesystem::current_path()), moved_(false) {
        std::error_code ec;
        std::filesystem::current_path(to, ec);
        moved_ = !ec;
    }
    ~ScopedCwd() {
        if (moved_) {
            std::error_code ec;
            std::filesystem::current_path(prev_, ec);
        }
    }
    ScopedCwd(const ScopedCwd&) = delete;
    ScopedCwd& operator=(const ScopedCwd&) = delete;
    bool moved() const { return moved_; }
private:
    std::filesystem::path prev_;
    bool moved_;
};

int runOnce(const char* inp, const char* rpt, const char* out,
            const Variant& v, double* out_elapsed, long* out_steps) {
    // The MCP server runs with a working directory that is NOT the deck's
    // (`.../Projects/default`), and §3 names that as a suspect. Reproducing
    // it needs the deck given as an ABSOLUTE path — which is what a session
    // driver passes — so the move cannot be mistaken for a path-resolution
    // failure. `..` is used rather than a fixed path so the tool has no
    // machine-specific assumption in it.
    std::unique_ptr<ScopedCwd> cwd_guard;
    if (v.chdir_elsewhere) {
        cwd_guard = std::make_unique<ScopedCwd>(
            std::filesystem::temp_directory_path());
        if (!cwd_guard->moved())
            std::printf("  [%s] WARNING: could not change directory; this "
                        "variant is now identical to `cli`\n", v.name);
    }

    SWMM_Engine e = swmm_engine_create();
    if (e == nullptr) {
        std::printf("  [%s] engine create FAILED\n", v.name);
        return 1;
    }

    // A session driver that keeps a model open across calls has already
    // opened and closed this deck at least once by the time it steps. That
    // is a real difference from a fresh CLI process and costs one line to
    // reproduce.
    if (v.reopen_first) {
        if (swmm_engine_open(e, inp, rpt, out, nullptr) == SWMM_OK)
            swmm_engine_close(e);
    }

    int err = swmm_engine_open(e, inp, rpt, out, nullptr);
    if (err != SWMM_OK) {
        std::printf("  [%s] open FAILED: %s\n", v.name,
                    swmm_get_last_error_msg(e));
        swmm_engine_destroy(e);
        return err;
    }

    err = swmm_engine_initialize(e);
    if (err != SWMM_OK) {
        std::printf("  [%s] initialize FAILED: %s\n", v.name,
                    swmm_get_last_error_msg(e));
        swmm_engine_close(e);
        swmm_engine_destroy(e);
        return err;
    }

    err = swmm_engine_start(e, v.save_results ? 1 : 0);
    if (err != SWMM_OK) {
        std::printf("  [%s] start FAILED: %s\n", v.name,
                    swmm_get_last_error_msg(e));
        swmm_engine_close(e);
        swmm_engine_destroy(e);
        return err;
    }

    double elapsed = 0.0;
    long   steps   = 0;
    while (true) {
        err = swmm_engine_step(e, &elapsed);
        if (err != SWMM_OK) {
            std::printf("  [%s] step %ld FAILED: %s\n", v.name, steps,
                        swmm_get_last_error_msg(e));
            break;
        }
        if (elapsed <= 0.0) break;
        ++steps;
    }

    // Order matters, and so does LOOKING AT WHAT THE ENGINE SAYS ABOUT IT.
    //
    // The first version of this driver discarded both return codes, and the
    // round that ran it recorded `report()` before `end()` as "legal and
    // silently lossy — no crash, no error code." There IS a code. An
    // instrument that ignores a return value cannot tell "the engine allowed
    // this" from "the engine refused and nobody asked".
    //
    // MEASURED, once the codes were printed — and it is not the call the
    // correction predicted:
    //
    //   * `step()` sets `ENDED` when the run finishes (SWMMEngine.cpp:1177),
    //     so by the time this driver reports, the engine is ALREADY ended.
    //     `report()`'s `state != ENDED` guard (4924) therefore never fires:
    //     **`report()` succeeds**, and sets `REPORTED` (4970).
    //   * `end()` accepts only `RUNNING` or `ENDED` (4813). It sees
    //     `REPORTED`. **`end()` is the call the engine refuses**, with
    //     SWMM_ERR_WRONG_STATE (6).
    //
    // And the two artefacts have two different causes, separated by
    // falsifier ii-b (let `end()` accept `REPORTED`):
    //
    //   * the `.out` is 24 bytes short because `end()` never wrote its
    //     closing block — ii-b makes it byte-identical to `cli`;
    //   * the `.rpt` says "All links are stable." and prints a flat
    //     `300.000 - 300.000 sec` ladder because `report()` ran BEFORE
    //     `end()` computed those two diagnostics — ii-b does NOT bring them
    //     back, because by then the report is already written.
    //
    // Removing `report()`'s guard entirely (falsifier ii) changes nothing at
    // all, which is how the attribution was settled rather than argued.
    // Reported at the CALL, not after both. `swmm_get_last_error_msg` returns
    // whatever the engine set most recently, so printing after the pair would
    // caption a failing `report()` with a later `end()`'s message -- an
    // instrument that mislabels which call failed is the same defect this
    // file was corrected for, one step smaller.
    auto call = [&](const char* what, int rc) {
        if (rc != SWMM_OK)
            std::printf("  [%s] %s returned %d: %s\n", v.name, what, rc,
                        swmm_get_last_error_msg(e));
        return rc;
    };

    if (v.report_before_end) {
        call("report()", swmm_engine_report(e));
        call("end()",    swmm_engine_end(e));
    } else {
        call("end()",    swmm_engine_end(e));
        call("report()", swmm_engine_report(e));
    }
    call("close()", swmm_engine_close(e));

    swmm_engine_destroy(e);

    *out_elapsed = elapsed;
    *out_steps   = steps;
    return SWMM_OK;
}

void usage() {
    std::printf(
        "o4_differential — run one deck through the C API several ways.\n\n"
        "  o4_differential <input.inp> <out-prefix>\n\n"
        "Writes <out-prefix>_<variant>.rpt / .out for each variant, then\n"
        "leaves the comparison to the caller:\n\n"
        "  cmp  <prefix>_cli.out <prefix>_<variant>.out\n"
        "  diff <prefix>_cli.rpt <prefix>_<variant>.rpt\n\n"
        "The `cli` variant is byte-for-byte the sequence in src/cli/main.cpp,\n"
        "so `<prefix>_cli.*` must match a real CLI run of the same deck. If\n"
        "it does not, THAT is the finding and nothing below it means\n"
        "anything.\n");
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc < 3) { usage(); return argc < 2 ? 1 : 0; }

    // Absolute, so the `elsewhere` variant tests the working directory and
    // not the engine's ability to resolve a relative path from one.
    std::error_code abs_ec;
    const std::string inp =
        std::filesystem::absolute(argv[1], abs_ec).string();
    if (abs_ec) {
        std::printf("cannot resolve %s: %s\n", argv[1], abs_ec.message().c_str());
        return 1;
    }
    const std::string prefix =
        std::filesystem::absolute(argv[2], abs_ec).string();

    // One variable at a time. `cli` is the control and MUST reproduce a real
    // CLI run; every other row changes exactly one thing against it.
    const Variant variants[] = {
        {"cli",        false, false, true,  false},
        {"reopened",   false, true,  true,  false},
        {"nosave",     false, false, false, false},
        {"reportfirst",false, false, true,  true },
        {"elsewhere",  true,  false, true,  false},
    };

    std::printf("o4_differential\n  deck: %s\n\n", inp.c_str());

    int failures = 0;
    for (const auto& v : variants) {
        const std::string rpt = prefix + "_" + v.name + ".rpt";
        const std::string out = prefix + "_" + v.name + ".out";
        double elapsed = 0.0;
        long   steps   = 0;
        const int err = runOnce(inp.c_str(), rpt.c_str(), out.c_str(), v,
                                &elapsed, &steps);
        if (err != SWMM_OK) { ++failures; continue; }
        std::printf("  %-12s steps=%-8ld rpt=%s\n", v.name, steps, rpt.c_str());
    }

    std::printf(
        "\nNext, per O4 protocol step 4 — compare, do NOT instrument yet:\n"
        "  cmp %s_cli.out %s_reopened.out\n"
        "  diff %s_cli.rpt %s_reopened.rpt\n\n"
        "and the control first:\n"
        "  openswmm %s /tmp/real_cli.rpt /tmp/real_cli.out\n"
        "  cmp /tmp/real_cli.out %s_cli.out\n",
        prefix.c_str(), prefix.c_str(), prefix.c_str(), prefix.c_str(),
        inp.c_str(), prefix.c_str());

    return failures == 0 ? 0 : 1;
}
