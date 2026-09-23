/*
 * rt_write — open a .inp through the engine and write it straight back out.
 *
 * The writer round-trip audit needs generation-1 of a deck without running a
 * simulation. The `openswmm` CLI has no convert mode and the installed Python
 * bindings lag the source tree, so this is the driver: open, write, close.
 *
 * Usage: rt_write <in.inp> <out.inp> [--lenient]
 *
 * --lenient mirrors the mode the GUI opens in (unresolved references survive
 * as retained names), which is the mode most likely to expose writer gaps.
 *
 * Exit: 0 on success, 1 on open failure, 2 on write failure, 3 on usage.
 * Warnings raised by the writer are printed to stdout, one per line, prefixed
 * "WARN " — the harness records them as evidence.
 */

#include <stdio.h>
#include <string.h>

#include "openswmm/engine/openswmm_engine.h"
#include "openswmm/engine/openswmm_model.h"

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: rt_write <in.inp> <out.inp> [--lenient]\n");
        return 3;
    }
    const char* in_path  = argv[1];
    const char* out_path = argv[2];
    int lenient = 0;
    for (int i = 3; i < argc; ++i)
        if (strcmp(argv[i], "--lenient") == 0) lenient = 1;

    SWMM_Engine e = swmm_engine_create();
    if (!e) {
        fprintf(stderr, "rt_write: could not create engine\n");
        return 1;
    }
    if (lenient) swmm_engine_set_lenient_open(e, 1);

    /* No .rpt / .out: this is a parse-and-serialise pass, not a run. */
    int rc = swmm_engine_open(e, in_path, NULL, NULL, NULL);
    if (rc != 0) {
        fprintf(stderr, "rt_write: open failed rc=%d\n", rc);
        swmm_engine_close(e);
        return 1;
    }

    rc = swmm_model_write(e, out_path);
    if (rc != 0) {
        fprintf(stderr, "rt_write: write failed rc=%d\n", rc);
        swmm_engine_close(e);
        return 2;
    }

    /* The writer reports every substitution it made; surface them all. */
    int nw = swmm_get_warning_count(e);
    for (int i = 0; i < nw; ++i) {
        const char* w = swmm_get_warning_at(e, i);
        if (w && *w) printf("WARN %s\n", w);
    }

    swmm_engine_close(e);
    return 0;
}
