/*!
 * \file worker_progress.h
 * \brief JSON progress serialization for openswmm-legacy-worker.
 *
 * The worker emits progress updates to stdout as JSON objects (one per line).
 * Each line is a complete, valid JSON object that can be independently parsed.
 * Uses plain C++ (no Qt dependency).
 */

#ifndef OPENSWMM_LEGACY_WORKER_PROGRESS_H
#define OPENSWMM_LEGACY_WORKER_PROGRESS_H

#include <cstdio>
#include <cstring>

namespace WorkerProgress {

/**
 * @brief Safely escape a string for JSON.
 *
 * Handles quotes, backslashes, newlines, etc.
 */
inline void escapeJson(const char *input, char *output, int maxLen)
{
    const char *src = input;
    char *dst = output;
    char *end = output + maxLen - 1;

    while (*src && dst < end) {
        switch (*src) {
            case '"':
                if (dst + 1 < end) { *dst++ = '\\'; *dst++ = '"'; }
                break;
            case '\\':
                if (dst + 1 < end) { *dst++ = '\\'; *dst++ = '\\'; }
                break;
            case '\n':
                if (dst + 1 < end) { *dst++ = '\\'; *dst++ = 'n'; }
                break;
            case '\r':
                if (dst + 1 < end) { *dst++ = '\\'; *dst++ = 'r'; }
                break;
            case '\t':
                if (dst + 1 < end) { *dst++ = '\\'; *dst++ = 't'; }
                break;
            default:
                *dst++ = *src;
        }
        ++src;
    }
    *dst = '\0';
}

/**
 * @brief Emit a progress update as a JSON line to stdout.
 *
 * @param stepCount Number of simulation steps completed so far
 * @param elapsed Cumulative elapsed simulation time (days)
 * @param runoffErrFrac Running runoff continuity error as a fraction
 *                      (0.001 = 0.1 %), or 0.0 if not yet meaningful.
 * @param routingErrFrac Running flow-routing continuity error as a fraction.
 *
 * The continuity values are sent as fractions (not percentages) so the GUI
 * pipeline, which multiplies by 100 for display, matches the refactored
 * engine's runoff/routing-error convention and the final "continuity" line.
 *
 * Format:
 * `{"type":"progress","stepCount":123,"elapsed":0.5,"runoff":0.0001,"routing":0.0002}\n`
 */
inline void emitProgress(int stepCount, double elapsed,
                         double runoffErrFrac, double routingErrFrac)
{
    std::printf("{\"type\":\"progress\",\"stepCount\":%d,\"elapsed\":%.6f,"
                "\"runoff\":%.8f,\"routing\":%.8f}\n",
                stepCount, elapsed, runoffErrFrac, routingErrFrac);
    std::fflush(stdout);
}

/**
 * @brief Emit the simulation window (start/end) as a JSON line to stdout.
 *
 * @param startOADate Simulation start as an OADate (decimal days since
 *                    1899-12-30 — the SWMM DateTime epoch).
 * @param endOADate   Simulation end as an OADate.
 *
 * Emitted once after swmm_start so the parent can derive a 0–1 progress
 * fraction from the per-step elapsed-days value and render readable
 * start/current/end columns.
 *
 * Format: `{"type":"dates","start":45000.000000,"end":45010.000000}\n`
 */
inline void emitDates(double startOADate, double endOADate)
{
    std::printf("{\"type\":\"dates\",\"start\":%.6f,\"end\":%.6f}\n",
                startOADate, endOADate);
    std::fflush(stdout);
}

/**
 * @brief Emit the final mass-balance (continuity) errors to stdout.
 *
 * @param runoffErrFrac  Runoff continuity error as a fraction (0.001 = 0.1 %).
 * @param routingErrFrac Flow-routing continuity error as a fraction.
 *
 * Emitted once after swmm_end() (the errors are only finalised then). The GUI
 * forwards these to the simulation status model, which multiplies by 100 for
 * display — so the values are sent as fractions, not percentages, to match the
 * refactored engine's runoff/routing-error convention.
 *
 * Format: `{"type":"continuity","runoff":0.00050000,"routing":0.00120000}\n`
 */
inline void emitContinuity(double runoffErrFrac, double routingErrFrac)
{
    std::printf("{\"type\":\"continuity\",\"runoff\":%.8f,\"routing\":%.8f}\n",
                runoffErrFrac, routingErrFrac);
    std::fflush(stdout);
}

/**
 * @brief Emit a warning message to stdout.
 *
 * @param message Warning text (e.g., "Unknown option KEY skipped")
 *
 * Format: `{"type":"warning","message":"text"}\n`
 */
inline void emitWarning(const char *message)
{
    char escaped[512];
    escapeJson(message, escaped, sizeof(escaped));
    std::printf("{\"type\":\"warning\",\"message\":\"%s\"}\n", escaped);
    std::fflush(stdout);
}

/**
 * @brief Emit an error message to stderr and stdout.
 *
 * @param code Error code (SWMM error code)
 * @param message Human-readable error message
 *
 * Writes to both stderr (for immediate logging) and stdout (for parsing).
 * Format: `{"type":"error","code":5,"message":"text"}\n`
 */
inline void emitError(int code, const char *message)
{
    std::fprintf(stderr, "ERROR %d: %s\n", code, message);
    std::fflush(stderr);

    char escaped[512];
    escapeJson(message, escaped, sizeof(escaped));
    std::printf("{\"type\":\"error\",\"code\":%d,\"message\":\"%s\"}\n", code, escaped);
    std::fflush(stdout);
}

} // namespace WorkerProgress

#endif // OPENSWMM_LEGACY_WORKER_PROGRESS_H
