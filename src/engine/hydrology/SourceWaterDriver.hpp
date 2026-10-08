// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin
#ifndef OPENSWMM_SOURCE_WATER_DRIVER_HPP
#define OPENSWMM_SOURCE_WATER_DRIVER_HPP
#include "SourceClockGroups.hpp"
#include "LID.hpp"
#include <memory>

namespace openswmm::runoff {

struct SourceWaterLedger {
    double initial_storage = 0.0, rain = 0.0, evaporation = 0.0, infiltration = 0.0;
    /// Completed destination split, ft3; sum equals infiltration.
    double spatial_infiltration = 0.0, outside_infiltration = 0.0;
    double outlet = 0.0, captured = 0.0, runon = 0.0, pervious_return = 0.0;
};
struct SourceWaterDelivery {
    int source = -1, node = -1;
    double start = 0.0, end = 0.0, volume = 0.0; // ft3; node -1 = unresolved external outlet
    bool drain = false;
};
enum class SourceEtKind { PERVIOUS, IMPERVIOUS, LID };
struct SourceAtmosphericLedger {
    SourceEtKind kind = SourceEtKind::PERVIOUS;
    int source = -1, type = -1, unit = -1;
    double area = 0.0; // ft2, physical footprint (capture does not enlarge it)
    bool soil_eligible = false;
    double potential = 0.0, evaporation = 0.0; // cumulative completed ft3
};

/// Internal water-only qualification driver, initialized at simulation start.
/// All context/kernel/volume histories are privately owned. Stage cannot edit
/// the live engine or write LID reports. Commit adopts the private water trial
/// and clock together; it DOES NOT settle a spatial receiver or activate R4.
/// Current profile: non-LID runoff and existing subcatchment LID stores, without
/// lumped GW, snow, quality/age/heat/MSX or hot starts. No production caller yet.
class SourceWaterDriver {
public:
    SourceWaterDriver();
    ~SourceWaterDriver();
    SourceWaterDriver(SourceWaterDriver&&) noexcept;
    SourceWaterDriver& operator=(SourceWaterDriver&&) noexcept;
    /// Independent water/clock snapshot for a multi-group receiving trial.
    SourceWaterDriver waterTrial() const;
    using BottomCeiling = std::function<double(int type, int unit, double start, double end)>;
    std::string initialize(const SimulationContext&, const std::vector<int>& seeds,
                           const std::vector<std::pair<int, double>>& non_lid_areas = {},
                           const lid::LIDSolver* initial_lids = nullptr);
    /// Bind reviewed uniform inside fractions at initial attachment; no water debit.
    std::string configureSpatialCoverage(const std::vector<double>& inside_fractions);
    const std::vector<double>& spatialFractions() const;
    std::string stage(int group, double end, double completed_through,
                      const RunoffSolver::InfiltrationBoundary* boundary = nullptr,
                      const BottomCeiling* bottom_ceiling = nullptr);
    std::string commit();
    void cancel();
    const SourceClockGroups& clocks() const { return clocks_; }
    const SimulationContext& context(bool trial = false) const;
    const RunoffSolver& runoff(bool trial = false) const;
    const lid::LIDSolver& lids(bool trial = false) const;
    const std::vector<SourceWaterLedger>& ledgers(bool trial = false) const;
    const std::vector<SourceWaterDelivery>& deliveries(bool trial = false) const;
    const std::vector<SourceAtmosphericLedger>& atmosphere(bool trial = false) const;
    double pendingVolume(int group, bool trial = false) const;
    double balanceResidual(int group, bool trial = false) const;
private:
    struct State;
    std::unique_ptr<State> state_, trial_;
    SourceClockGroups clocks_;
    int pending_group_ = -1;
    const State& view(bool trial) const;
    double storage(const State&, int source) const;
    void advance(State&, const SourceForcingInterval&,
                 const RunoffSolver::InfiltrationBoundary*, const BottomCeiling*);
};
}
#endif
