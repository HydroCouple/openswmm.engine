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
    std::printf("... Input: %s\n", inp_file);
    std::printf("... Report: %s\n", rpt_file);
    std::printf("... Output: %s\n", out_file);
    std::fflush(stdout);

    std::time_t start = std::time(nullptr);

    // ---- Create engine ----
    std::printf("... Creating engine...\n"); std::fflush(stdout);
    SWMM_Engine engine = swmm_engine_create();
    if (!engine) {
        std::printf("Error: failed to create engine instance.\n");
        return 1;
    }
    std::printf("... Engine created.\n"); std::fflush(stdout);

    // ---- Open input file ----
    std::printf("... Opening input file...\n"); std::fflush(stdout);
    int err = swmm_engine_open(engine, inp_file, rpt_file, out_file, nullptr);
    std::printf("... open() returned %d\n", err); std::fflush(stdout);
    if (err != SWMM_OK) {
        std::printf("Error opening input file: %s\n", swmm_get_last_error_msg(engine));
        swmm_engine_destroy(engine);
        return err;
    }

    // ---- Initialize ----
    std::printf("... Initializing...\n"); std::fflush(stdout);
    err = swmm_engine_initialize(engine);
    std::printf("... initialize() returned %d\n", err); std::fflush(stdout);
    if (err != SWMM_OK) {
        std::printf("Error initializing: %s\n", swmm_get_last_error_msg(engine));
        swmm_engine_close(engine);
        swmm_engine_destroy(engine);
        return err;
    }

    // ---- Start simulation ----
    std::printf("... Starting simulation...\n"); std::fflush(stdout);
    int save = (out_file[0] != '\0') ? 1 : 0;
    std::printf("... save=%d\n", save); std::fflush(stdout);
    err = swmm_engine_start(engine, save);
    std::printf("... start() returned %d\n", err); std::fflush(stdout);
    if (err != SWMM_OK) {
        std::printf("Error starting simulation: %s\n", swmm_get_last_error_msg(engine));
        swmm_engine_close(engine);
        swmm_engine_destroy(engine);
        return err;
    }

    // ---- Step loop ----
    double elapsed = 0.0;
    long step_count = 0;
    std::printf("... Starting step loop (state after start: err=%d)\n", err);
    std::fflush(stdout);
    while (true) {
        err = swmm_engine_step(engine, &elapsed);
        if (err != SWMM_OK) {
            std::printf("\nError at step %ld (err=%d): %s\n", step_count, err,
                        swmm_get_last_error_msg(engine));
            break;
        }
        if (elapsed <= 0.0) {
            std::printf("... step() returned elapsed=%.6f at step %ld — simulation complete\n",
                        elapsed, step_count);
            break;
        }

        step_count++;
        if (step_count % 1000 == 0) {
            std::printf("\r... %ld steps, elapsed = %.4f days", step_count, elapsed);
            std::fflush(stdout);
        }
    }
    std::printf("\n... %ld steps completed.\n", step_count);

    // ---- End and report ----
    std::time_t t0 = std::time(nullptr);
    std::printf("... Finalizing (end)...\n");
    std::fflush(stdout);
    swmm_engine_end(engine);
    std::printf("... end() took %.0f sec. Writing report...\n",
                std::difftime(std::time(nullptr), t0));
    std::fflush(stdout);

    std::time_t t1 = std::time(nullptr);
    swmm_engine_report(engine);
    std::printf("... report() took %.0f sec.\n",
                std::difftime(std::time(nullptr), t1));
    std::fflush(stdout);

    // ---- Close and destroy ----
    swmm_engine_close(engine);
    swmm_engine_destroy(engine);

    // ---- Summary ----
    double run_time = std::difftime(std::time(nullptr), start);
    std::printf("... OpenSWMM Engine completed in %.2f seconds.\n\n", run_time);

    return 0;
}
