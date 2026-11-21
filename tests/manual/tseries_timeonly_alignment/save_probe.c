/* Round-trip probe: open an .inp with the refactored engine and write it
 * back out, so the emitted [TIMESERIES] section can be inspected.
 *
 * Usage: save_probe <in.inp> <out.inp>
 */

/**
 * @file      save_probe.c
 * @brief     Tests for save probe.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include <stdio.h>
#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_model.h>

/**
 * @brief Open and write a model for manual time-only series inspection.
 *
 * Uses save_probe.rpt in cwd, writes the selected output deck, and does not close/destroy the raw
 * handle on success or error; process exit supplies final cleanup. No simulation is run.
 *
 * @param[in] argc Process argument count, requiring input and output deck paths.
 * @param[in] argv Borrowed input/output paths after the program name; matching output paths may be
 *     overwritten.
 * @return Two for missing arguments, one for allocation/open/write failure, otherwise zero.
 */
int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <in.inp> <out.inp>\n", argv[0]);
        return 2;
    }
    SWMM_Engine eng = swmm_engine_create();
    if (!eng) {
        fprintf(stderr, "create failed\n");
        return 1;
    }
    int err = swmm_engine_open(eng, argv[1], "save_probe.rpt", NULL, NULL);
    if (err != 0) {
        fprintf(stderr, "open failed: %d\n", err);
        return 1;
    }
    err = swmm_model_write(eng, argv[2]);
    if (err != 0) {
        fprintf(stderr, "write failed: %d\n", err);
        return 1;
    }
    printf("wrote %s\n", argv[2]);
    return 0;
}
