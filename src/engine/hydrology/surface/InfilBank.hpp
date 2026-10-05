// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin

#pragma once
#include "../Infiltration.hpp"
#include <vector>

namespace openswmm::surface {

/// One state owner and one dispatch for subcatchments and mesh cells. Kernels
/// retain their legacy feet arithmetic; the SI boundary converts only once.
class InfilBank {
public:
    enum class Owner { BANK, EXTERNAL };
    struct Factors { double infiltration = 1.0, recovery = 1.0; };
    void init(int n);
    void setMethod(int e, InfilModel method, const double p[5], const SimulationOptions& options);
    void setOwner(int e, Owner owner) { owners_[e] = owner; }
    Owner owner(int e) const { return owners_[e]; }
    double rate(int e, double precip_m_s, double runon_m_s, double depth_m,
                double dt, Factors factors);
    // Avoid a feet -> SI -> feet roundtrip on the existing runoff path.
    double rateFeet(int e, double precip, double runon, double depth, double dt, Factors factors);
    void pack(int e, int& method, double state[6]) const noexcept;
    void unpack(int e, int method, const double state[6]) noexcept;
    const std::vector<double>& cumulative() const noexcept { return cumulative_; }
    void clear();
private:
    std::vector<InfilModel> methods_;
    std::vector<Owner> owners_;
    std::vector<HortonState> horton_;
    std::vector<GreenAmptState> grnampt_;
    std::vector<CurveNumState> curvenum_;
    std::vector<double> constant_, cumulative_;
    double evaluate(int e, double precip, double runon, double depth, double dt, Factors factors);
};
} // namespace openswmm::surface
