/* Round-trip probe: open an .inp with the refactored engine and write it
 * back out, so the emitted [TIMESERIES] section can be inspected.
 *
 * Usage: save_probe <in.inp> <out.inp>
 */
#include <stdio.h>
#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_model.h>

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <in.inp> <out.inp>\n", argv[0]);
        return 2;
    }
    SWMM_Engine eng = swmm_engine_create();
    if (!eng) { fprintf(stderr, "create failed\n"); return 1; }
    int err = swmm_engine_open(eng, argv[1], "save_probe.rpt", NULL, NULL);
    if (err != 0) { fprintf(stderr, "open failed: %d\n", err); return 1; }
    err = swmm_model_write(eng, argv[2]);
    if (err != 0) { fprintf(stderr, "write failed: %d\n", err); return 1; }
    printf("wrote %s\n", argv[2]);
    return 0;
}
