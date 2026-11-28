/**
 * @file      error.c
 * @brief     Error messages
 * @author    L. Rossman
 * @date      Created: 2021-11-01
 * @date      Last updated: 2024-12-30
 * @version 5.3.0
 * @details
 *
 *   Error codes used in the SWMM model.
 *
 *   Update History
 *   ==============
 *   - Build 5.1.008:
 *       - Text of Error 217 for control rules modified.
 *   - Build 5.1.010:
 *       - Text of Error 318 for rainfall data files modified.
 *   - Build 5.1.015:
 *       - Added new Error 140 for storage nodes.
 *   - Build 5.2.0:
 *       - Re-designed error message system.
 *       - Added new Error 235 for invalid infiltration parameters.
 * ----------------------------------------------------------------------
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com> (OpenSWMM maintenance and extensions)
 * @copyright Copyright (c) 2026 Caleb Buahin. OpenSWMM contributions only; original EPA SWMM
 *            credits are retained.
 * @license   Apache-2.0 for OpenSWMM contributions; original EPA SWMM provenance is retained.
 */

#define _CRT_SECURE_NO_DEPRECATE

#include <string.h>
#include "consts.h"
#include "error.h"

char ErrString[MAXMSG];

/**
 * @brief Copy the catalog message for an error code into caller storage.
 * @par Implementation arguments
 * - errCode (in): Catalog error code; unknown codes select an empty string.
 * - msg (out): Non-null caller-owned buffer with at least MAXMSG bytes,
 * and enough room for the selected catalog message plus terminator.
 * @return The same borrowed msg pointer; it is not retained or allocated.
 */
char* error_getMsg(int errCode, char* msg) {
    switch (errCode) {

#ifdef _MSC_VER
#define ERR(code, string)                                                                          \
    case code:                                                                                     \
        strncpy_s(msg, MAXMSG, string, MAXMSG);                                                    \
        break;
#include "error.txt"
#undef ERR
#else
#define ERR(code, string)                                                                          \
    case code:                                                                                     \
        strcpy(msg, string);                                                                       \
        break;
#include "error.txt"
#undef ERR
#endif
    default:
#ifdef _MSC_VER
        strncpy_s(msg, MAXMSG, "", MAXMSG);
#else
        strcpy(msg, "");
#endif
    }
    return (msg);
};

/**
 * @brief Set the shared input-error token and pass through its code.
 * @par Implementation arguments
 * - errcode (in): Input error code to return unchanged.
 * - s (in): Non-null borrowed null-terminated diagnostic token that fits
 * in ErrString, including its terminator; copied with unbounded strcpy.
 * @return errcode. The global ErrString is overwritten; ErrorCode itself is not set here.
 */
int error_setInpError(int errcode, char* s) {
    strcpy(ErrString, s);
    return errcode;
}
