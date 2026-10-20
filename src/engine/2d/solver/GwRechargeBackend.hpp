// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "GwRechargePluginAbi.h"
#include <string>
#include <functional>
namespace openswmm::twoD {
struct GwRechargeBackend {
    using Evaluate=int (*)(const OpenSwmmGwRechargeInputV1*,int,double*,int);
    using Threads=int (*)();
    std::function<int(const OpenSwmmGwRechargeInputV1*,int,double*,int)> evaluate;
    std::function<int()> threads;
    OpenSwmmGwCapabilitiesV1 capabilities{};
    std::string diagnostic;
};
}
