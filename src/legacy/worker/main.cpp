/*!
 * \file main.cpp
 * \brief Legacy SWMM engine worker process for parallel simulation.
 *
 * Spawned by openswmm.gui's SimulationRunner with arguments:
 *   openswmm-legacy-worker <inp_path> <rpt_path> <out_path>
 *
 * Each worker process has isolated global state, allowing true parallel
 * execution of legacy simulations without global-variable conflicts.
 *
 * Communication with parent (GUI):
 *   - stdout: JSON progress/warning/error lines (one per line)
 *   - stderr: raw warnings/errors for logging
 *   - exit code: 0 on success, non-zero on failure
 */

#include "worker_progress.h"
#include <openswmm/legacy/engine/openswmm_solver.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

int main(int argc, char *argv[])
{
    // Validate arguments
    if (argc != 4) {
        WorkerProgress::emitError(1,
            "Usage: openswmm-legacy-worker <inp_path> <rpt_path> <out_path>");
        return 1;
    }

    const char *inpPath = argv[1];
    const char *rptPath = argv[2];
    const char *outPath = argv[3];

    // Step 1: Open the model
    int rc = swmm_open(inpPath, rptPath, outPath);
    if (rc != 0) {
        WorkerProgress::emitError(rc, "swmm_open failed");
        return rc;
    }

    // Step 2: Initialize (start) the simulation
    rc = swmm_start(1);  // 1 = save results to output file
    if (rc != 0) {
        WorkerProgress::emitError(rc, "swmm_start failed");
        swmm_close();
        return rc;
    }

    // Step 3: Step loop — advance the simulation
    double elapsed = 0.0;
    int stepCount = 0;

    while (true) {
        // Advance one routing step
        rc = swmm_step(&elapsed);

        // Check for fatal error or end of simulation
        if (rc != 0) {
            WorkerProgress::emitError(rc, "swmm_step failed");
            break;
        }

        // elapsed == 0.0 indicates end of simulation
        if (elapsed <= 0.0) {
            break;
        }

        ++stepCount;

        // Emit progress every 10 steps (rate-limit to avoid flooding parent)
        if (stepCount % 10 == 0) {
            WorkerProgress::emitProgress(stepCount, elapsed);
        }
    }

    // Step 4: Finalize
    swmm_end();
    swmm_close();

    // Return appropriate exit code
    return (rc == 0) ? 0 : rc;
}
