// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin
#pragma once
#include "PassiveStorageExchange.hpp"
namespace openswmm::quality {
PassiveStorageExchange::AppliedLeg passiveStorageAttempt(std::size_t point, int cell, int node,
    double water, double mass, double before, double volume_before, double offset, double duration,
    double withdrawal, double factor, double start, double end, PassiveStorageExchange::Phase phase) noexcept;
const char* passiveStorageLegError(const PassiveStorageExchange::AppliedLeg& leg, double signed_water) noexcept;
}
