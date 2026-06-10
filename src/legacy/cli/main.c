/*!
* \file main.c
* \author L. Rossman
* \date 2021-03-24
* \brief Main stub for the command line version of EPA SWMM 5.3
* \details This is the main stub for the command line version of EPA SWMM 5.3
* to be run with swmm5.dll.
* \version 5.3
*/
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include "openswmm_solver.h"
#include "legacy_version.h"

/* Seconds elapsed between two timespec marks (monotonic clock). */
static double secs_between(const struct timespec *a, const struct timespec *b)
{
    return (double)(b->tv_sec - a->tv_sec)
         + (double)(b->tv_nsec - a->tv_nsec) * 1e-9;
}

/*!
* \brief Main function for the command line version of EPA SWMM 5.3
* \param[in] argc Number of command line arguments
* \param[in] argv Array of command line arguments
* \return Error status
* \details Runs the command line version of EPA SWMM 5.2.
* Command line may be executed using the following command: 
* ```bash
* runswmm input_file report_file [output_file]
* ```
* where input_file  = name of input file (typically with extension .inp),
*       report_file = name of report file (typically with extension .rpt), and
*       output_file = name of binary output file if saved or blank if not 
*                     aved (typically with extension .out).
*/
int  main(int argc, char *argv[])
{
    char *inputFile;
    char *reportFile;
    char *binaryFile;
    char *arg1;
    char blank[] = "";
    char errMsg[128];
    int  msgLen = 127;
    time_t start;
    double runTime;

    start = time(0);

    // --- check for proper number of command line arguments
    if (argc == 1)
    {
        printf("\nNot Enough Arguments (See Help --help)\n\n");
    }
    else if (argc == 2)
    {
        // --- extract first argument
        arg1 = argv[1];

        if (strcmp(arg1, "--help") == 0 || strcmp(arg1, "-h") == 0)
        {
            // Help
            printf("\n\nOPEN-SOURCE STORMWATER MANAGEMENT MODEL (SWMM) HELP\n\n");
            printf("COMMANDS:\n");
            printf("\t--help (-h)       SWMM Help\n");
            printf("\t--version (-v)    Build Version\n");
            printf("\nRUNNING A SIMULATION:\n");
            printf("\t runswmm <input file> <report file> <optional output file>\n\n");
        }
        else if (strcmp(arg1, "--version") == 0 || strcmp(arg1, "-v") == 0)
        {
            // Output version number
            printf("\n%s (openswmm.legacy %s)\n\n",
                LEGACY_SWMM_VERSION_FULL, OPENSWMM_LEGACY_FULL_VERSION);
        }
        else
        {
            printf("\nUnknown Argument (See Help --help)\n\n");
        }
    }
    else
    {
        // --- extract file names from command line arguments
        inputFile = argv[1];
        reportFile = argv[2];
        if (argc > 3) binaryFile = argv[3];
        else          binaryFile = blank;
        printf("\n... EPA SWMM %d.%d (Build %s)\n",
            LEGACY_SWMM_VERSION_MAJOR, LEGACY_SWMM_VERSION_MINOR,
            LEGACY_SWMM_VERSION_FULL);

        // --- run SWMM via the documented open/start/step/end/report/close
        //     sequence (the exact body of swmm_run, swmm5.c:437-495) so each
        //     phase can be timed.  swmm_run only invokes swmm_report() for a
        //     scratch output file, i.e. when no binary-output path was given;
        //     the condition is mirrored here as binaryFile[0] == '\0'.
        {
            double elapsedTime = 0.0;
            long stepCount = 0;
            int err;
            struct timespec tpBegin, tpOpen1, tpStart0, tpStart1,
                            tpLoop1, tpEnd1, tpRpt1, tpDone;
            double tEnd = 0.0, tReport = 0.0;

            clock_gettime(CLOCK_MONOTONIC, &tpBegin);
            err = swmm_open(inputFile, reportFile, binaryFile);
            clock_gettime(CLOCK_MONOTONIC, &tpOpen1);
            tpStart0 = tpStart1 = tpLoop1 = tpEnd1 = tpRpt1 = tpOpen1;

            if (!err)
            {
                clock_gettime(CLOCK_MONOTONIC, &tpStart0);
                err = swmm_start(1);  /* TRUE: save results, as swmm_run does */
                clock_gettime(CLOCK_MONOTONIC, &tpStart1);
                tpLoop1 = tpStart1;

                if (!err)
                {
                    do
                    {
                        err = swmm_step(&elapsedTime);
                        stepCount++;
                    } while (elapsedTime > 0.0 && !err);
                    clock_gettime(CLOCK_MONOTONIC, &tpLoop1);
                    printf("\n... %ld steps completed.\n", stepCount);
                }

                swmm_end();
                clock_gettime(CLOCK_MONOTONIC, &tpEnd1);
                tEnd = secs_between(&tpLoop1, &tpEnd1);
                tpRpt1 = tpEnd1;

                if (!err && binaryFile[0] == '\0')
                {
                    swmm_report();
                    clock_gettime(CLOCK_MONOTONIC, &tpRpt1);
                    tReport = secs_between(&tpEnd1, &tpRpt1);
                }
            }

            swmm_close();
            clock_gettime(CLOCK_MONOTONIC, &tpDone);

            printf("PHASE_TIMING open=%.4f init=0.0000 start=%.4f "
                   "step_loop=%.4f steps=%ld end=%.4f report=%.4f total=%.4f\n",
                   secs_between(&tpBegin, &tpOpen1),
                   secs_between(&tpStart0, &tpStart1),
                   secs_between(&tpStart1, &tpLoop1),
                   stepCount, tEnd, tReport,
                   secs_between(&tpBegin, &tpDone));
        }

        // Display closing status on console
        runTime = difftime(time(0), start);
        printf("\n\n... OPEN-SOURCE SWMM completed in %.2f seconds.", runTime);
        if      ( swmm_getError(errMsg, msgLen) > 0 ) printf(" There are errors.\n");
        else if ( swmm_getWarnings() > 0 ) printf(" There are warnings.\n");
        else printf("\n");
    }

// --- Use the code below if you need to keep the console window visible
/* 
    printf("    Press Enter to continue...");
    getchar();
*/

    return 0;
}