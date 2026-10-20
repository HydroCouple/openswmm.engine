// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "GwRechargePluginAbi.h"
#include <string>
namespace openswmm::twoD {
struct GwRechargeBackend {
    using Evaluate=int (*)(const OpenSwmmGwRechargeInputV1*,int,double*,int);
    using Threads=int (*)();
    Evaluate evaluate=nullptr;
    Threads threads=nullptr;
    OpenSwmmGwCapabilitiesV1 capabilities{};
    std::string diagnostic;
};
}
