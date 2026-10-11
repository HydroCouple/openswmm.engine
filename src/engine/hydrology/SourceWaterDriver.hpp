// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin
#ifndef OPENSWMM_SOURCE_WATER_DRIVER_HPP
#define OPENSWMM_SOURCE_WATER_DRIVER_HPP
#include "SourceClockGroups.hpp"
#include "LID.hpp"
#include <array>
#include <memory>

namespace openswmm::detail { class CompletedSourceArchive; }

namespace openswmm::runoff {

struct SourceWaterLedger {
    double initial_storage = 0.0, rain = 0.0, evaporation = 0.0, infiltration = 0.0;
    /// Completed destination split, ft3; sum equals infiltration.
    double spatial_infiltration = 0.0, outside_infiltration = 0.0;
    double aquifer_return = 0.0; // cumulative paired surface credits, ft3, never rainfall
    double outfall_return = 0.0; // cumulative hydraulic receipts, ft3, never rainfall
    double outlet = 0.0, captured = 0.0, runon = 0.0, pervious_return = 0.0;
};
struct SourceWaterDelivery {
    int source = -1, node = -1;
    double start = 0.0, end = 0.0, volume = 0.0; // ft3; node -1 = unresolved external outlet
    bool drain = false;
    std::vector<double> mass; // pollutant concentration units times m3, paired with volume
};
/// Private completed source pollutant accounting, concentration units times
/// m3 (no kg/lb conversion). For LIDs, layers are surface/pavement/soil/storage;
/// type -1 denotes non-LID IMPERV0/IMPERV1/PERV pools (fourth slot unused).
/// Initial source water is clean; network INIT_CONC is not a source seed.
struct SourcePollutantLedger {
    int source = -1, type = -1, unit = -1;
    std::vector<std::array<double, 4>> layers;
    std::vector<double> rain, spatial_infiltration, outside_infiltration, outlet, removed;
    std::vector<double> surface_mass, drain_mass; // last completed forcing segment
    std::vector<double> impervious_mass, pervious_mass; // non-LID outlet pools before capture
    std::vector<double> aquifer_return; // cumulative paired surface credits
    std::vector<double> pending_routing, incoming; // internal subarea queue; cumulative received transfers
};
struct SourceSurfaceReturn {
    int source=-1,type=-1,unit=-1,pool=2; // non-LID subarea or exact LID unit
    double volume=0; // ft3, consumed once at the next source segment
    std::vector<double> mass; // pollutant concentration units times m3
};
struct SourceOutfallReturn {
    int source=-1;
    double available=0, volume=0; // accepted hydraulic seconds, ft3
};
enum class SourceEtKind { PERVIOUS, IMPERVIOUS, LID };
struct SourceAtmosphericLedger {
    SourceEtKind kind = SourceEtKind::PERVIOUS;
    int source = -1, type = -1, unit = -1;
    double area = 0.0; // ft2, physical footprint (capture does not enlarge it)
    bool soil_eligible = false;
    double potential = 0.0, evaporation = 0.0; // cumulative completed ft3
};

/// Internal completed-source qualification driver, initialized at simulation start.
/// All context/kernel/volume histories are privately owned. Stage cannot edit
/// the live engine or write LID reports. Commit adopts the private water trial
/// and clock together; it DOES NOT settle a spatial receiver or activate R4.
/// Water profile: non-LID runoff and existing subcatchment LID stores. Pollutant
/// profile: conservative pollutants in non-LID/LID pools and their captured,
/// returned and connected runoff, without land-use quality or reactions.
/// Neither profile supports lumped GW, snow, age/heat/MSX or production hot starts.
/// The internal accepted-boundary checkpoint requires a matching fresh model.
/// No production caller yet.
class SourceWaterDriver {
public:
    SourceWaterDriver();
    ~SourceWaterDriver();
    SourceWaterDriver(SourceWaterDriver&&) noexcept;
    SourceWaterDriver& operator=(SourceWaterDriver&&) noexcept;
    /// Independent water/clock snapshot for a multi-group receiving trial.
    SourceWaterDriver waterTrial() const;
    void checkpoint(detail::CompletedSourceArchive&,bool model); // internal accepted-boundary IO
    using BottomCeiling = std::function<double(int type, int unit, double start, double end)>;
    /// Transform a private copy of each resolved native weather segment.
    /// The caller supplies completed-interval prescriptions; native clocks stay immutable.
    using ForcingTransform = std::function<void(SourceForcingInterval&)>;
    std::string initialize(const SimulationContext&, const std::vector<int>& seeds,
                           const std::vector<std::pair<int, double>>& non_lid_areas = {},
                           const lid::LIDSolver* initial_lids = nullptr,
                           bool hydraulic_returns = false);
    /// Bind reviewed uniform inside fractions at initial attachment; no water debit.
    std::string configureSpatialCoverage(const std::vector<double>& inside_fractions);
    const std::vector<double>& spatialFractions() const;
    /// Atomic batch on a committed private snapshot, before interval trials.
    std::string creditSurfaceReturns(double start,const std::vector<SourceSurfaceReturn>&);
    /// Explicit water-only hydraulic receipt profile. Credit an accepted interval
    /// once; consume over the full physical footprint only from its end onward.
    /// Source groups may lag in a pending surface batch. Unowned entries are zero.
    std::string creditOutfallReturns(double start, double end, const std::vector<double>& volumes);
    bool acceptsOutfallReturns() const;
    double outfallReturnEnd() const;
    const std::vector<SourceOutfallReturn>& pendingOutfallReturns() const;
    std::string stage(int group, double end, double completed_through,
                      const RunoffSolver::InfiltrationBoundary* boundary = nullptr,
                      const BottomCeiling* bottom_ceiling = nullptr,
                      const ForcingTransform* forcing = nullptr);
    std::string commit();
    void cancel();
    const SourceClockGroups& clocks() const { return clocks_; }
    const SimulationContext& context(bool trial = false) const;
    const RunoffSolver& runoff(bool trial = false) const;
    const lid::LIDSolver& lids(bool trial = false) const;
    const std::vector<SourceWaterLedger>& ledgers(bool trial = false) const;
    const std::vector<SourceWaterDelivery>& deliveries(bool trial = false) const;
    const std::vector<SourceAtmosphericLedger>& atmosphere(bool trial = false) const;
    const std::vector<std::string>& pollutantNames() const;
    const std::vector<SourcePollutantLedger>& pollutants(bool trial = false) const;
    double pollutantBalanceResidual(int group, int pollutant, bool trial = false) const;
    double pendingPollutantMass(int group, int pollutant, bool trial = false) const;
    double pendingVolume(int group, bool trial = false) const;
    double storedVolume(int source) const;
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
