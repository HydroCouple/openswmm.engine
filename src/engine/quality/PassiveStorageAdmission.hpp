// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin
#pragma once
#include <cstddef>
namespace openswmm::twoD { struct SurfaceStateData; }

namespace openswmm::quality {
// Borrowed only during a real native producer window. No allocation or
// exception may cross an admission/application call in the cell writer.
class PassiveStorageAdmission {
public:
    virtual ~PassiveStorageAdmission() = default;
    virtual bool admit(std::size_t point, int cell, int node, double signed_water_si,
        double actual_mass_si, double before, double volume_before,
        double offset_s, double duration_s, double withdrawal_si = 0) noexcept = 0;
    virtual void applied(double actual_after, double volume_after) noexcept = 0;
    virtual void fail(const char* reason) noexcept = 0;
    virtual bool failed() const noexcept = 0;
    virtual const char* error() const noexcept = 0;
    virtual bool owns(const twoD::SurfaceStateData*) const noexcept { return true; }
};
}
