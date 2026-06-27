// QA Phase 4 Stage C — INP save harness. Opens a model and writes it back out
// via swmm_model_write so we can verify the InpWriter emits real subtype config
// (read from the relational side-table) rather than unpopulated wide defaults.
// Usage: save_inp <in.inp> <out.inp>
#include <cstdio>
#include "openswmm/engine/openswmm_engine.h"
#include "openswmm/engine/openswmm_model.h"

int main(int argc, char** argv) {
    if (argc < 3) { printf("usage: %s <in.inp> <out.inp>\n", argv[0]); return 2; }
    SWMM_Engine e = swmm_engine_create();
    int rc = swmm_engine_open(e, argv[1], "save_inp.rpt", "save_inp.out", nullptr);
    if (rc != SWMM_OK) { printf("open rc=%d\n", rc); return 2; }
    rc = swmm_model_write(e, argv[2]);
    if (rc != SWMM_OK) { printf("model_write rc=%d\n", rc); return 2; }
    swmm_engine_close(e);
    swmm_engine_destroy(e);
    printf("wrote %s\n", argv[2]);
    return 0;
}
