// SPDX-License-Identifier: Apache-2.0

/**
 * @file      legacy_consts_values.c
 * @brief     Exposes the legacy engine's consts.h macros as linkable doubles.
 *
 * @details consts.h defines PI, TINY, MISSING, TRUE and other macros that would
 *          collide with the C++ names in Constants.hpp, so the parity test reads
 *          them through this C translation unit instead of including the header.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include "consts.h"

const double legacy_consts_PI = PI;
const double legacy_consts_GRAVITY = GRAVITY;
const double legacy_consts_PHI = PHI;
const double legacy_consts_TINY = TINY;
const double legacy_consts_MISSING = MISSING;
const double legacy_consts_FUDGE = FUDGE;
const double legacy_consts_FLOW_TOL = FLOW_TOL;
const double legacy_consts_SECperDAY = SECperDAY;
const double legacy_consts_MSECperDAY = MSECperDAY;
const double legacy_consts_MperFT = MperFT;
const double legacy_consts_LperFT3 = LperFT3;
const double legacy_consts_M3perFT3 = M3perFT3;
const double legacy_consts_MMperINCH = MMperINCH;
const double legacy_consts_GPMperCFS = GPMperCFS;
const double legacy_consts_MGDperCFS = MGDperCFS;
const double legacy_consts_LPSperCFS = LPSperCFS;
const double legacy_consts_MLDperCFS = MLDperCFS;
