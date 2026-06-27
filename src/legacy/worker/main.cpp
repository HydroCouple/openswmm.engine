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
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// Emit the engine's real error message for `rc`, falling back to `context` if
// the engine has no text for it. swmm_getError translates the global ErrorCode
// (set by the failing swmm_* call) into a human-readable string, matching the
// descriptive messages the refactored engine surfaces via swmm_error_message.
static void emitEngineError(int rc, const char *context)
{
    char msg[1024] = {0};
    swmm_getError(msg, sizeof(msg));
    WorkerProgress::emitError(rc, msg[0] ? msg : context);
}

int main(int argc, char *argv[])
{
    // Validate arguments. The optional 4th argument is the progress emit
    // interval in milliseconds (wall clock); the GUI passes its progressTickMs
    // preference. Omitted/invalid → default ~1 Hz; 0 → emit every step.
    if (argc < 4 || argc > 5) {
        WorkerProgress::emitError(1,
            "Usage: openswmm-legacy-worker <inp_path> <rpt_path> <out_path> [tickIntervalMs]");
        return 1;
    }

    const char *inpPath = argv[1];
    const char *rptPath = argv[2];
    const char *outPath = argv[3];
    int tickIntervalMs = 1000;
    if (argc == 5) {
        tickIntervalMs = std::atoi(argv[4]);
        if (tickIntervalMs < 0) tickIntervalMs = 0;
    }

    // Forward each engine warning as a JSON "warning" line. Registered BEFORE
    // swmm_open so warnings issued during input parsing (unknown section/option)
    // are captured too. A captureless lambda converts to the C callback type;
    // the legacy engine is single-threaded per process, so this is safe.
    swmm_setWarningCallback(
        [](const char *msg, void *) { WorkerProgress::emitWarning(msg); },
        nullptr);

    // Step 1: Open the model
    int rc = swmm_open(inpPath, rptPath, outPath);
    if (rc != 0) {
        emitEngineError(rc, "swmm_open failed");
        return rc;
    }

    // Step 2: Initialize (start) the simulation
    rc = swmm_start(1);  // 1 = save results to output file
    if (rc != 0) {
        emitEngineError(rc, "swmm_start failed");
        swmm_close();
        return rc;
    }

    // Emit the simulation window once so the parent can derive a 0–1
    // progress fraction (and readable date columns) from the per-step
    // elapsed-days value. System date properties (< 100) are dispatched to
    // getSystemValue, which ignores the index argument.
    WorkerProgress::emitDates(swmm_getValue(swmm_STARTDATE, 0),
                              swmm_getValue(swmm_ENDDATE, 0));

    // Emit an initial step-0 progress so the GUI row paints at 0 % immediately,
    // before the first (possibly slow) step. Continuity is not yet meaningful.
    WorkerProgress::emitProgress(0, 0.0, 0.0, 0.0);

    // Step 3: Step loop — advance the simulation
    double elapsed = 0.0;
    int stepCount = 0;

    // Rate-limit progress emission by wall-clock (matches the in-process path
    // and honours the GUI's progressTickMs preference). Decoupled from step
    // count so fast models don't flood the pipe and slow models still update.
    const auto tickInterval = std::chrono::milliseconds(tickIntervalMs);
    auto lastEmit = std::chrono::steady_clock::now();
    bool firstStep = true;

    while (true) {
        // Advance one routing step
        rc = swmm_step(&elapsed);

        // Check for fatal error or end of simulation
        if (rc != 0) {
            emitEngineError(rc, "swmm_step failed");
            break;
        }

        // elapsed == 0.0 indicates end of simulation
        if (elapsed <= 0.0) {
            break;
        }

        ++stepCount;

        // Emit on the first step, then no more often than tickInterval.
        const auto now = std::chrono::steady_clock::now();
        if (firstStep || now - lastEmit >= tickInterval) {
            firstStep = false;
            lastEmit = now;
            // Running (timestep-by-timestep) continuity errors, recomputed from
            // the live accumulators. swmm_getRunningMassBalErr returns percent;
            // convert to fractions to match the final "continuity" line.
            float runoffErrPct = 0.0f, flowErrPct = 0.0f;
            swmm_getRunningMassBalErr(&runoffErrPct, &flowErrPct);
            WorkerProgress::emitProgress(stepCount, elapsed,
                                         runoffErrPct / 100.0, flowErrPct / 100.0);
        }
    }

    // Step 4: Finalize
    swmm_end();

    // Report the final mass-balance (continuity) errors to the parent. These
    // are only meaningful after swmm_end(), which closes the mass-balance
    // accounting. swmm_getMassBalErr returns percentages (e.g. 0.05 = 0.05 %);
    // emit them as fractions so the GUI pipeline (which multiplies by 100 for
    // display) shows the same values the refactored engine path reports.
    float runoffErrPct = 0.0f, flowErrPct = 0.0f, qualErrPct = 0.0f;
    swmm_getMassBalErr(&runoffErrPct, &flowErrPct, &qualErrPct);
    WorkerProgress::emitContinuity(runoffErrPct / 100.0, flowErrPct / 100.0);

    swmm_close();

    // Return appropriate exit code
    return (rc == 0) ? 0 : rc;
}
