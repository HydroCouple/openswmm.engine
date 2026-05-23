/**
 * @file ErrorCodes.cpp
 * @brief Error/warning description table and formatting — legacy-compatible.
 *
 * @details Description strings are taken verbatim from the legacy SWMM
 *          error.txt and text.h, preserving the `%s` placeholder convention.
 *          New 6.0 codes (600+) follow the same pattern.
 *
 * @see ErrorCodes.hpp
 * @see Legacy reference: src/solver/error.txt, src/solver/error.c
 * @ingroup engine_core
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#include "ErrorCodes.hpp"

#include <cstdio>
#include <unordered_map>

namespace openswmm {

// ============================================================================
// Error description table — from legacy error.txt
// ============================================================================

static const std::unordered_map<int, const char*>& error_table() {
    static const std::unordered_map<int, const char*> table = {
        // System
        {101, "memory allocation error."},
        {103, "cannot solve KW equations for Link %s."},
        {105, "cannot open ODE solver."},
        {107, "cannot compute a valid time step."},

        // Object validation
        {108, "ambiguous outlet ID name for Subcatchment %s."},
        {109, "invalid parameter values for Aquifer %s."},
        {110, "ground elevation is below water table for Subcatchment %s."},
        {111, "invalid length for Conduit %s."},
        {112, "elevation drop exceeds length for Conduit %s."},
        {113, "invalid roughness for Conduit %s."},
        {114, "invalid number of barrels for Conduit %s."},
        {115, "adverse slope for Conduit %s."},
        {117, "no cross section defined for Link %s."},
        {119, "invalid cross section for Link %s."},
        {121, "missing or invalid pump curve assigned to Pump %s."},
        {122, "startup depth not higher than shutoff depth for Pump %s."},
        {131, "the following links form cyclic loops in the drainage system:"},
        {133, "Node %s has more than one outlet link."},
        {134, "Node %s has illegal DUMMY link connections."},
        {135, "Divider %s does not have two outlet links."},
        {136, "Divider %s has invalid diversion link."},
        {137, "Weir Divider %s has invalid parameters."},
        {138, "Node %s has initial depth greater than maximum depth."},
        {139, "Regulator %s is the outlet of a non-storage node."},
        {140, "Storage node %s has negative volume at full depth."},
        {141, "Outfall %s has more than 1 inlet link or an outlet link."},
        {143, "Regulator %s has invalid cross-section shape."},
        {145, "Drainage system has no acceptable outlet nodes."},

        // Hydrology
        {151, "a Unit Hydrograph in set %s has invalid time base."},
        {153, "a Unit Hydrograph in set %s has invalid response ratios."},
        {155, "invalid sewer area for RDII at node %s."},
        {156, "ambiguous station ID for Rain Gage %s."},
        {157, "inconsistent rainfall format for Rain Gage %s."},
        {158, "time series for Rain Gage %s is also used by another object."},
        {159, "recording interval greater than time series interval for Rain Gage %s."},

        // Treatment
        {161, "cyclic dependency in treatment functions at node %s."},

        // Curves / timeseries
        {171, "Curve %s has invalid or out of sequence data."},
        {173, "Time Series %s has its data out of sequence."},

        // Snow / LID
        {181, "invalid Snow Melt Climatology parameters."},
        {182, "invalid parameters for Snow Pack %s."},
        {183, "no type specified for LID %s."},
        {184, "missing layer for LID %s."},
        {185, "invalid parameter value for LID %s."},
        {187, "LID area exceeds total area for Subcatchment %s."},
        {188, "LID capture area exceeds total impervious area for Subcatchment %s."},

        // Date / reporting
        {191, "simulation start date comes after ending date."},
        {193, "report start date comes after ending date."},
        {195, "reporting time step or duration is less than routing time step."},

        // Input parsing
        {200, "one or more errors in input file."},
        {201, "too many characters in input line."},
        {203, "too few items."},
        {205, "invalid keyword %s."},
        {207, "duplicate ID name %s."},
        {209, "undefined object %s."},
        {211, "invalid number %s."},
        {213, "invalid date/time %s."},
        {217, "control rule clause invalid or out of sequence."},
        {219, "data provided for unidentified transect."},
        {221, "transect station out of sequence."},
        {223, "Transect %s has too few stations."},
        {225, "Transect %s has too many stations."},
        {227, "Transect %s has no Manning's N."},
        {229, "Transect %s has invalid overbank locations."},
        {231, "Transect %s has no depth."},
        {233, "invalid math expression."},
        {235, "invalid infiltration parameters."},

        // File I/O
        {301, "files share same names."},
        {303, "cannot open input file."},
        {305, "cannot open report file."},
        {307, "cannot open binary results file."},
        {308, "amount of output produced will exceed maximum file size."},
        {309, "error writing to binary results file."},
        {311, "error reading from binary results file."},
        {313, "cannot open scratch rainfall interface file."},
        {315, "cannot open rainfall interface file %s."},
        {317, "cannot open rainfall data file %s."},
        {318, "the following line is out of sequence in rainfall data file %s."},
        {319, "unknown format for rainfall data file %s."},
        {320, "invalid format for rainfall interface file."},
        {321, "no data in rainfall interface file for gage %s."},
        {323, "cannot open runoff interface file %s."},
        {325, "incompatible data found in runoff interface file."},
        {327, "attempting to read beyond end of runoff interface file."},
        {329, "error in reading from runoff interface file."},
        {331, "cannot open hot start interface file %s."},
        {333, "incompatible data found in hot start interface file."},
        {335, "error in reading from hot start interface file."},
        {336, "no climate file specified for evaporation and/or wind speed."},
        {337, "cannot open climate file %s."},
        {338, "error in reading from climate file %s."},
        {339, "attempt to read beyond end of climate file %s."},
        {341, "cannot open scratch RDII interface file."},
        {343, "cannot open RDII interface file %s."},
        {345, "invalid format for RDII interface file."},
        {351, "cannot open routing interface file %s."},
        {353, "invalid format for routing interface file %s."},
        {355, "mis-matched names in routing interface file %s."},
        {357, "inflows and outflows interface files have same name."},
        {361, "could not open external file used for Time Series %s."},
        {363, "invalid data in external file used for Time Series %s."},

        // System / API (500–509)
        {500, "System exception thrown."},
        {501, "project not opened."},
        {502, "simulation not started."},
        {503, "simulation not ended."},
        {504, "invalid object type."},
        {505, "invalid object index."},
        {506, "invalid object name."},
        {507, "invalid property type."},
        {508, "invalid property value."},
        {509, "invalid time period."},

        // New 6.0
        {601, "Time Series %s has no data."},
        {603, "Time Series %s contains NaN or Inf values."},
        {605, "column count mismatch in data for %s."},
        {607, "Rain Gage %s references unknown time series."},
    };
    return table;
}

// ============================================================================
// Warning description table — from legacy text.h
// ============================================================================

static const std::unordered_map<int, const char*>& warning_table() {
    static const std::unordered_map<int, const char*> table = {
        {1,  "wet weather time step reduced to recording interval for Rain Gage %s."},
        {2,  "maximum depth increased for Node %s."},
        {3,  "negative offset ignored for Link %s."},
        {4,  "minimum elevation drop used for Conduit %s."},
        {5,  "minimum slope used for Conduit %s."},
        {6,  "dry weather time step increased to the wet weather time step."},
        {7,  "routing time step reduced to the wet weather time step."},
        {8,  "elevation drop exceeds length for Conduit %s."},
        {9,  "time series interval greater than recording interval for Rain Gage %s."},
        {10, "crest elevation is below downstream invert for regulator Link %s."},
        {11, "non-matching attributes in Control Rule %s."},
        {12, "inlet removed due to unsupported shape for Conduit %s."},

        // New 6.0
        {101, "Time Series %s has duplicate x values."},
        {102, "boundary regions overlap for %s."},
    };
    return table;
}

// ============================================================================
// Template lookup
// ============================================================================

std::string_view error_get_template(int code) noexcept {
    auto& t = error_table();
    auto it = t.find(code);
    return (it != t.end()) ? std::string_view(it->second) : std::string_view{};
}

std::string_view warning_get_template(int code) noexcept {
    auto& t = warning_table();
    auto it = t.find(code);
    return (it != t.end()) ? std::string_view(it->second) : std::string_view{};
}

// ============================================================================
// Formatting helpers
// ============================================================================

/**
 * @brief Substitute the first `%s` in a template with a name string.
 */
static std::string substitute(std::string_view tmpl, std::string_view name) {
    std::string result;
    result.reserve(tmpl.size() + name.size());
    auto pos = tmpl.find("%s");
    if (pos != std::string_view::npos) {
        result.append(tmpl.data(), pos);
        result.append(name);
        result.append(tmpl.data() + pos + 2, tmpl.size() - pos - 2);
    } else {
        result.append(tmpl);
    }
    return result;
}

std::string format_error(int code, std::string_view name) {
    auto tmpl = error_get_template(code);
    if (tmpl.empty()) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "  ERROR %d: unknown error.", code);
        return buf;
    }
    char prefix[32];
    std::snprintf(prefix, sizeof(prefix), "  ERROR %d: ", code);
    return std::string(prefix) + substitute(tmpl, name);
}

std::string format_error(int code, std::string_view name, std::string_view detail) {
    std::string msg = format_error(code, name);
    if (!detail.empty()) {
        // Insert detail before the trailing period (if any)
        if (!msg.empty() && msg.back() == '.') {
            msg.pop_back();
            msg += " ";
            msg.append(detail);
            msg += ".";
        } else {
            msg += " ";
            msg.append(detail);
        }
    }
    return msg;
}

std::string format_warning(int code, std::string_view name) {
    auto tmpl = warning_get_template(code);
    if (tmpl.empty()) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "  WARNING %02d: unknown warning.", code);
        return buf;
    }
    char prefix[32];
    std::snprintf(prefix, sizeof(prefix), "  WARNING %02d: ", code);
    return std::string(prefix) + substitute(tmpl, name);
}

std::string format_warning(int code, std::string_view name, std::string_view detail) {
    std::string msg = format_warning(code, name);
    if (!detail.empty()) {
        if (!msg.empty() && msg.back() == '.') {
            msg.pop_back();
            msg += " ";
            msg.append(detail);
            msg += ".";
        } else {
            msg += " ";
            msg.append(detail);
        }
    }
    return msg;
}

// ============================================================================
// CFFI / C API description tables
// ============================================================================

static const std::unordered_map<int, const char*>& cffi_error_table() {
    static const std::unordered_map<int, const char*> table = {
        {0,  "no error."},
        {1,  "memory allocation error."},
        {2,  "cannot open input file."},
        {3,  "cannot open report file."},
        {4,  "cannot open output file."},
        {5,  "input parsing error."},
        {6,  "invalid engine lifecycle state transition."},
        {7,  "invalid engine handle."},
        {8,  "invalid object index."},
        {9,  "invalid parameter value."},
        {10, "plugin initialization or execution error."},
        {11, "file I/O error."},
        {12, "hot start file error."},
        {13, "coordinate reference system error."},
        {14, "numerical solver error."},
        {99, "internal engine error."},
    };
    return table;
}

static const std::unordered_map<int, const char*>& cffi_warning_table() {
    static const std::unordered_map<int, const char*> table = {
        {0, "no warning."},
        {1, "hot start file not found, cold start used."},
        {2, "unknown section in input file."},
        {3, "unknown option in input file."},
        {4, "deprecated keyword used."},
        {5, "plugin initialization warning."},
        {6, "numerical approximation used."},
        {7, "time step reduced for stability."},
    };
    return table;
}

std::string_view cffi_error_get_template(int code) noexcept {
    auto& t = cffi_error_table();
    auto it = t.find(code);
    return (it != t.end()) ? std::string_view(it->second) : std::string_view{};
}

std::string_view cffi_warning_get_template(int code) noexcept {
    auto& t = cffi_warning_table();
    auto it = t.find(code);
    return (it != t.end()) ? std::string_view(it->second) : std::string_view{};
}

std::string format_cffi_error(int code, std::string_view name) {
    auto tmpl = cffi_error_get_template(code);
    if (tmpl.empty()) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "  ERROR %d: unknown CFFI error.", code);
        return buf;
    }
    char prefix[32];
    std::snprintf(prefix, sizeof(prefix), "  ERROR %d: ", code);
    return std::string(prefix) + substitute(tmpl, name);
}

std::string format_cffi_warning(int code, std::string_view name) {
    auto tmpl = cffi_warning_get_template(code);
    if (tmpl.empty()) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "  WARNING %d: unknown CFFI warning.", code);
        return buf;
    }
    char prefix[32];
    std::snprintf(prefix, sizeof(prefix), "  WARNING %d: ", code);
    return std::string(prefix) + substitute(tmpl, name);
}

} /* namespace openswmm */
