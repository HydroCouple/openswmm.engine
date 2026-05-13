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
 * @param elapsed Time elapsed in current step (seconds)
 *
 * Format: `{"type":"progress","stepCount":123,"elapsed":0.5}\n`
 */
inline void emitProgress(int stepCount, double elapsed)
{
    std::printf("{\"type\":\"progress\",\"stepCount\":%d,\"elapsed\":%.6f}\n",
                stepCount, elapsed);
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
