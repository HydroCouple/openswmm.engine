/**
 * @file      main.cpp
 * @brief     Command-line interface for OpenSWMM Engine 6.0.
 *
 * @details Usage:
 *   openswmm <input.inp> <report.rpt> [output.out]
 *   openswmm --help | -h
 *   openswmm --version | -v
 *
 * Uses the new data-oriented engine (openswmm_engine) via the C API.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include <cstdio>
#include <cstring>
#include <ctime>

#include "openswmm_engine.h"
#include "version.h"

/**
 * @brief Print command syntax, switches and engine/output descriptions to stdout.
 */
static void print_help() {
    std::printf("\n");
    std::printf("Open Source Storm Water Management Model %s\n", OPENSWMM_VERSION_FULL);
    std::printf("===================================================\n\n");
    std::printf("USAGE:\n");
    std::printf("  openswmm <input.inp> <report.rpt> [output.out]\n\n");
    std::printf("OPTIONS:\n");
    std::printf("  --help, -h       Show this help message\n");
    std::printf("  --version, -v    Show version number\n\n");
    std::printf("DESCRIPTION:\n");
    std::printf("  Runs a SWMM simulation using the new data-oriented engine.\n");
    std::printf("  The engine reads a standard SWMM .inp file and produces\n");
    std::printf("  a report (.rpt) and optional binary output (.out) file.\n\n");
    std::printf("  For the legacy EPA SWMM 5.x engine, use: openswmm-legacy\n\n");
}

/**
 * @brief Print the configured OpenSWMM version to stdout.
 */
static void print_version() {
    std::printf("openswmm.engine %s\n", OPENSWMM_VERSION_FULL);
}

/**
 * @brief Run one new-engine simulation from command-line paths.
 *
 * Creates an engine, opens files, initializes and runs until step completion/error, prints progress
 * every 1000 successful advancing steps, then ends/reports/closes/destroys it. Report/output files
 * may be created or overwritten according to the engine API. Prints failures to stdout;
 * end/report/close failures preserve the first failure status. Each created handle is destroyed on every
 * explicit return path.
 *
 * @param[in] argc Process argument count including program name; help/version need one switch,
 *     simulation needs input and report paths.
 * @param[in] argv Borrowed null-terminated argument strings: input, report, optional binary output.
 *     Extra arguments are ignored; an omitted output path disables saved results.
 * @return Zero for help/version or a successful simulation. Otherwise returns 1 for
 *     argument/allocation failures, or the first failing engine call's error code.
 */
int main(int argc, char* argv[]) {

    if (argc < 2) {
        std::printf("\nError: not enough arguments. Use --help for usage.\n\n");
        return 1;
    }

    // Handle --help and --version
    if (std::strcmp(argv[1], "--help") == 0 || std::strcmp(argv[1], "-h") == 0) {
        print_help();
        return 0;
    }
    if (std::strcmp(argv[1], "--version") == 0 || std::strcmp(argv[1], "-v") == 0) {
        print_version();
        return 0;
    }

    if (argc < 3) {
        std::printf("\nError: need at least input and report file paths.\n");
        std::printf("Usage: openswmm <input.inp> <report.rpt> [output.out]\n\n");
        return 1;
    }

    const char* inp_file = argv[1];
    const char* rpt_file = argv[2];
    const char* out_file = (argc > 3) ? argv[3] : "";

    std::printf("\n... OpenSWMM Engine %s\n", OPENSWMM_VERSION_FULL);
    std::printf("... Input: %s\n", inp_file);
    std::printf("... Report: %s\n", rpt_file);
    std::printf("... Output: %s\n", out_file);
    std::fflush(stdout);

    std::time_t start = std::time(nullptr);

    // ---- Create engine ----
    SWMM_Engine engine = swmm_engine_create();
    if (!engine) {
        std::printf("Error: failed to create engine instance.\n");
        return 1;
    }

    // ---- Open input file ----
    int err = swmm_engine_open(engine, inp_file, rpt_file, out_file, nullptr);
    if (err != SWMM_OK) {
        std::printf("Error opening input file: %s\n", swmm_get_last_error_msg(engine));
        swmm_engine_destroy(engine);
        return err;
    }

    // ---- Initialize ----
    err = swmm_engine_initialize(engine);
    if (err != SWMM_OK) {
        std::printf("Error initializing: %s\n", swmm_get_last_error_msg(engine));
        swmm_engine_close(engine);
        swmm_engine_destroy(engine);
        return err;
    }

    // ---- Start simulation ----
    int save = (out_file[0] != '\0') ? 1 : 0;
    err = swmm_engine_start(engine, save);
    if (err != SWMM_OK) {
        std::printf("Error starting simulation: %s\n", swmm_get_last_error_msg(engine));
        swmm_engine_close(engine);
        swmm_engine_destroy(engine);
        return err;
    }

    // ---- Step loop ----
    double elapsed = 0.0;
    long step_count = 0;
    while (true) {
        err = swmm_engine_step(engine, &elapsed);
        if (err != SWMM_OK) {
            std::printf("\nError at step %ld: %s\n", step_count, swmm_get_last_error_msg(engine));
            break;
        }
        if (elapsed <= 0.0)
            break; // simulation complete

        step_count++;
        if (step_count % 1000 == 0) {
            std::printf("\r... %ld steps, elapsed = %.4f days", step_count, elapsed);
            std::fflush(stdout);
        }
    }
    std::printf("\n... %ld steps completed.\n", step_count);

    // ---- End and report ----
    int result = err;
    const auto finalize_status = [&](int status, const char* phase) {
        if (status != SWMM_OK) {
            std::printf("Error %s: %s\n", phase, swmm_get_last_error_msg(engine));
            if (result == SWMM_OK)
                result = status;
        }
    };
    finalize_status(swmm_engine_end(engine), "ending simulation");
    finalize_status(swmm_engine_report(engine), "writing report");

    // ---- Close and destroy ----
    finalize_status(swmm_engine_close(engine), "closing simulation");
    swmm_engine_destroy(engine);

    // ---- Summary ----
    double run_time = std::difftime(std::time(nullptr), start);
    std::printf("... OpenSWMM Engine %s in %.2f seconds.\n\n",
                result == SWMM_OK ? "completed" : "failed", run_time);

    return result;
}
