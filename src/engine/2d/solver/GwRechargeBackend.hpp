// SPDX-License-Identifier: Apache-2.0

/**
 * @file      GwRechargeBackend.hpp
 * @brief     Backend contract for groundwater recharge operations.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#pragma once
#include "GwRechargePluginAbi.h"
#include <string>
#include <functional>
namespace openswmm::twoD {
struct GwRechargeBackend {
    using Evaluate = int (*)(const OpenSwmmGwRechargeInputV1*, int, double*, int);
    using Threads = int (*)();
    std::function<int(const OpenSwmmGwRechargeInputV1*, int, double*, int)> evaluate;
    std::function<int()> threads;
    OpenSwmmGwCapabilitiesV1 capabilities{};
    std::string diagnostic;
};
}
