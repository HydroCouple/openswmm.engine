/**
 * @file ErrorCodes.hpp
 * @brief Legacy-compatible error and warning codes with description lookup.
 *
 * @details Provides numeric error/warning codes matching the legacy SWMM
 *          error.txt / text.h definitions, plus new codes for 6.0 features.
 *          Each code has a template description string with a `%s` placeholder
 *          for the object name, matching legacy `error_getMsg()` behavior.
 *
 *          Usage:
 *          ```cpp
 *          ctx.errors.push_back(format_error(ERR_TIMESERIES_SEQUENCE, "RAIN1"));
 *          ctx.warnings.push_back(format_warning(WARN_WET_STEP_REDUCED, "RG1"));
 *          ```
 *
 * @see Legacy reference: src/solver/error.txt, src/solver/error.c, src/solver/text.h
 * @ingroup engine_core
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_ENGINE_ERROR_CODES_HPP
#define OPENSWMM_ENGINE_ERROR_CODES_HPP

#include <string>
#include <string_view>

namespace openswmm {

// ============================================================================
// Error codes — matching legacy error.h / error.txt numeric values
// ============================================================================

enum ErrorCode : int {
    ERR_NONE                    = 0,

    // --- System errors (100s) ---
    ERR_MEMORY                  = 101,  ///< memory allocation error
    ERR_KINWAVE                 = 103,  ///< cannot solve KW equations for Link %s
    ERR_ODE_SOLVER              = 105,  ///< cannot open ODE solver
    ERR_TIMESTEP                = 107,  ///< cannot compute a valid time step

    // --- Object validation errors (108–145) ---
    ERR_SUBCATCH_OUTLET         = 108,  ///< ambiguous outlet ID name for Subcatchment %s
    ERR_AQUIFER_PARAMS          = 109,  ///< invalid parameter values for Aquifer %s
    ERR_GROUND_ELEV             = 110,  ///< ground elevation is below water table for Subcatchment %s
    ERR_LENGTH                  = 111,  ///< invalid length for Conduit %s
    ERR_ELEV_DROP               = 112,  ///< elevation drop exceeds length for Conduit %s
    ERR_ROUGHNESS               = 113,  ///< invalid roughness for Conduit %s
    ERR_BARRELS                 = 114,  ///< invalid number of barrels for Conduit %s
    ERR_SLOPE                   = 115,  ///< adverse slope for Conduit %s
    ERR_NO_XSECT                = 117,  ///< no cross section defined for Link %s
    ERR_XSECT                   = 119,  ///< invalid cross section for Link %s
    ERR_NO_CURVE                = 121,  ///< missing or invalid pump curve assigned to Pump %s
    ERR_PUMP_LIMITS             = 122,  ///< startup depth not higher than shutoff depth for Pump %s
    ERR_LOOP                    = 131,  ///< links form cyclic loops in the drainage system
    ERR_MULTI_OUTLET            = 133,  ///< Node %s has more than one outlet link
    ERR_DUMMY_LINK              = 134,  ///< Node %s has illegal DUMMY link connections
    ERR_DIVIDER                 = 135,  ///< Divider %s does not have two outlet links
    ERR_DIVIDER_LINK            = 136,  ///< Divider %s has invalid diversion link
    ERR_WEIR_DIVIDER            = 137,  ///< Weir Divider %s has invalid parameters
    ERR_NODE_DEPTH              = 138,  ///< Node %s has initial depth greater than maximum depth
    ERR_REGULATOR               = 139,  ///< Regulator %s is the outlet of a non-storage node
    ERR_STORAGE_VOLUME          = 140,  ///< Storage node %s has negative volume at full depth
    ERR_OUTFALL                 = 141,  ///< Outfall %s has more than 1 inlet link or an outlet link
    ERR_REGULATOR_SHAPE         = 143,  ///< Regulator %s has invalid cross-section shape
    ERR_NO_OUTLETS              = 145,  ///< Drainage system has no acceptable outlet nodes

    // --- Hydrology errors (151–159) ---
    ERR_UNITHYD_TIMES           = 151,  ///< a Unit Hydrograph in set %s has invalid time base
    ERR_UNITHYD_RATIOS          = 153,  ///< a Unit Hydrograph in set %s has invalid response ratios
    ERR_RDII_AREA               = 155,  ///< invalid sewer area for RDII at node %s
    ERR_RAIN_FILE_CONFLICT      = 156,  ///< ambiguous station ID for Rain Gage %s
    ERR_RAIN_GAGE_FORMAT        = 157,  ///< inconsistent rainfall format for Rain Gage %s
    ERR_RAIN_GAGE_TSERIES       = 158,  ///< time series for Rain Gage %s is also used by another object
    ERR_RAIN_GAGE_INTERVAL      = 159,  ///< recording interval greater than time series interval for Rain Gage %s

    // --- Treatment ---
    ERR_CYCLIC_TREATMENT        = 161,  ///< cyclic dependency in treatment functions at node %s

    // --- Curves / timeseries (171–173) ---
    ERR_CURVE_SEQUENCE          = 171,  ///< Curve %s has invalid or out of sequence data
    ERR_TIMESERIES_SEQUENCE     = 173,  ///< Time Series %s has its data out of sequence

    // --- Snow / LID (181–188) ---
    ERR_SNOWMELT_PARAMS         = 181,  ///< invalid Snow Melt Climatology parameters
    ERR_SNOWPACK_PARAMS         = 182,  ///< invalid parameters for Snow Pack %s
    ERR_LID_TYPE                = 183,  ///< no type specified for LID %s
    ERR_LID_LAYER               = 184,  ///< missing layer for LID %s
    ERR_LID_PARAMS              = 185,  ///< invalid parameter value for LID %s
    ERR_LID_AREAS               = 187,  ///< LID area exceeds total area for Subcatchment %s
    ERR_LID_CAPTURE_AREA        = 188,  ///< LID capture area exceeds total impervious area for Subcatchment %s

    // --- Date / reporting (191–195) ---
    ERR_START_DATE              = 191,  ///< simulation start date comes after ending date
    ERR_REPORT_DATE             = 193,  ///< report start date comes after ending date
    ERR_REPORT_STEP             = 195,  ///< reporting time step or duration is less than routing time step

    // --- Input parsing (200–235) ---
    ERR_INPUT                   = 200,  ///< one or more errors in input file
    ERR_INPUT_LINE              = 201,  ///< too many characters in input line
    ERR_ITEMS                   = 203,  ///< too few items
    ERR_KEYWORD                 = 205,  ///< invalid keyword %s
    ERR_DUP_NAME                = 207,  ///< duplicate ID name %s
    ERR_NAME                    = 209,  ///< undefined object %s
    ERR_NUMBER                  = 211,  ///< invalid number %s
    ERR_DATETIME                = 213,  ///< invalid date/time %s
    ERR_CONTROL_RULE            = 217,  ///< control rule clause invalid or out of sequence
    ERR_TRANSECT_UNKNOWN        = 219,  ///< data provided for unidentified transect
    ERR_TRANSECT_SEQUENCE       = 221,  ///< transect station out of sequence
    ERR_TRANSECT_TOO_FEW        = 223,  ///< Transect %s has too few stations
    ERR_TRANSECT_TOO_MANY       = 225,  ///< Transect %s has too many stations
    ERR_TRANSECT_MANNING        = 227,  ///< Transect %s has no Manning's N
    ERR_TRANSECT_OVERBANK       = 229,  ///< Transect %s has invalid overbank locations
    ERR_TRANSECT_NO_DEPTH       = 231,  ///< Transect %s has no depth
    ERR_MATH_EXPR               = 233,  ///< invalid math expression
    ERR_INFILTRATION            = 235,  ///< invalid infiltration parameters

    // --- File I/O (301–363) ---
    ERR_FILE_NAME               = 301,  ///< files share same names
    ERR_INP_FILE                = 303,  ///< cannot open input file
    ERR_RPT_FILE                = 305,  ///< cannot open report file
    ERR_OUT_FILE                = 307,  ///< cannot open binary results file
    ERR_OUT_SIZE                = 308,  ///< output will exceed maximum file size
    ERR_OUT_WRITE               = 309,  ///< error writing to binary results file
    ERR_OUT_READ                = 311,  ///< error reading from binary results file
    ERR_RAIN_IFACE_SCRATCH      = 313,  ///< cannot open scratch rainfall interface file
    ERR_RAIN_IFACE              = 315,  ///< cannot open rainfall interface file %s
    ERR_RAIN_FILE_OPEN          = 317,  ///< cannot open rainfall data file %s
    ERR_RAIN_FILE_SEQUENCE      = 318,  ///< line is out of sequence in rainfall data file %s
    ERR_RAIN_FILE_FORMAT        = 319,  ///< unknown format for rainfall data file %s
    ERR_RAIN_IFACE_FORMAT       = 320,  ///< invalid format for rainfall interface file
    ERR_RAIN_IFACE_GAGE         = 321,  ///< no data in rainfall interface file for gage %s
    ERR_RUNOFF_IFACE            = 323,  ///< cannot open runoff interface file %s
    ERR_RUNOFF_IFACE_COMPAT     = 325,  ///< incompatible data found in runoff interface file
    ERR_RUNOFF_IFACE_EOF        = 327,  ///< attempting to read beyond end of runoff interface file
    ERR_RUNOFF_IFACE_READ       = 329,  ///< error in reading from runoff interface file
    ERR_HOTSTART_FILE           = 331,  ///< cannot open hot start interface file %s
    ERR_HOTSTART_COMPAT         = 333,  ///< incompatible data found in hot start interface file
    ERR_HOTSTART_READ           = 335,  ///< error in reading from hot start interface file
    ERR_NO_CLIMATE_FILE         = 336,  ///< no climate file specified for evaporation and/or wind speed
    ERR_CLIMATE_FILE_OPEN       = 337,  ///< cannot open climate file %s
    ERR_CLIMATE_FILE_READ       = 338,  ///< error in reading from climate file %s
    ERR_CLIMATE_FILE_EOF        = 339,  ///< attempt to read beyond end of climate file %s
    ERR_RDII_IFACE_SCRATCH      = 341,  ///< cannot open scratch RDII interface file
    ERR_RDII_IFACE              = 343,  ///< cannot open RDII interface file %s
    ERR_RDII_IFACE_FORMAT       = 345,  ///< invalid format for RDII interface file
    ERR_ROUTING_IFACE           = 351,  ///< cannot open routing interface file %s
    ERR_ROUTING_IFACE_FORMAT    = 353,  ///< invalid format for routing interface file %s
    ERR_ROUTING_IFACE_NAMES     = 355,  ///< mis-matched names in routing interface file %s
    ERR_ROUTING_IFACE_SAME      = 357,  ///< inflows and outflows interface files have same name
    ERR_TABLE_FILE_OPEN         = 361,  ///< could not open external file used for Time Series %s
    ERR_TABLE_FILE_READ         = 363,  ///< invalid data in external file used for Time Series %s

    // --- System / API errors (500–509) — legacy API error keys ---
    ERR_SYSTEM                  = 500,  ///< System exception thrown
    ERR_API_NOT_OPENED          = 501,  ///< project not opened
    ERR_API_NOT_STARTED         = 502,  ///< simulation not started
    ERR_API_NOT_ENDED           = 503,  ///< simulation not ended
    ERR_API_OBJECT_TYPE         = 504,  ///< invalid object type
    ERR_API_OBJECT_INDEX        = 505,  ///< invalid object index
    ERR_API_OBJECT_NAME         = 506,  ///< invalid object name
    ERR_API_PROPERTY_TYPE       = 507,  ///< invalid property type
    ERR_API_PROPERTY_VALUE      = 508,  ///< invalid property value
    ERR_API_TIME_PERIOD         = 509,  ///< invalid time period

    // --- New 6.0 errors (600+) ---
    ERR_TIMESERIES_EMPTY        = 601,  ///< Time Series %s has no data
    ERR_TIMESERIES_NAN          = 603,  ///< Time Series %s contains NaN or Inf values
    ERR_TABLE_COL_MISMATCH      = 605,  ///< column count mismatch in data for %s
    ERR_GAGE_TSERIES_NOTFOUND   = 607,  ///< Rain Gage %s references unknown time series
};

// ============================================================================
// CFFI error codes — categorical codes from the C API
// ============================================================================
// These map to the SWMM_ErrorCode enum in openswmm_engine.h and provide
// broad categories. Use error_get_cffi_template() to get descriptions.

enum CffiErrorCode : int {
    CFFI_OK              = 0,
    CFFI_ERR_NOMEM       = 1,   ///< memory allocation error
    CFFI_ERR_INPFILE     = 2,   ///< cannot open input file
    CFFI_ERR_RPTFILE     = 3,   ///< cannot open report file
    CFFI_ERR_OUTFILE     = 4,   ///< cannot open output file
    CFFI_ERR_PARSE       = 5,   ///< input parsing error
    CFFI_ERR_LIFECYCLE   = 6,   ///< invalid engine lifecycle state transition
    CFFI_ERR_BADHANDLE   = 7,   ///< invalid engine handle
    CFFI_ERR_BADINDEX    = 8,   ///< invalid object index
    CFFI_ERR_BADPARAM    = 9,   ///< invalid parameter value
    CFFI_ERR_PLUGIN      = 10,  ///< plugin initialization or execution error
    CFFI_ERR_IO          = 11,  ///< file I/O error
    CFFI_ERR_HOTSTART    = 12,  ///< hot start file error
    CFFI_ERR_CRS         = 13,  ///< coordinate reference system error
    CFFI_ERR_NUMERICAL   = 14,  ///< numerical solver error
    CFFI_ERR_INTERNAL    = 99,  ///< internal engine error
};

// ============================================================================
// CFFI warning codes
// ============================================================================

enum CffiWarnCode : int {
    CFFI_WARN_NONE              = 0,
    CFFI_WARN_HOTSTART_MISSING  = 1,   ///< hot start file not found, cold start used
    CFFI_WARN_UNKNOWN_SECTION   = 2,   ///< unknown section in input file
    CFFI_WARN_UNKNOWN_OPTION    = 3,   ///< unknown option in input file
    CFFI_WARN_DEPRECATED_KW     = 4,   ///< deprecated keyword used
    CFFI_WARN_PLUGIN_INIT       = 5,   ///< plugin initialization warning
    CFFI_WARN_NUMERICAL         = 6,   ///< numerical approximation used
    CFFI_WARN_STABILITY_LIMIT   = 7,   ///< time step reduced for stability
};

// ============================================================================
// Warning codes — matching legacy text.h WARN01–WARN12
// ============================================================================

enum WarnCode : int {
    WARN_NONE                   = 0,
    WARN_WET_STEP_REDUCED       = 1,   ///< wet weather time step reduced to recording interval for Rain Gage %s
    WARN_MAX_DEPTH_INCREASED    = 2,   ///< maximum depth increased for Node %s
    WARN_NEGATIVE_OFFSET        = 3,   ///< negative offset ignored for Link %s
    WARN_MIN_ELEV_DROP          = 4,   ///< minimum elevation drop used for Conduit %s
    WARN_MIN_SLOPE              = 5,   ///< minimum slope used for Conduit %s
    WARN_DRY_STEP_INCREASED     = 6,   ///< dry weather time step increased to the wet weather time step
    WARN_ROUTING_STEP_REDUCED   = 7,   ///< routing time step reduced to the wet weather time step
    WARN_ELEV_DROP_EXCEEDS      = 8,   ///< elevation drop exceeds length for Conduit %s
    WARN_GAGE_INTERVAL          = 9,   ///< time series interval greater than recording interval for Rain Gage %s
    WARN_REGULATOR_CREST_LOW    = 10,  ///< crest elevation is below downstream invert for regulator Link %s
    WARN_CONTROL_RULE_ATTR      = 11,  ///< non-matching attributes in Control Rule %s
    WARN_INLET_REMOVED          = 12,  ///< inlet removed due to unsupported shape for Conduit %s

    // --- New 6.0 warnings (100+) ---
    WARN_TIMESERIES_DUPLICATE_X = 101, ///< Time Series %s has duplicate x values
    WARN_BOUNDARY_OVERLAP       = 102, ///< boundary regions overlap for %s
};

// ============================================================================
// Lookup and formatting
// ============================================================================

/**
 * @brief Get the description template for an error code.
 *
 * @details Returns the legacy-compatible description string with `%s`
 *          placeholder for the object name. Returns empty string for
 *          unknown codes.
 *
 * @param code  Error code (from ErrorCode enum).
 * @returns     Description template string (e.g., "could not open external
 *              file used for Time Series %s.").
 */
std::string_view error_get_template(int code) noexcept;

/**
 * @brief Get the description template for a warning code.
 * @param code  Warning code (from WarnCode enum).
 * @returns     Description template string.
 */
std::string_view warning_get_template(int code) noexcept;

/**
 * @brief Format an error message with object name substitution.
 *
 * @details Produces output like:
 *          `"  ERROR 173: Time Series RAIN1 has its data out of sequence."`
 *
 * @param code  Error code.
 * @param name  Object name to substitute for `%s`.
 * @returns     Formatted error message string.
 */
std::string format_error(int code, std::string_view name = "");

/**
 * @brief Format an error message with object name and additional detail.
 *
 * @details Produces output like:
 *          `"  ERROR 173: Time Series RAIN1 has its data out of sequence at 01/01/2024 02:00:00."`
 *
 * @param code    Error code.
 * @param name    Object name to substitute for `%s`.
 * @param detail  Additional detail appended after the template.
 * @returns       Formatted error message string.
 */
std::string format_error(int code, std::string_view name, std::string_view detail);

/**
 * @brief Format a warning message with object name substitution.
 *
 * @param code  Warning code.
 * @param name  Object name to substitute for `%s`.
 * @returns     Formatted warning message string.
 */
std::string format_warning(int code, std::string_view name = "");

/**
 * @brief Format a warning with additional detail.
 */
std::string format_warning(int code, std::string_view name, std::string_view detail);

// ============================================================================
// CFFI / C API code lookup
// ============================================================================

/**
 * @brief Get the description for a CFFI error code (SWMM_ErrorCode).
 * @param code  CFFI error code (from CffiErrorCode / SWMM_ErrorCode enum).
 * @returns     Description string.
 */
std::string_view cffi_error_get_template(int code) noexcept;

/**
 * @brief Get the description for a CFFI warning code (SWMM_WarnCode).
 * @param code  CFFI warning code (from CffiWarnCode / SWMM_WarnCode enum).
 * @returns     Description string.
 */
std::string_view cffi_warning_get_template(int code) noexcept;

/**
 * @brief Format a CFFI error message.
 * @param code  CFFI error code.
 * @param name  Optional object name for context.
 * @returns     Formatted string like "  CFFI ERROR 5: input parsing error."
 */
std::string format_cffi_error(int code, std::string_view name = "");

/**
 * @brief Format a CFFI warning message.
 * @param code  CFFI warning code.
 * @param name  Optional object name for context.
 * @returns     Formatted string like "  CFFI WARNING 2: unknown section in input file."
 */
std::string format_cffi_warning(int code, std::string_view name = "");

} /* namespace openswmm */

#endif /* OPENSWMM_ENGINE_ERROR_CODES_HPP */
