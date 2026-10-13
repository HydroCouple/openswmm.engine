// SPDX-License-Identifier: Apache-2.0
#include "LidNodeExchange.hpp"
#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace openswmm::twoD {
namespace {
bool nonnegative(double x) { return std::isfinite(x) && x>=0; }
double award(double demand, long double total, double ceiling, long double& spent) {
    const long double scale=total>ceiling?ceiling/total:1;
    double v=static_cast<double>(std::min(std::max(0.L,ceiling-spent),demand*scale));
    while (spent+v>ceiling) v=std::nextafter(v,0.0);
    spent+=v;return v;
}
}
RichardsBedTrial trialRichardsBeds(const SubsurfaceSolver& gw,double dt,
                                  const std::vector<RichardsBedBinding>& input) {
    RichardsBedTrial result;
    auto fail=[](const std::string& error){RichardsBedTrial r;r.error=error;return r;};
    if (!std::isfinite(dt) || dt<=0) return fail("Invalid Richards bed trial interval.");
    auto bindings=input;
    const auto& beds=gw.nodeBeds();const auto& s=gw.state();
    for(const auto& b:bindings)
        if(b.bed<0 || b.bed>=int(beds.size()) || !b.column || !std::isfinite(b.invert) ||
           !nonnegative(b.max_bottom_water) || !nonnegative(b.clogging) || b.clogging>1)
            return fail("Invalid Richards bed binding.");
    std::sort(bindings.begin(),bindings.end(),[&](const auto& a,const auto& b){return beds[a.bed].node<beds[b.bed].node;});
    std::set<int> nodes;std::set<const richards::Column*> columns;
    std::map<int,SubsurfaceSolver::NodeWaterBudget> budgets;
    std::vector<double> node_donor,node_receiver;
    for(const auto& binding:bindings) {
        const auto& bed=beds[binding.bed];const auto& source=*binding.column;
        if(bed.node<0 || bed.cell<0 || !bed.exchange || !nodes.insert(bed.node).second ||
           !columns.insert(binding.column).second || source.cells.size()<2 || source.cells.size()!=source.water.size() ||
           !nonnegative(bed.area) || bed.area==0 || !nonnegative(bed.Kc) || !nonnegative(bed.dC))
            return fail("Unresolved, disabled or duplicate Richards bed/column.");
        if(!budgets.count(bed.cell)) {
            SubsurfaceSolver::NodeWaterBudget budget;
            const auto error=gw.nodeWaterBudget(bed.cell,dt,budget);if(!error.empty())return fail(error);
            budgets.emplace(bed.cell,budget);
        }
        // Validate material/state through the normal kernel before evaluating
        // retention inverses. The proposal boundary replaces outside drainage.
        auto col=source;col.bottom=richards::Bottom::Head;col.bottom_area=bed.area;
        col.bottom_factor=binding.clogging;
        col.bottom_head=budgets[bed.cell].head-binding.invert;
        col.bottom_K=bed.Kc>0?bed.Kc:s.Ks[bed.cell];
        col.bottom_distance=bed.Kc>0?bed.dC:0.5*s.zs[bed.cell];
        if(bed.Kc>0 && bed.dC<=0) return fail("Semi-confining bed requires positive thickness.");
        col.bottom_out_limit=col.bottom_in_limit=std::numeric_limits<double>::infinity();
        result.columns.push_back(std::move(col));
        result.receipts.push_back({bed.node,binding.bed,bed.cell,0,budgets[bed.cell].head});
        double donor=0,et=0;
        for(std::size_t k=1;k<source.water.size();++k) {
            donor+=std::max(0.0,source.water[k]-source.cells[k].material.theta_r*source.cells[k].volume);
            et+=source.cells[k].potential_et*dt;
        }
        node_donor.push_back(std::max(0.0,donor-et)); // no surface/future inflow counted
        node_receiver.push_back(std::max(0.0,binding.max_bottom_water-source.water.back()));
    }
    std::vector<richards::Column*> packed;
    for(auto& c:result.columns)packed.push_back(&c);
    auto proposed=richards::advance(packed,dt);
    for(const auto& r:proposed) {
        if(!r.ok)return fail(r.error);
        result.rhs+=r.rhs;result.newton+=r.newton;
    }
    std::map<int,long double> down,up,spent_down,spent_up;
    for(std::size_t j=0;j<bindings.size();++j) {
        auto& r=result.receipts[j];const auto& source=*bindings[j].column;const auto& c=source.cells.back();
        r.lid_head=bindings[j].invert+c.z+richards::pressure(c.material,c.theta_s,source.water.back()/c.volume);
        r.requested_down=proposed[j].bottom_outflow;r.requested_up=proposed[j].bottom_inflow;
        down[r.cell]+=std::min(r.requested_down,node_donor[j]);
        up[r.cell]+=std::min(r.requested_up,node_receiver[j]);
    }
    for(std::size_t j=0;j<bindings.size();++j) {
        auto& r=result.receipts[j];const auto& b=budgets[r.cell];
        r.reserved_down=award(std::min(r.requested_down,node_donor[j]),down[r.cell],b.receiver,spent_down[r.cell]);
        r.reserved_up=award(std::min(r.requested_up,node_receiver[j]),up[r.cell],b.donor,spent_up[r.cell]);
        auto& c=result.columns[j];c.water=bindings[j].column->water;
        c.bottom_out_limit=r.reserved_down;c.bottom_in_limit=r.reserved_up;
    }
    result.reports=richards::advance(packed,dt);
    for(std::size_t j=0;j<bindings.size();++j) {
        const auto& report=result.reports[j];if(!report.ok)return fail(report.error);
        auto& r=result.receipts[j];r.accepted_down=report.bottom_outflow;r.accepted_up=report.bottom_inflow;
        if(r.accepted_down>r.reserved_down || r.accepted_up>r.reserved_up)
            return fail("Richards accepted transfer exceeded a directional reservation.");
        r.unused_down=r.reserved_down-r.accepted_down;r.unused_up=r.reserved_up-r.accepted_up;
        result.rhs+=report.rhs;result.newton+=report.newton;
    }
    for(const auto& b:budgets)result.budgets.push_back(b);
    return result;
}
}
