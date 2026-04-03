/**
 * @file main.cpp
 * @brief Command-line interface for OpenSWMM Engine 6.0.
 *
 * @details Usage:
 *   openswmm <input.inp> <report.rpt> [output.out]
 *   openswmm --help | -h
 *   openswmm --version | -v
 *
 * Uses the new data-oriented engine (openswmm_engine) via the C API.
 */

#include <cstdio>
#include <cstring>
#include <ctime>

#include "openswmm_engine.h"

static void print_help() {
    std::printf("\n");
    std::printf("OpenSWMM Engine 6.0 — Storm Water Management Model\n");
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

static void print_version() {
    std::printf("openswmm.engine 6.0.0-alpha.1\n");
}

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

    std::printf("\n... OpenSWMM Engine 6.0.0-alpha.1\n");

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
        if (elapsed <= 0.0) break;  // simulation complete

        step_count++;
        if (step_count % 1000 == 0) {
            std::printf("\r... %ld steps, elapsed = %.4f days", step_count, elapsed);
            std::fflush(stdout);
        }
    }
    std::printf("\r... %ld steps completed.                          \n", step_count);

    // ---- End and report ----
    swmm_engine_end(engine);
    swmm_engine_report(engine);

    // ---- Close and destroy ----
    swmm_engine_close(engine);
    swmm_engine_destroy(engine);

    // ---- Summary ----
    double run_time = std::difftime(std::time(nullptr), start);
    std::printf("... OpenSWMM Engine completed in %.2f seconds.\n\n", run_time);

    return 0;
}
