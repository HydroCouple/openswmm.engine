// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "NativeSnow.hpp"
#include "../Runoff.hpp"
#include <vector>
namespace openswmm::detail {class CompletedSourceArchive;}
namespace openswmm::surface {
// Snow owns solid/liquid pack water. The source water ledger receives only
// completed liquid delivery; together their two balances recover precipitation.
struct SourceSnowLedger {
    double end=0,initial=0,precipitation=0,liquid=0,sublimation=0,removed=0,stored=0; // seconds, ft3
    double residual()const{return initial+precipitation-liquid-sublimation-removed-stored;}
};
class SourceSnow {
public:
    std::string initialize(SimulationContext&,const std::vector<std::vector<int>>& groups,const std::vector<double>& nonlid_area);
    void advance(SimulationContext&,int group,double start,double end,std::vector<runoff::RunoffSourceForcing>&);
    void checkpoint(detail::CompletedSourceArchive&,bool model,SimulationContext&);
    // Overlay accepted owner columns into a native hot-start view. Parameters
    // and native model identity remain unchanged; seeding resets flux ledgers.
    void exportAccepted(const SimulationContext& source,SimulationContext& target,snow::SnowSoA&)const;
    bool active()const{return active_;}
    const snow::SnowSoA& state()const{return solver_.state();}
    const std::vector<SourceSnowLedger>& ledgers()const{return ledger_;}
private:
    bool active_=false;
    snow::SnowSolver solver_;
    std::vector<std::vector<int>> groups_;
    std::vector<double> area_;
    std::vector<SourceSnowLedger> ledger_;
    double storage(int group)const;
};
}
