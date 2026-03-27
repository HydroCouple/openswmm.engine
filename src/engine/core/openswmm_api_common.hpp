/**
 * @file openswmm_api_common.hpp
 * @brief Internal shared helpers for C API impl files.
 *
 * @details NOT installed — used only by openswmm_*_impl.cpp files.
 *          Provides the opaque-handle cast and common guard macros.
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_API_COMMON_HPP
#define OPENSWMM_API_COMMON_HPP

#include "SWMMEngine.hpp"
#include "../../../include/openswmm/engine/openswmm_engine.h"

#include <cstring>
#include <algorithm>

static inline openswmm::SWMMEngine* to_engine(SWMM_Engine e) noexcept {
    return static_cast<openswmm::SWMMEngine*>(e);
}

#define CHECK_HANDLE(e)    do { if (!(e)) return SWMM_ERR_BADHANDLE; } while(0)
#define CHECK_INDEX(cond)  do { if (!(cond)) return SWMM_ERR_BADINDEX; } while(0)

// ============================================================================
// Lifecycle state guards
// ============================================================================

/// Geometry/topology setters: only BUILDING or OPENED
#define CHECK_GEOMETRY(ctx) \
    do { \
        if ((ctx).state != openswmm::EngineState::BUILDING && \
            (ctx).state != openswmm::EngineState::OPENED) \
            return SWMM_ERR_LIFECYCLE; \
    } while(0)

/// Initial-condition setters: BUILDING, OPENED, or INITIALIZED
#define CHECK_INITIAL_COND(ctx) \
    do { \
        if ((ctx).state != openswmm::EngineState::BUILDING && \
            (ctx).state != openswmm::EngineState::OPENED && \
            (ctx).state != openswmm::EngineState::INITIALIZED) \
            return SWMM_ERR_LIFECYCLE; \
    } while(0)

/// Runtime forcing / state override: only RUNNING
#define CHECK_RUNNING(ctx) \
    do { \
        if ((ctx).state != openswmm::EngineState::RUNNING) \
            return SWMM_ERR_LIFECYCLE; \
    } while(0)

/// Read-only access: any state where data exists (not CREATED, CLOSED, or ERROR)
#define CHECK_READABLE(ctx) \
    do { \
        if ((ctx).state == openswmm::EngineState::CREATED || \
            (ctx).state == openswmm::EngineState::CLOSED || \
            (ctx).state == openswmm::EngineState::ERROR_STATE) \
            return SWMM_ERR_LIFECYCLE; \
    } while(0)

#endif /* OPENSWMM_API_COMMON_HPP */
