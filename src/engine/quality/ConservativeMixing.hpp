// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin
#pragma once
#include "../hydraulics/fv/NativeGmres.hpp"
#include <cstddef>
#include <vector>

namespace openswmm::quality {
/// One accepted internal water transfer, ft3. Donor and receiver are control
/// volume indices; the same transported amount must be used on both sides.
struct WaterTransfer { std::size_t donor, receiver; double volume; };
struct MixingWater {
    std::vector<double> old_volume, new_volume, source_volume;
    std::vector<double> exported, seeped, evaporated;
    std::vector<WaterTransfer> transfers;
};
struct MixingScalar {
    std::vector<double> old_concentration, source_mass, held_dry_mass;
    bool nonnegative = true; // false for temperature-volume
};
struct MixedScalar {
    std::vector<double> concentration, held_dry_mass, transfer_mass, exported_mass, seeped_mass;
};
/// Native conservative complete-mix transport on accepted water receipts.
/// (V_new + all outgoing water except evaporation)*C_new - sum(v_in*C_donor)
/// = V_old*C_old + source_mass + held_dry_mass.
/// Shared matrix/workspace across scalar families; no external solver code.
class ConservativeMixing {
public:
    struct Result {
        bool converged=false;
        double max_water_residual=0, mass_residual=0;
        int iterations=0;
    };
    bool prepare(const MixingWater& water, double relative_water_tolerance=1e-9);
    Result solve(const MixingScalar& scalar, MixedScalar& output);
private:
    MixingWater water_;
    std::vector<double> diagonal_, rhs_, image_, correction_, guess_;
    fv::NativeGmres gmres_;
    MixedScalar candidate_;
    std::vector<double> incoming_, outgoing_;
    bool prepared_=false;
    double water_residual_=0;
};
} // namespace openswmm::quality
