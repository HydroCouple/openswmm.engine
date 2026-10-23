/**
 * @file      worker_progress.h
 * @brief     JSON progress serialization for openswmm-legacy-worker.
 *
 * The worker emits progress updates to stdout as JSON objects (one per line).
 * Each line is a complete, valid JSON object that can be independently parsed.
 * Uses plain C++ (no Qt dependency).
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com> (OpenSWMM maintenance and extensions)
 * @copyright Copyright (c) 2026 Caleb Buahin. OpenSWMM contributions only; original EPA SWMM
 *            credits are retained.
 * @license   Apache-2.0 for OpenSWMM contributions; original EPA SWMM provenance is retained.
 */

#ifndef OPENSWMM_LEGACY_WORKER_PROGRESS_H
#define OPENSWMM_LEGACY_WORKER_PROGRESS_H

#include <cstdio>
#include <cstring>

namespace WorkerProgress {

/**
 * @brief Escape a bounded diagnostic string for the worker JSON fields.
 *
 * Escapes quote, backslash, newline, carriage return and tab; copies other bytes unchanged,
 * including other control bytes. Stops at capacity minus one, always terminates under the
 * positive-capacity precondition, and silently omits a two-byte escape if only one payload byte
 * remains. Does not report truncation or validate UTF-8.
 *
 * @param[in] input Non-null null-terminated source, borrowed for this call.
 * @param[out] output Non-null writable buffer of at least maxLen bytes; must not overlap input.
 * @param[in] maxLen Positive buffer capacity including the final null terminator; zero/negative
 *     capacities are invalid and unchecked.
 */
inline void escapeJson(const char* input, char* output, int maxLen) {
    const char* src = input;
    char* dst = output;
    char* end = output + maxLen - 1;

    while (*src && dst < end) {
        switch (*src) {
        case '"':
            if (dst + 1 < end) {
                *dst++ = '\\';
                *dst++ = '"';
            }
            break;
        case '\\':
            if (dst + 1 < end) {
                *dst++ = '\\';
                *dst++ = '\\';
            }
            break;
        case '\n':
            if (dst + 1 < end) {
                *dst++ = '\\';
                *dst++ = 'n';
            }
            break;
        case '\r':
            if (dst + 1 < end) {
                *dst++ = '\\';
                *dst++ = 'r';
            }
            break;
        case '\t':
            if (dst + 1 < end) {
                *dst++ = '\\';
                *dst++ = 't';
            }
            break;
        default:
            *dst++ = *src;
        }
        ++src;
    }
    *dst = '\0';
}

/**
 * @brief Write and flush one progress JSON line to stdout.
 *
 * Formats elapsed to six decimal places and errors to eight; finite numeric values are required for
 * valid JSON. IO errors are ignored.
 *
 * @param[in] stepCount Number of successful advancing simulation steps.
 * @param[in] elapsed Elapsed simulation time in decimal days.
 * @param[in] runoffErrFrac Running runoff continuity fraction, not percent; zero if unavailable.
 * @param[in] routingErrFrac Running routing continuity fraction, not percent.
 */
inline void emitProgress(int stepCount, double elapsed, double runoffErrFrac,
                         double routingErrFrac) {
    std::printf("{\"type\":\"progress\",\"stepCount\":%d,\"elapsed\":%.6f,"
                "\"runoff\":%.8f,\"routing\":%.8f}\n",
                stepCount, elapsed, runoffErrFrac, routingErrFrac);
    std::fflush(stdout);
}

/**
 * @brief Write and flush the simulation window as one JSON line.
 *
 * Typically emitted after swmm_start; both dates use six decimal places. IO errors are ignored.
 *
 * @param[in] startOADate Finite start date in decimal days since the legacy SWMM epoch, 1899-12-30.
 * @param[in] endOADate Finite end date in the same units.
 */
inline void emitDates(double startOADate, double endOADate) {
    std::printf("{\"type\":\"dates\",\"start\":%.6f,\"end\":%.6f}\n", startOADate, endOADate);
    std::fflush(stdout);
}

/**
 * @brief Write and flush final runoff/routing continuity fractions as JSON.
 *
 * Caller obtains finalized errors after swmm_end and converts engine percentages to fractions.
 * Formats eight decimal places; IO errors are ignored.
 *
 * @param[in] runoffErrFrac Finite final runoff error fraction; 0.001 means 0.1 percent.
 * @param[in] routingErrFrac Finite final routing error fraction.
 */
inline void emitContinuity(double runoffErrFrac, double routingErrFrac) {
    std::printf("{\"type\":\"continuity\",\"runoff\":%.8f,\"routing\":%.8f}\n", runoffErrFrac,
                routingErrFrac);
    std::fflush(stdout);
}

/**
 * @brief Escape and flush one warning JSON line to stdout.
 *
 * Escapes through a 512-byte local buffer, silently truncating long messages under escapeJson
 * rules. Other control bytes are not normalized. IO errors are ignored.
 *
 * @param[in] message Non-null null-terminated text; borrowed only during the call.
 */
inline void emitWarning(const char* message) {
    char escaped[512];
    escapeJson(message, escaped, sizeof(escaped));
    std::printf("{\"type\":\"warning\",\"message\":\"%s\"}\n", escaped);
    std::fflush(stdout);
}

/**
 * @brief Flush a diagnostic to stderr and a matching error JSON line to stdout.
 *
 * Writes full raw text to stderr, then escapes through a 512-byte local buffer for stdout, so the
 * JSON message can be truncated. IO errors are ignored.
 *
 * @param[in] code Error code printed numerically.
 * @param[in] message Non-null null-terminated diagnostic; borrowed only during this call.
 */
inline void emitError(int code, const char* message) {
    std::fprintf(stderr, "ERROR %d: %s\n", code, message);
    std::fflush(stderr);

    char escaped[512];
    escapeJson(message, escaped, sizeof(escaped));
    std::printf("{\"type\":\"error\",\"code\":%d,\"message\":\"%s\"}\n", code, escaped);
    std::fflush(stdout);
}

} // namespace WorkerProgress

#endif // OPENSWMM_LEGACY_WORKER_PROGRESS_H
