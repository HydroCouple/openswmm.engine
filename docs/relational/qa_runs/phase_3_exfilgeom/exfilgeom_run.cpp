// QA Phase 3-exfilgeom — exfiltration storage-GEOMETRY side-table A/B harness.
// Generalizes the cold-config exfil_run harness to take the model path as an arg,
// so the SAME driver exercises both storage shapes:
//   - FUNCTIONAL (storage_a/b/c)  -> storage_kinwave_seep.inp
//   - TABULAR    (storage_curve)  -> storage_tabular_exfil.inp
// Sets exfil (Green-Ampt) + seep on STOR1 via the C-API, then drives a FULL
// simulation so the run path builds node_subtypes and ExfilSolver::init reads the
// storage GEOMETRY from the SIDE-TABLE. The written .out is byte-compared between
// the exfilgeom engine and the pre-exfilgeom baseline.
//
// Usage: exfilgeom_run <inp_path> <out_path>

#include <cstdio>
#include "openswmm/engine/openswmm_engine.h"
#include "openswmm/engine/openswmm_nodes.h"

int main(int argc, char** argv) {
    if (argc < 3) { printf("usage: %s <inp> <out>\n", argv[0]); return 2; }
    const char* inp = argv[1];
    const char* out = argv[2];
    SWMM_Engine e = swmm_engine_create();
    int rc = swmm_engine_open(e, inp, "exfilgeom_run.rpt", out, nullptr);
    if (rc != SWMM_OK) { printf("open failed rc=%d\n", rc); return 2; }

    int s = swmm_node_index(e, "STOR1");
    if (s < 0) { printf("STOR1 not found\n"); return 2; }
    // exfil (Green-Ampt) — same params as test_exfiltration; seep alongside.
    rc = swmm_node_set_exfil_params(e, s, 6.0, 4.32, 0.2);  if (rc) { printf("set_exfil rc=%d\n", rc); return 2; }
    rc = swmm_node_set_storage_seep_rate(e, s, 0.5);        if (rc) { printf("set_seep rc=%d\n", rc); return 2; }

    rc = swmm_engine_initialize(e);                          if (rc) { printf("init rc=%d\n", rc); return 2; }
    rc = swmm_engine_start(e, 1);                             if (rc) { printf("start rc=%d\n", rc); return 2; }
    double elapsed = 0.0; int steps = 0;
    do { rc = swmm_engine_step(e, &elapsed); ++steps; } while (rc == SWMM_OK && elapsed > 0.0);
    if (rc != SWMM_OK) { printf("step rc=%d\n", rc); return 2; }
    swmm_engine_close(e);     // finalize/flush the .out
    swmm_engine_destroy(e);
    printf("ran %d steps -> %s\n", steps, out);
    return 0;
}
