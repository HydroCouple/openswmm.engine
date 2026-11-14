/**
 * @file      main.c
 * @author    L. Rossman
 * @date      2021-03-24
 * @brief     Main stub for the command line version of EPA SWMM 5.3
 * @details This is the main stub for the command line version of EPA SWMM 5.3
 * to be run with swmm5.dll.
 * @version 5.3
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com> (OpenSWMM maintenance and extensions)
 * @copyright Copyright (c) 2026 Caleb Buahin. OpenSWMM contributions only; original EPA SWMM
 *            credits are retained.
 * @license   Apache-2.0 for OpenSWMM contributions; original EPA SWMM provenance is retained.
 */

#include <string.h>
#include <stdio.h>
#include <time.h>
#include "openswmm_solver.h"
#include "legacy_version.h"

/**
 * @brief Dispatch legacy CLI help/version or a complete global-engine run.
 *
 * Prints help/version for recognized switches, otherwise invokes swmm_run and displays elapsed wall
 * time plus global error/warning status. The legacy run routine owns its open/start/end/close
 * sequence and may create/overwrite report/output files. This CLI does not return the engine
 * failure status to its caller.
 *
 * @param[in] argc Process argument count: one prints missing-argument text, two interprets a
 *     switch, and three or more runs a model.
 * @param[in] argv Borrowed input/report/optional binary-output filenames after program name; an
 *     omitted output uses an empty scratch filename. Additional arguments are ignored.
 * @return Always zero, including invalid arguments and engine failures; diagnostics must be
 *     inspected separately.
 */
int main(int argc, char* argv[]) {
    char* inputFile;
    char* reportFile;
    char* binaryFile;
    char* arg1;
    char blank[] = "";
    char errMsg[128];
    int msgLen = 127;
    time_t start;
    double runTime;

    start = time(0);

    // --- check for proper number of command line arguments
    if (argc == 1) {
        printf("\nNot Enough Arguments (See Help --help)\n\n");
    } else if (argc == 2) {
        // --- extract first argument
        arg1 = argv[1];

        if (strcmp(arg1, "--help") == 0 || strcmp(arg1, "-h") == 0) {
            // Help
            printf("\n\nOPEN-SOURCE STORMWATER MANAGEMENT MODEL (SWMM) HELP\n\n");
            printf("COMMANDS:\n");
            printf("\t--help (-h)       SWMM Help\n");
            printf("\t--version (-v)    Build Version\n");
            printf("\nRUNNING A SIMULATION:\n");
            printf("\t runswmm <input file> <report file> <optional output file>\n\n");
        } else if (strcmp(arg1, "--version") == 0 || strcmp(arg1, "-v") == 0) {
            // Output version number
            printf("\n%s (openswmm.legacy %s)\n\n", LEGACY_SWMM_VERSION_FULL,
                   OPENSWMM_LEGACY_FULL_VERSION);
        } else {
            printf("\nUnknown Argument (See Help --help)\n\n");
        }
    } else {
        // --- extract file names from command line arguments
        inputFile = argv[1];
        reportFile = argv[2];
        if (argc > 3)
            binaryFile = argv[3];
        else
            binaryFile = blank;
        printf("\n... EPA SWMM %d.%d (Build %s)\n", LEGACY_SWMM_VERSION_MAJOR,
               LEGACY_SWMM_VERSION_MINOR, LEGACY_SWMM_VERSION_FULL);

        // --- run SWMM
        swmm_run(inputFile, reportFile, binaryFile);

        // Display closing status on console
        runTime = difftime(time(0), start);
        printf("\n\n... OPEN-SOURCE SWMM completed in %.2f seconds.", runTime);
        if (swmm_getError(errMsg, msgLen) > 0)
            printf(" There are errors.\n");
        else if (swmm_getWarnings() > 0)
            printf(" There are warnings.\n");
        else
            printf("\n");
    }

    // --- Use the code below if you need to keep the console window visible
    /*
    printf("    Press Enter to continue...");
    getchar();
*/

    return 0;
}