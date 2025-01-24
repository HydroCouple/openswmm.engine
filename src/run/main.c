/*!
* \file main.c
* \author L. Rossman
* \date 2021-03-24
* \brief Main stub for the command line version of EPA SWMM 5.2
* \details This is the main stub for the command line version of EPA SWMM 5.2
* to be run with swmm5.dll.
* \version 5.3
*/
#include <string.h>
#include <stdio.h>
#include <time.h>
#include "swmm5.h"

/*!
* \brief Main function for the command line version of EPA SWMM 5.2
* \param[in] argc Number of command line arguments
* \param[in] argv Array of command line arguments
* \return Error status
* \details Runs the command line version of EPA SWMM 5.2.
* Command line may be executed using the following command: 
* ```bash
* runswmm f1 f2 f3
* ```
* where f1 = name of input file (typically with extension .inp),
*       f2 = name of report file (typically with extension .rpt), and
*       f3 = name of binary output file if saved or blank if not 
*            saved (typically with extension .out).
*/
int  main(int argc, char *argv[])
{
    char *inputFile;
    char *reportFile;
    char *binaryFile;
    char *arg1;
    char blank[] = "";
    int  version, vMajor, vMinor, vRelease;
    char errMsg[128];
    int  msgLen = 127;
    time_t start;
    double runTime;
    
    version = swmm_getVersion();
    vMajor = version / 10000;
    vMinor = (version - 10000 * vMajor) / 1000;
    vRelease = (version - 10000 * vMajor - 1000 * vMinor);
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
            printf("\n\nSTORMWATER MANAGEMENT MODEL (SWMM) HELP\n\n");
            printf("COMMANDS:\n");
            printf("\t--help (-h)       SWMM Help\n");
            printf("\t--version (-v)    Build Version\n");
            printf("\nRUNNING A SIMULATION:\n");
            printf("\t runswmm <input file> <report file> <optional output file>\n\n");
        }
        else if (strcmp(arg1, "--version") == 0 || strcmp(arg1, "-v") == 0)
        {
            // Output version number
            printf("\n%d.%d.%0d\n\n", vMajor, vMinor, vRelease);
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
        printf("\n... EPA SWMM %d.%d (Build %d.%d.%0d)\n", vMajor, vMinor,
            vMajor, vMinor, vRelease);

        // --- run SWMM
        swmm_run(inputFile, reportFile, binaryFile);

        // Display closing status on console
        runTime = difftime(time(0), start);
        printf("\n\n... EPA SWMM completed in %.2f seconds.", runTime);
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