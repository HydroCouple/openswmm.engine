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

#endif /* OPENSWMM_API_COMMON_HPP */
